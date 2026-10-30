// Copyright (c) 2026 OceanBase.
//
// Licensed under the Apache License, Version 2.0.

//! Bounded-memory readers used by file SQL.  The SQL layer owns path policy;
//! this module owns parsing, deterministic inference, and file identity checks.

use crate::bridge::ffi::FileScanResult;
use parquet::file::reader::{FileReader, SerializedFileReader};
use parquet::record::reader::RowIter;
use parquet::record::{Field, Row};
use parquet::schema::types::Type;
use serde_json::{json, Map, Number, Value};
use std::collections::{HashMap, HashSet};
use std::fs::{self, File};
use std::io::{BufRead, BufReader};
use std::time::UNIX_EPOCH;

const OK: i32 = 0;
const END: i32 = 1;
const INVALID: i32 = 2;
const IO: i32 = 3;
const CHANGED: i32 = 4;
const MAX_VALUE_BYTES: usize = 64 * 1024 * 1024;
const MAX_BATCH_BYTES: usize = 64 * 1024 * 1024;
const MAX_COLUMNS: usize = 4096;
const MAX_COLUMN_NAME_BYTES: usize = 128;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ScalarType {
    Null,
    Boolean,
    BigInt,
    Double,
    Date,
    DateTime,
    Varchar,
}

impl ScalarType {
    fn sql_name(self) -> &'static str {
        match self {
            Self::Null => "NULL",
            Self::Boolean => "BOOLEAN",
            Self::BigInt => "BIGINT",
            Self::Double => "DOUBLE",
            Self::Date => "DATE",
            Self::DateTime => "DATETIME",
            Self::Varchar => "VARCHAR",
        }
    }

    fn from_sql_name(value: &str) -> Option<Self> {
        Some(match value {
            "NULL" => Self::Null,
            "BOOLEAN" => Self::Boolean,
            "BIGINT" => Self::BigInt,
            "DOUBLE" => Self::Double,
            "DATE" => Self::Date,
            "DATETIME" => Self::DateTime,
            "VARCHAR" => Self::Varchar,
            _ => return None,
        })
    }
}

