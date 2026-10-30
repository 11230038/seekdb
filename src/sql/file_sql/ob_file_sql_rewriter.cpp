/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#define USING_LOG_PREFIX SQL

#include "sql/file_sql/ob_file_sql_rewriter.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include "sql/file_sql/file_sql_access.h"
#include "sql/session/ob_basic_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace
{

struct FileColumn
{
  std::string name_;
  std::string type_;
  bool nullable_ = true;
};

struct FileCall
{
  size_t begin_ = 0;
  size_t end_ = 0;
  std::string name_;
  std::string path_;
  std::map<std::string, std::string> options_;
};

constexpr size_t MAX_GLOB_FILES = 100000;
constexpr size_t MAX_GLOB_BYTES = 64 * 1024 * 1024;

bool ascii_iequal(const char left, const char right)
{
  return std::tolower(static_cast<unsigned char>(left)) ==
         std::tolower(static_cast<unsigned char>(right));
}

std::string lower_ascii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string trim_copy(const std::string &value)
{
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) { ++begin; }
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) { --end; }
  return value.substr(begin, end - begin);
}

bool starts_with_word_ci(const std::string &text, const size_t position, const char *word)
{
  size_t offset = 0;
  while (word[offset] != '\0' && position + offset < text.size() &&
         ascii_iequal(text[position + offset], word[offset])) {
    ++offset;
  }
  return word[offset] == '\0' &&
         (position + offset == text.size() ||
          !std::isalnum(static_cast<unsigned char>(text[position + offset])));
}

bool is_identifier_char(const char value)
{
  return std::isalnum(static_cast<unsigned char>(value)) || value == '_' || value == '$';
}

bool is_file_function(const std::string &name)
{
  const std::string lower = lower_ascii(name);
  return lower == "read_csv" || lower == "read_json" || lower == "read_ndjson" ||
         lower == "read_parquet" || lower == "glob";
}

size_t skip_sql_comment(const std::string &text, size_t position);

bool has_table_alias(const std::string &sql, size_t position)
{
  while (position < sql.size()) {
    if (std::isspace(static_cast<unsigned char>(sql[position]))) {
      ++position;
    } else if (const size_t next = skip_sql_comment(sql, position); next != position) {
      position = next;
    } else {
      break;
    }
  }
  if (position >= sql.size() || sql[position] == ',' || sql[position] == ')' ||
      sql[position] == ';') {
    return false;
  }
  if (sql[position] == '`' || sql[position] == '"') { return true; }
  if (!std::isalpha(static_cast<unsigned char>(sql[position])) && sql[position] != '_' &&
      sql[position] != '$') {
    return false;
  }
  const size_t begin = position;
  while (position < sql.size() && is_identifier_char(sql[position])) { ++position; }
  const std::string word = lower_ascii(sql.substr(begin, position - begin));
  if (word == "as") { return true; }
  static const std::set<std::string> CLAUSES = {
      "where", "join", "inner", "left", "right", "full", "cross", "natural", "on",
      "order", "group", "having", "limit", "union", "intersect", "except", "minus",
      "into", "for", "window", "qualify", "partition", "sample"};
  return CLAUSES.count(word) == 0;
}

size_t skip_sql_comment(const std::string &text, const size_t position)
{
  if (position >= text.size()) { return position; }
  const bool dash_comment = text[position] == '-' && position + 1 < text.size() &&
                            text[position + 1] == '-' &&
                            (position + 2 == text.size() ||
                             std::isspace(static_cast<unsigned char>(text[position + 2])));
  if (text[position] == '#' || dash_comment) {
    const size_t newline = text.find('\n', position + 1);
    return newline == std::string::npos ? text.size() : newline + 1;
  }
  if (text[position] == '/' && position + 1 < text.size() && text[position + 1] == '*') {
    const size_t close = text.find("*/", position + 2);
    return close == std::string::npos ? text.size() : close + 2;
  }
  return position;
}

