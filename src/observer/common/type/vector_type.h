/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/type/data_type.h"

/**
 * @brief 向量类型
 * @ingroup DataType
 * @details 向量类型的值存储为 float 数组，length_ 表示向量的维度（元素个数）。
 * 向量仅支持与同类型向量进行等值比较（=, !=），不支持大小比较和算术运算。
 * 默认维度为 2048，最大维度为 16383。
 */
class VectorType : public DataType
{
public:
  VectorType() : DataType(AttrType::VECTORS) {}
  virtual ~VectorType() = default;

  /// 比较两个向量：逐元素比较（使用浮点 epsilon），全部相等则返回 0
  int compare(const Value &left, const Value &right) const override;

  /// 向量不支持算术运算
  RC add(const Value &left, const Value &right, Value &result) const override { return RC::UNSUPPORTED; }
  RC subtract(const Value &left, const Value &right, Value &result) const override { return RC::UNSUPPORTED; }
  RC multiply(const Value &left, const Value &right, Value &result) const override { return RC::UNSUPPORTED; }

  /// 将向量转换为字符串，格式为 [1.0,2.0,3.0]
  RC to_string(const Value &val, string &result) const override;

  /// 从字符串设置向量值，支持 "[1,2,3]" 格式
  RC set_value_from_str(Value &val, const string &data) const override;

  /// 隐式转换代价：仅支持 VECTOR -> VECTOR
  int cast_cost(AttrType type) override;

  /// 默认向量维度
  static constexpr int DEFAULT_DIM = 2048;
  /// 最大向量维度
  static constexpr int MAX_DIM = 16383;
};