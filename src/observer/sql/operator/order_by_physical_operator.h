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

#include "sql/operator/physical_operator.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"

/**
 * @brief ORDER BY 物理算子
 * @ingroup PhysicalOperator
 * @details 读取所有子算子元组，按 ORDER BY 表达式排序后逐行输出。
 * 排序在 open() 阶段完成，next() 按排序顺序返回。
 */
class OrderByPhysicalOperator : public PhysicalOperator
{
public:
  OrderByPhysicalOperator(vector<unique_ptr<Expression>> &&order_by_expressions, vector<bool> &&order_by_asc);

  virtual ~OrderByPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::ORDER_BY; }
  OpType               get_op_type() const override { return OpType::ORDERBY; }

  RC    open(Trx *trx) override;
  RC    next() override;
  RC    close() override;
  Tuple *current_tuple() override;

private:
  vector<unique_ptr<Expression>> order_by_expressions_;
  vector<bool>                   order_by_asc_;

  vector<ValueListTuple> sorted_tuples_;
  int                    current_index_ = -1;
  ValueListTuple         current_tuple_;
};
