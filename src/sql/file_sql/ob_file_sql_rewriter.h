/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <string>

#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{
class ObBasicSessionInfo;

class FileSqlRewriter
{
public:
  static int rewrite(const common::ObString &sql,
                     const ObBasicSessionInfo &session,
                     std::string &rewritten_sql,
                     bool &rewritten);
};

} // namespace sql
} // namespace oceanbase
