/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ob_hipvs_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef OB_ENABLE_HIPVS
#include <hip/hip_runtime.h>

#include <cuvs/distance/distance.hpp>
#include <cuvs/neighbors/cagra.hpp>
#include <raft/core/device_mdarray.hpp>
#include <raft/core/device_mdspan.hpp>
#include <raft/core/host_mdspan.hpp>

#if __has_include(<raft/core/resources.hpp>)
#include <raft/core/resources.hpp>
namespace oceanbase
{
namespace common
{
namespace hipvs
{
namespace detail
{
using RaftResources = raft::resources;
}  // namespace detail
}  // namespace hipvs
}  // namespace common
}  // namespace oceanbase
#else
#include <raft/core/device_resources.hpp>
namespace oceanbase
{
namespace common
{
namespace hipvs
{
namespace detail
{
using RaftResources = raft::device_resources;
}  // namespace detail
}  // namespace hipvs
}  // namespace common
}  // namespace oceanbase
#endif
#endif  // OB_ENABLE_HIPVS

namespace oceanbase
{
namespace common
{
namespace hipvs
{

namespace
{

thread_local std::string g_last_error;
std::atomic<int> g_available(-1);  // -1 unknown, 0 unavailable, 1 available
std::atomic<int> g_forced_available(-1);

void set_error(const std::string &msg)
{
  g_last_error = msg;
}

bool metric_is_l2(const std::string &metric)
{
  return metric.empty() || metric == "l2" || metric == "L2";
}

float l2_square(const float *lhs, const float *rhs, int64_t dim)
{
  float sum = 0.0f;
  for (int64_t i = 0; i < dim; ++i) {
    const float diff = lhs[i] - rhs[i];
    sum += diff * diff;
  }
  return sum;
}

template <typename T>
void append_pod(std::vector<char> &out, const T &value)
{
  const char *ptr = reinterpret_cast<const char *>(&value);
  out.insert(out.end(), ptr, ptr + sizeof(T));
}

template <typename T>
bool read_pod(const char *data, int64_t size, int64_t &offset, T &value)
{
  if (offset < 0 || offset + static_cast<int64_t>(sizeof(T)) > size) {
    return false;
  }
  memcpy(&value, data + offset, sizeof(T));
  offset += static_cast<int64_t>(sizeof(T));
  return true;
}

bool bytes_mul_overflow(int64_t count, int64_t elem_size, int64_t &bytes)
{
  bool overflow = false;
  if (count < 0 || elem_size < 0) {
    overflow = true;
  } else if (count != 0 && elem_size > std::numeric_limits<int64_t>::max() / count) {
    overflow = true;
  } else {
    bytes = count * elem_size;
  }
  return overflow;
}

bool add_overflow(int64_t lhs, int64_t rhs, int64_t &sum)
{
  bool overflow = false;
  if (lhs < 0 || rhs < 0
      || lhs > std::numeric_limits<int64_t>::max() - rhs) {
    overflow = true;
  } else {
    sum = lhs + rhs;
  }
  return overflow;
}

}  // namespace

void set_availability_for_test(bool available)
{
  g_forced_available.store(available ? 1 : 0);
}

struct ObHipvsIndex::Impl
{
  Impl() = default;

  bool build_gpu(int device_id);
  int search_gpu(const float *queries,
                 int64_t query_count,
                 int64_t k,
                 int64_t itopk,
                 int64_t *out_ids,
                 float *out_distances) const;

  int64_t dim_ = 0;
  int64_t base_count_ = 0;
  std::string metric_;
  int graph_degree_ = 64;
  int intermediate_graph_degree_ = 128;
  int ef_construction_ = 200;

  std::vector<int64_t> ids_;
  std::vector<float> vectors_;
  std::unordered_map<int64_t, int64_t> id_to_pos_;

  std::vector<int64_t> delta_ids_;
  std::vector<float> delta_vectors_;

