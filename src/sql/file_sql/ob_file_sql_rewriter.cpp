/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

#define USING_LOG_PREFIX SQL

#include "sql/file_sql/ob_file_sql_rewriter.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include "local_bridge.h"
#include "sql/session/ob_basic_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace
{

enum class FileValueType
{
  NULL_VALUE,
  BOOLEAN,
  BIGINT,
  DOUBLE,
  DATE,
  DATETIME,
  VARCHAR
};

struct FileColumn
{
  std::string name_;
  FileValueType type_ = FileValueType::NULL_VALUE;
  bool nullable_ = false;
};

struct FileData
{
  rapidjson::Document rows_;
  std::vector<FileColumn> columns_;
  FileData() { rows_.SetArray(); }
};

struct FileCall
{
  size_t begin_ = 0;
  size_t end_ = 0;
  std::string name_;
  std::string path_;
  std::map<std::string, std::string> options_;
};

bool ascii_iequal(const char lhs, const char rhs)
{
  return std::tolower(static_cast<unsigned char>(lhs)) ==
         std::tolower(static_cast<unsigned char>(rhs));
}

bool starts_with_ci(const std::string &text, const size_t pos, const char *word)
{
  size_t i = 0;
  while (word[i] != '\0' && pos + i < text.size() && ascii_iequal(text[pos + i], word[i])) {
    ++i;
  }
  return word[i] == '\0';
}

std::string lower_ascii(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
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

bool is_ident_char(const char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

bool is_file_function(const std::string &name)
{
  const std::string lower = lower_ascii(name);
  return lower == "read_csv" || lower == "read_json" || lower == "read_ndjson" ||
         lower == "read_parquet" || lower == "glob";
}

size_t find_matching_paren(const std::string &text, const size_t open)
{
  int depth = 0;
  char quote = 0;
  for (size_t i = open; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == '\\' && i + 1 < text.size()) {
        ++i;
      } else if (c == quote) {
        if (i + 1 < text.size() && text[i + 1] == quote) {
          ++i;
        } else {
          quote = 0;
        }
      }
    } else if (c == '\'' || c == '"' || c == '`') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')' && --depth == 0) {
      return i;
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
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == '\\' && i + 1 < text.size()) {
        ++i;
      } else if (c == quote) {
        if (i + 1 < text.size() && text[i + 1] == quote) { ++i; } else { quote = 0; }
      }
    } else if (c == '\'' || c == '"') {
      quote = c;
    } else if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
    } else if (c == ',' && depth == 0) {
      result.push_back(trim_copy(text.substr(begin, i - begin)));
      begin = i + 1;
    }
  }
  result.push_back(trim_copy(text.substr(begin)));
  return result;
}

size_t find_top_level_equal(const std::string &text)
{
  char quote = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == '\\' && i + 1 < text.size()) { ++i; }
      else if (c == quote) { quote = 0; }
    } else if (c == '\'' || c == '"') {
      quote = c;
    } else if (c == '=') {
      return i;
    }
  }
  return std::string::npos;
}

bool decode_sql_string(const std::string &literal, std::string &value)
{
  const std::string input = trim_copy(literal);
  if (input.size() < 2 || (input.front() != '\'' && input.front() != '"') ||
      input.back() != input.front()) {
    return false;
  }
  const char quote = input.front();
  value.clear();
  for (size_t i = 1; i + 1 < input.size(); ++i) {
    char c = input[i];
    if (c == quote && i + 1 < input.size() - 1 && input[i + 1] == quote) {
      value.push_back(quote);
      ++i;
    } else if (c == '\\' && i + 1 < input.size() - 1) {
      const char next = input[++i];
      switch (next) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case '0': value.push_back('\0'); break;
        default: value.push_back(next); break;
      }
    } else {
      value.push_back(c);
    }
  }
  return true;
}