size_t matching_parenthesis(const std::string &text, const size_t open)
{
  int depth = 0;
  char quote = 0;
  for (size_t index = open; index < text.size(); ++index) {
    const char value = text[index];
    if (quote != 0) {
      if (value == '\\' && index + 1 < text.size()) {
        ++index;
      } else if (value == quote) {
        if (index + 1 < text.size() && text[index + 1] == quote) { ++index; }
        else { quote = 0; }
      }
    } else if (value == '\'' || value == '"' || value == '`') {
      quote = value;
    } else if (const size_t next = skip_sql_comment(text, index); next != index) {
      index = next - 1;
    } else if (value == '(') {
      ++depth;
    } else if (value == ')' && --depth == 0) {
      return index;
    }
  }
  return std::string::npos;
}

std::vector<std::string> split_arguments(const std::string &text)
{
  std::vector<std::string> result;
  size_t begin = 0;
  int depth = 0;
  char quote = 0;
  for (size_t index = 0; index < text.size(); ++index) {
    const char value = text[index];
    if (quote != 0) {
      if (value == '\\' && index + 1 < text.size()) { ++index; }
      else if (value == quote) {
        if (index + 1 < text.size() && text[index + 1] == quote) { ++index; }
        else { quote = 0; }
      }
    } else if (value == '\'' || value == '"') {
      quote = value;
    } else if (const size_t next = skip_sql_comment(text, index); next != index) {
      index = next - 1;
    } else if (value == '(') {
      ++depth;
    } else if (value == ')') {
      --depth;
    } else if (value == ',' && depth == 0) {
      result.push_back(trim_copy(text.substr(begin, index - begin)));
      begin = index + 1;
    }
  }
  result.push_back(trim_copy(text.substr(begin)));
  return result;
}

size_t top_level_equal(const std::string &text)
{
  char quote = 0;
  for (size_t index = 0; index < text.size(); ++index) {
    const char value = text[index];
    if (quote != 0) {
      if (value == '\\' && index + 1 < text.size()) { ++index; }
      else if (value == quote) { quote = 0; }
    } else if (value == '\'' || value == '"') {
      quote = value;
    } else if (const size_t next = skip_sql_comment(text, index); next != index) {
      index = next - 1;
    } else if (value == '=') {
      return index;
    }
  }
  return std::string::npos;
}

bool decode_sql_string(const std::string &literal, std::string &result)
{
  const std::string value = trim_copy(literal);
  if (value.size() < 2 || (value.front() != '\'' && value.front() != '"') ||
      value.back() != value.front()) {
    return false;
  }
  result.clear();
  const char quote = value.front();
  for (size_t index = 1; index + 1 < value.size(); ++index) {
    const char current = value[index];
    if (current == quote && index + 1 < value.size() - 1 && value[index + 1] == quote) {
      result.push_back(quote);
      ++index;
    } else if (current == '\\' && index + 1 < value.size() - 1) {
      const char escaped = value[++index];
      switch (escaped) {
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case '0': result.push_back('\0'); break;
        default: result.push_back(escaped); break;
      }
    } else {
      result.push_back(current);
    }
  }
  return true;
}

int parse_call(const std::string &sql, const size_t begin, FileCall &call)
{
  int ret = OB_SUCCESS;
  size_t name_end = begin;
  while (name_end < sql.size() && is_identifier_char(sql[name_end])) { ++name_end; }
  size_t open = name_end;
  while (open < sql.size() && std::isspace(static_cast<unsigned char>(sql[open]))) { ++open; }
  const size_t close = open < sql.size() && sql[open] == '(' ? matching_parenthesis(sql, open)
                                                              : std::string::npos;
  if (close == std::string::npos) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    call.begin_ = begin;
    call.end_ = close + 1;
    call.name_ = lower_ascii(sql.substr(begin, name_end - begin));
    const std::vector<std::string> arguments =
        split_arguments(sql.substr(open + 1, close - open - 1));
    if (arguments.empty() || !decode_sql_string(arguments.front(), call.path_)) {
      ret = OB_ERR_PARSE_SQL;
    }
    for (size_t index = 1; OB_SUCC(ret) && index < arguments.size(); ++index) {
      const size_t equal = top_level_equal(arguments[index]);
      if (equal == std::string::npos) {
        ret = OB_ERR_PARSE_SQL;
      } else {
        const std::string name = lower_ascii(trim_copy(arguments[index].substr(0, equal)));
        if (name.empty() || call.options_.count(name) != 0) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          call.options_.emplace(name, trim_copy(arguments[index].substr(equal + 1)));
        }
      }
    }
  }
  return ret;
}

