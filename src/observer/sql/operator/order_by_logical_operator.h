/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by dogeggly on 2025/06/25.
//

#pragma once

#include "sql/expr/expression.h"
#include "sql/operator/logical_operator.h"

/**
 * @brief ORDER BY 逻辑算子
 * @ingroup LogicalOperator
 * @details 对输入元组按指定表达式排序。
 * 排序表达式存储在基类 expressions_ 中，order_by_asc_ 按索引对应。
 */
class OrderByLogicalOperator : public LogicalOperator
{
public:
  OrderByLogicalOperator(vector<unique_ptr<Expression>> &&order_by_expressions, vector<bool> &&order_by_asc);
  virtual ~OrderByLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::ORDER_BY; }
  OpType              get_op_type() const override { return OpType::ORDERBY; }

  const vector<bool> &order_by_asc() const { return order_by_asc_; }

private:
  vector<bool> order_by_asc_;
};
