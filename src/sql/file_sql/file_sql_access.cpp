/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#define USING_LOG_PREFIX SQL

#include "sql/file_sql/file_sql_access.h"

#include <climits>
#include <filesystem>
#include <new>

#include "lib/ob_errno.h"
#include "local_bridge.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_basic_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace
{

int map_result(const local::FileScanResult &result, std::string &json,
               bool *end, std::string &error)
{
  int ret = OB_SUCCESS;
  if (end != nullptr) {
    *end = false;
  }
  if (result.code == 0) {
    json.assign(result.json.data(), result.json.size());
  } else if (result.code == 1 && end != nullptr) {
    *end = true;
  } else {
    error.assign(result.message.data(), result.message.size());
    ret = result.code == 2 ? OB_INVALID_ARGUMENT : OB_IO_ERROR;
    LOG_WARN("file SQL scan failed", K(ret), "error", error.c_str(), "scan_code", result.code);
    if (ret == OB_INVALID_ARGUMENT) {
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, error.c_str());
    }
  }
  return ret;
}

} // namespace

class FileSqlCursor::Impl
{
public:
  explicit Impl(rust::Box<local::FileScan> scan) : scan_(std::move(scan)) {}
  rust::Box<local::FileScan> scan_;
};

FileSqlCursor::FileSqlCursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FileSqlCursor::~FileSqlCursor() = default;

int FileSqlCursor::next(const int64_t max_rows, std::string &json,
                        bool &end, std::string &error)
{
  int ret = OB_SUCCESS;
  if (impl_ == nullptr || max_rows <= 0 || max_rows > UINT32_MAX) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ret = map_result(local::file_scan_next(*impl_->scan_, static_cast<uint32_t>(max_rows)),
                     json, &end, error);
  }
  return ret;
}

int FileSqlCursor::rewind(std::string &error)
{
  int ret = OB_SUCCESS;
  std::string ignored;
  if (impl_ == nullptr) {
    ret = OB_NOT_INIT;
  } else {
    ret = map_result(local::file_scan_rewind(*impl_->scan_), ignored, nullptr, error);
  }
  return ret;
}

int infer_file_sql_schema(const std::string &descriptor,
                          std::string &schema,
                          std::string &error)
{
  return map_result(local::infer_file_schema(descriptor), schema, nullptr, error);
}

int open_file_sql_cursor(const std::string &descriptor,
                         FileSqlCursor *&cursor,
                         std::string &error)
{
  int ret = OB_SUCCESS;
  cursor = nullptr;
  rust::Box<local::FileScan> scan = local::open_file_scan(descriptor);
  const local::FileScanResult status = local::file_scan_status(*scan);
  std::string ignored;
  if (OB_FAIL(map_result(status, ignored, nullptr, error))) {
  } else {
    std::unique_ptr<FileSqlCursor::Impl> impl(
        new (std::nothrow) FileSqlCursor::Impl(std::move(scan)));
    if (impl == nullptr) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(cursor = new (std::nothrow) FileSqlCursor(std::move(impl)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  return ret;
}

int check_file_sql_path(const ObBasicSessionInfo &session,
                        const std::string &path,
                        const bool check_parent)
{
  int ret = OB_SUCCESS;
  std::filesystem::path candidate = std::filesystem::u8path(path);
  if (check_parent) {
    candidate = candidate.has_parent_path() ? candidate.parent_path() : std::filesystem::path(".");
  }
  std::error_code error;
  const std::filesystem::path canonical = std::filesystem::canonical(candidate, error);
  if (error) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    const std::string canonical_text = canonical.string();
    common::ObString secure_file_priv;
    if (OB_FAIL(session.get_secure_file_priv(secure_file_priv))) {
    } else if (OB_FAIL(ObResolverUtils::check_secure_path(
                 secure_file_priv, common::ObString(canonical_text.size(), canonical_text.data())))) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
