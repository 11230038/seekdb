#!/usr/bin/env bash
# HNSW 向量索引构建压测：CPU（vsag）对比 GPU（AMD hipVS / CAGRA）
#
# 在同一张表、同一份数据上先后用 LIB=vsag 与 LIB=cuvs 构建 HNSW 索引，
# 记录构建耗时、近似查询延迟与 recall@K，用于验证 hipVS 对索引构建链路的
# 加速效果。
#
# 注意：服务端编译了 hipVS 且能看到 GPU 时，HNSW 索引默认就会走 GPU，
# LIB=vsag 与 LIB=cuvs 都会加速。要得到 CPU 基线，请用
# OB_HIPVS_DISABLE=1 启动 seekdb 后跑一遍本脚本，再正常启动服务端跑一遍，
# 对比两份报告中的 build_sec（cuvs/vsag 两行应分别一致）。
#
# 前提：
#   1. seekdb 编译时探测到 hipVS（OB_ENABLE_HIPVS 默认 AUTO；也可显式
#      指定 ON），且运行机器可见 AMD GPU；
#   2. 服务端已启动并可连接。
#
# 用法:
#   ./tools/benchmark/hnsw_build_bench.sh
#   ROWS=200000 DIM=256 ./tools/benchmark/hnsw_build_bench.sh
#   SKIP_LOAD=1 ./tools/benchmark/hnsw_build_bench.sh          # 复用已有数据
#   OUTPUT=./hnsw_bench.txt ./tools/benchmark/hnsw_build_bench.sh
#
# 环境变量:
#   MYSQL          mysql 客户端命令（默认 -h127.0.0.1 -P2881 -uroot -N -s）
#   DIM            向量维度（默认 128，最大 4096）
#   ROWS           数据行数（默认 50000）
#   BATCH          每批 INSERT 行数（默认 500）
#   SAMPLES        构建采样次数（默认 3，每次都会重建索引）
#   QUERIES        计算 recall 的查询条数（默认 20，不得超过 ROWS）
#   QUERY_ROUNDS   每条近似查询的重复次数（默认 100）
#   K              近邻条数（默认 10）
#   M_PARAM        HNSW M（默认 16）
#   EF_CONSTRUCTION HNSW ef_construction（默认 200）
#   LIB_CPU        CPU 基线库（默认 vsag）
#   LIB_GPU        GPU 库（默认 cuvs）
#   DB             测试库名（默认 hnsw_bench）
#   TABLE          测试表名（默认 docs）
#   IDX            索引名（默认 idx_v）
#   SKIP_LOAD      设为 1 跳过建表与灌数
#   KEEP_DATA      设为 1 保留测试库（默认 0，结束时 DROP DATABASE）
#   OUTPUT         结果追加写入的文件（可选）
#
# 服务端环境变量:
#   OB_HIPVS_DISABLE=1  在 GPU 机器上强制走 vsag，用于采集 CPU 基线

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MYSQL="${MYSQL:-mysql -h127.0.0.1 -P2881 -uroot -N -s}"
DIM="${DIM:-128}"
ROWS="${ROWS:-50000}"
BATCH="${BATCH:-500}"
SAMPLES="${SAMPLES:-3}"
QUERIES="${QUERIES:-20}"
QUERY_ROUNDS="${QUERY_ROUNDS:-100}"
K="${K:-10}"
M_PARAM="${M_PARAM:-16}"
EF_CONSTRUCTION="${EF_CONSTRUCTION:-200}"
LIB_CPU="${LIB_CPU:-vsag}"
LIB_GPU="${LIB_GPU:-cuvs}"
DB="${DB:-hnsw_bench}"
TABLE="${TABLE:-docs}"
IDX="${IDX:-idx_v}"
SKIP_LOAD="${SKIP_LOAD:-0}"
KEEP_DATA="${KEEP_DATA:-0}"
OUTPUT="${OUTPUT:-}"

declare -A METRICS
declare -a QIDS=()
declare -a QVECS=()
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/hnsw_build_bench.XXXXXX")"

cleanup() { rm -rf "${TMP_DIR}"; }
trap cleanup EXIT

log() { echo "$@" >&2; }

now_ns() { date +%s%N; }

elapsed_sec() {
  local start_ns="$1" end_ns="$2"
  awk "BEGIN {printf \"%.3f\", (${end_ns} - ${start_ns}) / 1000000000}"
}