#[derive(Clone, Debug)]
struct Column {
    name: String,
    kind: ScalarType,
    nullable: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Identity {
    length: u64,
    modified_ns: u128,
}

#[derive(Clone, Debug)]
struct ScanSpec {
    format: String,
    path: String,
    header: bool,
    delimiter: u8,
    quote: u8,
    escape: Option<u8>,
    null_text: Option<String>,
    json_array: bool,
    identity: Option<Identity>,
    columns: Vec<Column>,
    projection: Vec<String>,
}

fn success(json: String) -> FileScanResult {
    FileScanResult {
        code: OK,
        message: String::new(),
        json,
    }
}

fn failure(code: i32, message: impl Into<String>) -> FileScanResult {
    FileScanResult {
        code,
        message: message.into(),
        json: String::new(),
    }
}

fn identity(path: &str) -> Result<Identity, String> {
    let meta = fs::metadata(path).map_err(|e| format!("cannot stat '{}': {}", path, e))?;
    if !meta.is_file() {
        return Err(format!("'{}' is not a regular file", path));
    }
    let modified = meta
        .modified()
        .map_err(|e| format!("cannot read modification time for '{}': {}", path, e))?
        .duration_since(UNIX_EPOCH)
        .map_err(|_| format!("invalid modification time for '{}'", path))?;
    Ok(Identity {
        length: meta.len(),
        modified_ns: modified.as_nanos(),
    })
}

fn one_byte(value: Option<&Value>, default: u8, name: &str) -> Result<u8, String> {
    let Some(value) = value else {
        return Ok(default);
    };
    let text = value
        .as_str()
        .ok_or_else(|| format!("{} must be a string", name))?;
    let bytes = text.as_bytes();
    if bytes.len() != 1 {
        return Err(format!("{} must be exactly one byte", name));
    }
    Ok(bytes[0])
}

fn parse_columns(root: &Value) -> Result<Vec<Column>, String> {
    let Some(values) = root.get("columns") else {
        return Ok(Vec::new());
    };
    values
        .as_array()
        .ok_or_else(|| "columns must be an array".to_string())?
        .iter()
        .map(|item| {
            let name = item
                .get("name")
                .and_then(Value::as_str)
                .ok_or_else(|| "column name must be a string".to_string())?;
            let kind = item
                .get("type")
                .and_then(Value::as_str)
                .and_then(ScalarType::from_sql_name)
                .ok_or_else(|| format!("invalid type for column '{}'", name))?;
            Ok(Column {
                name: name.to_string(),
                kind,
                nullable: item
                    .get("nullable")
                    .and_then(Value::as_bool)
                    .unwrap_or(true),
            })
        })
        .collect()
}

fn parse_projection(root: &Value, columns: &[Column]) -> Result<Vec<String>, String> {
    let Some(values) = root.get("projection") else {
        return Ok(Vec::new());
    };
    let values = values
        .as_array()
        .ok_or_else(|| "projection must be an array".to_string())?;
    let mut projection = Vec::with_capacity(values.len());
    let mut seen = HashSet::new();
    for value in values {
        let name = value
            .as_str()
            .ok_or_else(|| "projection column name must be a string".to_string())?;
        if !columns.iter().any(|column| column.name == name) {
            return Err(format!("unknown projection column '{}'", name));
        }
        if !seen.insert(name.to_string()) {
            return Err(format!("duplicate projection column '{}'", name));
        }
        projection.push(name.to_string());
    }
    Ok(projection)
}

fn parse_spec(text: &str) -> Result<ScanSpec, String> {
    let root: Value =
        serde_json::from_str(text).map_err(|e| format!("invalid scan descriptor: {}", e))?;
    let format = root
        .get("format")
        .and_then(Value::as_str)
        .ok_or_else(|| "missing file format".to_string())?
        .to_ascii_lowercase();
    let path = root
        .get("path")
        .and_then(Value::as_str)
        .ok_or_else(|| "missing file path".to_string())?
        .to_string();
    let escape = match root.get("escape") {
        None | Some(Value::Null) => None,
        value => Some(one_byte(value, b'\\', "escape")?),
    };
    let identity = root
        .get("identity")
        .map(|id| -> Result<Identity, String> {
            let length = id
                .get("length")
                .and_then(Value::as_u64)
                .ok_or_else(|| "invalid file identity length".to_string())?;
            let modified_ns = id
                .get("modified_ns")
                .and_then(Value::as_str)
                .ok_or_else(|| "invalid file identity timestamp".to_string())?
                .parse::<u128>()
                .map_err(|_| "invalid file identity timestamp".to_string())?;
            Ok(Identity {
                length,
                modified_ns,
            })
        })
        .transpose()?;
    let columns = parse_columns(&root)?;
    let projection = parse_projection(&root, &columns)?;
    Ok(ScanSpec {
        format,
        path,
        header: root.get("header").and_then(Value::as_bool).unwrap_or(true),
        delimiter: one_byte(root.get("delimiter"), b',', "delimiter")?,
        quote: one_byte(root.get("quote"), b'"', "quote")?,
        escape,
        null_text: root
            .get("null_text")
            .and_then(Value::as_str)
            .map(str::to_string),
        json_array: root
            .get("json_array")
            .and_then(Value::as_bool)
            .unwrap_or(false),
        identity,
        columns,
        projection,
    })
}

fn check_identity(spec: &ScanSpec) -> Result<(), String> {
    if let Some(expected) = &spec.identity {
        let current = identity(&spec.path)?;
        if &current != expected {
            return Err(format!(
                "file changed after schema inference: '{}'",
                spec.path
            ));
        }
    }
    Ok(())
}

fn validate_column_name(name: &str, format: &str) -> Result<(), String> {
    if name.is_empty() {
        return Err(format!("{} column name must not be empty", format));
    }
    if name.len() > MAX_COLUMN_NAME_BYTES {
        return Err(format!(
            "{} column name exceeds {} bytes",
            format, MAX_COLUMN_NAME_BYTES
        ));
    }
    if name.bytes().any(|byte| byte < 0x20 || byte == 0x7f) {
        return Err(format!("{} column name contains a control byte", format));
    }
    Ok(())
}

fn bounded_line(reader: &mut BufReader<File>, line: u64) -> Result<Option<String>, String> {
    let mut bytes = Vec::new();
    loop {
        let buffer = reader
            .fill_buf()
            .map_err(|e| format!("JSON read failed at line {}: {}", line, e))?;
        if buffer.is_empty() {
            if bytes.is_empty() {
                return Ok(None);
            }
            break;
        }
        let newline = buffer.iter().position(|byte| *byte == b'\n');
        let take = newline.map_or(buffer.len(), |position| position + 1);
        if bytes.len() + take > MAX_VALUE_BYTES {
            return Err(format!(
                "JSON line {} exceeds {} bytes",
                line, MAX_VALUE_BYTES
            ));
        }
        bytes.extend_from_slice(&buffer[..take]);
        reader.consume(take);
        if newline.is_some() {
            break;
        }
    }
    String::from_utf8(bytes)
        .map(Some)
        .map_err(|_| format!("JSON is not valid UTF-8 at line {}", line))
}

fn is_leap(year: i32) -> bool {
    year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
}

fn valid_date(text: &str) -> bool {
    let bytes = text.as_bytes();
    if bytes.len() != 10 || bytes[4] != b'-' || bytes[7] != b'-' {
        return false;
    }
    if bytes
        .iter()
        .enumerate()
        .any(|(i, b)| i != 4 && i != 7 && !b.is_ascii_digit())
    {
        return false;
    }
    let Ok(year) = text[0..4].parse::<i32>() else {
        return false;
    };
    let Ok(month) = text[5..7].parse::<usize>() else {
        return false;
    };
    let Ok(day) = text[8..10].parse::<u32>() else {
        return false;
    };
    if year == 0 || !(1..=12).contains(&month) {
        return false;
    }
    let mut days = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if is_leap(year) {
        days[1] = 29;
    }
    day >= 1 && day <= days[month - 1]
}

fn valid_datetime(text: &str) -> bool {
    if text.len() < 19 || !valid_date(&text[..10]) {
        return false;
    }
    let bytes = text.as_bytes();
    if !matches!(bytes[10], b' ' | b'T') || bytes[13] != b':' || bytes[16] != b':' {
        return false;
    }
    if bytes[11..13]
        .iter()
        .chain(&bytes[14..16])
        .chain(&bytes[17..19])
        .any(|b| !b.is_ascii_digit())
    {
        return false;
    }
    let hour = text[11..13].parse::<u32>().unwrap_or(99);
    let minute = text[14..16].parse::<u32>().unwrap_or(99);
    let second = text[17..19].parse::<u32>().unwrap_or(99);
    let fraction_ok = text.len() == 19
        || (text.len() > 20
            && text.len() <= 26
            && bytes[19] == b'.'
            && bytes[20..].iter().all(u8::is_ascii_digit));
    hour <= 23 && minute <= 59 && second <= 59 && fraction_ok
}

fn text_type(text: &str, null_text: Option<&str>) -> ScalarType {
    if text.is_empty() || null_text == Some(text) {
        return ScalarType::Null;
    }
    if text.eq_ignore_ascii_case("true") || text.eq_ignore_ascii_case("false") {
        return ScalarType::Boolean;
    }
    if text.parse::<i64>().is_ok() {
        return ScalarType::BigInt;
    }
    if text.parse::<f64>().is_ok_and(f64::is_finite) {
        return ScalarType::Double;
    }
    if valid_datetime(text) {
        return ScalarType::DateTime;
    }
    if valid_date(text) {
        return ScalarType::Date;
    }
    ScalarType::Varchar
}

fn value_type(value: &Value) -> Result<ScalarType, String> {
    Ok(match value {
        Value::Null => ScalarType::Null,
        Value::Bool(_) => ScalarType::Boolean,
        Value::Number(number) if number.as_i64().is_some() => ScalarType::BigInt,
        Value::Number(number) if number.as_u64().is_some() => ScalarType::Varchar,
        Value::Number(_) => ScalarType::Double,
        Value::String(text) if valid_datetime(text) => ScalarType::DateTime,
        Value::String(text) if valid_date(text) => ScalarType::Date,
        Value::String(_) => ScalarType::Varchar,
        Value::Array(_) | Value::Object(_) => {
            return Err("nested JSON values are not supported".to_string())
        }
    })
}

fn merge_type(left: ScalarType, right: ScalarType) -> ScalarType {
    use ScalarType::*;
    match (left, right) {
        (Null, value) | (value, Null) => value,
        (left, right) if left == right => left,
        (BigInt, Double) | (Double, BigInt) => Double,
        (Date, DateTime) | (DateTime, Date) => DateTime,
        _ => Varchar,
    }
}

struct CsvReader {
    input: BufReader<File>,
    delimiter: u8,
    quote: u8,
    escape: Option<u8>,
    line: u64,
    eof: bool,
}

impl CsvReader {
    fn open(spec: &ScanSpec) -> Result<Self, String> {
        let file =
            File::open(&spec.path).map_err(|e| format!("cannot open '{}': {}", spec.path, e))?;
        Ok(Self {
            input: BufReader::new(file),
            delimiter: spec.delimiter,
            quote: spec.quote,
            escape: spec.escape,
            line: 1,
            eof: false,
        })
    }