int parse_call_at(const std::string &sql, const size_t begin, FileCall &call)
{
  int ret = OB_SUCCESS;
  size_t name_end = begin;
  while (name_end < sql.size() && is_ident_char(sql[name_end])) { ++name_end; }
  size_t open = name_end;
  while (open < sql.size() && std::isspace(static_cast<unsigned char>(sql[open]))) { ++open; }
  if (open >= sql.size() || sql[open] != '(') {
    ret = OB_ERR_PARSE_SQL;
  } else {
    const size_t close = find_matching_paren(sql, open);
    if (close == std::string::npos) {
      ret = OB_ERR_PARSE_SQL;
    } else {
      call.begin_ = begin;
      call.end_ = close + 1;
      call.name_ = lower_ascii(sql.substr(begin, name_end - begin));
      const std::vector<std::string> args = split_arguments(sql.substr(open + 1, close - open - 1));
      if (args.empty() || !decode_sql_string(args[0], call.path_)) {
        ret = OB_ERR_PARSE_SQL;
      }
      for (size_t i = 1; OB_SUCC(ret) && i < args.size(); ++i) {
        const size_t equal = find_top_level_equal(args[i]);
        if (equal == std::string::npos) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          const std::string key = lower_ascii(trim_copy(args[i].substr(0, equal)));
          if (key.empty() || call.options_.count(key) != 0) {
            ret = OB_ERR_PARSE_SQL;
          } else {
            call.options_[key] = trim_copy(args[i].substr(equal + 1));
          }
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
  for (size_t i = 0; OB_SUCC(ret) && i < sql.size();) {
    const char c = sql[i];
    if (quote != 0) {
      if (c == '\\' && i + 1 < sql.size()) { i += 2; }
      else if (c == quote) { quote = 0; ++i; }
      else { ++i; }
    } else if (c == '\'' || c == '"' || c == '`') {
      quote = c;
      ++i;
    } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const size_t begin = i;
      while (i < sql.size() && is_ident_char(sql[i])) { ++i; }
      const std::string name = sql.substr(begin, i - begin);
      size_t open = i;
      while (open < sql.size() && std::isspace(static_cast<unsigned char>(sql[open]))) { ++open; }
      if (is_file_function(name) && open < sql.size() && sql[open] == '(') {
        FileCall call;
        if (OB_FAIL(parse_call_at(sql, begin, call))) {
        } else {
          calls.push_back(call);
          i = call.end_;
        }
      }
    } else {
      ++i;
    }
  }
  return ret;
}

bool parse_bool_option(const std::string &value, bool &result)
{
  const std::string lower = lower_ascii(trim_copy(value));
  if (lower == "true" || lower == "on" || lower == "1") { result = true; return true; }
  if (lower == "false" || lower == "off" || lower == "0") { result = false; return true; }
  return false;
}

bool valid_date(const std::string &value)
{
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') { return false; }
  for (size_t i = 0; i < value.size(); ++i) {
    if (i != 4 && i != 7 && !std::isdigit(static_cast<unsigned char>(value[i]))) { return false; }
  }
  return true;
}

bool valid_datetime(const std::string &value)
{
  if (value.size() < 19 || !valid_date(value.substr(0, 10)) ||
      (value[10] != ' ' && value[10] != 'T') || value[13] != ':' || value[16] != ':') {
    return false;
  }
  for (size_t i = 11; i < 19; ++i) {
    if (i != 13 && i != 16 && !std::isdigit(static_cast<unsigned char>(value[i]))) { return false; }
  }
  return true;
}

FileValueType scalar_type(const rapidjson::Value &value)
{
  if (value.IsNull()) { return FileValueType::NULL_VALUE; }
  if (value.IsBool()) { return FileValueType::BOOLEAN; }
  if (value.IsInt64() || value.IsUint64() || value.IsInt() || value.IsUint()) { return FileValueType::BIGINT; }
  if (value.IsNumber()) { return FileValueType::DOUBLE; }
  if (value.IsString()) {
    const std::string text(value.GetString(), value.GetStringLength());
    if (valid_datetime(text)) { return FileValueType::DATETIME; }
    if (valid_date(text)) { return FileValueType::DATE; }
    return FileValueType::VARCHAR;
  }
  return FileValueType::VARCHAR;
}

FileValueType merge_type(const FileValueType lhs, const FileValueType rhs)
{
  if (lhs == FileValueType::NULL_VALUE) { return rhs; }
  if (rhs == FileValueType::NULL_VALUE || lhs == rhs) { return lhs; }
  if ((lhs == FileValueType::BIGINT && rhs == FileValueType::DOUBLE) ||
      (lhs == FileValueType::DOUBLE && rhs == FileValueType::BIGINT)) { return FileValueType::DOUBLE; }
  if ((lhs == FileValueType::DATE && rhs == FileValueType::DATETIME) ||
      (lhs == FileValueType::DATETIME && rhs == FileValueType::DATE)) { return FileValueType::DATETIME; }
  return FileValueType::VARCHAR;
}

void infer_columns(FileData &data)
{
  std::map<std::string, size_t> indexes;
  for (const auto &row : data.rows_.GetArray()) {
    if (!row.IsObject()) { continue; }
    std::set<std::string> present;
    for (auto member = row.MemberBegin(); member != row.MemberEnd(); ++member) {
      const std::string name(member->name.GetString(), member->name.GetStringLength());
      present.insert(name);
      auto found = indexes.find(name);
      if (found == indexes.end()) {
        FileColumn column;
        column.name_ = name;
        column.nullable_ = data.rows_.Size() > 1;
        data.columns_.push_back(column);
        found = indexes.emplace(name, data.columns_.size() - 1).first;
      }
      FileColumn &column = data.columns_[found->second];
      column.type_ = merge_type(column.type_, scalar_type(member->value));
      column.nullable_ = column.nullable_ || member->value.IsNull();
    }
    for (FileColumn &column : data.columns_) {
      if (present.count(column.name_) == 0) { column.nullable_ = true; }
    }
  }
  for (FileColumn &column : data.columns_) {
    if (column.type_ == FileValueType::NULL_VALUE) { column.type_ = FileValueType::VARCHAR; }
  }
}

int read_file(const std::string &path, std::string &contents)
{
  int ret = OB_SUCCESS;
  std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
  if (!input) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) { ret = OB_IO_ERROR; }
    else { contents = buffer.str(); }
  }
  return ret;
}