int find_calls(const std::string &sql, std::vector<FileCall> &calls)
{
  int ret = OB_SUCCESS;
  char quote = 0;
  for (size_t index = 0; OB_SUCC(ret) && index < sql.size();) {
    const char value = sql[index];
    if (quote != 0) {
      if (value == '\\' && index + 1 < sql.size()) { index += 2; }
      else if (value == quote) {
        if (index + 1 < sql.size() && sql[index + 1] == quote) { index += 2; }
        else { quote = 0; ++index; }
      } else { ++index; }
    } else if (value == '\'' || value == '"' || value == '`') {
      quote = value;
      ++index;
    } else if (const size_t next = skip_sql_comment(sql, index); next != index) {
      index = next;
    } else if (std::isalpha(static_cast<unsigned char>(value)) || value == '_') {
      const size_t begin = index;
      while (index < sql.size() && is_identifier_char(sql[index])) { ++index; }
      const std::string name = sql.substr(begin, index - begin);
      size_t open = index;
      while (open < sql.size() && std::isspace(static_cast<unsigned char>(sql[open]))) { ++open; }
      size_t previous = begin;
      while (previous > 0 && std::isspace(static_cast<unsigned char>(sql[previous - 1]))) {
        --previous;
      }
      const bool qualified = previous > 0 && sql[previous - 1] == '.';
      if (!qualified && is_file_function(name) && open < sql.size() && sql[open] == '(') {
        FileCall call;
        if (OB_FAIL(parse_call(sql, begin, call))) {
        } else {
          calls.push_back(call);
          index = call.end_;
        }
      }
    } else {
      ++index;
    }
  }
  return ret;
}

bool parse_bool(const std::string &text, bool &result)
{
  const std::string value = lower_ascii(trim_copy(text));
  if (value == "true" || value == "on" || value == "1") { result = true; return true; }
  if (value == "false" || value == "off" || value == "0") { result = false; return true; }
  return false;
}

std::string canonical_path(const std::string &path, std::error_code &error)
{
  const std::filesystem::path value = std::filesystem::canonical(std::filesystem::u8path(path), error);
  if (error) { return std::string(); }
  const auto utf8 = value.u8string();
  std::string result(reinterpret_cast<const char *>(utf8.data()), utf8.size());
  std::replace(result.begin(), result.end(), '\\', '/');
  return result;
}

void add_json_string(rapidjson::Document &document, const char *name, const std::string &value)
{
  rapidjson::Value json_value;
  json_value.SetString(value.data(), static_cast<rapidjson::SizeType>(value.size()), document.GetAllocator());
  document.AddMember(rapidjson::StringRef(name), json_value, document.GetAllocator());
}

int string_option(const std::map<std::string, std::string> &options,
                  const std::string &name, std::string &value, bool &found)
{
  int ret = OB_SUCCESS;
  const auto option = options.find(name);
  found = option != options.end();
  if (found && !decode_sql_string(option->second, value)) { ret = OB_ERR_PARSE_SQL; }
  return ret;
}