elapsed_ms() {
  local start_ns="$1" end_ns="$2"
  awk "BEGIN {printf \"%.3f\", (${end_ns} - ${start_ns}) / 1000000}"
}

mysql_exec() {
  $MYSQL -e "SET ob_query_timeout = 100000000000; SET ob_trx_timeout = 100000000000; $1"
}

validate_positive_int() {
  local name="$1" value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    log "ERROR: ${name} must be a positive integer, got '${value}'"
    exit 1
  fi
}

validate_config() {
  local name
  for name in DIM ROWS BATCH SAMPLES QUERIES QUERY_ROUNDS K M_PARAM EF_CONSTRUCTION; do
    validate_positive_int "${name}" "${!name}"
  done
  if (( DIM > 4096 )); then
    log "ERROR: DIM must be <= 4096 (seekdb vector limit), got ${DIM}"
    exit 1
  fi
  if (( QUERIES > ROWS )); then
    log "ERROR: QUERIES (${QUERIES}) must be <= ROWS (${ROWS})"
    exit 1
  fi
  if (( BATCH > ROWS )); then
    BATCH="${ROWS}"
  fi
}

# 生成随机向量数据，每批一个 SQL 文件，供 MySQL 客户端批量导入。
gen_batches() {
  local out_dir="$1"
  python3 - "${out_dir}" "${ROWS}" "${DIM}" "${BATCH}" <<'PY'
import random
import sys

out_dir, rows, dim, batch = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
random.seed(20260101)

values = []
file_no = 0
for row_id in range(1, rows + 1):
    vec = ",".join("%.6f" % random.random() for _ in range(dim))
    values.append("(%d,'[%s]')" % (row_id, vec))
    if len(values) == batch or row_id == rows:
        file_no += 1
        path = "%s/load.%05d.sql" % (out_dir, file_no)
        with open(path, "w") as f:
            f.write("SET ob_query_timeout = 100000000000;\n")
            f.write("INSERT INTO docs (id, v) VALUES " + ",".join(values) + ";\n")
        values = []
print(file_no)
PY
}

load_data() {
  local total start_ns end_ns load_sec rows_per_sec loaded file
  log "==> creating database and table (dim=${DIM}, rows=${ROWS})"
  mysql_exec "DROP DATABASE IF EXISTS ${DB}; CREATE DATABASE ${DB};"
  mysql_exec "CREATE TABLE ${DB}.${TABLE} (id BIGINT PRIMARY KEY, v VECTOR(${DIM}));"

  log "==> generating vectors"
  total="$(gen_batches "${TMP_DIR}")"
  log "    generated ${total} batch files"

  log "==> loading ${ROWS} rows"
  start_ns=$(now_ns)
  for file in "${TMP_DIR}"/load.*.sql; do
    $MYSQL "${DB}" < "${file}"
  done
  end_ns=$(now_ns)
  load_sec=$(elapsed_sec "${start_ns}" "${end_ns}")
  rows_per_sec=$(awk "BEGIN {printf \"%.0f\", ${ROWS} / ${load_sec}}")
  METRICS[load_sec]="${load_sec}"
  METRICS[load_rows_per_sec]="${rows_per_sec}"
  log "  load: ${load_sec}s (${rows_per_sec} rows/s)"

  loaded=$(mysql_exec "SELECT COUNT(*) FROM ${DB}.${TABLE};" | tail -1)
  if [[ "${loaded}" != "${ROWS}" ]]; then
    log "ERROR: expected ${ROWS} rows after load, found '${loaded}'"
    exit 1
  fi
}

drop_index_if_exists() {
  mysql_exec "DROP INDEX IF EXISTS ${IDX} ON ${DB}.${TABLE};" >/dev/null 2>&1 || \
    mysql_exec "DROP INDEX ${IDX} ON ${DB}.${TABLE};" >/dev/null 2>&1 || true
}