std::vector<std::vector<std::string>> parse_csv_rows(const std::string &contents, bool &valid)
{
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  valid = true;
  for (size_t i = 0; i < contents.size(); ++i) {
    const char c = contents[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < contents.size() && contents[i + 1] == '"') { field.push_back('"'); ++i; }
        else { quoted = false; }
      } else { field.push_back(c); }
    } else if (c == '"' && field.empty()) {
      quoted = true;
    } else if (c == ',') {
      row.push_back(field); field.clear();
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < contents.size() && contents[i + 1] == '\n') { ++i; }
      row.push_back(field); field.clear();
      rows.push_back(row); row.clear();
    } else { field.push_back(c); }
  }
  if (quoted) { valid = false; }
  if (!field.empty() || !row.empty()) { row.push_back(field); rows.push_back(row); }
  return rows;
}

void add_csv_value(const std::string &text, rapidjson::Value &value,
                   rapidjson::Document::AllocatorType &allocator)
{
  const std::string lower = lower_ascii(text);
  if (text.empty()) {
    value.SetNull();
  } else if (lower == "true" || lower == "false") {
    value.SetBool(lower == "true");
  } else {
    char *end = nullptr;
    errno = 0;
    const long long integer = std::strtoll(text.c_str(), &end, 10);
    if (errno == 0 && end == text.c_str() + text.size()) {
      value.SetInt64(integer);
    } else {
      errno = 0;
      end = nullptr;
      const double number = std::strtod(text.c_str(), &end);
      if (errno == 0 && end == text.c_str() + text.size()) {
        value.SetDouble(number);
      } else {
        std::string normalized = text;
        if (valid_datetime(normalized) && normalized[10] == 'T') { normalized[10] = ' '; }
        value.SetString(normalized.data(), static_cast<rapidjson::SizeType>(normalized.size()), allocator);
      }
    }
  }
}

