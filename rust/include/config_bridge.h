#pragma once
#include "check_callback.h"
#include "local/src/bridge.rs.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#if __cplusplus >= 201703L
#include <string_view>
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif // __clang__

namespace rust {
inline namespace cxxbridge1 {
// #include "rust/cxx.h"

struct unsafe_bitcopy_t;

namespace {
template <typename T>
class impl;
} // namespace

#ifndef CXXBRIDGE1_RUST_STRING
#define CXXBRIDGE1_RUST_STRING
class String final {
public:
  String() noexcept;
  String(const String &) noexcept;
  String(String &&) noexcept;
  ~String() noexcept;

  String(const std::string &);
  String(const char *);
  String(const char *, std::size_t);
  String(const char16_t *);
  String(const char16_t *, std::size_t);
#ifdef __cpp_char8_t
  String(const char8_t *s);
  String(const char8_t *s, std::size_t len);
#endif

  static String lossy(const std::string &) noexcept;
  static String lossy(const char *) noexcept;
  static String lossy(const char *, std::size_t) noexcept;
  static String lossy(const char16_t *) noexcept;
  static String lossy(const char16_t *, std::size_t) noexcept;

  String &operator=(const String &) & noexcept;
  String &operator=(String &&) & noexcept;

  explicit operator std::string() const;

  const char *data() const noexcept;
  std::size_t size() const noexcept;
  std::size_t length() const noexcept;
  bool empty() const noexcept;

  const char *c_str() noexcept;

  std::size_t capacity() const noexcept;
  void reserve(size_t new_cap) noexcept;

  using iterator = char *;
  iterator begin() noexcept;
  iterator end() noexcept;

  using const_iterator = const char *;
  const_iterator begin() const noexcept;
  const_iterator end() const noexcept;
  const_iterator cbegin() const noexcept;
  const_iterator cend() const noexcept;

  bool operator==(const String &) const noexcept;
  bool operator!=(const String &) const noexcept;
  bool operator<(const String &) const noexcept;
  bool operator<=(const String &) const noexcept;
  bool operator>(const String &) const noexcept;
  bool operator>=(const String &) const noexcept;

  void swap(String &) noexcept;

  String(unsafe_bitcopy_t, const String &) noexcept;

private:
  struct lossy_t;
  String(lossy_t, const char *, std::size_t) noexcept;
  String(lossy_t, const char16_t *, std::size_t) noexcept;
  friend void swap(String &lhs, String &rhs) noexcept { lhs.swap(rhs); }

  std::array<std::uintptr_t, 3> repr;
};
#endif // CXXBRIDGE1_RUST_STRING

#ifndef CXXBRIDGE1_RUST_STR
#define CXXBRIDGE1_RUST_STR
class Str final {
public:
  Str() noexcept;
  Str(const String &) noexcept;
  Str(const std::string &);
  Str(const char *);
  Str(const char *, std::size_t);

  Str &operator=(const Str &) & noexcept = default;

  explicit operator std::string() const;
#if __cplusplus >= 201703L
  explicit operator std::string_view() const;
#endif

  const char *data() const noexcept;
  std::size_t size() const noexcept;
  std::size_t length() const noexcept;
  bool empty() const noexcept;

  Str(const Str &) noexcept = default;
  ~Str() noexcept = default;

  using iterator = const char *;
  using const_iterator = const char *;
  const_iterator begin() const noexcept;
  const_iterator end() const noexcept;
  const_iterator cbegin() const noexcept;
  const_iterator cend() const noexcept;

  bool operator==(const Str &) const noexcept;
  bool operator!=(const Str &) const noexcept;
  bool operator<(const Str &) const noexcept;
  bool operator<=(const Str &) const noexcept;
  bool operator>(const Str &) const noexcept;
  bool operator>=(const Str &) const noexcept;

  void swap(Str &) noexcept;

private:
  class uninit;
  Str(uninit) noexcept;
  friend impl<Str>;

  std::array<std::uintptr_t, 2> repr;
};
#endif // CXXBRIDGE1_RUST_STR
} // namespace cxxbridge1
} // namespace rust