  mutable std::shared_mutex rw_mutex_;

#ifdef OB_ENABLE_HIPVS
  std::unique_ptr<detail::RaftResources> res_;
  // cuVS/hipVS instantiate CAGRA with a 32-bit vertex id type. The dataset rows
  // are translated back to the caller's 64-bit vector ids on the host.
  std::unique_ptr<cuvs::neighbors::cagra::index<float, uint32_t>> index_;
#endif
};

ObHipvsIndex::ObHipvsIndex() : impl_(new Impl())
{
}

ObHipvsIndex::~ObHipvsIndex()
{
  delete impl_;
  impl_ = nullptr;
}

bool ObHipvsIndex::is_available()
{
#ifdef OB_ENABLE_HIPVS
  const int forced = g_forced_available.load();
  if (forced >= 0) {
    return forced == 1;
  }
  int cached = g_available.load();
  if (cached < 0) {
    int count = 0;
    hipError_t err = hipGetDeviceCount(&count);
    cached = (err == hipSuccess && count > 0) ? 1 : 0;
    g_available.store(cached);
    if (cached == 0) {
      set_error(err == hipSuccess ? "no HIP device is visible"
                                  : std::string("hipGetDeviceCount failed: ") + hipGetErrorString(err));
    }
  }
  return cached == 1;
#else
  return false;
#endif
}

int ObHipvsIndex::device_count()
{
#ifdef OB_ENABLE_HIPVS
  int count = 0;
  if (hipGetDeviceCount(&count) != hipSuccess) {
    count = 0;
  }
  return count;
#else
  return 0;
#endif
}

const char *ObHipvsIndex::last_error()
{
  return g_last_error.c_str();
}

#ifdef OB_ENABLE_HIPVS

bool ObHipvsIndex::Impl::build_gpu(int device_id)
{
  bool ok = true;
  try {
    if (device_id >= 0) {
      hipError_t err = hipSetDevice(device_id);
      if (err != hipSuccess) {
        set_error(std::string("hipSetDevice failed: ") + hipGetErrorString(err));
        ok = false;
      }
    }
    if (ok && (base_count_ <= 0 || dim_ <= 0)) {
      set_error("cagra build requires a positive element count and dimension");
      ok = false;
    }
    if (ok) {
      res_.reset(new detail::RaftResources());
      index_.reset();
      cuvs::neighbors::cagra::index_params params;
      params.metric = cuvs::distance::DistanceType::L2Expanded;
      params.graph_degree = static_cast<size_t>(std::max(1, graph_degree_));
      params.intermediate_graph_degree =
          static_cast<size_t>(std::max(graph_degree_, intermediate_graph_degree_));
      // Keep the dataset attached so that the graph is self contained and can
      // be searched without an external dataset handle.
      params.attach_dataset_on_build = true;
      // Hand the host buffer to build(). cuVS copies it to the device and the
      // returned index owns that copy; passing a device view instead would make
      // the index keep a non-owning reference to memory owned by this scope.
      auto built = cuvs::neighbors::cagra::build(
          *res_, params,
          raft::make_host_matrix_view<const float, int64_t, raft::row_major>(
              vectors_.data(), base_count_, dim_));
      index_.reset(new cuvs::neighbors::cagra::index<float, uint32_t>(std::move(built)));
      const hipError_t err = hipDeviceSynchronize();
      if (err != hipSuccess) {
        set_error(std::string("hipDeviceSynchronize failed: ") + hipGetErrorString(err));
        index_.reset();
        ok = false;
      }
    }
  } catch (const std::exception &e) {
    set_error(std::string("cagra build failed: ") + e.what());
    index_.reset();
    ok = false;
  } catch (...) {
    set_error("cagra build failed with an unknown exception");
    index_.reset();
    ok = false;
  }
  return ok;
}

int ObHipvsIndex::Impl::search_gpu(const float *queries,
                                   int64_t query_count,
                                   int64_t k,
                                   int64_t itopk,
                                   int64_t *out_ids,
                                   float *out_distances) const
{
  int ret = OB_HIPVS_OK;
  try {
    auto query_matrix = raft::make_device_matrix<float, int64_t>(*res_, query_count, dim_);
    hipError_t err = hipMemcpy(query_matrix.data_handle(),
                               queries,
                               sizeof(float) * static_cast<size_t>(query_count) * dim_,
                               hipMemcpyHostToDevice);
    if (err != hipSuccess) {
      set_error(std::string("hipMemcpy queries failed: ") + hipGetErrorString(err));
      ret = OB_HIPVS_RUNTIME_ERROR;
    } else {
      auto neighbors = raft::make_device_matrix<uint32_t, int64_t>(*res_, query_count, k);
      auto distances = raft::make_device_matrix<float, int64_t>(*res_, query_count, k);
      cuvs::neighbors::cagra::search_params params;
      // itopk_size is the main accuracy/latency knob and must be >= k.
      params.itopk_size = static_cast<size_t>(std::max<int64_t>(k, itopk));
      cuvs::neighbors::cagra::search(
          *res_,
          params,
          *index_,
          raft::make_device_matrix_view<const float, int64_t>(
              query_matrix.data_handle(), query_count, dim_),
          raft::make_device_matrix_view<uint32_t, int64_t>(
              neighbors.data_handle(), query_count, k),
          raft::make_device_matrix_view<float, int64_t>(
              distances.data_handle(), query_count, k));
      err = hipDeviceSynchronize();
      if (err == hipSuccess) {
        std::vector<uint32_t> host_neighbors;
        try {
          host_neighbors.resize(static_cast<size_t>(query_count) * static_cast<size_t>(k));
        } catch (const std::exception &e) {
          set_error(std::string("failed to allocate neighbor buffer: ") + e.what());
          return OB_HIPVS_OUT_OF_MEMORY;
        }
        err = hipMemcpy(host_neighbors.data(),
                        neighbors.data_handle(),
                        sizeof(uint32_t) * static_cast<size_t>(query_count) * k,
                        hipMemcpyDeviceToHost);
        if (err == hipSuccess) {
          const int64_t total = query_count * k;
          for (int64_t i = 0; i < total; ++i) {
            out_ids[i] = static_cast<int64_t>(host_neighbors[static_cast<size_t>(i)]);
          }
        }
      }
      if (err == hipSuccess) {
        err = hipMemcpy(out_distances,
                        distances.data_handle(),
                        sizeof(float) * static_cast<size_t>(query_count) * k,
                        hipMemcpyDeviceToHost);
      }
      if (err != hipSuccess) {
        set_error(std::string("cagra search failed: ") + hipGetErrorString(err));
        ret = OB_HIPVS_RUNTIME_ERROR;
      }
    }
  } catch (const std::exception &e) {
    set_error(std::string("cagra search failed: ") + e.what());
    ret = OB_HIPVS_RUNTIME_ERROR;
  } catch (...) {
    set_error("cagra search failed with an unknown exception");
    ret = OB_HIPVS_RUNTIME_ERROR;
  }
  return ret;
}

#else  // OB_ENABLE_HIPVS

bool ObHipvsIndex::Impl::build_gpu(int)
{
  return false;
}

int ObHipvsIndex::Impl::search_gpu(const float *, int64_t, int64_t, int64_t, int64_t *, float *) const
{
  return OB_HIPVS_UNAVAILABLE;
}

#endif  // OB_ENABLE_HIPVS

ObHipvsIndex *ObHipvsIndex::build(const BuildOptions &options)
{
#ifdef OB_ENABLE_HIPVS
  if (!is_available()) {
    if (g_last_error.empty()) {
      set_error("hipVS is not available on this host");
    }
    return nullptr;
  }
  if (options.vectors_ == nullptr || options.ids_ == nullptr || options.count_ <= 0
      || options.dim_ <= 0) {
    set_error("invalid vectors/ids/count/dim for hipVS build");
    return nullptr;
  }
  if (options.metric_ == nullptr || !metric_is_l2(options.metric_)) {
    set_error("hipVS backend only supports the l2 metric, falling back to vsag");
    return nullptr;
  }
  std::unique_ptr<ObHipvsIndex> index(new ObHipvsIndex());
  Impl &impl = *index->impl_;
  impl.dim_ = options.dim_;
  impl.base_count_ = options.count_;
  impl.metric_ = options.metric_;
  impl.graph_degree_ = options.graph_degree_;
  impl.intermediate_graph_degree_ = options.intermediate_graph_degree_;
  impl.ef_construction_ = options.ef_construction_;
  int64_t total = 0;
  if (bytes_mul_overflow(options.count_, options.dim_, total)) {
    set_error("vector count * dim overflows");
    return nullptr;
  }
  try {
    impl.ids_.assign(options.ids_, options.ids_ + options.count_);
    impl.vectors_.assign(options.vectors_, options.vectors_ + total);
    impl.id_to_pos_.reserve(static_cast<size_t>(options.count_));
    for (int64_t i = 0; i < options.count_; ++i) {
      impl.id_to_pos_[impl.ids_[static_cast<size_t>(i)]] = i;
    }
  } catch (const std::exception &e) {
    set_error(std::string("failed to copy vectors: ") + e.what());
    return nullptr;
  }
  if (!impl.build_gpu(options.device_id_)) {
    return nullptr;
  }
  return index.release();
#else
  (void)options;
  set_error("seekdb was built without hipVS support (OB_ENABLE_HIPVS is off)");
  return nullptr;
#endif
}

ObHipvsIndex *ObHipvsIndex::deserialize(const char *data, int64_t size)
{
  if (!is_available()) {
    if (g_last_error.empty()) {
      set_error("hipVS is not available on this host");
    }
    return nullptr;
  }
  if (data == nullptr || size < 0) {
    set_error("invalid snapshot buffer");
    return nullptr;
  }
  int64_t offset = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  int64_t base_count = 0;
  int64_t delta_count = 0;
  int64_t dim = 0;
  int32_t metric_code = 0;
  int32_t graph_degree = 64;
  int32_t intermediate_graph_degree = 128;
  int32_t ef_construction = 200;
  if (!read_pod(data, size, offset, magic) || !read_pod(data, size, offset, version)
      || !read_pod(data, size, offset, base_count) || !read_pod(data, size, offset, delta_count)
      || !read_pod(data, size, offset, dim) || !read_pod(data, size, offset, metric_code)
      || !read_pod(data, size, offset, graph_degree)
      || !read_pod(data, size, offset, intermediate_graph_degree)
      || !read_pod(data, size, offset, ef_construction)) {
    set_error("truncated hipVS snapshot header");
    return nullptr;
  }
  if (magic != SNAPSHOT_MAGIC || version != SNAPSHOT_VERSION) {
    set_error("unexpected hipVS snapshot magic or version");
    return nullptr;
  }
  if (dim <= 0 || base_count < 0 || delta_count < 0) {
    set_error("invalid hipVS snapshot header values");
    return nullptr;
  }
  if (metric_code != 0) {
    set_error("hipVS snapshot carries an unsupported metric");
    return nullptr;
  }
  int64_t base_elems = 0;   // base_count * dim
  int64_t delta_elems = 0;  // delta_count * dim
  int64_t base_vec_bytes = 0;
  int64_t delta_vec_bytes = 0;
  int64_t base_ids_bytes = 0;
  int64_t delta_ids_bytes = 0;
  int64_t payload_bytes = 0;
  int64_t expected_size = 0;
  bool size_ok = !bytes_mul_overflow(base_count, dim, base_elems)
                 && !bytes_mul_overflow(delta_count, dim, delta_elems)
                 && !bytes_mul_overflow(base_elems, static_cast<int64_t>(sizeof(float)),
                                        base_vec_bytes)
                 && !bytes_mul_overflow(delta_elems, static_cast<int64_t>(sizeof(float)),
                                        delta_vec_bytes)
                 && !bytes_mul_overflow(base_count, static_cast<int64_t>(sizeof(int64_t)),
                                        base_ids_bytes)
                 && !bytes_mul_overflow(delta_count, static_cast<int64_t>(sizeof(int64_t)),
                                        delta_ids_bytes)
                 && !add_overflow(base_ids_bytes, base_vec_bytes, payload_bytes)
                 && !add_overflow(payload_bytes, delta_ids_bytes, payload_bytes)
                 && !add_overflow(payload_bytes, delta_vec_bytes, payload_bytes)
                 && !add_overflow(offset, payload_bytes, expected_size);
  if (!size_ok || expected_size != size) {
    set_error("hipVS snapshot payload size mismatch");
    return nullptr;
  }
  std::unique_ptr<ObHipvsIndex> index(new ObHipvsIndex());
  Impl &impl = *index->impl_;
  impl.dim_ = dim;
  impl.base_count_ = base_count;
  impl.metric_ = "l2";
  impl.graph_degree_ = graph_degree;
  impl.intermediate_graph_degree_ = intermediate_graph_degree;
  impl.ef_construction_ = ef_construction;
  try {
    impl.ids_.resize(static_cast<size_t>(base_count));
    impl.vectors_.resize(static_cast<size_t>(base_elems));
    impl.delta_ids_.resize(static_cast<size_t>(delta_count));
    impl.delta_vectors_.resize(static_cast<size_t>(delta_elems));
  } catch (const std::exception &e) {
    set_error(std::string("failed to allocate snapshot buffers: ") + e.what());
    return nullptr;
  }
  auto read_ints = [&](std::vector<int64_t> &out, int64_t count) {
    bool ok = true;
    const int64_t bytes = count * static_cast<int64_t>(sizeof(int64_t));
    if (bytes > 0) {
      if (offset + bytes > size) {
        ok = false;
      } else {
        memcpy(out.data(), data + offset, static_cast<size_t>(bytes));
        offset += bytes;
      }
    }
    return ok;
  };
  auto read_floats = [&](std::vector<float> &out, int64_t count) {
    bool ok = true;
    const int64_t bytes = count * static_cast<int64_t>(sizeof(float));
    if (bytes > 0) {
      if (offset + bytes > size) {
        ok = false;
      } else {
        memcpy(out.data(), data + offset, static_cast<size_t>(bytes));
        offset += bytes;
      }
    }
    return ok;
  };
  if (!read_ints(impl.ids_, base_count) || !read_floats(impl.vectors_, base_elems)
      || !read_ints(impl.delta_ids_, delta_count)
      || !read_floats(impl.delta_vectors_, delta_elems)) {
    set_error("truncated hipVS snapshot payload");
    return nullptr;
  }
  try {
    impl.id_to_pos_.reserve(static_cast<size_t>(base_count + delta_count));
    for (int64_t i = 0; i < base_count; ++i) {
      impl.id_to_pos_[impl.ids_[static_cast<size_t>(i)]] = i;
    }
    for (int64_t i = 0; i < delta_count; ++i) {
      impl.id_to_pos_[impl.delta_ids_[static_cast<size_t>(i)]] = base_count + i;
    }
  } catch (const std::exception &e) {
    set_error(std::string("failed to rebuild id map: ") + e.what());
    return nullptr;
  }
  if (base_count > 0 && !impl.build_gpu(-1)) {
    set_error(std::string("failed to rebuild hipVS graph from snapshot: ") + g_last_error);
    return nullptr;
  }
  return index.release();
}

int ObHipvsIndex::append(const float *vectors, const int64_t *ids, int64_t count)
{
  if (is_available() == false) {
    return OB_HIPVS_UNAVAILABLE;
  }
  if ((count > 0 && (vectors == nullptr || ids == nullptr)) || count < 0) {
    set_error("invalid arguments to hipVS append");
    return OB_HIPVS_INVALID_ARG;
  }
  if (count == 0) {
    return OB_HIPVS_OK;
  }
  std::unique_lock<std::shared_mutex> lock(impl_->rw_mutex_);
  if (impl_->dim_ <= 0) {
    set_error("hipVS append before build");
    return OB_HIPVS_INVALID_ARG;
  }
  try {
    const int64_t old_delta = static_cast<int64_t>(impl_->delta_ids_.size());
    impl_->delta_ids_.reserve(static_cast<size_t>(old_delta + count));
    impl_->delta_vectors_.reserve(static_cast<size_t>((old_delta + count) * impl_->dim_));
    for (int64_t i = 0; i < count; ++i) {
      impl_->delta_ids_.push_back(ids[i]);
      impl_->delta_vectors_.insert(impl_->delta_vectors_.end(),
                                   vectors + i * impl_->dim_,
                                   vectors + (i + 1) * impl_->dim_);
      impl_->id_to_pos_[ids[i]] = impl_->base_count_ + old_delta + i;
    }
  } catch (const std::exception &e) {
    set_error(std::string("failed to append vectors: ") + e.what());
    return OB_HIPVS_OUT_OF_MEMORY;
  }
  return OB_HIPVS_OK;
}

int ObHipvsIndex::search(const float *queries,
                         int64_t query_count,
                         const SearchOptions &options,
                         int64_t *ids,
                         float *distances,
                         int64_t *result_count)
{
  if (is_available() == false) {
    return OB_HIPVS_UNAVAILABLE;
  }
  if (queries == nullptr || query_count <= 0 || options.topk_ <= 0 || ids == nullptr
      || distances == nullptr || result_count == nullptr) {
    set_error("invalid arguments to hipVS search");
    return OB_HIPVS_INVALID_ARG;
  }
  std::shared_lock<std::shared_mutex> lock(impl_->rw_mutex_);
  if (!metric_is_l2(impl_->metric_)) {
    return OB_HIPVS_UNSUPPORTED;
  }
  if (impl_->dim_ <= 0) {
    set_error("hipVS search before build");
    return OB_HIPVS_INVALID_ARG;
  }

  const int64_t topk = options.topk_;
#ifdef OB_ENABLE_HIPVS
  // The caller's ef_search is the accuracy floor for CAGRA's intermediate
  // top-k list; CAGRA requires it to be at least the requested k.
  const int64_t itopk = std::max<int64_t>(options.ef_search_ > 0 ? options.ef_search_ : 64, 1);
#endif
  const int64_t base_count = impl_->base_count_;
  const int64_t delta_count = static_cast<int64_t>(impl_->delta_ids_.size());
  const bool use_filter = options.is_id_excluded_ != nullptr;
  const int64_t overfetch = (use_filter || delta_count > 0) ? 4 : 1;
  std::vector<int64_t> gpu_ids;
  std::vector<float> gpu_distances;
  std::vector<std::pair<float, int64_t>> merged;
  try {
    merged.reserve(static_cast<size_t>(std::min<int64_t>(topk, 65536)) + 8);
  } catch (const std::exception &) {
    // Reserve is only a hint, keep going with on-demand growth.
  }

  int64_t k = 0;
  int64_t attempt = 0;
  bool enough = true;
  do {
    if (base_count <= 0) {
      k = 0;
    } else {
      // overfetch <= 4 and attempt < 5, so the factor itself cannot overflow.
      const int64_t factor = overfetch * (int64_t(1) << attempt);
      int64_t want = base_count;
      if (topk <= base_count / factor) {
        want = topk * factor;
      }
      k = std::min<int64_t>(base_count, std::max<int64_t>(topk, want));
    }
    if (k > 0) {
      int64_t total = 0;
      if (bytes_mul_overflow(query_count, k, total)) {
        set_error("search result buffer size overflows");
        return OB_HIPVS_INVALID_ARG;
      }
      try {
        gpu_ids.resize(static_cast<size_t>(total));
        gpu_distances.resize(static_cast<size_t>(total));
      } catch (const std::exception &e) {
        set_error(std::string("failed to allocate search buffers: ") + e.what());
        return OB_HIPVS_OUT_OF_MEMORY;
      }
#ifdef OB_ENABLE_HIPVS
      const int ret = impl_->search_gpu(queries, query_count, k, itopk, gpu_ids.data(),
                                        gpu_distances.data());
      if (ret != OB_HIPVS_OK) {
        return ret;
      }
#else
      return OB_HIPVS_UNAVAILABLE;
#endif
    }
    enough = true;
    for (int64_t q = 0; q < query_count; ++q) {
      try {
        merged.clear();
        const float *query = queries + q * impl_->dim_;
        if (k > 0) {
          const int64_t *row_ids = gpu_ids.data() + q * k;
          const float *row_dists = gpu_distances.data() + q * k;
          for (int64_t j = 0; j < k; ++j) {
            const int64_t internal = row_ids[j];
            if (internal < 0 || internal >= base_count) {
              continue;
            }
            const int64_t external = impl_->ids_[static_cast<size_t>(internal)];
            if (use_filter && options.is_id_excluded_(external, options.filter_ctx_)) {
              continue;
            }
            merged.emplace_back(row_dists[j], external);
          }
        }
        for (int64_t j = 0; j < delta_count; ++j) {
          const int64_t external = impl_->delta_ids_[static_cast<size_t>(j)];
          if (use_filter && options.is_id_excluded_(external, options.filter_ctx_)) {
            continue;
          }
          const float dist = l2_square(query,
                                       impl_->delta_vectors_.data() + j * impl_->dim_,
                                       impl_->dim_);
          merged.emplace_back(dist, external);
        }
        std::sort(merged.begin(), merged.end(), [](const std::pair<float, int64_t> &lhs,
                                                   const std::pair<float, int64_t> &rhs) {
          if (lhs.first != rhs.first) {
            return lhs.first < rhs.first;
          }
          return lhs.second < rhs.second;
        });
        const int64_t result_size = static_cast<int64_t>(merged.size()) < topk
                                        ? static_cast<int64_t>(merged.size())
                                        : topk;
        if (result_size < topk) {
          enough = false;
        }
        for (int64_t j = 0; j < result_size; ++j) {
          ids[q * topk + j] = merged[static_cast<size_t>(j)].second;
          distances[q * topk + j] = merged[static_cast<size_t>(j)].first;
        }
        result_count[q] = result_size;
      } catch (const std::exception &e) {
        set_error(std::string("failed to merge search results: ") + e.what());
        return OB_HIPVS_OUT_OF_MEMORY;
      }
    }
    ++attempt;
  } while (!enough && k < base_count && attempt < 5);
  return OB_HIPVS_OK;
}

int ObHipvsIndex::calc_distance_by_id(const float *query,
                                      const int64_t *ids,
                                      int64_t count,
                                      float *distances) const
{
  if (is_available() == false) {
    return OB_HIPVS_UNAVAILABLE;
  }
  if (query == nullptr || (count > 0 && (ids == nullptr || distances == nullptr)) || count < 0) {
    set_error("invalid arguments to hipVS calc_distance_by_id");
    return OB_HIPVS_INVALID_ARG;
  }
  std::shared_lock<std::shared_mutex> lock(impl_->rw_mutex_);
  for (int64_t i = 0; i < count; ++i) {
    auto it = impl_->id_to_pos_.find(ids[i]);
    if (it == impl_->id_to_pos_.end()) {
      set_error("vector id not found in the hipVS index");
      return OB_HIPVS_INVALID_ARG;
    }
    const int64_t pos = it->second;
    const float *rhs = pos < impl_->base_count_
                           ? impl_->vectors_.data() + pos * impl_->dim_
                           : impl_->delta_vectors_.data() + (pos - impl_->base_count_) * impl_->dim_;
    distances[i] = l2_square(query, rhs, impl_->dim_);
  }
  return OB_HIPVS_OK;
}

int ObHipvsIndex::get_vid_bound(int64_t &min_vid, int64_t &max_vid) const
{
  if (is_available() == false) {
    return OB_HIPVS_UNAVAILABLE;
  }
  std::shared_lock<std::shared_mutex> lock(impl_->rw_mutex_);
  const int64_t total = impl_->base_count_ + static_cast<int64_t>(impl_->delta_ids_.size());
  for (int64_t i = 0; i < total; ++i) {
    const int64_t vid = i < impl_->base_count_
                            ? impl_->ids_[static_cast<size_t>(i)]
                            : impl_->delta_ids_[static_cast<size_t>(i - impl_->base_count_)];
    if (i == 0 || vid < min_vid) {
      min_vid = vid;
    }
    if (i == 0 || vid > max_vid) {
      max_vid = vid;
    }
  }
  return OB_HIPVS_OK;
}

int64_t ObHipvsIndex::size() const
{
  std::shared_lock<std::shared_mutex> lock(impl_->rw_mutex_);
  return impl_->base_count_ + static_cast<int64_t>(impl_->delta_ids_.size());
}

int ObHipvsIndex::serialize(std::vector<char> &out) const
{
  if (is_available() == false) {
    return OB_HIPVS_UNAVAILABLE;
  }
  out.clear();
  std::shared_lock<std::shared_mutex> lock(impl_->rw_mutex_);
  try {
    const int64_t delta_count = static_cast<int64_t>(impl_->delta_ids_.size());
    int64_t total_count = 0;
    int64_t vec_bytes = 0;
    int64_t id_bytes = 0;
    int64_t payload_bytes = 0;
    int64_t total_bytes = 0;
    const int64_t header_bytes = static_cast<int64_t>(sizeof(uint32_t) * 2)
                                 + static_cast<int64_t>(sizeof(int64_t) * 3)
                                 + static_cast<int64_t>(sizeof(int32_t) * 4);
    if (add_overflow(impl_->base_count_, delta_count, total_count)
        || bytes_mul_overflow(total_count, impl_->dim_, vec_bytes)
        || bytes_mul_overflow(vec_bytes, static_cast<int64_t>(sizeof(float)), vec_bytes)
        || bytes_mul_overflow(total_count, static_cast<int64_t>(sizeof(int64_t)), id_bytes)
        || add_overflow(vec_bytes, id_bytes, payload_bytes)
        || add_overflow(payload_bytes, header_bytes, total_bytes)) {
      set_error("hipVS snapshot size overflows");
      return OB_HIPVS_OUT_OF_MEMORY;
    }
    out.reserve(static_cast<size_t>(total_bytes));
    append_pod(out, SNAPSHOT_MAGIC);
    append_pod(out, SNAPSHOT_VERSION);
    append_pod(out, impl_->base_count_);
    append_pod(out, delta_count);
    append_pod(out, impl_->dim_);
    const int32_t metric_code = metric_is_l2(impl_->metric_) ? 0 : 1;
    append_pod(out, metric_code);
    append_pod(out, static_cast<int32_t>(impl_->graph_degree_));
    append_pod(out, static_cast<int32_t>(impl_->intermediate_graph_degree_));
    append_pod(out, static_cast<int32_t>(impl_->ef_construction_));
    if (impl_->base_count_ > 0) {
      const char *ids_ptr = reinterpret_cast<const char *>(impl_->ids_.data());
      out.insert(out.end(), ids_ptr, ids_ptr + impl_->base_count_ * sizeof(int64_t));
      const char *vec_ptr = reinterpret_cast<const char *>(impl_->vectors_.data());
      out.insert(out.end(), vec_ptr,
                 vec_ptr + impl_->base_count_ * impl_->dim_ * sizeof(float));
    }
    if (delta_count > 0) {
      const char *ids_ptr = reinterpret_cast<const char *>(impl_->delta_ids_.data());
      out.insert(out.end(), ids_ptr, ids_ptr + delta_count * sizeof(int64_t));
      const char *vec_ptr = reinterpret_cast<const char *>(impl_->delta_vectors_.data());
      out.insert(out.end(), vec_ptr, vec_ptr + delta_count * impl_->dim_ * sizeof(float));
    }
  } catch (const std::exception &e) {
    set_error(std::string("failed to serialize hipVS index: ") + e.what());
    return OB_HIPVS_OUT_OF_MEMORY;
  }
  return OB_HIPVS_OK;
}

}  // namespace hipvs
}  // namespace common
}  // namespace oceanbase
