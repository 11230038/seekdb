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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_UTIL_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_UTIL_H_

#include <stdlib.h>
#include <string.h>
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_iarray.h"
#include "lib/container/ob_se_array.h"
#include "lib/ob_define.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// AI column generation spec is stored in ObColumnSchemaV2::extended_type_info_,
// one "KEY=VALUE" entry per attribute. The entries are plain strings, the value
// part never needs escaping because ObString is length based.
static const char *const OB_AI_TYPE_INFO_FUNC_PREFIX = "AI_FUNC=";
static const char *const OB_AI_TYPE_INFO_MODEL_PREFIX = "AI_MODEL=";
static const char *const OB_AI_TYPE_INFO_PROMPT_PREFIX = "AI_PROMPT=";
static const char *const OB_AI_TYPE_INFO_EXPR_PREFIX = "AI_EXPR=";
static const char *const OB_AI_TYPE_INFO_SYNC_PREFIX = "AI_SYNC=";
static const char *const OB_AI_TYPE_INFO_RETRY_PREFIX = "AI_RETRY=";
static const char *const OB_AI_TYPE_INFO_RESULT_TYPE_PREFIX = "AI_RESULT_TYPE=";
static const char *const OB_AI_TYPE_INFO_DEP_PREFIX = "AI_DEP=";
static const char *const OB_AI_TYPE_INFO_STATE_COL_PREFIX = "AI_STATE_COL=";
static const char *const OB_AI_TYPE_INFO_REQUEST_COL_PREFIX = "AI_REQUEST_COL=";
static const char *const OB_AI_TYPE_INFO_SPEC_COL_PREFIX = "AI_SPEC_COL=";

static const char *const OB_AI_FUNC_COMPLETE = "AI_COMPLETE";
static const char *const OB_AI_FUNC_EMBED = "AI_EMBED";

// Typed instructions persisted in __all_ai_column.normalized_expr. The worker
// sends the normalized expression as the system prompt, so the model is always
// aware of the expected result shape.
static const char *const OB_AI_JSON_INSTRUCTION =
    "\nReturn exactly one valid JSON value and nothing else.";
static const char *const OB_AI_INTEGER_INSTRUCTION =
    "\nReturn the result as a base-10 integer and nothing else.";
static const char *const OB_AI_TEXT_INSTRUCTION =
    "\nReturn only the result value and nothing else.";

struct ObAiColumnSpec
{
public:
  ObAiColumnSpec()
    : func_name_(),
      model_name_(),
      prompt_expr_(),
      full_expr_(),
      sync_mode_(),
      result_type_(),
      max_retries_(0),
      state_column_id_(OB_INVALID_ID),
      request_column_id_(OB_INVALID_ID),
      spec_column_id_(OB_INVALID_ID),
      deps_()
  {}
  ~ObAiColumnSpec() {}
  void reset()
  {
    func_name_.reset();
    model_name_.reset();
    prompt_expr_.reset();
    full_expr_.reset();
    sync_mode_.reset();
    result_type_.reset();
    max_retries_ = 0;
    state_column_id_ = OB_INVALID_ID;
    request_column_id_ = OB_INVALID_ID;
    spec_column_id_ = OB_INVALID_ID;
    deps_.reset();
  }
  bool is_valid() const { return !func_name_.empty() && !model_name_.empty(); }
  TO_STRING_KV(K_(func_name), K_(model_name), K_(prompt_expr), K_(sync_mode),
               K_(result_type), K_(max_retries), K_(state_column_id),
               K_(request_column_id), K_(spec_column_id), K_(deps));

  common::ObString func_name_;
  common::ObString model_name_;
  common::ObString prompt_expr_;
  common::ObString full_expr_;
  common::ObString sync_mode_;
  common::ObString result_type_;
  int64_t max_retries_;
  int64_t state_column_id_;
  int64_t request_column_id_;
  int64_t spec_column_id_;
  common::ObSEArray<common::ObString, 4> deps_;
};

inline bool is_ai_column_type_info(const common::ObIArray<common::ObString> &type_info)
{
  bool is_ai = false;
  for (int64_t i = 0; !is_ai && i < type_info.count(); ++i) {
    is_ai = type_info.at(i).prefix_match(OB_AI_TYPE_INFO_FUNC_PREFIX);
  }
  return is_ai;
}

inline int64_t ai_type_info_value_offset(const common::ObString &entry)
{
  const char *pos = static_cast<const char *>(memchr(entry.ptr(), '=', entry.length()));
  return NULL == pos ? -1 : static_cast<int64_t>(pos - entry.ptr()) + 1;
}