    fn byte(&mut self) -> Result<Option<u8>, String> {
        let buffer = self
            .input
            .fill_buf()
            .map_err(|e| format!("CSV read failed at line {}: {}", self.line, e))?;
        if buffer.is_empty() {
            return Ok(None);
        }
        let byte = buffer[0];
        self.input.consume(1);
        Ok(Some(byte))
    }

    fn record(&mut self) -> Result<Option<(u64, Vec<String>)>, String> {
        if self.eof {
            return Ok(None);
        }
        let start_line = self.line;
        let mut fields = Vec::new();
        let mut field = Vec::new();
        let mut quoted = false;
        let mut after_quote = false;
        let mut any = false;
        let mut record_bytes = 0usize;
        loop {
            if record_bytes > MAX_VALUE_BYTES {
                return Err(format!(
                    "CSV record starting at line {} exceeds {} bytes",
                    start_line, MAX_VALUE_BYTES
                ));
            }
            let next = self.byte()?;
            let Some(byte) = next else {
                self.eof = true;
                if quoted {
                    return Err(format!(
                        "unterminated quoted CSV field starting at line {}",
                        start_line
                    ));
                }
                if !any && field.is_empty() && fields.is_empty() {
                    return Ok(None);
                }
                fields.push(
                    String::from_utf8(field)
                        .map_err(|_| format!("CSV is not valid UTF-8 at line {}", start_line))?,
                );
                return Ok(Some((start_line, fields)));
            };
            any = true;
            record_bytes += 1;
            if quoted {
                if Some(byte) == self.escape && self.escape != Some(self.quote) {
                    let escaped = self
                        .byte()?
                        .ok_or_else(|| format!("dangling CSV escape at line {}", self.line))?;
                    record_bytes += 1;
                    if escaped == b'\n' {
                        self.line += 1;
                    }
                    field.push(escaped);
                } else if byte == self.quote {
                    let buffer = self.input.fill_buf().map_err(|e| e.to_string())?;
                    if buffer.first() == Some(&self.quote) {
                        self.input.consume(1);
                        record_bytes += 1;
                        field.push(self.quote);
                    } else {
                        quoted = false;
                        after_quote = true;
                    }
                } else {
                    if byte == b'\r' {
                        let buffer = self.input.fill_buf().map_err(|e| e.to_string())?;
                        if buffer.first() == Some(&b'\n') {
                            self.input.consume(1);
                            record_bytes += 1;
                        }
                        self.line += 1;
                        field.push(b'\n');
                    } else {
                        if byte == b'\n' {
                            self.line += 1;
                        }
                        field.push(byte);
                    }
                }
            } else if after_quote {
                if byte == self.delimiter {
                    fields.push(
                        String::from_utf8(std::mem::take(&mut field)).map_err(|_| {
                            format!("CSV is not valid UTF-8 at line {}", start_line)
                        })?,
                    );
                    after_quote = false;
                } else if byte == b'\n' || byte == b'\r' {
                    if byte == b'\r' {
                        let buffer = self.input.fill_buf().map_err(|e| e.to_string())?;
                        if buffer.first() == Some(&b'\n') {
                            self.input.consume(1);
                            record_bytes += 1;
                        }
                    }
                    if record_bytes > MAX_VALUE_BYTES {
                        return Err(format!(
                            "CSV record starting at line {} exceeds {} bytes",
                            start_line, MAX_VALUE_BYTES
                        ));
                    }
                    self.line += 1;
                    fields.push(
                        String::from_utf8(field).map_err(|_| {
                            format!("CSV is not valid UTF-8 at line {}", start_line)
                        })?,
                    );
                    return Ok(Some((start_line, fields)));
                } else {
                    return Err(format!(
                        "unexpected byte after closing quote at CSV line {}",
                        self.line
                    ));
                }
            } else if byte == self.quote && field.is_empty() {
                quoted = true;
            } else if byte == self.delimiter {
                fields.push(
                    String::from_utf8(std::mem::take(&mut field))
                        .map_err(|_| format!("CSV is not valid UTF-8 at line {}", start_line))?,
                );
            } else if byte == b'\n' || byte == b'\r' {
                if byte == b'\r' {
                    let buffer = self.input.fill_buf().map_err(|e| e.to_string())?;
                    if buffer.first() == Some(&b'\n') {
                        self.input.consume(1);
                        record_bytes += 1;
                    }
                }
                if record_bytes > MAX_VALUE_BYTES {
                    return Err(format!(
                        "CSV record starting at line {} exceeds {} bytes",
                        start_line, MAX_VALUE_BYTES
                    ));
                }
                self.line += 1;
                fields.push(
                    String::from_utf8(field)
                        .map_err(|_| format!("CSV is not valid UTF-8 at line {}", start_line))?,
                );
                return Ok(Some((start_line, fields)));
            } else {
                field.push(byte);
            }
        }
    }
}

fn deduplicate_headers(values: Vec<String>) -> Vec<String> {
    let mut counts = HashMap::<String, usize>::new();
    let mut used = HashSet::<String>::new();
    values
        .into_iter()
        .enumerate()
        .map(|(index, value)| {
            let base = if value.is_empty() {
                format!("column{}", index)
            } else {
                value
            };
            let key = base.to_ascii_lowercase();
            let count = counts.entry(key).or_insert(0);
            loop {
                *count += 1;
                let candidate = if *count == 1 {
                    base.clone()
                } else {
                    format!("{}__{}", base, count)
                };
                if used.insert(candidate.to_ascii_lowercase()) {
                    break candidate;
                }
            }
        })
        .collect()
}

fn infer_csv(spec: &ScanSpec) -> Result<Vec<Column>, String> {
    let mut reader = CsvReader::open(spec)?;
    let (first_line, mut first_fields) = reader
        .record()?
        .ok_or_else(|| "CSV file is empty".to_string())?;
    let width = first_fields.len();
    if width == 0 {
        return Err("CSV has no columns".to_string());
    }
    if width > MAX_COLUMNS {
        return Err(format!("CSV has more than {} columns", MAX_COLUMNS));
    }
    if let Some(name) = first_fields.first_mut() {
        if name.starts_with('\u{feff}') {
            *name = name.trim_start_matches('\u{feff}').to_string();
        }
    }
    let names = if spec.header {
        deduplicate_headers(first_fields.clone())
    } else {
        (0..width).map(|i| format!("column{}", i)).collect()
    };
    for name in &names {
        validate_column_name(name, "CSV")?;
    }
    let mut columns: Vec<Column> = names
        .into_iter()
        .map(|name| Column {
            name,
            kind: ScalarType::Null,
            nullable: false,
        })
        .collect();
    let mut process = |line: u64, fields: &[String]| -> Result<(), String> {
        if fields.len() != width {
            return Err(format!(
                "CSV line {} has {} fields; expected {}",
                line,
                fields.len(),
                width
            ));
        }
        for (column, text) in columns.iter_mut().zip(fields) {
            let kind = text_type(text, spec.null_text.as_deref());
            column.nullable |= kind == ScalarType::Null;
            column.kind = merge_type(column.kind, kind);
        }
        Ok(())
    };
    if !spec.header {
        process(first_line, &first_fields)?;
    }
    while let Some((line, fields)) = reader.record()? {
        process(line, &fields)?;
    }
    for column in &mut columns {
        if column.kind == ScalarType::Null {
            column.kind = ScalarType::Varchar;
            column.nullable = true;
        }
    }
    Ok(columns)
}

fn add_json_row(
    value: Value,
    columns: &mut Vec<Column>,
    indexes: &mut HashMap<String, usize>,
    row: u64,
) -> Result<(), String> {
    let object = value
        .as_object()
        .ok_or_else(|| format!("JSON record {} is not an object", row))?;
    let prior_columns = columns.len();
    for (name, value) in object {
        validate_column_name(name, "JSON")
            .map_err(|error| format!("JSON record {}: {}", row, error))?;
        let kind = value_type(value)
            .map_err(|e| format!("JSON record {}, field '{}': {}", row, name, e))?;
        let index = match indexes.get(name) {
            Some(index) => *index,
            None => {
                if columns.len() >= MAX_COLUMNS {
                    return Err(format!("JSON has more than {} fields", MAX_COLUMNS));
                }
                if indexes
                    .keys()
                    .any(|existing| existing.eq_ignore_ascii_case(name))
                {
                    return Err(format!(
                        "JSON field '{}' conflicts case-insensitively with another field",
                        name
                    ));
                }
                let index = columns.len();
                columns.push(Column {
                    name: name.clone(),
                    kind: ScalarType::Null,
                    nullable: row > 1,
                });
                indexes.insert(name.clone(), index);
                index
            }
        };
        columns[index].nullable |= kind == ScalarType::Null;
        columns[index].kind = merge_type(columns[index].kind, kind);
    }
    for column in columns.iter_mut().take(prior_columns) {
        if !object.contains_key(&column.name) {
            column.nullable = true;
        }
    }
    Ok(())
}

struct JsonArrayReader {
    input: BufReader<File>,
    started: bool,
    finished: bool,
    after_comma: bool,
    line: u64,
}

impl JsonArrayReader {
    fn open(path: &str) -> Result<Self, String> {
        Ok(Self {
            input: BufReader::new(
                File::open(path).map_err(|e| format!("cannot open '{}': {}", path, e))?,
            ),
            started: false,
            finished: false,
            after_comma: false,
            line: 1,
        })
    }
    fn byte(&mut self) -> Result<Option<u8>, String> {
        let buffer = self.input.fill_buf().map_err(|e| e.to_string())?;
        if buffer.is_empty() {
            return Ok(None);
        }
        let value = buffer[0];
        self.input.consume(1);
        if value == b'\n' {
            self.line += 1;
        }
        Ok(Some(value))
    }
    fn finish(&mut self) -> Result<(), String> {
        while let Some(value) = self.byte()? {
            if !value.is_ascii_whitespace() {
                return Err(format!(
                    "unexpected content after JSON array at line {}",
                    self.line
                ));
            }
        }
        self.finished = true;
        Ok(())
    }
    fn value(&mut self) -> Result<Option<(u64, Value)>, String> {
        if self.finished {
            return Ok(None);
        }
        if !self.started {
            loop {
                match self.byte()? {
                    Some(b) if b.is_ascii_whitespace() => {}
                    Some(b'[') => {
                        self.started = true;
                        break;
                    }
                    _ => return Err("read_json array input must start with '['".to_string()),
                }
            }
        }
        let mut raw = Vec::new();
        let mut depth = 0i32;
        let mut string = false;
        let mut escaped = false;
        let start_line = self.line;
        loop {
            if raw.len() > MAX_VALUE_BYTES {
                return Err(format!(
                    "JSON value starting at line {} exceeds {} bytes",
                    start_line, MAX_VALUE_BYTES
                ));
            }
            let byte = self
                .byte()?
                .ok_or_else(|| "unterminated JSON array".to_string())?;
            if raw.is_empty() && byte.is_ascii_whitespace() {
                continue;
            }
            if !string && depth == 0 && byte == b']' {
                if self.after_comma {
                    return Err(format!(
                        "trailing comma in JSON array at line {}",
                        self.line
                    ));
                }
                self.finish()?;
                return Ok(None);
            }
            if string {
                raw.push(byte);
                if escaped {
                    escaped = false;
                } else if byte == b'\\' {
                    escaped = true;
                } else if byte == b'"' {
                    string = false;
                }
            } else if byte == b'"' {
                string = true;
                raw.push(byte);
            } else if matches!(byte, b'{' | b'[') {
                depth += 1;
                raw.push(byte);
            } else if matches!(byte, b'}' | b']') && depth > 0 {
                depth -= 1;
                raw.push(byte);
            } else if depth == 0 && matches!(byte, b',' | b']') {
                if byte == b']' {
                    self.finish()?;
                    self.after_comma = false;
                } else {
                    self.after_comma = true;
                }
                let value = serde_json::from_slice(&raw)
                    .map_err(|e| format!("invalid JSON value at line {}: {}", start_line, e))?;
                return Ok(Some((start_line, value)));
            } else {
                raw.push(byte);
            }
        }
    }
}

fn infer_json(spec: &ScanSpec) -> Result<Vec<Column>, String> {
    let mut columns = Vec::new();
    let mut indexes = HashMap::new();
    let mut row = 0u64;
    if spec.json_array {
        let mut reader = JsonArrayReader::open(&spec.path)?;
        while let Some((_line, value)) = reader.value()? {
            row += 1;
            add_json_row(value, &mut columns, &mut indexes, row)?;
        }
    } else {
        let input =
            File::open(&spec.path).map_err(|e| format!("cannot open '{}': {}", spec.path, e))?;
        let mut reader = BufReader::new(input);
        let mut line = 0u64;
        while let Some(text) = bounded_line(&mut reader, line + 1)? {
            line += 1;
            if text.trim().is_empty() {
                continue;
            }
            let value = serde_json::from_str(&text)
                .map_err(|e| format!("invalid JSON at line {}: {}", line, e))?;
            row += 1;
            add_json_row(value, &mut columns, &mut indexes, row)?;
        }
    }
    if columns.is_empty() {
        return Err("JSON input has no object fields".to_string());
    }
    for column in &mut columns {
        if column.kind == ScalarType::Null {
            column.kind = ScalarType::Varchar;
            column.nullable = true;
        }
    }
    Ok(columns)
}

fn infer_parquet(spec: &ScanSpec) -> Result<Vec<Column>, String> {
    use parquet::basic::{LogicalType, Type as PhysicalType};
    let reader = SerializedFileReader::new(File::open(&spec.path).map_err(|e| e.to_string())?)
        .map_err(|e| format!("cannot read Parquet footer: {}", e))?;
    let schema = reader.metadata().file_metadata().schema_descr();
    let mut result = Vec::new();
    let mut names = HashSet::new();
    for descriptor in schema.columns() {
        if result.len() >= MAX_COLUMNS {
            return Err(format!("Parquet has more than {} columns", MAX_COLUMNS));
        }
        let parts = descriptor.path().parts();
        if parts.len() != 1 || descriptor.max_rep_level() > 0 {
            return Err(format!(
                "nested Parquet column '{}' is not supported",
                parts.join(".")
            ));
        }
        validate_column_name(&parts[0], "Parquet")?;
        if !names.insert(parts[0].to_ascii_lowercase()) {
            return Err(format!(
                "Parquet column '{}' conflicts case-insensitively with another column",
                parts[0]
            ));
        }
        let kind = match descriptor.logical_type() {
            Some(LogicalType::Date) => ScalarType::Date,
            Some(LogicalType::Timestamp { .. }) => ScalarType::DateTime,
            Some(LogicalType::Integer {
                bit_width: 64,
                is_signed: false,
            }) => ScalarType::Varchar,
            Some(LogicalType::Integer { .. }) => ScalarType::BigInt,
            Some(LogicalType::Time { .. }) => ScalarType::Varchar,
            Some(LogicalType::Float16) => ScalarType::Double,
            Some(LogicalType::String)
            | Some(LogicalType::Enum)
            | Some(LogicalType::Json)
            | Some(LogicalType::Uuid) => ScalarType::Varchar,
            Some(LogicalType::Decimal { .. }) => ScalarType::Double,
            _ => match descriptor.physical_type() {
                PhysicalType::BOOLEAN => ScalarType::Boolean,
                PhysicalType::INT32 | PhysicalType::INT64 => ScalarType::BigInt,
                PhysicalType::FLOAT | PhysicalType::DOUBLE => ScalarType::Double,
                _ => ScalarType::Varchar,
            },
        };
        result.push(Column {
            name: parts[0].clone(),
            kind,
            nullable: descriptor.max_def_level() > 0,
        });
    }
    if result.is_empty() {
        return Err("Parquet file has no scalar columns".to_string());
    }
    Ok(result)
}

fn schema_json(columns: &[Column], id: &Identity) -> String {
    json!({
        "identity": { "length": id.length, "modified_ns": id.modified_ns.to_string() },
        "columns": columns.iter().map(|column| json!({
            "name": column.name, "type": column.kind.sql_name(), "nullable": column.nullable
        })).collect::<Vec<_>>()
    })
    .to_string()
}

pub fn infer_file_schema(spec_json: &str) -> FileScanResult {
    let result = (|| -> Result<String, String> {
        let spec = parse_spec(spec_json)?;
        let id = identity(&spec.path)?;
        let columns = match spec.format.as_str() {
            "csv" => infer_csv(&spec)?,
            "json" | "ndjson" => infer_json(&spec)?,
            "parquet" => infer_parquet(&spec)?,
            _ => return Err(format!("unsupported file format '{}'", spec.format)),
        };
        if identity(&spec.path)? != id {
            return Err(format!(
                "file changed during schema inference: '{}'",
                spec.path
            ));
        }
        Ok(schema_json(&columns, &id))
    })();
    match result {
        Ok(value) => success(value),
        Err(error) => failure(INVALID, error),
    }
}

fn csv_value(text: String, kind: ScalarType, null_text: Option<&str>) -> Value {
    if text.is_empty() || null_text == Some(text.as_str()) {
        return Value::Null;
    }
    match kind {
        ScalarType::Boolean => Value::Bool(text.eq_ignore_ascii_case("true")),
        ScalarType::BigInt => text
            .parse::<i64>()
            .map_or_else(|_| Value::String(text.clone()), |v| Value::Number(v.into())),
        ScalarType::Double => text
            .parse::<f64>()
            .ok()
            .and_then(Number::from_f64)
            .map_or(Value::String(text), Value::Number),
        ScalarType::DateTime if text.as_bytes().get(10) == Some(&b'T') => {
            Value::String(format!("{} {}", &text[..10], &text[11..]))
        }
        _ => Value::String(text),
    }
}

fn civil_date(days_since_epoch: i64) -> String {
    let shifted = days_since_epoch + 719_468;
    let era = if shifted >= 0 {
        shifted
    } else {
        shifted - 146_096
    } / 146_097;
    let day_of_era = shifted - era * 146_097;
    let year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let mut year = year_of_era + era * 400;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_prime = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    let month = month_prime + if month_prime < 10 { 3 } else { -9 };
    if month <= 2 {
        year += 1;
    }
    format!("{:04}-{:02}-{:02}", year, month, day)
}

fn timestamp_text(value: i64, divisor: i64) -> String {
    let seconds = value.div_euclid(divisor);
    let days = seconds.div_euclid(86_400);
    let in_day = seconds.rem_euclid(86_400);
    let mut result = format!(
        "{} {:02}:{:02}:{:02}",
        civil_date(days),
        in_day / 3600,
        (in_day % 3600) / 60,
        in_day % 60
    );
    append_fraction(&mut result, value.rem_euclid(divisor), divisor);
    result
}

fn time_text(value: i64, divisor: i64) -> String {
    let seconds = value.div_euclid(divisor);
    let in_day = seconds.rem_euclid(86_400);
    let mut result = format!(
        "{:02}:{:02}:{:02}",
        in_day / 3600,
        (in_day % 3600) / 60,
        in_day % 60
    );
    append_fraction(&mut result, value.rem_euclid(divisor), divisor);
    result
}

fn append_fraction(result: &mut String, fraction: i64, divisor: i64) {
    if fraction == 0 {
        return;
    }
    let width = if divisor == 1_000 { 3 } else { 6 };
    let mut digits = format!("{:0width$}", fraction, width = width);
    while digits.ends_with('0') {
        digits.pop();
    }
    result.push('.');
    result.push_str(&digits);
}

fn hex_text(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut result = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        result.push(HEX[(byte >> 4) as usize] as char);
        result.push(HEX[(byte & 0x0f) as usize] as char);
    }
    result
}

