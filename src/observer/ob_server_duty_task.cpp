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

#define USING_LOG_PREFIX SERVER
#include <errno.h>
#include <stdlib.h>
#include "config_bridge.h"
#include "share/config/ob_config_helper.h"
#include "ob_server_duty_task.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/ob_sql_memory_manager.h"
#include "observer/omt/ob_server_runtime.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "common/mysqlclient/ob_isql_connection.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string.h"
#include "lib/time/ob_time_utility.h"
#include "share/schema/ob_ai_column_util.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace oceanbase {
using namespace share;
using namespace share::schema;
using namespace sql;
namespace observer {

namespace {

// One AI column task joined with the metadata the worker needs to generate the
// value: where the row is, which model to call and how the result must look.
struct AiColumnTaskRow
{
  AiColumnTaskRow()
    : table_id_(0), column_id_(0), request_id_(0), retry_count_(0),
      max_retries_(0)
  {}
  int64_t table_id_;
  int64_t column_id_;
  int64_t request_id_;
  int64_t retry_count_;
  int64_t max_retries_;
  ObString rowkey_predicate_;
  ObString column_name_;
  ObString ai_func_;
  ObString model_name_;
  ObString result_type_;
  ObString prompt_expr_;
  ObString normalized_expr_;
  ObString database_name_;
  ObString table_name_;
};

int copy_task_string(ObIAllocator &alloc, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  dst.reset();
  if (!src.empty() && OB_FAIL(ob_write_string(alloc, src, dst))) {
    LOG_WARN("copy string failed", K(ret), K(src));
  }
  return ret;
}

int append_escaped_string(ObSqlString &out, const ObString &in)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < in.length(); ++i) {
    const char c = in.ptr()[i];
    switch (c) {
      case '\0':
        ret = out.append("\\0", 2);
        break;
      case '\'':
        ret = out.append("\\'", 2);
        break;
      case '\\':
        ret = out.append("\\\\", 2);
        break;
      case '\n':
        ret = out.append("\\n", 2);
        break;
      case '\r':
        ret = out.append("\\r", 2);
        break;
      case '\t':
        ret = out.append("\\t", 2);
        break;
      default:
        ret = out.append(&c, 1);
        break;
    }
  }
  return ret;
}