int load_csv(const FileCall &call, FileData &data)
{
  int ret = OB_SUCCESS;
  bool header = true;
  for (const auto &option : call.options_) {
    if (option.first == "header") {
      if (!parse_bool_option(option.second, header)) { ret = OB_ERR_PARSE_SQL; }
    } else if (option.first == "sample_size") {
      if (trim_copy(option.second) != "-1") { ret = OB_NOT_SUPPORTED; }
    } else {
      ret = OB_NOT_SUPPORTED;
    }
  }
  std::string contents;
  bool valid = false;
  std::vector<std::vector<std::string>> rows;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(read_file(call.path_, contents))) {
  } else if (FALSE_IT(rows = parse_csv_rows(contents, valid))) {
  } else if (!valid || rows.empty()) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    const size_t column_count = rows[0].size();
    std::vector<std::string> names(column_count);
    std::map<std::string, int> duplicates;
    for (size_t i = 0; i < column_count; ++i) {
      std::string name = header ? rows[0][i] : "column" + std::to_string(i);
      if (name.empty()) { name = "column" + std::to_string(i); }
      const int occurrence = ++duplicates[name];
      names[i] = occurrence == 1 ? name : name + "__" + std::to_string(occurrence);
    }
    auto &allocator = data.rows_.GetAllocator();
    for (size_t r = header ? 1 : 0; r < rows.size(); ++r) {
      rapidjson::Value object(rapidjson::kObjectType);
      for (size_t c = 0; c < column_count; ++c) {
        rapidjson::Value name;
        name.SetString(names[c].data(), static_cast<rapidjson::SizeType>(names[c].size()), allocator);
        rapidjson::Value value;
        if (c >= rows[r].size()) { value.SetNull(); }
        else { add_csv_value(rows[r][c], value, allocator); }
        object.AddMember(name, value, allocator);
      }
      data.rows_.PushBack(object, allocator);
    }
    infer_columns(data);
  }
  return ret;
}

int append_json_document(const rapidjson::Document &parsed, FileData &data)
{
  int ret = OB_SUCCESS;
  auto &allocator = data.rows_.GetAllocator();
  if (parsed.IsArray()) {
    for (const auto &value : parsed.GetArray()) {
      if (!value.IsObject()) { ret = OB_ERR_PARSE_SQL; break; }
      rapidjson::Value copy;
      copy.CopyFrom(value, allocator);
      data.rows_.PushBack(copy, allocator);
    }
  } else if (parsed.IsObject()) {
    rapidjson::Value copy;
    copy.CopyFrom(parsed, allocator);
    data.rows_.PushBack(copy, allocator);
  } else {
    ret = OB_ERR_PARSE_SQL;
  }
  return ret;
}

int load_json(const FileCall &call, FileData &data)
{
  int ret = OB_SUCCESS;
  bool newline_delimited = call.name_ == "read_ndjson";
  for (const auto &option : call.options_) {
    if (option.first == "format") {
      std::string format;
      if (!decode_sql_string(option.second, format)) { ret = OB_ERR_PARSE_SQL; }
      else if (lower_ascii(format) == "newline_delimited" || lower_ascii(format) == "ndjson") {
        newline_delimited = true;
      } else if (lower_ascii(format) == "array" || lower_ascii(format) == "auto") {
        newline_delimited = false;
      } else { ret = OB_NOT_SUPPORTED; }
    } else { ret = OB_NOT_SUPPORTED; }
  }
  std::string contents;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(read_file(call.path_, contents))) {
  } else if (newline_delimited) {
    std::istringstream stream(contents);
    std::string line;
    while (OB_SUCC(ret) && std::getline(stream, line)) {
      line = trim_copy(line);
      if (line.empty()) { continue; }
      rapidjson::Document parsed;
      parsed.Parse(line.data(), line.size());
      if (parsed.HasParseError()) { ret = OB_ERR_PARSE_SQL; }
      else if (OB_FAIL(append_json_document(parsed, data))) {}
    }
  } else {
    rapidjson::Document parsed;
    parsed.Parse(contents.data(), contents.size());
    if (parsed.HasParseError()) { ret = OB_ERR_PARSE_SQL; }
    else if (OB_FAIL(append_json_document(parsed, data))) {}
  }
  if (OB_SUCC(ret)) { infer_columns(data); }
  return ret;
}

int load_parquet(const FileCall &call, FileData &data)
{
  int ret = OB_SUCCESS;
  if (!call.options_.empty()) {
    ret = OB_NOT_SUPPORTED;
  } else {
    local::ParquetResult result = local::read_parquet_json(call.path_);
    if (result.code != 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to read parquet file", K(ret), "path", call.path_.c_str(),
               "reason", result.message.c_str());
    } else {
      rapidjson::Document parsed;
      parsed.Parse(result.json.data(), result.json.size());
      if (parsed.HasParseError() || !parsed.IsArray()) { ret = OB_ERR_PARSE_SQL; }
      else if (OB_FAIL(append_json_document(parsed, data))) {}
      else { infer_columns(data); }
    }
  }
  return ret;
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