fn row_json(row: &Row) -> Result<Value, String> {
    let mut object = Map::new();
    for (name, field) in row.get_column_iter() {
        let value = match field {
            Field::Null => Value::Null,
            Field::Bool(v) => Value::Bool(*v),
            Field::Byte(v) => Value::Number((*v as i64).into()),
            Field::Short(v) => Value::Number((*v as i64).into()),
            Field::Int(v) => Value::Number((*v as i64).into()),
            Field::Long(v) => Value::Number((*v).into()),
            Field::UByte(v) => Value::Number((*v as u64).into()),
            Field::UShort(v) => Value::Number((*v as u64).into()),
            Field::UInt(v) => Value::Number((*v as u64).into()),
            Field::ULong(v) => Value::Number((*v).into()),
            Field::Float16(v) => Number::from_f64(f64::from(*v)).map_or(Value::Null, Value::Number),
            Field::Float(v) => Number::from_f64(*v as f64).map_or(Value::Null, Value::Number),
            Field::Double(v) => Number::from_f64(*v).map_or(Value::Null, Value::Number),
            Field::Str(v) => Value::String(v.clone()),
            Field::Bytes(v) => Value::String(hex_text(v.data())),
            Field::Date(v) => Value::String(civil_date(*v as i64)),
            Field::TimeMillis(v) => Value::String(time_text(*v as i64, 1_000)),
            Field::TimeMicros(v) => Value::String(time_text(*v, 1_000_000)),
            Field::TimestampMillis(v) => Value::String(timestamp_text(*v, 1_000)),
            Field::TimestampMicros(v) => Value::String(timestamp_text(*v, 1_000_000)),
            Field::Decimal(_) => Value::String(field.to_string()),
            _ => return Err(format!("nested Parquet field '{}' is not supported", name)),
        };
        object.insert(name.clone(), value);
    }
    Ok(Value::Object(object))
}

