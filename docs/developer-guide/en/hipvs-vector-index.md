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

The backend is compiled only when `OB_ENABLE_HIPVS` is on. hipVS exposes the
cuVS CMake package (`cuvs::cuvs`), so both hipVS and ROCm must be installed:

```bash
cmake -DOB_ENABLE_HIPVS=ON -DHIPVS_ROOT=/opt/hipvs ..
```

`HIPVS_ROOT` defaults to `/opt/hipvs` and is added to the include path when it
exists. With the option off, `LIB=CUVS` is rejected at DDL time and the bridge
compiles as a stub, so builds on machines without a GPU are unaffected.

## Architecture

`src/oblib/lib/vector/ob_hipvs_bridge.{h,cpp}` is the only place that includes
HIP/cuVS headers. It exposes a small, dependency-free facade that:

- copies the vectors and vector ids to the GPU and builds a CAGRA graph;
- keeps a host copy of the dataset, so `calc_distance_by_id` and serialization
  do not need to read data back from the GPU;
- accepts incremental inserts in an in-memory delta buffer that is merged with
  GPU results at query time;
- applies row filters by over-fetching from the graph and post-filtering on the
  host, retrying with a larger `k` when too few rows survive.

`ObVsagAdaptor` dispatches to the bridge when the index was created with
`LIB=CUVS`; every other path (`LIB=VSAG`, `LIB=OB`, IVF, sparse) is unchanged.

## Semantics and limitations

- **Metric**: only `DISTANCE=L2` is supported. Distances are squared L2, the
  same convention vsag uses, so thresholds and ordering behave identically.
- **Fallback**: when hipVS is compiled in but no GPU is visible, or the GPU
  build fails, the index is built with vsag instead and a `[HIPVS] ... fallback
  to vsag` warning is logged. `LIB=CUVS` therefore always yields a working HNSW
  index; check the server log (or the benchmark output) to confirm that the GPU
  path actually ran. A snapshot written by a GPU build is tagged and requires a
  working hipVS backend to be loaded.
- **No sparse/IVF support**: `LIB=CUVS` is valid only for HNSW-shaped vector
  indexes. Sparse vector indexes (SINDI) and IVF types remain vsag-only.
- **Persistence**: the on-disk snapshot stores ids, vectors and the delta
  buffer, and rebuilds the CAGRA graph when it is loaded. GPU graph state is
  not persisted, so a snapshot created on one machine can be opened on another
  as long as hipVS is available there.
- **Parameter mapping**: `M` maps to CAGRA's `graph_degree` as `max(32, 2*M)`
  and `intermediate_graph_degree` as `max(64, 2*graph_degree)`; CAGRA is a
  denser graph than HNSW at the same `M`. `EF_CONSTRUCTION` is accepted for DDL
  compatibility with the vsag backend.

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
