/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <memory>
#include <string>

#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{
class ObBasicSessionInfo;

// Owns the Rust streaming cursor without exposing cxx bridge types to SQL headers.
class FileSqlCursor
{
public:
  ~FileSqlCursor();
  int next(int64_t max_rows, std::string &json, bool &end, std::string &error);
  int rewind(std::string &error);

private:
  class Impl;
  explicit FileSqlCursor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend int open_file_sql_cursor(const std::string &, FileSqlCursor *&, std::string &);
};

int infer_file_sql_schema(const std::string &descriptor,
                          std::string &schema,
                          std::string &error);
int open_file_sql_cursor(const std::string &descriptor,
                         FileSqlCursor *&cursor,
                         std::string &error);
int check_file_sql_path(const ObBasicSessionInfo &session,
                        const std::string &path,
                        bool check_parent);

} // namespace sql
} // namespace oceanbase