fn parquet_projection(
    reader: &SerializedFileReader<File>,
    names: &[String],
) -> Result<Option<Type>, String> {
    if names.is_empty() {
        return Ok(None);
    }
    let root = reader
        .metadata()
        .file_metadata()
        .schema_descr()
        .root_schema();
    let mut fields = Vec::with_capacity(names.len());
    for name in names {
        let field = root
            .get_fields()
            .iter()
            .find(|field| field.name() == name)
            .ok_or_else(|| format!("Parquet projection column '{}' does not exist", name))?;
        fields.push(field.clone());
    }
    Type::group_type_builder(root.name())
        .with_fields(fields)
        .build()
        .map(Some)
        .map_err(|error| format!("invalid Parquet projection: {}", error))
}

enum State {
    Csv { reader: CsvReader, width: usize },
    Ndjson { reader: BufReader<File>, line: u64 },
    JsonArray(JsonArrayReader),
    Parquet { iter: RowIter<'static> },
}

fn append_bounded_row(
    rows: &mut Vec<Value>,
    pending: &mut Option<Value>,
    batch_bytes: &mut usize,
    row: Value,
) -> Result<bool, String> {
    let row_bytes = serde_json::to_vec(&row)
        .map_err(|error| format!("cannot encode file SQL row: {}", error))?
        .len();
    if row_bytes + 2 > MAX_BATCH_BYTES {
        return Err(format!(
            "file SQL row exceeds {} encoded bytes",
            MAX_BATCH_BYTES
        ));
    }
    let separator = usize::from(!rows.is_empty());
    if !rows.is_empty() && *batch_bytes + separator + row_bytes > MAX_BATCH_BYTES {
        *pending = Some(row);
        Ok(false)
    } else {
        *batch_bytes += separator + row_bytes;
        rows.push(row);
        Ok(true)
    }
}

pub struct FileScan {
    spec: Option<ScanSpec>,
    state: Option<State>,
    pending: Option<Value>,
    error: Option<String>,
}

impl FileScan {
    fn build(spec: ScanSpec) -> Result<Self, String> {
        check_identity(&spec)?;
        if spec.columns.is_empty() {
            return Err("scan descriptor has no columns".to_string());
        }
        let state = match spec.format.as_str() {
            "csv" => {
                let mut reader = CsvReader::open(&spec)?;
                let width = spec.columns.len();
                if spec.header {
                    reader
                        .record()?
                        .ok_or_else(|| "CSV file is empty".to_string())?;
                }
                State::Csv { reader, width }
            }
            "json" | "ndjson" if spec.json_array => {
                State::JsonArray(JsonArrayReader::open(&spec.path)?)
            }
            "json" | "ndjson" => State::Ndjson {
                reader: BufReader::new(File::open(&spec.path).map_err(|e| e.to_string())?),
                line: 0,
            },
            "parquet" => {
                let reader =
                    SerializedFileReader::new(File::open(&spec.path).map_err(|e| e.to_string())?)
                        .map_err(|e| e.to_string())?;
                let projection = parquet_projection(&reader, &spec.projection)?;
                State::Parquet {
                    iter: RowIter::from_file_into(Box::new(reader))
                        .project(projection)
                        .map_err(|error| format!("cannot apply Parquet projection: {}", error))?
                        .with_batch_size(256),
                }
            }
            _ => return Err(format!("unsupported file format '{}'", spec.format)),
        };
        Ok(Self {
            spec: Some(spec),
            state: Some(state),
            pending: None,
            error: None,
        })
    }

