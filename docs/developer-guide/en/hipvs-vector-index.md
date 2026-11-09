# hipVS (GPU) vector index backend

seekdb can build vector indexes on an AMD Radeon GPU through
[AMD hipVS](https://rocm.docs.amd.com/projects/hipVS/), the HIP port of NVIDIA
cuVS. The backend uses the CAGRA graph construction algorithm from cuVS/hipVS
and is selected per index with `LIB=CUVS`:

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, embedding VECTOR(128));

CREATE VECTOR INDEX idx_embedding ON docs(embedding)
  WITH (DISTANCE=L2, TYPE=HNSW, LIB=CUVS, M=32, EF_CONSTRUCTION=200);
```

Queries use the usual approximate syntax and are planned exactly like an HNSW
index:

```sql
SELECT id FROM docs
ORDER BY l2_distance(embedding, '[0.1,0.2,...]') APPROXIMATE LIMIT 10;
```

## Building

The backend is compiled whenever the hipVS CMake package (`cuvs::cuvs`) is
discoverable, so a machine with hipVS and ROCm installed needs no extra flag:

```bash
cmake ..                                    # auto-detects hipVS
cmake -DOB_ENABLE_HIPVS=ON  -DHIPVS_ROOT=/opt/hipvs ..   # force it on
cmake -DOB_ENABLE_HIPVS=OFF ..                           # force it off
```

`OB_ENABLE_HIPVS` takes `AUTO` (default), `ON`, or `OFF`. `HIPVS_ROOT` defaults
to `/opt/hipvs` and is added to the package search path. When hipVS is not
found, the bridge compiles as a stub, so builds on machines without a GPU are
unaffected; `LIB=CUVS` still creates a usable index, but it is built by vsag and
the server logs the fallback.

The bridge instantiates device code from the hipVS headers, so it must be
compiled by the HIP toolchain (`amdclang++`/`hipcc`). HIP discovery is pointed
at `$ROCM_PATH` and `/opt/rocm`, and the target GPUs are taken from
`rocm_agent_enumerator`; if no HIP compiler is found the backend is disabled
with a warning instead of breaking the build.

HNSW indexes use the GPU automatically. When the binary was built with hipVS
and a GPU is visible, every HNSW index with `DISTANCE=L2` and FP32 vectors is
built with CAGRA, whether or not `LIB` is specified, so the existing HNSW build
path is accelerated without changing any DDL. `LIB=CUVS` remains an explicit
alias for the same backend, and `LIB=OB` (IVF, sparse) is untouched. Hosts
without a usable GPU keep the plain vsag snapshot format, so data files stay
compatible with servers that have no hipVS build. Set `OB_HIPVS_DISABLE=1` in
the server environment to force the CPU path on a GPU host, for example to
collect a CPU baseline.

## Architecture

`src/oblib/lib/vector/ob_hipvs_bridge.{h,cpp}` is the only place that includes
HIP/cuVS headers. It exposes a small, dependency-free facade that:

- copies the vectors and vector ids to the GPU and builds a CAGRA graph;
- keeps a host copy of the dataset, so `calc_distance_by_id` and serialization
  do not need to read data back from the GPU;
- accepts incremental inserts. The DDL build path feeds rows one at a time, so
  new rows are buffered in a host delta and folded into the graph once the delta
  reaches `max(4096, current row count)`. The threshold doubles after every
  fold, which keeps total GPU build work linear while the graph is really built
  during `CREATE VECTOR INDEX` instead of after it. Rows that are still in the
  delta are merged into GPU results at query time;
- applies row filters by over-fetching from the graph and post-filtering on the
  host, retrying with a larger `k` when too few rows survive.

`ObVsagAdaptor` dispatches to the bridge for every eligible HNSW index (L2,
FP32, no extra info) on a host with a usable GPU, whether it was created with
`LIB=CUVS` or the default `LIB=VSAG`; the other paths (`LIB=OB`, IVF, sparse)
are unchanged.

## Semantics and limitations

- **Metric**: only `DISTANCE=L2` is supported. Distances are squared L2, the
  same convention vsag uses, so thresholds and ordering behave identically.
- **Fallback**: when the binary has no hipVS support, no GPU is visible, or the
  GPU build fails, the index is built with vsag instead and a
  `[HIPVS] ... fallback to vsag` warning is logged. `LIB=CUVS` therefore always yields a working HNSW
  index; check the server log (or the benchmark output) to confirm that the GPU
  path actually ran. A snapshot written by a GPU build is tagged and requires a
  working hipVS backend to be loaded.
- **No sparse/IVF support**: `LIB=CUVS` is valid only for HNSW-shaped vector
  indexes. Sparse vector indexes (SINDI) and IVF types remain vsag-only.
- **Persistence**: the on-disk snapshot stores ids, vectors and the delta
  buffer, and rebuilds the CAGRA graph when it is loaded. GPU graph state is
  not persisted, so a snapshot created on one machine can be opened on another
  as long as hipVS is available there.
- **Parameter mapping**: `M` maps to CAGRA's `graph_degree` as
  `min(128, max(32, 2*M))` and `intermediate_graph_degree` as
  `min(256, max(64, 2*graph_degree))`; CAGRA is a denser graph than HNSW at the
  same `M`. `EF_CONSTRUCTION` is accepted for DDL compatibility with the vsag
  backend.

## Benchmarking

`tools/benchmark/hnsw_build_bench.sh` loads a synthetic dataset once and builds
the same index twice, first with `LIB=vsag` and then with `LIB=cuvs`, reporting
build wall time, query latency and recall@K:

```bash
ROWS=200000 DIM=256 ./tools/benchmark/hnsw_build_bench.sh
```

See the comment block at the top of the script for the full list of knobs
(`SAMPLES`, `K`, `M_PARAM`, `EF_CONSTRUCTION`, `OUTPUT`, ...). A build speedup
is printed as `build_speedup = vsag_build_sec / cuvs_build_sec`; values above
1.0 mean the GPU path finished sooner. Query latency and recall must stay
comparable for the speedup to be meaningful. If the server fell back to vsag,
the two build times will be nearly identical — confirm the GPU path from the
`[HIPVS] build index on gpu success` log line before trusting the number.
