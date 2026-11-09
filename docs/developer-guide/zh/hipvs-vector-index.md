# hipVS（GPU）向量索引后端

seekdb 可以通过 [AMD hipVS](https://rocm.docs.amd.com/projects/hipVS/)（NVIDIA
cuVS 的 HIP 移植）在 AMD Radeon GPU 上构建向量索引。该后端使用 cuVS/hipVS 的
CAGRA 图构建算法，按索引通过 `LIB=CUVS` 选择：

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, embedding VECTOR(128));

CREATE VECTOR INDEX idx_embedding ON docs(embedding)
  WITH (DISTANCE=L2, TYPE=HNSW, LIB=CUVS, M=32, EF_CONSTRUCTION=200);
```

查询使用常规的近似语法，查询计划与 HNSW 索引完全一致：

```sql
SELECT id FROM docs
ORDER BY l2_distance(embedding, '[0.1,0.2,...]') APPROXIMATE LIMIT 10;
```

## 编译

只要能够找到 hipVS 的 CMake 包（`cuvs::cuvs`）就会编译该后端，因此在已安装
hipVS 与 ROCm 的机器上无需额外参数：

```bash
cmake ..                                                 # 自动探测 hipVS
cmake -DOB_ENABLE_HIPVS=ON  -DHIPVS_ROOT=/opt/hipvs ..    # 强制开启
cmake -DOB_ENABLE_HIPVS=OFF ..                            # 强制关闭
```

`OB_ENABLE_HIPVS` 取值 `AUTO`（默认）、`ON` 或 `OFF`；`HIPVS_ROOT` 默认是
`/opt/hipvs`，会加入 CMake 包搜索路径。找不到 hipVS 时，桥接层以桩实现编译，
因此无 GPU 的机器上的构建完全不受影响；`LIB=CUVS` 仍然能创建出可用的索引，
但实际由 vsag 构建，服务端会打印回退日志。

桥接层会实例化 hipVS 头文件中的设备代码，因此必须由 HIP 工具链
（`amdclang++`/`hipcc`）编译。CMake 会在 `$ROCM_PATH` 与 `/opt/rocm` 下查找
工具链，并通过 `rocm_agent_enumerator` 确定目标 GPU；找不到 HIP 编译器时后端
会被关闭并给出告警，而不会让构建失败。

HNSW 索引会自动使用 GPU：只要二进制编译了 hipVS 且运行机器可见 GPU，所有
`DISTANCE=L2`、FP32 向量的 HNSW 索引都会用 CAGRA 构建，无论是否显式指定
`LIB`，因此无需修改 DDL 即可加速既有 HNSW 构建链路。`LIB=CUVS` 仍然是该后端的
显式写法，`LIB=OB`（IVF、稀疏）保持不变。看不到可用 GPU 的机器继续使用 vsag
的原始快照格式，数据文件与未编译 hipVS 的服务端保持兼容。在 GPU 机器上如需
强制走 CPU（例如采集基线），可在服务端环境变量中设置 `OB_HIPVS_DISABLE=1`。

## 架构

`src/oblib/lib/vector/ob_hipvs_bridge.{h,cpp}` 是唯一包含 HIP/cuVS 头文件的
位置。它暴露一个无外部依赖的门面，负责：

- 把向量和向量 id 拷贝到 GPU 并构建 CAGRA 图；
- 在主机侧保留一份数据副本，使 `calc_distance_by_id` 和序列化无需从 GPU 回读；
- 支持增量插入。DDL 建索引链路是逐行调用追加接口的，因此新行先写入主机侧的
  delta 缓冲区，当 delta 达到 `max(4096, 当前行数)` 时并入图并重建 CAGRA 图；
  阈值每次翻倍，因此总的 GPU 构建量仍是线性的，同时图确实在
  `CREATE VECTOR INDEX` 期间构建，而不是在其之后。仍留在 delta 中的行在查询
  时与 GPU 结果合并；
- 行过滤通过从图中超取（over-fetch）后在主机侧过滤实现，命中数不足时自动
  加大 `k` 重试。

`ObVsagAdaptor` 对运行机器可见 GPU、满足条件（L2、FP32、无 extra info）的
HNSW 索引分派到桥接层，无论建索引时写的是 `LIB=CUVS` 还是默认的
`LIB=VSAG`；其余路径（`LIB=OB`、IVF、稀疏）保持不变。

## 语义与限制

- **距离**：仅支持 `DISTANCE=L2`，距离为平方 L2，与 vsag 的约定一致，因此
  阈值比较和排序行为完全相同。
- **回退**：二进制未编译 hipVS、运行时看不到 GPU，或 GPU 构建失败时，会改用
  vsag 建索引并打印 `[HIPVS] ... fallback to vsag` 告警。因此 `LIB=CUVS` 总能得到
  一个可用的 HNSW 索引；要确认确实走了 GPU，需要查看服务端日志（或压测输出）。
  GPU 构建写出的快照带专用标记，加载时要求 hipVS 后端可用。
- **不支持稀疏/IVF**：`LIB=CUVS` 仅对 HNSW 形态的向量索引有效；稀疏向量索引
  （SINDI）和 IVF 类型仍只由 vsag 支持。
- **持久化**：落盘快照保存 id、向量和 delta 缓冲区，加载时重建 CAGRA 图。
  GPU 图状态不落盘，因此只要目标机器同样可用 hipVS，快照可以在不同机器上打开。
- **参数映射**：`M` 按 `min(128, max(32, 2*M))` 映射到 CAGRA 的
  `graph_degree`，`intermediate_graph_degree` 取
  `min(256, max(64, 2*graph_degree))`；相同 `M` 下 CAGRA 的图比 HNSW 更稠密。
  `EF_CONSTRUCTION` 为兼容 vsag 的 DDL 写法而接受。

## 压测

`tools/benchmark/hnsw_build_bench.sh` 只加载一次合成数据集，然后分别用
`LIB=vsag` 与 `LIB=cuvs` 构建同一个索引，输出构建耗时、查询延迟和 recall@K：

```bash
ROWS=200000 DIM=256 ./tools/benchmark/hnsw_build_bench.sh
```

完整参数（`SAMPLES`、`K`、`M_PARAM`、`EF_CONSTRUCTION`、`OUTPUT` 等）见脚本
顶部的注释。报告中的 `build_speedup = vsag_build_sec / cuvs_build_sec`，大于
1.0 表示 GPU 路径更快；只有查询延迟与 recall 可比时，加速比才有意义。若服务端
发生了 vsag 回退，两次构建耗时几乎相同，因此采信数字前请先在服务端日志中确认
出现 `[HIPVS] build index on gpu success`。