#if __cplusplus >= 201402L
#define CXX_DEFAULT_VALUE(value) = value
#else
#define CXX_DEFAULT_VALUE(value)
#endif

namespace oceanbase {
  namespace config {
    struct MomentTime;
    struct ParameterRow;
    struct Status;
    using CheckCallback = ::oceanbase::config::CheckCallback;
  }
}

namespace oceanbase {
namespace config {
#ifndef CXXBRIDGE1_STRUCT_oceanbase$config$MomentTime
#define CXXBRIDGE1_STRUCT_oceanbase$config$MomentTime
struct MomentTime final {
  bool disabled CXX_DEFAULT_VALUE(false);
  ::std::uint8_t hour CXX_DEFAULT_VALUE(0);
  ::std::uint8_t minute CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_oceanbase$config$MomentTime

#ifndef CXXBRIDGE1_STRUCT_oceanbase$config$ParameterRow
#define CXXBRIDGE1_STRUCT_oceanbase$config$ParameterRow
struct ParameterRow final {
  ::rust::String name;
  ::rust::String data_type;
  ::rust::String value;
  ::rust::String info;
  ::rust::String section;
  ::rust::String scope;
  ::rust::String source;
  ::rust::String edit_level;
  ::rust::String default_value;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_oceanbase$config$ParameterRow

#ifndef CXXBRIDGE1_STRUCT_oceanbase$config$Status
#define CXXBRIDGE1_STRUCT_oceanbase$config$Status
struct Status final {
  bool ok CXX_DEFAULT_VALUE(false);
  bool commit_uncertain CXX_DEFAULT_VALUE(false);
  ::rust::String message;

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_oceanbase$config$Status

::std::size_t parameter_count() noexcept;

::oceanbase::config::ParameterRow parameter_row(::std::size_t index) noexcept;

bool parameter_exists(::rust::Str name) noexcept;

bool parameter_readonly(::rust::Str name) noexcept;

::rust::String parameter_default(::rust::Str name) noexcept;

bool parameter_valid(::rust::Str name, ::rust::Str value) noexcept;

::std::int64_t generation() noexcept;

::std::int64_t server_create_time() noexcept;

::rust::String server_role_info() noexcept;

::oceanbase::config::Status load_active(::oceanbase::local::Store const &store, bool startup) noexcept;

::oceanbase::config::Status save_bootstrap(::oceanbase::local::Store const &store) noexcept;

::oceanbase::config::Status bootstrap_set(::std::string const &name, ::std::string const &value) noexcept;

::oceanbase::config::Status update_internal_state(::oceanbase::local::Store const &store, ::std::string const &name, ::std::string const &value) noexcept;

::oceanbase::config::Status update_checked(::oceanbase::local::Store const &store, ::std::string const &name, ::std::string const &value, bool reset, ::oceanbase::config::CheckCallback const &callback) noexcept;

::rust::String data_dir() noexcept;

::rust::String redo_dir() noexcept;

::std::int64_t datafile_size() noexcept;

::std::int64_t datafile_next() noexcept;

::std::int64_t datafile_maxsize() noexcept;

::std::int64_t datafile_disk_percentage() noexcept;

::std::int64_t _datafile_usage_upper_bound_percentage() noexcept;

bool enable_rpc_service() noexcept;

bool enable_rpc_tls() noexcept;

::std::int64_t rpc_port() noexcept;

::std::int64_t mysql_port() noexcept;

::rust::String mysql_port_mode() noexcept;

::std::int64_t internal_sql_execute_timeout() noexcept;

::std::int64_t net_thread_count() noexcept;

::std::int64_t server_task_queue_size() noexcept;

::std::int64_t memory_budget() noexcept;

::std::int64_t memory_limit() noexcept;

::std::int64_t kvcache_memory_limit() noexcept;

::std::int64_t memstore_memory_limit() noexcept;

::std::int64_t vector_memory_limit() noexcept;

::std::int64_t cpu_count() noexcept;

::std::int64_t trace_log_slow_query_watermark() noexcept;

bool enable_record_trace_log() noexcept;

::std::int64_t max_string_print_length() noexcept;

bool enable_record_trace_id() noexcept;

bool enable_rich_error_msg() noexcept;

::std::int64_t debug_sync_timeout() noexcept;

::std::int64_t dead_socket_detection_timeout() noexcept;

bool enable_perf_event() noexcept;

::std::int64_t schema_history_expire_time() noexcept;

::rust::String default_compress_func() noexcept;

::rust::String default_row_format() noexcept;

::std::int64_t storage_rowsets_size() noexcept;

::std::int64_t weak_read_version_refresh_interval() noexcept;

::rust::String syslog_level() noexcept;

::std::int64_t syslog_io_bandwidth_limit() noexcept;

::std::int64_t diag_syslog_per_error_limit() noexcept;

::std::int64_t max_syslog_file_count() noexcept;

bool enable_async_syslog() noexcept;

::std::int64_t cache_wash_threshold() noexcept;

::std::int64_t memory_chunk_cache_size() noexcept;

bool enable_sql_operator_dump() noexcept;

::std::int64_t _chunk_row_store_mem_limit() noexcept;

::std::int64_t _sort_area_size() noexcept;

::std::int64_t _hash_area_size() noexcept;

bool _enable_partition_level_retry() noexcept;

::std::int64_t _enable_defensive_check() noexcept;

bool _sql_insert_multi_values_split_opt() noexcept;

::std::int64_t _min_malloc_sample_interval() noexcept;

::std::int64_t _max_malloc_sample_interval() noexcept;

bool _enable_values_table_folding() noexcept;

::rust::String spill_compression_codec() noexcept;

::std::int64_t max_stale_time_for_weak_consistency() noexcept;

bool enable_monotonic_weak_read() noexcept;

double server_cpu_quota_min() noexcept;

double server_cpu_quota_max() noexcept;

::std::int64_t workers_per_cpu_quota() noexcept;

::std::int64_t _ob_max_thread_num() noexcept;

double cpu_quota_concurrency() noexcept;

::std::int64_t px_workers_per_cpu_quota() noexcept;

::std::int64_t undo_retention() noexcept;

bool _mvcc_gc_using_min_txn_snapshot() noexcept;

::std::int64_t _rowsets_target_maxsize() noexcept;

::std::int64_t _rowsets_max_rows() noexcept;

::rust::String _ctx_memory_limit() noexcept;

bool _enable_convert_real_to_decimal() noexcept;

bool _enable_decimal_int_type() noexcept;

::std::int64_t _optimizer_ads_time_limit() noexcept;

bool _hash_join_enabled() noexcept;

bool _optimizer_sortmerge_join_enabled() noexcept;

bool _nested_loop_join_enabled() noexcept;

bool _enable_mysql_compatible_dates() noexcept;

::std::int64_t _min_const_integer_precision() noexcept;

::std::int64_t _lob_rowsets_max_rows() noexcept;

::rust::String _use_hash_rollup() noexcept;

bool _enable_constant_type_demotion() noexcept;

::rust::String _non_standard_comparison_level() noexcept;

::std::int64_t _memstore_limit_percentage() noexcept;

::std::int64_t freeze_trigger_percentage() noexcept;

::std::int64_t writing_throttling_trigger_percentage() noexcept;

::std::int64_t data_disk_write_limit_percentage() noexcept;

::std::int64_t writing_throttling_maximum_duration() noexcept;

::std::int64_t plan_cache_evict_interval() noexcept;

::std::int64_t default_progressive_merge_num() noexcept;

::std::int64_t _parallel_min_message_pool() noexcept;

double _parallel_server_sleep_time() noexcept;

double _px_max_message_pool_pct() noexcept;

bool _px_message_compression() noexcept;

::std::int64_t _px_chunklist_count_ratio() noexcept;

bool _sqlexec_disable_hash_based_distagg_tiv() noexcept;

bool _force_hash_groupby_dump() noexcept;

bool _force_hash_join_spill() noexcept;

::std::int64_t _enable_hash_join_hasher() noexcept;

::std::int64_t _enable_hash_join_processor() noexcept;

::std::int64_t _pushdown_storage_level() noexcept;

::rust::String workarea_size_policy() noexcept;

::std::int64_t temporary_file_max_disk_size() noexcept;

::std::int64_t _temporary_file_io_area_size() noexcept;

::std::int64_t _storage_meta_memory_limit_percentage() noexcept;

::std::int64_t _max_tablet_cnt_per_gb() noexcept;

bool _enable_filter_reordering() noexcept;

bool enable_sys_table_ddl() noexcept;

::std::int64_t tablet_meta_table_scan_batch_count() noexcept;

::std::int64_t tablet_meta_table_check_interval() noexcept;

bool enable_ddl() noexcept;

bool _enable_parallel_table_creation() noexcept;

bool enable_major_freeze() noexcept;

::std::int64_t ob_event_history_recycle_interval() noexcept;

::std::int64_t _recyclebin_object_purge_frequency() noexcept;

::std::int64_t recyclebin_object_expire_time() noexcept;

::rust::String _parallel_ddl_control() noexcept;

::std::int64_t log_disk_size() noexcept;

::std::int64_t log_disk_percentage() noexcept;

::std::int64_t log_disk_utilization_limit_threshold() noexcept;

::std::int64_t log_disk_utilization_threshold() noexcept;

::std::int64_t log_disk_throttling_percentage() noexcept;

::std::int64_t log_disk_throttling_maximum_duration() noexcept;

::std::int64_t log_storage_warning_tolerance_time() noexcept;

bool _enable_log_cache() noexcept;

::std::int64_t _lcl_op_interval() noexcept;

::rust::String major_freeze_duty_time() noexcept;

::oceanbase::config::MomentTime major_freeze_duty_time_parts() noexcept;

::std::int64_t merger_check_interval() noexcept;

::std::int64_t trx_commit_retry_interval() noexcept;

::std::int64_t clog_sync_time_warn_threshold() noexcept;

::std::int64_t row_compaction_update_limit() noexcept;

bool enable_early_lock_release() noexcept;

::std::int64_t _tx_result_retention() noexcept;

bool _enable_parallel_redo_logging() noexcept;

::std::int64_t _parallel_redo_logging_trigger() noexcept;

::std::int64_t _tx_debug_level() noexcept;

::std::int64_t rpc_timeout() noexcept;

::std::int64_t location_cache_refresh_sql_timeout() noexcept;

::std::int64_t bf_cache_miss_count_threshold() noexcept;

::std::int64_t _data_storage_io_timeout() noexcept;

::std::int64_t data_storage_warning_tolerance_time() noexcept;

::std::int64_t data_disk_usage_limit_percentage() noexcept;

::std::int64_t sys_bkgd_net_percentage() noexcept;

::std::int64_t disk_io_thread_count() noexcept;

::std::int64_t sync_io_thread_count() noexcept;

::std::int64_t _io_callback_thread_count() noexcept;

bool _enable_parallel_minor_merge() noexcept;

bool _enable_adaptive_compaction() noexcept;

::std::int64_t compaction_dag_cnt_limit() noexcept;

::std::int64_t compaction_schedule_tablet_batch_cnt() noexcept;

::std::int64_t compaction_low_thread_score() noexcept;

::std::int64_t compaction_mid_thread_score() noexcept;

::std::int64_t compaction_high_thread_score() noexcept;

::std::int64_t storage_high_thread_score() noexcept;

::std::int64_t ddl_thread_score() noexcept;

::std::int64_t minor_compact_trigger() noexcept;

bool _enable_compaction_diagnose() noexcept;

::rust::String _force_skip_encoding_partition_id() noexcept;

::std::int64_t _private_buffer_size() noexcept;

::std::int64_t _fast_commit_callback_count() noexcept;

::std::int64_t _trx_max_log_cb_limit() noexcept;

::std::int64_t _minor_compaction_amplification_factor() noexcept;

::std::int64_t major_compact_trigger() noexcept;

::std::int64_t _minor_merge_write_amplification_threshold() noexcept;

::std::int64_t ob_compaction_schedule_interval() noexcept;

::std::int64_t _ob_elr_fast_freeze_threshold() noexcept;

bool _ob_enable_fast_freeze() noexcept;

::std::int64_t _compaction_prewarm_percentage() noexcept;

::std::int64_t tablet_size() noexcept;

::std::int64_t builtin_db_data_verify_cycle() noexcept;

::std::int64_t micro_block_merge_verify_level() noexcept;

::std::int64_t _cache_wash_interval() noexcept;

::std::int64_t _io_read_batch_size() noexcept;

::std::int64_t _io_read_redundant_limit_percentage() noexcept;

::std::int64_t dtl_buffer_size() noexcept;

::std::int64_t px_task_size() noexcept;

::std::int64_t _max_elr_dependent_trx_count() noexcept;

::std::int64_t stack_size() noexcept;

::std::int64_t _px_max_pipeline_depth() noexcept;

bool ssl_client_authentication() noexcept;

bool use_ipv6() noexcept;

::std::int64_t _ob_ddl_timeout() noexcept;

bool ob_enable_batched_multi_statement() noexcept;

::std::int64_t _bloom_filter_ratio() noexcept;

::std::int64_t schema_history_recycle_interval() noexcept;

bool _bloom_filter_enabled() noexcept;

::rust::String ob_ssl_invited_common_names() noexcept;

::rust::String _ob_ssl_invited_nodes() noexcept;

::std::int64_t _max_schema_slot_num() noexcept;

bool _enable_add_fulltext_index_to_existing_table() noexcept;

::std::int64_t open_cursors() noexcept;

bool _enable_px_batch_rescan() noexcept;

bool _enable_spf_batch_rescan() noexcept;

bool _enable_das_keep_order() noexcept;

bool _enable_index_merge() noexcept;

bool _enable_distributed_das_scan() noexcept;

::std::int64_t _enable_das_batch_rescan_flag() noexcept;

::std::int64_t _parallel_max_active_sessions() noexcept;

bool enable_tcp_keepalive() noexcept;

::std::int64_t tcp_keepidle() noexcept;

::std::int64_t tcp_keepintvl() noexcept;

::std::int64_t tcp_keepcnt() noexcept;

::std::int64_t _send_bloom_filter_size() noexcept;

bool _enable_px_ordered_coord() noexcept;

::std::int64_t job_queue_processes() noexcept;

::rust::String _px_bloom_filter_group_size() noexcept;

bool _enable_block_file_punch_hole() noexcept;

bool _ob_enable_fast_parser() noexcept;

::std::int64_t _ob_obj_dep_maint_task_interval() noexcept;

bool _enable_newsort() noexcept;

::std::int64_t _px_object_sampling() noexcept;

::std::int64_t _follower_snapshot_read_retry_duration() noexcept;

::std::int64_t _print_sample_ppm() noexcept;

bool _enable_parallel_das_dml() noexcept;

bool _ob_immediate_row_conflict_check() noexcept;

bool _enable_var_assign_use_das() noexcept;

::std::int64_t sql_net_thread_count() noexcept;

bool _display_non_session_cursor() noexcept;

::std::int64_t _ob_plan_cache_auto_flush_interval() noexcept;

bool _px_join_skew_handling() noexcept;

::std::int64_t _px_join_skew_minfreq() noexcept;

bool _enable_transaction_internal_routing() noexcept;

bool _enable_hgby_skew_detection() noexcept;

::std::int64_t _pdml_thread_cache_size() noexcept;

bool _enable_hgby_llc_ndv_adaptive() noexcept;

::rust::String rpc_client_authentication_method() noexcept;

::rust::String rpc_server_authentication_method() noexcept;

bool _enable_backtrace_function() noexcept;

::std::int64_t _with_subquery() noexcept;

bool _optimizer_group_by_placement() noexcept;

::std::int64_t _complex_cbqt_table_num() noexcept;

bool _enable_in_range_optimization() noexcept;

bool _force_subquery_unnest() noexcept;

::std::int64_t range_optimizer_max_mem_size() noexcept;

::std::int64_t _schema_memory_recycle_interval() noexcept;

bool _optimizer_better_inlist_costing() noexcept;

bool _enable_skip_index() noexcept;

::rust::String _ob_ddl_temp_file_compress_func() noexcept;

bool _enable_prefetch_limiting() noexcept;

bool _ob_sqlstat_enable() noexcept;

bool _enable_inner_session_mgr() noexcept;

bool _enable_trace_tablet_leak() noexcept;

::std::int64_t _inlist_rewrite_threshold() noexcept;

::rust::String vector_index_optimize_duty_time() noexcept;

::std::int64_t model_request_timeout() noexcept;

::std::int64_t model_max_retries() noexcept;

::rust::String sql_protocol_min_tls_version() noexcept;

::std::int64_t shared_log_retention() noexcept;

bool _enable_optimizer_qualify_filter() noexcept;

bool _enable_range_extraction_for_not_in() noexcept;

::std::int64_t optimizer_index_cost_adj() noexcept;

bool enable_rpc_authentication_bypass() noexcept;

bool _preserve_order_for_pagination() noexcept;

bool _preserve_order_for_groupby() noexcept;

::std::int64_t max_partition_num() noexcept;

::std::int64_t json_document_max_depth() noexcept;

::std::int64_t _multimodel_memory_trace_level() noexcept;

::std::int64_t _query_record_size_limit() noexcept;

bool _allow_skip_replay_redo_after_detete_tablet() noexcept;

bool strict_check_os_params() noexcept;

bool _enable_tree_based_io_scheduler() noexcept;

::std::int64_t clog_io_isolation_mode() noexcept;

bool _enable_memleak_light_backtrace() noexcept;

bool _enable_dbms_job_package() noexcept;

::std::int64_t lob_enable_block_cache_threshold() noexcept;

::std::int64_t _ob_pl_compile_max_concurrency() noexcept;

bool enable_lock_priority() noexcept;

bool _preset_runtime_bloom_filter_size() noexcept;

bool _enable_ddl_worker_isolation() noexcept;

::std::int64_t ob_vector_memory_limit_percentage() noexcept;

bool vector_index_memory_saving_mode() noexcept;

bool _enable_drop_and_add_index() noexcept;

::std::int64_t ob_encoding_granularity() noexcept;

bool _partition_wise_plan_enabled() noexcept;

bool _enable_adaptive_auto_dop() noexcept;

::std::int64_t query_memory_limit_percentage() noexcept;

bool _ndv_runtime_bloom_filter_size() noexcept;

::std::int64_t _max_px_workers_per_cpu() noexcept;

::rust::String default_table_organization() noexcept;

bool _enable_topn_runtime_filter() noexcept;

bool _enable_async_load_sys_package() noexcept;

bool _enable_px_task_rebalance() noexcept;

::std::int64_t _px_task_rebalance_trigger_time() noexcept;

bool _ob_enable_truncate_partition_preserve_global_index() noexcept;

bool _px_worker_share_plan_enabled() noexcept;

bool _enable_drop_column_instant() noexcept;

bool _enable_routine_call_param_defend() noexcept;

::std::int64_t _hnsw_max_scan_vectors() noexcept;

::std::int64_t _advance_checkpoint_interval() noexcept;

::rust::String log_restore_source() noexcept;
} // namespace config
} // namespace oceanbase

#ifdef __clang__
#pragma clang diagnostic pop
#endif // __clang__