# 构建一次索引并输出耗时；失败时返回非 0，错误信息写入 $2。
build_index_once() {
  local lib="$1" out_file="$2"
  local start_ns end_ns
  drop_index_if_exists
  start_ns=$(now_ns)
  if ! mysql_exec "CREATE VECTOR INDEX ${IDX} ON ${DB}.${TABLE}(v) WITH (DISTANCE=L2, TYPE=HNSW, LIB=${lib}, M=${M_PARAM}, EF_CONSTRUCTION=${EF_CONSTRUCTION});" \
      > "${out_file}" 2>&1; then
    return 1
  fi
  end_ns=$(now_ns)
  elapsed_sec "${start_ns}" "${end_ns}"
}

build_index_samples() {
  local lib="$1" label="$2" sec_file="${TMP_DIR}/${label}.builds"
  local sample sec_out out_file
  : > "${sec_file}"
  for ((sample = 1; sample <= SAMPLES; sample++)); do
    out_file="${TMP_DIR}/${label}.build.${sample}.log"
    if ! sec_out="$(build_index_once "${lib}" "${out_file}")"; then
      METRICS[${label}_error]="$(tr '\n' ' ' < "${out_file}" | head -c 500)"
      log "  ${label}: build failed: ${METRICS[${label}_error]}"
      return 1
    fi
    echo "${sec_out}" >> "${sec_file}"
    log "  ${label}: sample=${sample}/${SAMPLES} build_sec=${sec_out}"
  done
  read -r mean stdev min < <(awk '
    { a[NR] = $1; sum += $1; if (NR == 1 || $1 < min) min = $1 }
    END {
      if (NR == 0) { print "N/A N/A N/A"; exit }
      mean = sum / NR
      var = 0
      for (i = 1; i <= NR; i++) { d = a[i] - mean; var += d * d }
      var = (NR > 1) ? var / (NR - 1) : 0
      printf "%.3f %.3f %.3f\n", mean, sqrt(var), min
    }
  ' "${sec_file}")
  METRICS[${label}_build_sec]="${mean}"
  METRICS[${label}_build_stdev_sec]="${stdev}"
  METRICS[${label}_build_min_sec]="${min}"
  return 0
}

# 复用已有行的向量作为查询向量，避免额外生成。
load_query_vectors() {
  local q qid vec step
  step=$(( ROWS / QUERIES ))
  for ((q = 1; q <= QUERIES; q++)); do
    qid=$(( (q - 1) * step + 1 ))
    vec=$(mysql_exec "SELECT v FROM ${DB}.${TABLE} WHERE id = ${qid};" | tail -1)
    if [[ -z "${vec}" ]]; then
      log "ERROR: cannot read query vector for id=${qid}"
      exit 1
    fi
    QIDS[q]="${qid}"
    QVECS[q]="${vec}"
  done
}

run_query_bench() {
  local label="$1" rounds="$2"
  local q sample sql_file sample_file start_ns end_ns total_ms avg_ms
  local approx_sql
  : > "${TMP_DIR}/${label}.samples"
  for ((q = 0; q < QUERIES; q++)); do
    approx_sql="SELECT id FROM ${DB}.${TABLE} ORDER BY l2_distance(v, '${QVECS[q]}') APPROXIMATE LIMIT ${K};"
    sql_file="${TMP_DIR}/${label}.query.${q}.sql"
    {
      echo "SET ob_query_timeout = 100000000000;"
      echo "SET ob_trx_timeout = 100000000000;"
      for ((sample = 0; sample < rounds; sample++)); do
        echo "${approx_sql}"
      done
    } > "${sql_file}"
    start_ns=$(now_ns)
    $MYSQL "${DB}" < "${sql_file}" >/dev/null
    end_ns=$(now_ns)
    total_ms=$(elapsed_ms "${start_ns}" "${end_ns}")
    avg_ms=$(awk "BEGIN {printf \"%.4f\", ${total_ms} / ${rounds}}")
    echo "${avg_ms}" >> "${TMP_DIR}/${label}.samples"
  done
  sample_file="${TMP_DIR}/${label}.samples"
  read -r avg_ms med_ms < <(awk '
    { a[NR] = $1; sum += $1 }
    END {
      if (NR == 0) { print "N/A N/A"; exit }
      for (i = 1; i <= NR; i++) for (j = i + 1; j <= NR; j++) if (a[i] > a[j]) { t = a[i]; a[i] = a[j]; a[j] = t }
      median = (NR % 2) ? a[(NR + 1) / 2] : (a[NR / 2] + a[NR / 2 + 1]) / 2
      printf "%.4f %.4f\n", sum / NR, median
    }
  ' "${sample_file}")
  METRICS[${label}_query_avg_ms]="${avg_ms}"
  METRICS[${label}_query_median_ms]="${med_ms}"
  log "  ${label}: query avg=${avg_ms}ms median=${med_ms}ms"
}

# recall@K：近似结果与全表精确扫描结果的交集比例。
compute_recall() {
  local label="$1" q
  local exact_file="${TMP_DIR}/${label}.exact" approx_file="${TMP_DIR}/${label}.approx"
  : > "${exact_file}"
  : > "${approx_file}"
  for ((q = 0; q < QUERIES; q++)); do
    mysql_exec "SELECT id FROM ${DB}.${TABLE} ORDER BY l2_distance(v, '${QVECS[q]}') LIMIT ${K};" >> "${exact_file}"
    mysql_exec "SELECT id FROM ${DB}.${TABLE} ORDER BY l2_distance(v, '${QVECS[q]}') APPROXIMATE LIMIT ${K};" >> "${approx_file}"
  done
  local hits total
  hits=$(awk -v k="${K}" '
    {
      key = int((FNR - 1) / k) ":" $1
      if (FILENAME == ARGV[1]) { seen[key] = 1 }
      else if (seen[key]) { hits++ }
    }
    END { print hits + 0 }
  ' "${exact_file}" "${approx_file}")
  total=$(( K * QUERIES ))
  METRICS[${label}_recall_at_k]="$(awk -v h="${hits}" -v t="${total}" 'BEGIN { printf "%.4f", h / t }')"
  log "  ${label}: recall@${K}=${METRICS[${label}_recall_at_k]}"
}

write_report() {
  local ts git_head git_dirty speedup report
  ts="$(date '+%Y-%m-%d %H:%M:%S')"
  git_head="$(git -C "${SCRIPT_DIR}/../.." rev-parse --short HEAD 2>/dev/null || echo unknown)"
  if git -C "${SCRIPT_DIR}/../.." diff --quiet --ignore-submodules -- 2>/dev/null; then
    git_dirty=0
  else
    git_dirty=1
  fi

  speedup="N/A"
  if [[ "${METRICS[${LIB_CPU}_build_sec]:-N/A}" != "N/A" && "${METRICS[${LIB_GPU}_build_sec]:-N/A}" != "N/A" ]]; then
    speedup=$(awk "BEGIN { if (${METRICS[${LIB_GPU}_build_sec]} > 0) printf \"%.2f\", ${METRICS[${LIB_CPU}_build_sec]} / ${METRICS[${LIB_GPU}_build_sec]}; else print \"N/A\" }")
  fi

  report="$(cat <<EOF
========================================
HNSW Build Benchmark Report (vsag vs hipVS)
========================================
timestamp:              ${ts}
git_head:               ${git_head}
git_dirty:              ${git_dirty}
dim:                    ${DIM}
rows:                   ${ROWS}
batch:                  ${BATCH}
samples:                ${SAMPLES}
k:                      ${K}
m:                      ${M_PARAM}
ef_construction:        ${EF_CONSTRUCTION}
cpu_lib:                ${LIB_CPU}
gpu_lib:                ${LIB_GPU}
----------------------------------------
load_sec:               ${METRICS[load_sec]:-N/A}
load_rows_per_sec:      ${METRICS[load_rows_per_sec]:-N/A}
----------------------------------------
${LIB_CPU}_build_sec:        ${METRICS[${LIB_CPU}_build_sec]:-N/A}
${LIB_CPU}_build_stdev_sec:  ${METRICS[${LIB_CPU}_build_stdev_sec]:-N/A}
${LIB_CPU}_build_min_sec:    ${METRICS[${LIB_CPU}_build_min_sec]:-N/A}
${LIB_CPU}_query_avg_ms:     ${METRICS[${LIB_CPU}_query_avg_ms]:-N/A}
${LIB_CPU}_query_median_ms:  ${METRICS[${LIB_CPU}_query_median_ms]:-N/A}
${LIB_CPU}_recall_at_k:      ${METRICS[${LIB_CPU}_recall_at_k]:-N/A}
${LIB_CPU}_error:            ${METRICS[${LIB_CPU}_error]:-none}
----------------------------------------
${LIB_GPU}_build_sec:        ${METRICS[${LIB_GPU}_build_sec]:-N/A}
${LIB_GPU}_build_stdev_sec:  ${METRICS[${LIB_GPU}_build_stdev_sec]:-N/A}
${LIB_GPU}_build_min_sec:    ${METRICS[${LIB_GPU}_build_min_sec]:-N/A}
${LIB_GPU}_query_avg_ms:     ${METRICS[${LIB_GPU}_query_avg_ms]:-N/A}
${LIB_GPU}_query_median_ms:  ${METRICS[${LIB_GPU}_query_median_ms]:-N/A}
${LIB_GPU}_recall_at_k:      ${METRICS[${LIB_GPU}_recall_at_k]:-N/A}
${LIB_GPU}_error:            ${METRICS[${LIB_GPU}_error]:-none}
----------------------------------------
build_speedup:          ${speedup}
========================================
EOF
)"
  echo "${report}" >&2
  if [[ -n "${OUTPUT}" ]]; then
    echo "${report}" >> "${OUTPUT}"
  fi
}

main() {
  validate_config
  log "==== HNSW build benchmark: ${LIB_CPU} (CPU) vs ${LIB_GPU} (hipVS/GPU) ===="

  if [[ "${SKIP_LOAD}" != "1" ]]; then
    load_data
  else
    log "==> SKIP_LOAD=1, reusing ${DB}.${TABLE}"
  fi
  load_query_vectors

  log "==> building ${LIB_CPU} index"
  build_index_samples "${LIB_CPU}" "cpu" || true

  log "==> building ${LIB_GPU} index"
  build_index_samples "${LIB_GPU}" "gpu" || true

  # 用最后成功构建的索引做查询与 recall 压测。
  if [[ -n "${METRICS[gpu_build_sec]:-}" ]]; then
    run_query_bench "gpu" "${QUERY_ROUNDS}"
    compute_recall "gpu"
    METRICS[${LIB_GPU}_query_avg_ms]="${METRICS[gpu_query_avg_ms]}"
    METRICS[${LIB_GPU}_query_median_ms]="${METRICS[gpu_query_median_ms]}"
    METRICS[${LIB_GPU}_recall_at_k]="${METRICS[gpu_recall_at_k]}"
  else
    log "WARN: ${LIB_GPU} index build failed, skipping its query benchmark"
  fi
  if [[ -n "${METRICS[cpu_build_sec]:-}" ]]; then
    if ! build_index_once "${LIB_CPU}" "${TMP_DIR}/${LIB_CPU}.restore.log" >/dev/null; then
      log "WARN: failed to rebuild ${LIB_CPU} index for the query benchmark"
    else
      run_query_bench "cpu" "${QUERY_ROUNDS}"
      compute_recall "cpu"
      METRICS[${LIB_CPU}_query_avg_ms]="${METRICS[cpu_query_avg_ms]}"
      METRICS[${LIB_CPU}_query_median_ms]="${METRICS[cpu_query_median_ms]}"
      METRICS[${LIB_CPU}_recall_at_k]="${METRICS[cpu_recall_at_k]}"
    fi
  fi

  # sample 统计记录在 cpu_*/gpu_* 键上，报告中按库名读取。
  METRICS[${LIB_CPU}_build_sec]="${METRICS[cpu_build_sec]:-N/A}"
  METRICS[${LIB_CPU}_build_stdev_sec]="${METRICS[cpu_build_stdev_sec]:-N/A}"
  METRICS[${LIB_CPU}_build_min_sec]="${METRICS[cpu_build_min_sec]:-N/A}"
  METRICS[${LIB_CPU}_error]="${METRICS[cpu_error]:-none}"
  METRICS[${LIB_GPU}_build_sec]="${METRICS[gpu_build_sec]:-N/A}"
  METRICS[${LIB_GPU}_build_stdev_sec]="${METRICS[gpu_build_stdev_sec]:-N/A}"
  METRICS[${LIB_GPU}_build_min_sec]="${METRICS[gpu_build_min_sec]:-N/A}"
  METRICS[${LIB_GPU}_error]="${METRICS[gpu_error]:-none}"

  write_report

  if [[ "${KEEP_DATA}" != "1" ]]; then
    mysql_exec "DROP DATABASE IF EXISTS ${DB};" >/dev/null 2>&1 || true
  fi
}

main "$@"