int make_scan_descriptor(const FileCall &call, rapidjson::Document &descriptor)
{
  int ret = OB_SUCCESS;
  descriptor.SetObject();
  descriptor.AddMember("__seekdb_file_sql_v1", true, descriptor.GetAllocator());
  std::error_code path_error;
  const std::string path = canonical_path(call.path_, path_error);
  if (path_error) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    add_json_string(descriptor, "path", path);
    const std::string format = call.name_ == "read_csv" ? "csv" :
                               call.name_ == "read_parquet" ? "parquet" :
                               call.name_ == "read_ndjson" ? "ndjson" : "json";
    add_json_string(descriptor, "format", format);
  }

  std::set<std::string> used;
  if (OB_SUCC(ret) && call.name_ == "read_csv") {
    bool header = true;
    const auto header_option = call.options_.find("header");
    if (header_option != call.options_.end()) {
      used.insert("header");
      if (!parse_bool(header_option->second, header)) { ret = OB_ERR_PARSE_SQL; }
    }
    descriptor.AddMember("header", header, descriptor.GetAllocator());
    const std::pair<const char *, const char *> string_options[] = {
      {"delimiter", "delimiter"}, {"delim", "delimiter"}, {"quote", "quote"},
      {"escape", "escape"}, {"nullstr", "null_text"}, {"null_string", "null_text"}
    };
    std::set<std::string> targets;
    for (const auto &mapping : string_options) {
      std::string value;
      bool found = false;
      if (OB_SUCC(ret) && OB_FAIL(string_option(call.options_, mapping.first, value, found))) {
      } else if (found) {
        if (!targets.insert(mapping.second).second) { ret = OB_ERR_PARSE_SQL; }
        else { used.insert(mapping.first); add_json_string(descriptor, mapping.second, value); }
      }
    }
  } else if (OB_SUCC(ret) && (call.name_ == "read_json" || call.name_ == "read_ndjson")) {
    bool json_array = false;
    if (call.name_ == "read_json") {
      std::ifstream input(std::filesystem::u8path(call.path_), std::ios::binary);
      char value_char = 0;
      while (input.get(value_char) && std::isspace(static_cast<unsigned char>(value_char))) {}
      json_array = value_char == '[';
    }
    const auto format = call.options_.find("format");
    if (format != call.options_.end()) {
      used.insert("format");
      std::string value;
      if (!decode_sql_string(format->second, value)) { ret = OB_ERR_PARSE_SQL; }
      else if (lower_ascii(value) == "newline_delimited" || lower_ascii(value) == "ndjson") { json_array = false; }
      else if (lower_ascii(value) == "array") { json_array = true; }
      else if (lower_ascii(value) == "auto") {
        std::ifstream input(std::filesystem::u8path(call.path_), std::ios::binary);
        char value_char = 0;
        while (input.get(value_char) && std::isspace(static_cast<unsigned char>(value_char))) {}
        json_array = value_char == '[';
      } else { ret = OB_NOT_SUPPORTED; }
    }
    descriptor.AddMember("json_array", json_array, descriptor.GetAllocator());
  }

  const auto sample_size = call.options_.find("sample_size");
  if (OB_SUCC(ret) && sample_size != call.options_.end()) {
    used.insert("sample_size");
    if (trim_copy(sample_size->second) != "-1") { ret = OB_NOT_SUPPORTED; }
  }
  if (OB_SUCC(ret)) {
    for (const auto &option : call.options_) {
      if (used.count(option.first) == 0) { ret = OB_NOT_SUPPORTED; break; }
    }
  }
  return ret;
}