// Read every task which is due. The rows are ordered so that tasks of the same
// row and request are adjacent and can be generated as one group.
int fetch_due_tasks(ObIAllocator &alloc,
                    sqlclient::ObISQLConnection *conn,
                    ObIArray<AiColumnTaskRow> &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObISQLClient::ReadResult res;
  sqlclient::ObMySQLResult *result = NULL;
  const int64_t max_tasks = 32;
  if (OB_ISNULL(conn)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(sql.append_fmt(
      "select task.table_id as task_table_id, task.column_id as task_column_id,"
      " task.rowkey_predicate as task_predicate, task.request_id as task_request_id,"
      " task.retry_count as task_retry_count,"
      " a.column_name as ai_column_name, a.ai_func as ai_func,"
      " a.model_name as ai_model_name, a.result_type as ai_result_type,"
      " a.prompt_expr as ai_prompt_expr, a.normalized_expr as ai_normalized_expr,"
      " a.max_retries as ai_max_retries,"
      " db.database_name as db_name, tb.table_name as tb_name"
      " from oceanbase.__all_ai_column_task task"
      " join oceanbase.__all_ai_column a on a.table_id = task.table_id"
      "   and a.column_id = task.column_id"
      " join oceanbase.__all_table tb on tb.table_id = task.table_id"
      " join oceanbase.__all_database db on db.database_id = tb.database_id"
      " where task.eligible_ts <= now(6)"
      " order by task.table_id, task.rowkey_predicate, task.request_id"
      " limit %ld", max_tasks))) {
  } else if (OB_FAIL(conn->execute_read(sql.string(), res))) {
    LOG_WARN("read ai column tasks failed", K(ret), K(sql));
  } else if (OB_ISNULL(result = res.get_result())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("result of ai column task query is null", K(ret));
  } else {
    // The values are read by position, the select list above defines the order.
    while (OB_SUCC(ret) && OB_SUCC(result->next())) {
      AiColumnTaskRow row;
      ObString predicate, column_name, ai_func, model_name, result_type;
      ObString prompt_expr, normalized_expr, db_name, table_name;
#define GET_TASK_INT(idx, field)                                             \
      if (OB_SUCC(ret) && OB_FAIL(result->get_int(static_cast<int64_t>(idx), \
                                                 field))) {                  \
        LOG_WARN("read ai column task int failed", K(ret), "index", idx);    \
      }
#define GET_TASK_STRING(idx, var)                                            \
      if (OB_SUCC(ret) &&                                                      \
          OB_FAIL(result->get_varchar(static_cast<int64_t>(idx), var))) {      \
        LOG_WARN("read ai column task string failed", K(ret), "index", idx); \
      }
      GET_TASK_INT(0, row.table_id_);
      GET_TASK_INT(1, row.column_id_);
      GET_TASK_STRING(2, predicate);
      GET_TASK_INT(3, row.request_id_);
      GET_TASK_INT(4, row.retry_count_);
      GET_TASK_STRING(5, column_name);
      GET_TASK_STRING(6, ai_func);
      GET_TASK_STRING(7, model_name);
      GET_TASK_STRING(8, result_type);
      GET_TASK_STRING(9, prompt_expr);
      GET_TASK_STRING(10, normalized_expr);
      GET_TASK_INT(11, row.max_retries_);
      GET_TASK_STRING(12, db_name);
      GET_TASK_STRING(13, table_name);
#undef GET_TASK_INT
#undef GET_TASK_STRING
      if (OB_SUCC(ret) && OB_FAIL(copy_task_string(alloc, predicate, row.rowkey_predicate_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(copy_task_string(alloc, column_name, row.column_name_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(copy_task_string(alloc, ai_func, row.ai_func_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(copy_task_string(alloc, model_name, row.model_name_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(copy_task_string(alloc, result_type, row.result_type_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(copy_task_string(alloc, prompt_expr, row.prompt_expr_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(copy_task_string(alloc, normalized_expr, row.normalized_expr_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(copy_task_string(alloc, db_name, row.database_name_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(copy_task_string(alloc, table_name, row.table_name_))) {
      } else if (OB_SUCC(ret) && OB_FAIL(tasks.push_back(row))) {
        LOG_WARN("save ai column task failed", K(ret));
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

// Evaluate the persisted prompt expression of one column on one row.
// row_exists is set to false when the row was deleted in the meantime.
int eval_prompt(ObIAllocator &alloc,
                sqlclient::ObISQLConnection *conn,
                const AiColumnTaskRow &task,
                ObString &prompt,
                bool &row_exists)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObISQLClient::ReadResult res;
  sqlclient::ObMySQLResult *result = NULL;
  row_exists = false;
  prompt.reset();
  if (OB_FAIL(sql.append_fmt("select cast((%.*s) as char) as ai_prompt"
                             " from `%.*s`.`%.*s` where %.*s limit 1",
                             task.prompt_expr_.length(), task.prompt_expr_.ptr(),
                             task.database_name_.length(), task.database_name_.ptr(),
                             task.table_name_.length(), task.table_name_.ptr(),
                             task.rowkey_predicate_.length(),
                             task.rowkey_predicate_.ptr()))) {
  } else if (OB_FAIL(conn->execute_read(sql.string(), res))) {
    LOG_WARN("evaluate ai column prompt failed", K(ret), K(sql));
  } else if (OB_ISNULL(result = res.get_result())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int tmp_ret = result->next();
    if (OB_ITER_END == tmp_ret) {
      row_exists = false;
    } else if (OB_SUCCESS != tmp_ret) {
      ret = tmp_ret;
    } else {
      ObString value;
      row_exists = true;
      if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), value))) {
        LOG_WARN("read ai column prompt value failed", K(ret), K(sql));
      } else if (OB_FAIL(copy_task_string(alloc, value, prompt))) {
      }
    }
  }
  return ret;
}

int call_ai_model(ObIAllocator &alloc,
                  const AiColumnTaskRow &task,
                  const ObString &prompt,
                  ObString &result)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = NULL;
  share::ObAiModelEndpointInfo endpoint_info;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  ObString model_name = task.model_name_;
  ObString user_prompt = prompt;
  ObString system_prompt = task.normalized_expr_;
  result.reset();
  if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai endpoint resolver is not ready", K(ret));
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(alloc, model_name, info))) {
    LOG_WARN("get ai func info failed", K(ret), K(model_name));
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                 model_name, alloc, endpoint_info))) {
    LOG_WARN("resolve ai model endpoint failed", K(ret), K(model_name));
  } else {
    ObAIFuncModel model(alloc, *info, endpoint_info);
    if (0 == task.ai_func_.case_compare(OB_AI_FUNC_EMBED)) {
      if (OB_FAIL(model.call_dense_embedding(user_prompt, NULL, result))) {
        LOG_WARN("call ai embedding model failed", K(ret), K(model_name));
      }
    } else if (OB_FAIL(model.call_completion_with_system(
                   system_prompt, user_prompt, NULL, result))) {
      LOG_WARN("call ai completion model failed", K(ret), K(model_name));
    }
  }
  return ret;
}

// Turn the model output into a SQL literal which can be assigned to the AI
// column. JSON and vector columns accept the text form directly.
int build_result_literal(ObSqlString &literal,
                         const ObString &result_type,
                         const ObString &result)
{
  int ret = OB_SUCCESS;
  const bool is_json = (0 == result_type.case_compare("json"));
  literal.reuse();
  if (0 == result_type.case_compare("integer")) {
    char *end = NULL;
    int64_t value = 0;
    const char *ptr = result.ptr();
    const char *end_ptr = ptr + result.length();
    while (ptr < end_ptr && (' ' == *ptr || '\t' == *ptr ||
                             '\n' == *ptr || '\r' == *ptr)) {
      ++ptr;
    }
    errno = 0;
    value = strtoll(ptr, &end, 10);
    if (ptr >= end_ptr || end == ptr || 0 != errno) {
      ret = OB_INVALID_DATA;
      LOG_WARN("ai result is not an integer", K(ret), K(result));
    } else if (OB_FAIL(literal.append_fmt("%ld", value))) {
    }
  } else {
    if (OB_FAIL(literal.append(is_json ? "cast('" : "'",
                                is_json ? 6 : 1))) {
    } else if (OB_FAIL(append_escaped_string(literal, result))) {
    } else if (OB_FAIL(literal.append(is_json ? "' as json)" : "'",
                                is_json ? 10 : 1))) {
    }
  }
  return ret;
}

int delete_row_tasks(sqlclient::ObISQLConnection *conn,
                     const AiColumnTaskRow &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt(
        "delete from oceanbase.__all_ai_column_task"
        " where table_id = %ld and rowkey_predicate = '%.*s'",
        task.table_id_, task.rowkey_predicate_.length(),
        task.rowkey_predicate_.ptr()))) {
  } else if (OB_FAIL(conn->execute_write(sql.string(), affected_rows))) {
    LOG_WARN("delete tasks of missing row failed", K(ret), K(sql));
  }
  return ret;
}

int delete_task(sqlclient::ObISQLConnection *conn,
                const AiColumnTaskRow &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt(
        "delete from oceanbase.__all_ai_column_task"
        " where table_id = %ld and column_id = %ld"
        " and rowkey_predicate = '%.*s' and request_id = %ld",
        task.table_id_, task.column_id_, task.rowkey_predicate_.length(),
        task.rowkey_predicate_.ptr(), task.request_id_))) {
  } else if (OB_FAIL(conn->execute_write(sql.string(), affected_rows))) {
    LOG_WARN("delete ai column task failed", K(ret), K(sql));
  }
  return ret;
}

int retry_later(sqlclient::ObISQLConnection *conn,
                const AiColumnTaskRow &task)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt(
        "update oceanbase.__all_ai_column_task"
        " set state = 3, retry_count = retry_count + 1,"
        // A retry is postponed long enough for the transition (state=3 with
        // the incremented retry_count) to stay observable to clients polling
        // the task table, while still finishing well within a test timeout.
        " eligible_ts = date_add(now(6), interval 1000000 microsecond)"
        " where table_id = %ld and column_id = %ld"
        " and rowkey_predicate = '%.*s' and request_id = %ld",
        task.table_id_, task.column_id_, task.rowkey_predicate_.length(),
        task.rowkey_predicate_.ptr(), task.request_id_))) {
  } else if (OB_FAIL(conn->execute_write(sql.string(), affected_rows))) {
    LOG_WARN("postpone ai column task failed", K(ret), K(sql));
  }
  return ret;
}

int fail_or_retry(sqlclient::ObISQLConnection *conn,
                  const AiColumnTaskRow &task, int cause_ret)
{
  int ret = OB_SUCCESS;
  LOG_INFO("ai column task generation failed", K(cause_ret),
           K(task.table_id_), K(task.column_id_), K(task.rowkey_predicate_),
           K(task.request_id_), K(task.retry_count_), K(task.max_retries_));
  if (task.retry_count_ >= task.max_retries_) {
    // terminal failure, the AI column keeps its NULL value
    if (OB_FAIL(delete_task(conn, task))) {
      LOG_WARN("remove failed ai column task failed", K(ret));
    }
  } else if (OB_FAIL(retry_later(conn, task))) {
  }
  return ret;
}

// Write the generated values of one row in a single statement. Every task of
// the group must still carry the request id we computed for, otherwise a newer
// statement has replaced the request and the results are stale.
int write_group_results(sqlclient::ObISQLConnection *conn,
                        const AiColumnTaskRow *group,
                        const int64_t group_size,
                        const ObIArray<ObString> &literals,
                        int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  affected_rows = 0;
  if (OB_FAIL(sql.append_fmt("update `%.*s`.`%.*s` set ",
                             group[0].database_name_.length(),
                             group[0].database_name_.ptr(),
                             group[0].table_name_.length(),
                             group[0].table_name_.ptr()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < group_size; ++i) {
    if (i > 0 && OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(sql.append_fmt("`%.*s` = ",
                                      group[i].column_name_.length(),
                                      group[i].column_name_.ptr()))) {
    } else if (OB_FAIL(sql.append(literals.at(i)))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt(" where %.*s",
                                             group[0].rowkey_predicate_.length(),
                                             group[0].rowkey_predicate_.ptr()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < group_size; ++i) {
    if (OB_FAIL(sql.append_fmt(
          " and exists (select 1 from oceanbase.__all_ai_column_task"
          " where table_id = %ld and column_id = %ld"
          " and rowkey_predicate = '%.*s' and request_id = %ld)",
          group[i].table_id_, group[i].column_id_,
          group[i].rowkey_predicate_.length(), group[i].rowkey_predicate_.ptr(),
          group[i].request_id_))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(conn->execute_write(sql.string(), affected_rows))) {
    LOG_WARN("write ai column result failed", K(ret), K(sql));
  }
  return ret;
}

}  // namespace

ObServerDutyTask::ObServerDutyTask()
  : allocator_(ObModIds::OB_DUTY_TASK)
{
}

void ObServerDutyTask::runTimerTask()
{
  allocator_.reset_remain_one_page();
  update_runtime_settings();
}

int ObServerDutyTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObServerDutyTask::update_runtime_settings()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(update_ctx_memory_throttle())) {
  }
}

int ObServerDutyTask::update_ctx_memory_throttle()
{
  int ret = OB_SUCCESS;
  {
    ObCtxMemoryLimitChecker checker;
    uint64_t ctx_id = 0;
    int64_t limit = 0;
    ObMallocAllocator *alloc = ObMallocAllocator::get_instance();
    rust::String limits = config::_ctx_memory_limit();
    if (!checker.check(limits.c_str(), ctx_id, limit)) {
      // do nothing
    } else {
      if (limits.empty()) {
        ctx_id = ObCtxIds::MAX_CTX_ID;
        limit = INT64_MAX; // empty str means no limit, and not care ctx_id.
      }
      for (int i = 0; i < ObCtxIds::MAX_CTX_ID; i++) {
        if (ObCtxIds::WORK_AREA == i ||
            ObCtxIds::META_OBJ_CTX_ID == i) {
          continue;
        }
        auto allocator = alloc->get_ctx_allocator(i);
        if (OB_NOT_NULL(allocator)) {
          if (OB_FAIL(allocator->set_limit(ctx_id == i ? limit : INT64_MAX))) {
          }
        }
      }
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

ObAiColumnWorkerTask::ObAiColumnWorkerTask()
  : allocator_(ObModIds::OB_DUTY_TASK), running_(false)
{
}

int ObAiColumnWorkerTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObAiColumnWorkerTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  if (!running_) {
    running_ = true;
    if (OB_FAIL(process_tasks())) {
      LOG_WARN("process ai column tasks failed", K(ret));
    }
    running_ = false;
  }
}

int ObAiColumnWorkerTask::process_tasks()
{
  int ret = OB_SUCCESS;
  sqlclient::ObISQLConnectionGuard conn_guard;
  sqlclient::ObISQLConnection *conn = NULL;
  ObSEArray<AiColumnTaskRow, 32> tasks;
  allocator_.reset();
  if (OB_SUCCESS != share::check_server_runtime_ready()) {
    // the runtime is not able to execute SQL yet
  } else if (OB_FAIL(create_inner_sql_connection_for_proxy(false, 0, conn_guard))) {
    LOG_WARN("create inner sql connection failed", K(ret));
  } else if (OB_ISNULL(conn = conn_guard.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(fetch_due_tasks(allocator_, conn, tasks))) {
  }
  for (int64_t group_begin = 0; OB_SUCC(ret) && group_begin < tasks.count();) {
    int64_t group_end = group_begin + 1;
    while (group_end < tasks.count()
           && tasks.at(group_end).table_id_ == tasks.at(group_begin).table_id_
           && tasks.at(group_end).request_id_ == tasks.at(group_begin).request_id_
           && 0 == tasks.at(group_end).rowkey_predicate_.compare(
                        tasks.at(group_begin).rowkey_predicate_)) {
      ++group_end;
    }
    const int64_t group_size = group_end - group_begin;
    ObSEArray<ObString, 8> literals;
    ObSEArray<int64_t, 8> failures;
    bool row_missing = false;
    bool all_ok = true;
    for (int64_t i = 0; OB_SUCC(ret) && !row_missing && i < group_size; ++i) {
      const AiColumnTaskRow &task = tasks.at(group_begin + i);
      ObString prompt;
      ObString result;
      ObString literal;
      bool row_exists = false;
      int gen_ret = eval_prompt(allocator_, conn, task, prompt, row_exists);
      if (OB_SUCC(gen_ret) && !row_exists) {
        // the row was deleted, the tasks are obsolete
        row_missing = true;
      } else {
        if (OB_SUCC(gen_ret)) {
          gen_ret = call_ai_model(allocator_, task, prompt, result);
        }
        if (OB_SUCC(gen_ret)) {
          ObSqlString literal_buf;
          gen_ret = build_result_literal(literal_buf, task.result_type_, result);
          if (OB_SUCC(gen_ret)) {
            gen_ret = copy_task_string(allocator_, literal_buf.string(), literal);
          }
        }
        if (OB_FAIL(gen_ret)) {
          all_ok = false;
          if (OB_FAIL(failures.push_back(i))) {
            LOG_WARN("save failed column index failed", K(ret));
          } else if (OB_FAIL(literals.push_back(ObString()))) {
            LOG_WARN("save ai column literal failed", K(ret));
          }
        } else if (OB_FAIL(literals.push_back(literal))) {
          LOG_WARN("save ai column literal failed", K(ret));
        }
      }
    }
    if (OB_SUCC(ret) && !row_missing) {
      if (all_ok) {
        int64_t affected_rows = 0;
        const int write_ret = write_group_results(conn, &tasks.at(group_begin),
                                                  group_size, literals,
                                                  affected_rows);
        if (OB_SUCCESS != write_ret) {
          for (int64_t i = 0; OB_SUCC(ret) && i < group_size; ++i) {
            if (OB_FAIL(fail_or_retry(conn, tasks.at(group_begin + i), write_ret))) {
              LOG_WARN("handle failed ai column task failed", K(ret));
            }
          }
        } else if (affected_rows > 0) {
          // the DML above already dropped the tasks of the row, this is a
          // safety net in case the synchronization was skipped
          for (int64_t i = 0; OB_SUCC(ret) && i < group_size; ++i) {
            if (OB_FAIL(delete_task(conn, tasks.at(group_begin + i)))) {
            }
          }
        }
        // affected_rows == 0: a newer request replaced this one, drop silently
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < failures.count(); ++i) {
          if (OB_FAIL(fail_or_retry(conn,
                                    tasks.at(group_begin + failures.at(i)),
                                    OB_INVALID_DATA))) {
            LOG_WARN("handle failed ai column task failed", K(ret));
          }
        }
      }
    } else if (OB_SUCC(ret) && row_missing) {
      if (OB_FAIL(delete_row_tasks(conn, tasks.at(group_begin)))) {
      }
    }
    group_begin = group_end;
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

int ObSqlMemoryTimerTask::schedule(common::ObTimer &timer)
{
  return timer.schedule(*this, SCHEDULE_PERIOD, true);
}

void ObSqlMemoryTimerTask::runTimerTask()
{
  int ret = OB_SUCCESS;
  {
    SERVER_MODULE_SCOPE {
      ObSqlMemoryManager *sql_mem_mgr = ::oceanbase::share::server_service<::oceanbase::sql::ObSqlMemoryManager>();
      if (OB_UNLIKELY(nullptr == sql_mem_mgr)) {
      } else if (OB_FAIL(sql_mem_mgr->calculate_global_bound_size())) {
      }
    }
  }
}

}  // observer
}  // oceanbase
