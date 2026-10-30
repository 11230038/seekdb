/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#include <gtest/gtest.h>

#include "lib/allocator/ob_arena_allocator.h"
#include "lib/ob_errno.h"
#include "sql/parser/ob_parser.h"

namespace oceanbase
{
namespace sql
{
namespace
{

void expect_parse_success(const char *sql)
{
  common::ObArenaAllocator allocator("FileSqlParser");
  ObParser parser(allocator, DEFAULT_MYSQL_MODE);
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parser.parse(common::ObString::make_string(sql), result)) << sql;
  ASSERT_NE(nullptr, result.result_tree_) << sql;
}

TEST(FileSqlParser, ReadsAndAliases)
{
  expect_parse_success(
      "select f.id from read_csv('/tmp/a.csv', header = true, sample_size = -1) as f");
  expect_parse_success(
      "select * from read_json('/tmp/a.jsonl', format = 'newline_delimited') j");
  expect_parse_success("select * from read_ndjson('/tmp/a.jsonl')");
  expect_parse_success("select * from read_parquet('/tmp/a.parquet') p");
  expect_parse_success("select file from glob('/tmp/*.csv') order by file");
}

TEST(FileSqlParser, DescribeAndJoin)
{
  expect_parse_success("describe select * from read_csv('/tmp/a.csv')");
  expect_parse_success(
      "select c.id from read_csv('/tmp/a.csv') c "
      "join read_json('/tmp/b.jsonl') j on c.id = j.id");
}

TEST(FileSqlParser, CopyTableOrQuery)
{
  expect_parse_success(
      "copy (select id, name from read_csv('/tmp/a.csv')) "
      "to '/tmp/out.csv' "
      "(format csv, header, delimiter ';', quote '\"', escape '\"', nullstr 'NULL')");
  expect_parse_success("copy source_table to '/tmp/out.csv' (format csv, header)");
}

} // namespace
} // namespace sql
} // namespace oceanbase