std::string serialize_json(const rapidjson::Value &value)
{
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

int infer_call(const FileCall &call, const ObBasicSessionInfo &session,
               std::string &descriptor_text, std::vector<FileColumn> &columns)
{
  int ret = OB_SUCCESS;
  rapidjson::Document descriptor;
  if (OB_FAIL(make_scan_descriptor(call, descriptor))) {
  } else if (!descriptor.HasMember("path") || !descriptor["path"].IsString()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const std::string canonical(descriptor["path"].GetString(),
                                descriptor["path"].GetStringLength());
    if (OB_FAIL(check_file_sql_path(session, canonical, false))) {
    } else {
      std::string schema_text;
      std::string error;
      if (OB_FAIL(infer_file_sql_schema(serialize_json(descriptor), schema_text, error))) {
        LOG_WARN("failed to infer file SQL schema", K(ret), "error", error.c_str(), "path", call.path_.c_str());
      } else {
        rapidjson::Document schema;
        schema.Parse(schema_text.data(), schema_text.size());
        if (schema.HasParseError() || !schema.IsObject() || !schema.HasMember("identity") ||
            !schema.HasMember("columns") || !schema["columns"].IsArray()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          rapidjson::Value identity;
          identity.CopyFrom(schema["identity"], descriptor.GetAllocator());
          descriptor.AddMember("identity", identity, descriptor.GetAllocator());
          rapidjson::Value json_columns;
          json_columns.CopyFrom(schema["columns"], descriptor.GetAllocator());
          descriptor.AddMember("columns", json_columns, descriptor.GetAllocator());
          for (const auto &item : schema["columns"].GetArray()) {
            if (!item.IsObject() || !item.HasMember("name") || !item["name"].IsString() ||
                !item.HasMember("type") || !item["type"].IsString()) {
              ret = OB_ERR_UNEXPECTED;
              break;
            }
            FileColumn column;
            column.name_.assign(item["name"].GetString(), item["name"].GetStringLength());
            column.type_.assign(item["type"].GetString(), item["type"].GetStringLength());
            column.nullable_ = item.HasMember("nullable") && item["nullable"].IsBool()
                                 ? item["nullable"].GetBool() : true;
            columns.push_back(column);
          }
          if (OB_SUCC(ret) && columns.empty()) { ret = OB_INVALID_ARGUMENT; }
          if (OB_SUCC(ret)) { descriptor_text = serialize_json(descriptor); }
        }
      }
    }
  }
  return ret;
}

std::string quote_identifier(const std::string &name)
{
  std::string result("`");
  for (const char value : name) { result += value == '`' ? "``" : std::string(1, value); }
  result.push_back('`');
  return result;
}

std::string quote_sql_string(const std::string &value)
{
  std::string result("'");
  for (const char current : value) {
    if (current == '\\') { result += "\\\\"; }
    else if (current == '\'') { result += "''"; }
    else if (current == '\0') { result += "\\0"; }
    else { result.push_back(current); }
  }
  result.push_back('\'');
  return result;
}

std::string json_path(const std::string &name)
{
  std::string result("$.\"");
  for (const char value : name) {
    if (value == '\\' || value == '"') { result.push_back('\\'); }
    result.push_back(value);
  }
  result += "\"";
  return result;
}

std::string sql_type(const std::string &type)
{
  return type == "VARCHAR" || type == "NULL" ? "VARCHAR(65535)" : type;
}

std::string make_file_table(const std::string &descriptor, const std::vector<FileColumn> &columns)
{
  std::string result = "JSON_TABLE(" + quote_sql_string(descriptor) + ", '$[*]' COLUMNS (";
  for (size_t index = 0; index < columns.size(); ++index) {
    if (index != 0) { result += ", "; }
    result += quote_identifier(columns[index].name_) + " " + sql_type(columns[index].type_) +
              " PATH " + quote_sql_string(json_path(columns[index].name_)) +
              " NULL ON EMPTY NULL ON ERROR";
  }
  result += "))";
  return result;
}

std::string make_describe_query(const std::vector<FileColumn> &columns)
{
  std::string result;
  for (size_t index = 0; index < columns.size(); ++index) {
    if (index != 0) { result += " UNION ALL "; }
    result += "SELECT " + quote_sql_string(columns[index].name_) + " AS column_name, " +
              quote_sql_string(columns[index].type_ == "NULL" ? "VARCHAR" : columns[index].type_) +
              " AS column_type, " + quote_sql_string(columns[index].nullable_ ? "YES" : "NO") +
              " AS `null`, NULL AS `key`, NULL AS `default`, NULL AS extra";
  }
  return result;
}

bool wildcard_match(const char *pattern, const char *text)
{
  const char *star = nullptr;
  const char *retry = nullptr;
  while (*text != '\0') {
    if (*pattern == '?' || *pattern == *text) { ++pattern; ++text; }
    else if (*pattern == '*') { star = pattern++; retry = text; }
    else if (star != nullptr) { pattern = star + 1; text = ++retry; }
    else { return false; }
  }
  while (*pattern == '*') { ++pattern; }
  return *pattern == '\0';
}

int make_glob_table(const FileCall &call, const ObBasicSessionInfo &session,
                    std::string &table, std::vector<FileColumn> &columns)
{
  int ret = OB_SUCCESS;
  if (!call.options_.empty()) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(check_file_sql_path(session, call.path_, true))) {
  } else {
    const std::filesystem::path pattern_path = std::filesystem::u8path(call.path_);
    const std::filesystem::path requested_directory = pattern_path.has_parent_path()
                                                        ? pattern_path.parent_path()
                                                        : std::filesystem::path(".");
    std::error_code directory_error;
    const std::string canonical_directory = canonical_path(requested_directory.string(), directory_error);
    const auto filename_utf8 = pattern_path.filename().u8string();
    const std::string pattern(reinterpret_cast<const char *>(filename_utf8.data()), filename_utf8.size());
    std::error_code error;
    std::vector<std::string> matches;
    size_t match_bytes = 0;
    if (directory_error) {
      ret = OB_FILE_NOT_EXIST;
    } else if (OB_FAIL(check_file_sql_path(session, canonical_directory, false))) {
    }
    for (const auto &entry : OB_SUCC(ret)
                              ? std::filesystem::directory_iterator(
                                  std::filesystem::u8path(canonical_directory), error)
                              : std::filesystem::directory_iterator()) {
      const auto name_utf8 = entry.path().filename().u8string();
      const std::string name(reinterpret_cast<const char *>(name_utf8.data()), name_utf8.size());
      if (wildcard_match(pattern.c_str(), name.c_str()) && entry.is_regular_file()) {
        const auto path_utf8 = entry.path().u8string();
        std::string path(reinterpret_cast<const char *>(path_utf8.data()), path_utf8.size());
        std::error_code canonical_error;
        std::string canonical = canonical_path(path, canonical_error);
        if (canonical_error) { ret = OB_FILE_NOT_EXIST; break; }
        if (OB_FAIL(check_file_sql_path(session, canonical, false))) { break; }
        match_bytes += canonical.size();
        if (matches.size() >= MAX_GLOB_FILES || match_bytes > MAX_GLOB_BYTES) {
          ret = OB_SIZE_OVERFLOW;
          break;
        }
        matches.push_back(std::move(canonical));
      }
    }
    if (OB_SUCC(ret) && error) { ret = OB_IO_ERROR; }
    if (OB_SUCC(ret)) {
      std::sort(matches.begin(), matches.end());
      rapidjson::Document rows;
      rows.SetArray();
      for (const std::string &match : matches) {
        rapidjson::Value row(rapidjson::kObjectType);
        rapidjson::Value value;
        value.SetString(match.data(), static_cast<rapidjson::SizeType>(match.size()), rows.GetAllocator());
        row.AddMember("file", value, rows.GetAllocator());
        rows.PushBack(row, rows.GetAllocator());
      }
      FileColumn column;
      column.name_ = "file";
      column.type_ = "VARCHAR";
      column.nullable_ = false;
      columns.push_back(column);
      table = "JSON_TABLE(" + quote_sql_string(serialize_json(rows)) +
              ", '$[*]' COLUMNS (`file` VARCHAR(65535) PATH '$.\"file\"' NULL ON EMPTY NULL ON ERROR))";
    }
  }
  return ret;
}

