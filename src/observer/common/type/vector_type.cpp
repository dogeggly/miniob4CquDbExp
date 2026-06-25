/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cmath>
#include <cstdlib>

#include "common/lang/vector.h"
#include "common/lang/sstream.h"
#include "common/log/log.h"
#include "common/type/vector_type.h"
#include "common/value.h"

int VectorType::compare(const Value &left, const Value &right) const
{
  ASSERT(left.attr_type() == AttrType::VECTORS && right.attr_type() == AttrType::VECTORS, "invalid type");

  // 维度不同，不相等
  if (left.length_ != right.length_) {
    return -1;
  }

  const float *lv = reinterpret_cast<const float *>(left.value_.pointer_value_);
  const float *rv = reinterpret_cast<const float *>(right.value_.pointer_value_);

  for (int i = 0; i < left.length_; i++) {
    if (std::fabs(lv[i] - rv[i]) > 1e-6f) {
      return (lv[i] < rv[i]) ? -1 : 1;
    }
  }
  return 0;
}

RC VectorType::to_string(const Value &val, string &result) const
{
  stringstream ss;
  const float *data = reinterpret_cast<const float *>(val.value_.pointer_value_);
  ss << "[";
  for (int i = 0; i < val.length_; i++) {
    if (i > 0) {
      ss << ",";
    }
    ss << data[i];
  }
  ss << "]";
  result = ss.str();
  return RC::SUCCESS;
}

RC VectorType::set_value_from_str(Value &val, const string &data) const
{
  // 期望格式: [1,2,3] 或 [1.0,2.0,3.0]
  string s = data;
  // 去除前后空白
  size_t start = s.find_first_not_of(" \t\n\r");
  if (start == string::npos) {
    return RC::INVALID_ARGUMENT;
  }
  size_t end = s.find_last_not_of(" \t\n\r");
  s = s.substr(start, end - start + 1);

  // 必须以 '[' 开头，以 ']' 结尾
  if (s.empty() || s[0] != '[' || s.back() != ']') {
    return RC::INVALID_ARGUMENT;
  }

  // 去除 '[' 和 ']'
  string content = s.substr(1, s.size() - 2);

  // 统计逗号数量来预判维度
  vector<float> elements;
  size_t pos = 0;
  while (pos < content.size()) {
    // 跳过空白
    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) {
      pos++;
    }
    if (pos >= content.size()) {
      break;
    }

    // 找数字结尾
    size_t num_end = pos;
    while (num_end < content.size() && content[num_end] != ',') {
      num_end++;
    }

    string num_str = content.substr(pos, num_end - pos);
    // 去除尾部空白
    size_t ns_end = num_str.find_last_not_of(" \t");
    if (ns_end != string::npos) {
      num_str = num_str.substr(0, ns_end + 1);
    }

    if (num_str.empty()) {
      pos = num_end + 1;
      continue;
    }

    char *endptr = nullptr;
    float val_f  = strtof(num_str.c_str(), &endptr);
    if (endptr != num_str.c_str() + num_str.size()) {
      LOG_WARN("invalid float in vector: %s", num_str.c_str());
      return RC::INVALID_ARGUMENT;
    }
    elements.push_back(val_f);

    pos = num_end + 1;  // 跳过逗号
  }

  if (elements.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  val.set_vector(elements.data(), static_cast<int>(elements.size()));
  return RC::SUCCESS;
}

int VectorType::cast_cost(AttrType type)
{
  if (type == AttrType::VECTORS) {
    return 0;
  }
  return INT32_MAX;
}
