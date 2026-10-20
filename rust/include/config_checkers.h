// Generated from the Rust instance parameter catalog.
#pragma once
#include "share/config/ob_config_helper.h"
#include "lib/string/ob_string.h"
namespace oceanbase { namespace config {
inline bool check_parameter(const char *name, const char *value) {
if (nullptr == name || nullptr == value) return false;
const common::ObString key = common::ObString::make_string(name);
  if (0 == key.case_compare("memory_budget")) { common::MemoryBudgetConfigChecker checker; return checker.check(value); }
  if (0 == key.case_compare("memory_limit")) { common::MemoryBudgetConfigChecker checker; return checker.check(value); }
  if (0 == key.case_compare("kvcache_memory_limit")) { common::KVCacheMemoryLimitConfigChecker checker; return checker.check(value); }
  if (0 == key.case_compare("default_compress_func")) { common::ObConfigCompressFuncChecker checker; return checker.check(value); }
  if (0 == key.case_compare("default_row_format")) { common::ObConfigRowFormatChecker checker; return checker.check(value); }
  if (0 == key.case_compare("syslog_level")) { common::ObConfigLogLevelChecker checker; return checker.check(value); }
  if (0 == key.case_compare("max_syslog_file_count")) { common::ObConfigMaxSyslogFileCountChecker checker; return checker.check(value); }
  if (0 == key.case_compare("_enable_defensive_check")) { common::ObConfigEnableDefensiveChecker checker; return checker.check(value); }
  if (0 == key.case_compare("spill_compression_codec")) { common::ObConfigSQLSpillCompressionCodecChecker checker; return checker.check(value); }
  if (0 == key.case_compare("max_stale_time_for_weak_consistency")) { common::ObConfigStaleTimeChecker checker; return checker.check(value); }
  if (0 == key.case_compare("_ctx_memory_limit")) { common::ObCtxMemoryLimitChecker checker; return checker.check(value); }
  if (0 == key.case_compare("_use_hash_rollup")) { common::ObConfigEnableHashRollupChecker checker; return checker.check(value); }
  if (0 == key.case_compare("_non_standard_comparison_level")) { common::ObConfigNonStdCmpLevelChecker checker; return checker.check(value); }
  if (0 == key.case_compare("workarea_size_policy")) { common::ObConfigWorkAreaPolicyChecker checker; return checker.check(value); }
  if (0 == key.case_compare("_parallel_ddl_control")) { common::ObParallelDDLControlChecker checker; return checker.check(value); }
  if (0 == key.case_compare("disk_io_thread_count")) { common::ObConfigEvenIntChecker checker; return checker.check(value); }
  if (0 == key.case_compare("tablet_size")) { common::ObConfigTabletSizeChecker checker; return checker.check(value); }
  if (0 == key.case_compare("_px_bloom_filter_group_size")) { common::ObConfigPxBFGroupSizeChecker checker; return checker.check(value); }
  if (0 == key.case_compare("rpc_client_authentication_method")) { common::ObCallClientAuthMethodChecker checker; return checker.check(value); }
  if (0 == key.case_compare("rpc_server_authentication_method")) { common::ObCallServerAuthMethodChecker checker; return checker.check(value); }
  if (0 == key.case_compare("_ob_ddl_temp_file_compress_func")) { common::ObConfigTempStoreFormatChecker checker; return checker.check(value); }
  if (0 == key.case_compare("vector_index_optimize_duty_time")) { common::ObVecIndexOptDutyTimeChecker checker; return checker.check(value); }
  if (0 == key.case_compare("sql_protocol_min_tls_version")) { common::ObConfigSQLTlsVersionChecker checker; return checker.check(value); }
  if (0 == key.case_compare("_hnsw_max_scan_vectors")) { common::ObHNSWIterFilterScanNumChecker checker; return checker.check(value); }
  return true;
}
}} // namespace oceanbase::config