struct CopyOptions
{
  bool header_ = false;
  std::string delimiter_ = ",";
  std::string quote_ = "\"";
  std::string escape_ = "\"";
  std::string null_text_ = "NULL";
};

void skip_copy_separators(const std::string &text, size_t &position)
{
  while (position < text.size() &&
         (std::isspace(static_cast<unsigned char>(text[position])) ||
          text[position] == '(' || text[position] == ')' || text[position] == ',')) {
    ++position;
  }
}

int parse_copy_value(const std::string &text, size_t &position, std::string &value)
{
  int ret = OB_SUCCESS;
  while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) { ++position; }
  if (position < text.size() && text[position] == '=') {
    ++position;
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) { ++position; }
  }
  if (position >= text.size() || (text[position] != '\'' && text[position] != '"')) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    const size_t begin = position;
    const char quote = text[position++];
    bool closed = false;
    while (position < text.size() && !closed) {
      if (text[position] == quote) {
        if (position + 1 < text.size() && text[position + 1] == quote) { position += 2; }
        else { ++position; closed = true; }
      } else if (text[position] == '\\' && position + 1 < text.size()) {
        position += 2;
      } else {
        ++position;
      }
    }
    if (!closed || !decode_sql_string(text.substr(begin, position - begin), value)) {
      ret = OB_ERR_PARSE_SQL;
    }
  }
  return ret;
}

