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

#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace oceanbase
{
namespace common
{
namespace hipvs
{

/*
 * Thin, dependency free facade over AMD hipVS/cuVS.
 *
 * The rest of seekdb only talks to this interface, so it builds and runs
 * unchanged when hipVS is not available: every entry point then reports
 * "unavailable" and the caller falls back to the vsag CPU backend.
 *
 * Build with -DOB_ENABLE_HIPVS=ON and point HIPVS_ROOT at the hipVS
 * installation (default /opt/hipvs) to enable the GPU implementation.
 */

enum ObHipvsStatus
{
  OB_HIPVS_OK = 0,
  OB_HIPVS_UNAVAILABLE = 1,
  OB_HIPVS_INVALID_ARG = 2,
  OB_HIPVS_OUT_OF_MEMORY = 3,
  OB_HIPVS_UNSUPPORTED = 4,
  OB_HIPVS_RUNTIME_ERROR = 5,
  OB_HIPVS_MAX
};

struct BuildOptions
{
  const float *vectors_ = nullptr;  // row major, count * dim, host memory
  const int64_t *ids_ = nullptr;    // external vector ids, count entries
  int64_t count_ = 0;
  int64_t dim_ = 0;
  const char *metric_ = nullptr;  // "l2", "ip" or "cosine"
  int graph_degree_ = 64;
  int intermediate_graph_degree_ = 128;
  int ef_construction_ = 200;
  int device_id_ = -1;
};

struct SearchOptions
{
  int64_t topk_ = 0;
  int64_t ef_search_ = 64;
  // Called for every candidate id. Returning true removes the id from the
  // result. May be null when no filtering is required.
  bool (*is_id_excluded_)(int64_t id, void *ctx) = nullptr;
  void *filter_ctx_ = nullptr;
};

class ObHipvsIndex
{
public:
  // Whether the binary was built with hipVS and at least one usable GPU is
  // present. The result is cached after the first call.
  static bool is_available();
  static int device_count();
  // Human readable reason of the last failure on the calling thread.
  static const char *last_error();

  // Builds a GPU graph index (CAGRA) from host vectors and ids.
  // Returns nullptr on failure; use last_error() for details.
  static ObHipvsIndex *build(const BuildOptions &options);

  // Restores an index produced by serialize().
  static ObHipvsIndex *deserialize(const char *data, int64_t size);

  ~ObHipvsIndex();
  ObHipvsIndex(const ObHipvsIndex &) = delete;
  ObHipvsIndex &operator=(const ObHipvsIndex &) = delete;

  // Appends vectors to the host side delta buffer. GPU graph construction is
  // not touched, which keeps the fast path immutable for concurrent readers.
  int append(const float *vectors, const int64_t *ids, int64_t count);

  // Searches the GPU graph and the host delta buffer, merging both result
  // sets. ids/distances are caller owned arrays of capacity topk * query_count.
  // result_count receives the number of results per query (topk unless the
  // index/filter holds fewer elements).
  int search(const float *queries,
             int64_t query_count,
             const SearchOptions &options,
             int64_t *ids,
             float *distances,
             int64_t *result_count);

  // Exact distance between a query and the indexed vectors referenced by ids.
  int calc_distance_by_id(const float *query,
                          const int64_t *ids,
                          int64_t count,
                          float *distances) const;

  int get_vid_bound(int64_t &min_vid, int64_t &max_vid) const;
  int64_t size() const;

  // Serializes the external id array, the vectors and the host delta buffer
  // into a self contained payload. deserialize() rebuilds the GPU graph from
  // it, so the snapshot does not depend on GPU-specific graph state.
  int serialize(std::vector<char> &out) const;

  // Serialization magic/version. Written by seekdb in front of the payload so
  // that a CPU-only restart can detect a GPU snapshot and report a clear error
  // instead of trying to feed it into vsag.
  static constexpr uint32_t SNAPSHOT_MAGIC = 0x4F424856;  // "OBHV"
  static constexpr uint32_t SNAPSHOT_VERSION = 1;

private:
  ObHipvsIndex();
  struct Impl;
  Impl *impl_ = nullptr;
};

// Overwrite the cached availability probe. Only used by tests.
void set_availability_for_test(bool available);

}  // namespace hipvs
}  // namespace common
}  // namespace oceanbase