    fn next(&mut self, max_rows: usize) -> Result<Vec<Value>, String> {
        let spec = self
            .spec
            .as_ref()
            .ok_or_else(|| "scan is not initialized".to_string())?;
        check_identity(spec)?;
        let mut rows = Vec::with_capacity(max_rows.min(1024));
        let mut batch_bytes = 2usize;
        if let Some(row) = self.pending.take() {
            append_bounded_row(&mut rows, &mut self.pending, &mut batch_bytes, row)?;
        }
        let pending = &mut self.pending;
        match self
            .state
            .as_mut()
            .ok_or_else(|| "scan is not initialized".to_string())?
        {
            State::Csv { reader, width } => {
                while rows.len() < max_rows {
                    let Some((line, fields)) = reader.record()? else {
                        break;
                    };
                    if fields.len() != *width {
                        return Err(format!(
                            "CSV line {} has {} fields; expected {}",
                            line,
                            fields.len(),
                            width
                        ));
                    }
                    let mut object = Map::new();
                    for (column, field) in spec.columns.iter().zip(fields) {
                        object.insert(
                            column.name.clone(),
                            csv_value(field, column.kind, spec.null_text.as_deref()),
                        );
                    }
                    if !append_bounded_row(
                        &mut rows,
                        pending,
                        &mut batch_bytes,
                        Value::Object(object),
                    )? {
                        break;
                    }
                }
            }
            State::Ndjson { reader, line } => {
                while rows.len() < max_rows {
                    let Some(text) = bounded_line(reader, *line + 1)? else {
                        break;
                    };
                    *line += 1;
                    if text.trim().is_empty() {
                        continue;
                    }
                    let value: Value = serde_json::from_str(&text)
                        .map_err(|e| format!("invalid JSON at line {}: {}", line, e))?;
                    if !value.is_object() {
                        return Err(format!("JSON line {} is not an object", line));
                    }
                    if !append_bounded_row(&mut rows, pending, &mut batch_bytes, value)? {
                        break;
                    }
                }
            }
            State::JsonArray(reader) => {
                while rows.len() < max_rows {
                    let Some((line, value)) = reader.value()? else {
                        break;
                    };
                    if !value.is_object() {
                        return Err(format!("JSON value at line {} is not an object", line));
                    }
                    if !append_bounded_row(&mut rows, pending, &mut batch_bytes, value)? {
                        break;
                    }
                }
            }
            State::Parquet { iter } => {
                while rows.len() < max_rows {
                    let Some(row) = iter.next() else {
                        break;
                    };
                    let value = row_json(&row.map_err(|e| e.to_string())?)?;
                    if !append_bounded_row(&mut rows, pending, &mut batch_bytes, value)? {
                        break;
                    }
                }
            }
        }
        // Detect a replacement or write that raced with this batch.  Returning an
        // error is preferable to exposing a batch assembled from two file states.
        check_identity(spec)?;
        Ok(rows)
    }
}

pub fn open_file_scan(spec_json: &str) -> Box<FileScan> {
    match parse_spec(spec_json).and_then(FileScan::build) {
        Ok(scan) => Box::new(scan),
        Err(error) => Box::new(FileScan {
            spec: None,
            state: None,
            pending: None,
            error: Some(error),
        }),
    }
}

pub fn file_scan_status(scan: &FileScan) -> FileScanResult {
    match &scan.error {
        Some(error) => failure(INVALID, error.clone()),
        None => success(String::new()),
    }
}

pub fn file_scan_next(scan: &mut FileScan, max_rows: u32) -> FileScanResult {
    if max_rows == 0 {
        return failure(INVALID, "batch size must be positive");
    }
    if let Some(error) = &scan.error {
        return failure(INVALID, error.clone());
    }
    match scan.next(max_rows as usize) {
        Ok(rows) if rows.is_empty() => failure(END, "end of file"),
        Ok(rows) => success(Value::Array(rows).to_string()),
        Err(error) if error.contains("file changed") => failure(CHANGED, error),
        Err(error) => failure(IO, error),
    }
}

pub fn file_scan_rewind(scan: &mut FileScan) -> FileScanResult {
    let Some(spec) = scan.spec.clone() else {
        return file_scan_status(scan);
    };
    match FileScan::build(spec) {
        Ok(new_scan) => {
            *scan = new_scan;
            success(String::new())
        }
        Err(error) => failure(IO, error),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use parquet::basic::Compression;
    use parquet::data_type::Int64Type;
    use parquet::file::properties::WriterProperties;
    use parquet::file::writer::SerializedFileWriter;
    use parquet::schema::parser::parse_message_type;
    use std::sync::Arc;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn fixture(suffix: &str, contents: &str) -> String {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!(
            "seekdb_file_scan_{}_{}_{}",
            std::process::id(),
            nonce,
            suffix
        ));
        fs::write(&path, contents).unwrap();
        path.to_string_lossy().into_owned()
    }

    fn spec(path: String, format: &str) -> ScanSpec {
        ScanSpec {
            format: format.to_string(),
            path,
            header: true,
            delimiter: b',',
            quote: b'"',
            escape: None,
            null_text: None,
            json_array: false,
            identity: None,
            columns: Vec::new(),
            projection: Vec::new(),
        }
    }

    #[test]
    fn csv_inference_handles_multiline_quotes_and_type_widening() {
        let path = fixture(
            "quoted.csv",
            "id,id,score,note,day\r\n1,9,2,\"hello\r\nworld\",2024-02-29\r\n2,8,2.5,,bad\r\n",
        );
        let scan_spec = spec(path.clone(), "csv");
        let mut reader = CsvReader::open(&scan_spec).unwrap();
        reader.record().unwrap().unwrap();
        let (_, first_row) = reader.record().unwrap().unwrap();
        assert_eq!(first_row[3], "hello\nworld");
        let columns = infer_csv(&scan_spec).unwrap();
        assert_eq!(
            columns.iter().map(|c| c.name.as_str()).collect::<Vec<_>>(),
            vec!["id", "id__2", "score", "note", "day"]
        );
        assert_eq!(columns[2].kind, ScalarType::Double);
        assert_eq!(columns[3].kind, ScalarType::Varchar);
        assert!(columns[3].nullable);
        assert_eq!(columns[4].kind, ScalarType::Varchar);
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn csv_reports_the_bad_logical_record() {
        let path = fixture("bad.csv", "a,b\n1,2\n3\n");
        let error = infer_csv(&spec(path.clone(), "csv")).unwrap_err();
        assert!(error.contains("line 3"), "{error}");
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn json_union_schema_rejects_nested_values() {
        let path = fixture(
            "nested.jsonl",
            "{\"id\":1}\n{\"id\":2,\"x\":{\"nested\":true}}\n",
        );
        let error = infer_json(&spec(path.clone(), "ndjson")).unwrap_err();
        assert!(
            error.contains("record 2") && error.contains("nested"),
            "{error}"
        );
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn json_union_marks_missing_fields_and_widens_numbers() {
        let path = fixture(
            "union.jsonl",
            "{\"id\":1,\"left\":true}\n{\"id\":2.5,\"right\":\"x\"}\n",
        );
        let columns = infer_json(&spec(path.clone(), "ndjson")).unwrap();
        assert_eq!(columns[0].name, "id");
        assert_eq!(columns[0].kind, ScalarType::Double);
        assert!(!columns[0].nullable);
        assert!(columns[1].nullable);
        assert!(columns[2].nullable);
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn json_array_rejects_trailing_comma_and_content() {
        for contents in ["[{\"id\":1},]", "[{\"id\":1}] trailing"] {
            let path = fixture("bad_array.json", contents);
            let mut scan_spec = spec(path.clone(), "json");
            scan_spec.json_array = true;
            assert!(infer_json(&scan_spec).is_err());
            fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn cursor_batches_and_detects_file_changes() {
        let path = fixture("batch.csv", "id,name\n1,a\n2,b\n3,c\n");
        let mut scan_spec = spec(path.clone(), "csv");
        scan_spec.columns = infer_csv(&scan_spec).unwrap();
        scan_spec.identity = Some(identity(&path).unwrap());
        let mut scan = FileScan::build(scan_spec).unwrap();
        assert_eq!(scan.next(2).unwrap().len(), 2);
        assert_eq!(scan.next(2).unwrap().len(), 1);
        assert!(scan.next(2).unwrap().is_empty());
        fs::write(&path, "id,name\n4,d\n").unwrap();
        assert!(scan.next(1).unwrap_err().contains("file changed"));
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn parquet_scans_multiple_snappy_row_groups_in_batches() {
        let path = fixture("groups.parquet", "");
        let schema = Arc::new(parse_message_type("message schema { OPTIONAL INT64 c1; }").unwrap());
        let properties = Arc::new(
            WriterProperties::builder()
                .set_compression(Compression::SNAPPY)
                .build(),
        );
        let mut writer =
            SerializedFileWriter::new(File::create(&path).unwrap(), schema, properties).unwrap();
        for values in [&[1_i64, 2][..], &[3_i64, 4, 5][..]] {
            let mut group = writer.next_row_group().unwrap();
            let mut column = group.next_column().unwrap().unwrap();
            column
                .typed::<Int64Type>()
                .write_batch(values, Some(&vec![1_i16; values.len()]), None)
                .unwrap();
            column.close().unwrap();
            group.close().unwrap();
        }
        writer.close().unwrap();

        let mut scan_spec = spec(path.clone(), "parquet");
        scan_spec.columns = infer_parquet(&scan_spec).unwrap();
        scan_spec.identity = Some(identity(&path).unwrap());
        let mut scan = FileScan::build(scan_spec).unwrap();
        let first = scan.next(2).unwrap();
        let second = scan.next(2).unwrap();
        let third = scan.next(2).unwrap();
        assert_eq!(first[0]["c1"], 1);
        assert_eq!(first[1]["c1"], 2);
        assert_eq!(second[0]["c1"], 3);
        assert_eq!(second[1]["c1"], 4);
        assert_eq!(third[0]["c1"], 5);
        assert!(scan.next(2).unwrap().is_empty());
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn parquet_projection_only_decodes_requested_columns() {
        let path = fixture("projection.parquet", "");
        let schema = Arc::new(
            parse_message_type(
                "message schema { REQUIRED INT64 ignored; REQUIRED INT64 selected; }",
            )
            .unwrap(),
        );
        let properties = Arc::new(WriterProperties::builder().build());
        let mut writer =
            SerializedFileWriter::new(File::create(&path).unwrap(), schema, properties).unwrap();
        let mut group = writer.next_row_group().unwrap();
        for values in [&[10_i64, 20][..], &[1_i64, 2][..]] {
            let mut column = group.next_column().unwrap().unwrap();
            column
                .typed::<Int64Type>()
                .write_batch(values, None, None)
                .unwrap();
            column.close().unwrap();
        }
        group.close().unwrap();
        writer.close().unwrap();

        let mut scan_spec = spec(path.clone(), "parquet");
        scan_spec.columns = infer_parquet(&scan_spec).unwrap();
        scan_spec.projection = vec!["selected".to_string()];
        let mut scan = FileScan::build(scan_spec).unwrap();
        let rows = scan.next(10).unwrap();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0]["selected"], 1);
        assert_eq!(rows[1]["selected"], 2);
        assert!(rows[0].get("ignored").is_none());
        fs::remove_file(path).unwrap();
    }

    #[test]
    fn parquet_temporal_text_preserves_fraction_and_negative_epoch() {
        assert_eq!(timestamp_text(1_234, 1_000), "1970-01-01 00:00:01.234");
        assert_eq!(timestamp_text(-1, 1_000), "1969-12-31 23:59:59.999");
        assert_eq!(time_text(1_230_000, 1_000_000), "00:00:01.23");
    }
}