int parse_copy_options(const std::string &text, CopyOptions &options)
{
  int ret = OB_SUCCESS;
  bool has_format = false;
  std::set<std::string> seen;
  size_t position = 0;
  while (OB_SUCC(ret)) {
    skip_copy_separators(text, position);
    if (position >= text.size() || text[position] == ';') { break; }
    const size_t begin = position;
    while (position < text.size() && is_identifier_char(text[position])) { ++position; }
    if (begin == position) {
      ret = OB_ERR_PARSE_SQL;
      break;
    }
    const std::string name = lower_ascii(text.substr(begin, position - begin));
    if (!seen.insert(name).second) {
      ret = OB_ERR_PARSE_SQL;
    } else if (name == "format") {
      while (position < text.size() &&
             (std::isspace(static_cast<unsigned char>(text[position])) || text[position] == '=')) {
        ++position;
      }
      std::string format;
      if (position < text.size() && (text[position] == '\'' || text[position] == '"')) {
        if (OB_FAIL(parse_copy_value(text, position, format))) {
        }
      } else {
        const size_t format_begin = position;
        while (position < text.size() && is_identifier_char(text[position])) { ++position; }
        format = text.substr(format_begin, position - format_begin);
      }
      if (OB_SUCC(ret) && lower_ascii(format) != "csv") { ret = OB_NOT_SUPPORTED; }
      has_format = OB_SUCC(ret);
    } else if (name == "header") {
      options.header_ = true;
      size_t value_position = position;
      while (value_position < text.size() &&
             std::isspace(static_cast<unsigned char>(text[value_position]))) { ++value_position; }
      if (value_position < text.size() && text[value_position] == '=') {
        position = value_position + 1;
        while (position < text.size() &&
               std::isspace(static_cast<unsigned char>(text[position]))) { ++position; }
        const size_t token_begin = position;
        while (position < text.size() && is_identifier_char(text[position])) { ++position; }
        if (token_begin == position ||
            !parse_bool(text.substr(token_begin, position - token_begin), options.header_)) {
          ret = OB_ERR_PARSE_SQL;
        }
      } else {
        size_t token_end = value_position;
        while (token_end < text.size() && is_identifier_char(text[token_end])) { ++token_end; }
        bool explicit_header = false;
        if (token_end > value_position &&
            parse_bool(text.substr(value_position, token_end - value_position), explicit_header)) {
          options.header_ = explicit_header;
          position = token_end;
        }
      }
    } else if (name == "delimiter") {
      ret = parse_copy_value(text, position, options.delimiter_);
    } else if (name == "quote") {
      ret = parse_copy_value(text, position, options.quote_);
    } else if (name == "escape") {
      ret = parse_copy_value(text, position, options.escape_);
    } else if (name == "nullstr" || name == "null_string") {
      ret = parse_copy_value(text, position, options.null_text_);
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  if (OB_SUCC(ret) && !has_format) { ret = OB_NOT_SUPPORTED; }
  return ret;
}

int rewrite_copy(std::string &sql, bool &rewritten)
{
  int ret = OB_SUCCESS;
  rewritten = false;
  const std::string input = trim_copy(sql);
  if (!starts_with_word_ci(input, 0, "copy")) { return ret; }
  size_t position = 4;
  while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position]))) { ++position; }
  std::string query;
  if (position < input.size() && input[position] == '(') {
    const size_t close = matching_parenthesis(input, position);
    if (close == std::string::npos) { return OB_ERR_PARSE_SQL; }
    query = input.substr(position + 1, close - position - 1);
    position = close + 1;
  } else {
    const std::string lower = lower_ascii(input);
    const size_t to = lower.find(" to ", position);
    if (to == std::string::npos) { return OB_ERR_PARSE_SQL; }
    const std::string source = trim_copy(input.substr(position, to - position));
    query = starts_with_word_ci(source, 0, "select") ? source : "SELECT * FROM " + source;
    position = to + 1;
  }
  while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position]))) { ++position; }
  if (!starts_with_word_ci(input, position, "to")) { return OB_ERR_PARSE_SQL; }
  position += 2;
  while (position < input.size() && std::isspace(static_cast<unsigned char>(input[position]))) { ++position; }
  if (position >= input.size() || (input[position] != '\'' && input[position] != '"')) { return OB_ERR_PARSE_SQL; }
  const size_t path_begin = position;
  const char quote = input[position++];
  while (position < input.size()) {
    if (input[position] == quote) {
      if (position + 1 < input.size() && input[position + 1] == quote) { position += 2; }
      else { ++position; break; }
    } else if (input[position] == '\\' && position + 1 < input.size()) { position += 2; }
    else { ++position; }
  }
  if (position > input.size() || input[position - 1] != quote) { return OB_ERR_PARSE_SQL; }
  const std::string path_literal = input.substr(path_begin, position - path_begin);
  CopyOptions options;
  if (OB_FAIL(parse_copy_options(input.substr(position), options))) {
  }
  if (OB_FAIL(ret)) { return ret; }
  if (options.delimiter_.size() != 1 || options.quote_.size() != 1 ||
      options.escape_.size() != 1) {
    return OB_INVALID_ARGUMENT;
  }
  sql = query + " INTO OUTFILE " + path_literal +
        " FORMAT=(TYPE='CSV', PARSE_HEADER=" + std::string(options.header_ ? "TRUE" : "FALSE") +
        ", FIELD_DELIMITER=" + quote_sql_string(options.delimiter_) +
        ", FIELD_OPTIONALLY_ENCLOSED_BY=" + quote_sql_string(options.quote_) +
        ", ESCAPE=" + quote_sql_string(options.escape_) +
        ", NULL_IF=(" + quote_sql_string(options.null_text_) + "))";
  rewritten = true;
  return ret;
}

} // namespace