std::string path_to_utf8(const std::filesystem::path &path)
{
  const auto utf8 = path.u8string();
  return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
}

int load_glob(const FileCall &call, FileData &data)
{
  int ret = OB_SUCCESS;
  if (!call.options_.empty()) {
    ret = OB_NOT_SUPPORTED;
  } else {
    const std::filesystem::path pattern_path = std::filesystem::u8path(call.path_);
    const std::filesystem::path directory = pattern_path.has_parent_path()
                                              ? pattern_path.parent_path()
                                              : std::filesystem::path(".");
    const std::string pattern = path_to_utf8(pattern_path.filename());
    std::error_code error;
    std::vector<std::string> matches;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
      const std::string filename = path_to_utf8(entry.path().filename());
      if (wildcard_match(pattern.c_str(), filename.c_str())) {
        std::string path = path_to_utf8(entry.path().lexically_normal());
        std::replace(path.begin(), path.end(), '\\', '/');
        matches.push_back(path);
      }
    }
    if (error) {
      ret = OB_IO_ERROR;
    } else {
      std::sort(matches.begin(), matches.end());
      auto &allocator = data.rows_.GetAllocator();
      for (const std::string &match : matches) {
        rapidjson::Value object(rapidjson::kObjectType);
        rapidjson::Value value;
        value.SetString(match.data(), static_cast<rapidjson::SizeType>(match.size()), allocator);
        object.AddMember("file", value, allocator);
        data.rows_.PushBack(object, allocator);
      }
      FileColumn column;
      column.name_ = "file";
      column.type_ = FileValueType::VARCHAR;
      data.columns_.push_back(column);
    }
  }
  return ret;
}

int load_call(const FileCall &call, FileData &data)
{
  int ret = OB_SUCCESS;
  if (call.name_ == "read_csv") { ret = load_csv(call, data); }
  else if (call.name_ == "read_json" || call.name_ == "read_ndjson") { ret = load_json(call, data); }
  else if (call.name_ == "read_parquet") { ret = load_parquet(call, data); }
  else if (call.name_ == "glob") { ret = load_glob(call, data); }
  else { ret = OB_NOT_SUPPORTED; }
  if (OB_SUCC(ret) && data.columns_.empty()) { ret = OB_ERR_PARSE_SQL; }
  return ret;
}

std::string quote_identifier(const std::string &name)
{
  std::string result = "`";
  for (const char c : name) { result += c == '`' ? "``" : std::string(1, c); }
  result += '`';
  return result;
}

std::string quote_sql_string(const std::string &value)
{
  std::string result = "'";
  for (const char c : value) {
    if (c == '\\') { result += "\\\\"; }
    else if (c == '\'') { result += "''"; }
    else if (c == '\0') { result += "\\0"; }
    else { result.push_back(c); }
  }
  result += '\'';
  return result;
}

std::string json_path(const std::string &name)
{
  std::string path = "$.\\\"";
  for (const char c : name) {
    if (c == '\\' || c == '"') { path.push_back('\\'); }
    path.push_back(c);
  }
  path += "\\\"";
  return path;
}

const char *sql_type(const FileValueType type)
{
  switch (type) {
    case FileValueType::BOOLEAN: return "BOOLEAN";
    case FileValueType::BIGINT: return "BIGINT";
    case FileValueType::DOUBLE: return "DOUBLE";
    case FileValueType::DATE: return "DATE";
    case FileValueType::DATETIME: return "DATETIME";
    default: return "VARCHAR(65535)";
  }
}

const char *describe_type(const FileValueType type)
{
  return type == FileValueType::VARCHAR ? "VARCHAR" : sql_type(type);
}

std::string serialize_rows(const rapidjson::Document &rows)
{
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  rows.Accept(writer);
  return std::string(buffer.GetString(), buffer.GetSize());
}