inline int add_ai_type_info_entry(const char *prefix,
                                  const common::ObString &value,
                                  common::ObIAllocator &allocator,
                                  common::ObIArray<common::ObString> &type_info)
{
  int ret = OB_SUCCESS;
  const int64_t prefix_len = static_cast<int64_t>(strlen(prefix));
  common::ObString entry;
  if (OB_ISNULL(prefix)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    char *buf = static_cast<char *>(allocator.alloc(prefix_len + value.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      SHARE_SCHEMA_LOG(WARN, "failed to alloc ai type info entry", K(ret), K(prefix_len), K(value));
    } else {
      MEMCPY(buf, prefix, prefix_len);
      if (value.length() > 0) {
        MEMCPY(buf + prefix_len, value.ptr(), value.length());
      }
      entry.assign_ptr(buf, static_cast<int32_t>(prefix_len + value.length()));
      if (OB_FAIL(type_info.push_back(entry))) {
        SHARE_SCHEMA_LOG(WARN, "failed to push ai type info entry", K(ret), K(entry));
      }
    }
  }
  return ret;
}

inline int add_ai_type_info_int_entry(const char *prefix,
                                      const int64_t value,
                                      common::ObIAllocator &allocator,
                                      common::ObIArray<common::ObString> &type_info)
{
  char buf[32] = {0};
  int64_t pos = 0;
  int ret = OB_SUCCESS;
  if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "%ld", value))) {
    SHARE_SCHEMA_LOG(WARN, "failed to format int value", K(ret), K(value));
  } else {
    ret = add_ai_type_info_entry(prefix, common::ObString(static_cast<int32_t>(pos), buf),
                                 allocator, type_info);
  }
  return ret;
}

inline int parse_ai_column_spec(const common::ObIArray<common::ObString> &type_info,
                                ObAiColumnSpec &spec)
{
  int ret = OB_SUCCESS;
  spec.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < type_info.count(); ++i) {
    const common::ObString &entry = type_info.at(i);
    const int64_t value_offset = ai_type_info_value_offset(entry);
    if (value_offset <= 0 || value_offset > entry.length()) {
      // not a key=value entry, e.g. VECTOR(4)
    } else {
      const common::ObString value(entry.length() - static_cast<int32_t>(value_offset),
                                   entry.ptr() + value_offset);
      if (entry.prefix_match(OB_AI_TYPE_INFO_FUNC_PREFIX)) {
        spec.func_name_ = value;
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_MODEL_PREFIX)) {
        spec.model_name_ = value;
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_PROMPT_PREFIX)) {
        spec.prompt_expr_ = value;
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_EXPR_PREFIX)) {
        spec.full_expr_ = value;
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_SYNC_PREFIX)) {
        spec.sync_mode_ = value;
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_RESULT_TYPE_PREFIX)) {
        spec.result_type_ = value;
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_RETRY_PREFIX)) {
        spec.max_retries_ = atoi(value.ptr());
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_DEP_PREFIX)) {
        if (OB_FAIL(spec.deps_.push_back(value))) {
          SHARE_SCHEMA_LOG(WARN, "failed to push ai column dependency", K(ret), K(value));
        }
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_STATE_COL_PREFIX)) {
        spec.state_column_id_ = atoi(value.ptr());
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_REQUEST_COL_PREFIX)) {
        spec.request_column_id_ = atoi(value.ptr());
      } else if (entry.prefix_match(OB_AI_TYPE_INFO_SPEC_COL_PREFIX)) {
        spec.spec_column_id_ = atoi(value.ptr());
      }
    }
  }
  return ret;
}

// Append the spec entries to the column's extended type info. Existing entries
// (e.g. the VECTOR(N) marker of a vector column) are kept untouched.
inline int add_ai_column_spec_to_type_info(const ObAiColumnSpec &spec,
                                           common::ObIAllocator &allocator,
                                           common::ObIArray<common::ObString> &type_info)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(add_ai_type_info_entry(OB_AI_TYPE_INFO_FUNC_PREFIX, spec.func_name_, allocator, type_info))) {
  } else if (OB_FAIL(add_ai_type_info_entry(OB_AI_TYPE_INFO_MODEL_PREFIX, spec.model_name_, allocator, type_info))) {
  } else if (OB_FAIL(add_ai_type_info_entry(OB_AI_TYPE_INFO_PROMPT_PREFIX, spec.prompt_expr_, allocator, type_info))) {
  } else if (OB_FAIL(add_ai_type_info_entry(OB_AI_TYPE_INFO_EXPR_PREFIX, spec.full_expr_, allocator, type_info))) {
  } else if (OB_FAIL(add_ai_type_info_entry(OB_AI_TYPE_INFO_SYNC_PREFIX, spec.sync_mode_, allocator, type_info))) {
  } else if (OB_FAIL(add_ai_type_info_entry(OB_AI_TYPE_INFO_RESULT_TYPE_PREFIX, spec.result_type_, allocator, type_info))) {
  } else if (OB_FAIL(add_ai_type_info_int_entry(OB_AI_TYPE_INFO_RETRY_PREFIX, spec.max_retries_, allocator, type_info))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < spec.deps_.count(); ++i) {
      if (OB_FAIL(add_ai_type_info_entry(OB_AI_TYPE_INFO_DEP_PREFIX, spec.deps_.at(i), allocator, type_info))) {
      }
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_UTIL_H_