int FileSqlRewriter::rewrite(const common::ObString &sql,
                             const ObBasicSessionInfo &session,
                             std::string &rewritten_sql,
                             bool &rewritten)
{
  int ret = OB_SUCCESS;
  rewritten = false;
  rewritten_sql.assign(sql.ptr(), sql.length());
  bool copy_rewritten = false;
  if (OB_FAIL(rewrite_copy(rewritten_sql, copy_rewritten))) {
  } else {
    std::vector<FileCall> calls;
    if (OB_FAIL(find_calls(rewritten_sql, calls))) {
    } else if (!calls.empty() || copy_rewritten) {
      int64_t enabled = 0;
      const common::ObString variable_name("enable_file_sql");
      if (OB_FAIL(session.get_sys_variable_by_name(variable_name, enabled))) {
      } else if (enabled == 0) {
        ret = OB_NOT_SUPPORTED;
      } else if (!calls.empty() &&
                 (starts_with_word_ci(trim_copy(rewritten_sql), 0, "describe") ||
                  starts_with_word_ci(trim_copy(rewritten_sql), 0, "desc"))) {
        if (calls.size() != 1 || calls[0].name_ == "glob") {
          ret = OB_NOT_SUPPORTED;
        } else {
          std::string descriptor;
          std::vector<FileColumn> columns;
          if (OB_FAIL(infer_call(calls[0], session, descriptor, columns))) {
          } else {
            rewritten_sql = make_describe_query(columns);
            rewritten = true;
          }
        }
      } else {
        for (size_t index = calls.size(); OB_SUCC(ret) && index > 0; --index) {
          const FileCall &call = calls[index - 1];
          std::string table;
          std::vector<FileColumn> columns;
          if (call.name_ == "glob") {
            ret = make_glob_table(call, session, table, columns);
          } else {
            std::string descriptor;
            if (OB_FAIL(infer_call(call, session, descriptor, columns))) {
            } else {
              table = make_file_table(descriptor, columns);
            }
          }
          if (OB_SUCC(ret)) {
            if (!has_table_alias(rewritten_sql, call.end_)) {
              table += " AS __file_sql_scan_" + std::to_string(index);
            }
            rewritten_sql.replace(call.begin_, call.end_ - call.begin_, table);
          }
        }
        rewritten = OB_SUCC(ret) && (!calls.empty() || copy_rewritten);
      }
    }
  }
  rewritten = rewritten || copy_rewritten;
  return ret;
}

} // namespace sql
} // namespace oceanbase