std::string make_json_table(const FileData &data)
{
  std::string result = "JSON_TABLE(" + quote_sql_string(serialize_rows(data.rows_)) +
                       ", '$[*]' COLUMNS (";
  for (size_t i = 0; i < data.columns_.size(); ++i) {
    if (i != 0) { result += ", "; }
    const FileColumn &column = data.columns_[i];
    result += quote_identifier(column.name_) + " " + sql_type(column.type_) +
              " PATH " + quote_sql_string(json_path(column.name_)) +
              " NULL ON EMPTY NULL ON ERROR";
  }
  result += "))";
  return result;
}

std::string make_describe_query(const FileData &data)
{
  std::string result;
  for (size_t i = 0; i < data.columns_.size(); ++i) {
    if (i != 0) { result += " UNION ALL "; }
    const FileColumn &column = data.columns_[i];
    result += "SELECT " + quote_sql_string(column.name_) + " AS column_name, " +
              quote_sql_string(describe_type(column.type_)) + " AS column_type, " +
              quote_sql_string(column.nullable_ ? "YES" : "NO") + " AS `null`, " +
              "NULL AS `key`, NULL AS `default`, NULL AS extra";
  }
  return result;
}

int rewrite_copy(std::string &sql, bool &copy_rewritten)
{
  int ret = OB_SUCCESS;
  copy_rewritten = false;
  const std::string trimmed = trim_copy(sql);
  if (!starts_with_ci(trimmed, 0, "copy")) { return ret; }
  size_t open = 4;
  while (open < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[open]))) { ++open; }
  if (open >= trimmed.size() || trimmed[open] != '(') { return OB_ERR_PARSE_SQL; }
  const size_t close = find_matching_paren(trimmed, open);
  if (close == std::string::npos) { return OB_ERR_PARSE_SQL; }
  size_t pos = close + 1;
  while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) { ++pos; }
  if (!starts_with_ci(trimmed, pos, "to") || (pos + 2 < trimmed.size() && is_ident_char(trimmed[pos + 2]))) {
    return OB_ERR_PARSE_SQL;
  }
  pos += 2;
  while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) { ++pos; }
  if (pos >= trimmed.size() || (trimmed[pos] != '\'' && trimmed[pos] != '"')) { return OB_ERR_PARSE_SQL; }
  const char quote = trimmed[pos];
  const size_t path_begin = pos++;
  while (pos < trimmed.size()) {
    if (trimmed[pos] == quote) {
      if (pos + 1 < trimmed.size() && trimmed[pos + 1] == quote) { pos += 2; }
      else { ++pos; break; }
    } else { ++pos; }
  }
  if (pos > trimmed.size()) { return OB_ERR_PARSE_SQL; }
  const std::string path_literal = trimmed.substr(path_begin, pos - path_begin);
  std::string options = lower_ascii(trimmed.substr(pos));
  if (options.find("format csv") == std::string::npos) { return OB_NOT_SUPPORTED; }
  const bool header = options.find("header") != std::string::npos;
  sql = trimmed.substr(open + 1, close - open - 1) + " INTO OUTFILE " + path_literal +
        " FORMAT=(TYPE='CSV', PARSE_HEADER=" + std::string(header ? "TRUE" : "FALSE") +
        ", NULL_IF=('NULL'))";
  copy_rewritten = true;
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
    } else if (!calls.empty()) {
      int64_t enabled = 0;
      const common::ObString variable_name("enable_file_sql");
      if (OB_FAIL(session.get_sys_variable_by_name(variable_name, enabled))) {
      } else if (enabled == 0) {
        ret = OB_NOT_SUPPORTED;
      } else if (starts_with_ci(trim_copy(rewritten_sql), 0, "describe")) {
        if (calls.size() != 1) {
          ret = OB_NOT_SUPPORTED;
        } else {
          FileData data;
          if (OB_FAIL(load_call(calls[0], data))) {
          } else {
            rewritten_sql = make_describe_query(data);
            rewritten = true;
          }
        }
      } else {
        for (size_t index = calls.size(); OB_SUCC(ret) && index > 0; --index) {
          const FileCall &call = calls[index - 1];
          FileData data;
          if (OB_FAIL(load_call(call, data))) {
          } else {
            rewritten_sql.replace(call.begin_, call.end_ - call.begin_, make_json_table(data));
          }
        }
        rewritten = OB_SUCC(ret);
      }
    }
  }
  rewritten = rewritten || copy_rewritten;
  return ret;
}

} // namespace sql
} // namespace oceanbase
