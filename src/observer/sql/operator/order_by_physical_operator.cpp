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

#include "sql/operator/order_by_physical_operator.h"
#include "common/log/log.h"
#include <algorithm>

using namespace std;

OrderByPhysicalOperator::OrderByPhysicalOperator(
    vector<unique_ptr<Expression>> &&order_by_expressions, vector<bool> &&order_by_asc)
    : order_by_expressions_(std::move(order_by_expressions)), order_by_asc_(std::move(order_by_asc))
{}

RC OrderByPhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }

  PhysicalOperator *child = children_[0].get();
  RC                rc    = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  // 读取所有元组
  vector<ValueListTuple> all_tuples;
  while (true) {
    rc = child->next();
    if (rc == RC::RECORD_EOF) {
      break;
    }
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get next tuple from child: %s", strrc(rc));
      return rc;
    }

    Tuple *tuple = child->current_tuple();
    if (tuple == nullptr) {
      LOG_WARN("child returned null tuple");
      return RC::INTERNAL;
    }

    ValueListTuple value_tuple;
    rc = ValueListTuple::make(*tuple, value_tuple);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to make value list tuple: %s", strrc(rc));
      return rc;
    }

    all_tuples.emplace_back(std::move(value_tuple));
  }

  // 排序
  if (!order_by_expressions_.empty() && all_tuples.size() > 1) {
    // 获取排序键：对每个元组求值所有 ORDER BY 表达式
    vector<vector<Value>> sort_keys(all_tuples.size());
    for (size_t i = 0; i < all_tuples.size(); i++) {
      sort_keys[i].reserve(order_by_expressions_.size());
      for (size_t j = 0; j < order_by_expressions_.size(); j++) {
        Value val;
        RC    key_rc = order_by_expressions_[j]->get_value(all_tuples[i], val);
        if (key_rc != RC::SUCCESS) {
          // 如果求值失败，用一个 NULL 类型的值代替，排在最后
          val.set_type(AttrType::UNDEFINED);
        }
        sort_keys[i].emplace_back(val);
      }
    }

    // 构建索引数组并排序
    vector<size_t> indices(all_tuples.size());
    for (size_t i = 0; i < all_tuples.size(); i++) {
      indices[i] = i;
    }

    auto comparator = [&](size_t a, size_t b) -> bool {
      for (size_t j = 0; j < order_by_expressions_.size(); j++) {
        auto &va = sort_keys[a][j];
        auto &vb = sort_keys[b][j];
        // UNDEFINED 表示求值失败（如零向量余弦距离），排在所有有效值之后
        bool a_undef = (va.attr_type() == AttrType::UNDEFINED);
        bool b_undef = (vb.attr_type() == AttrType::UNDEFINED);
        if (a_undef && b_undef) {
          continue;
        }
        if (a_undef) {
          return false;  // a 未定义，b 排在前面
        }
        if (b_undef) {
          return true;   // b 未定义，a 排在前面
        }
        int cmp = va.compare(vb);
        if (cmp != 0) {
          return order_by_asc_[j] ? cmp < 0 : cmp > 0;
        }
      }
      return false;  // 完全相等，保持原顺序
    };

    std::sort(indices.begin(), indices.end(), comparator);

    // 按排序后的索引重新排列
    vector<ValueListTuple> sorted(all_tuples.size());
    for (size_t i = 0; i < all_tuples.size(); i++) {
      sorted[i] = std::move(all_tuples[indices[i]]);
    }
    sorted_tuples_ = std::move(sorted);
  } else {
    sorted_tuples_ = std::move(all_tuples);
  }

  current_index_ = -1;
  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::next()
{
  current_index_++;
  if (current_index_ >= static_cast<int>(sorted_tuples_.size())) {
    return RC::RECORD_EOF;
  }
  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::close()
{
  sorted_tuples_.clear();
  current_index_ = -1;
  if (!children_.empty()) {
    children_[0]->close();
  }
  return RC::SUCCESS;
}

Tuple *OrderByPhysicalOperator::current_tuple()
{
  if (current_index_ < 0 || current_index_ >= static_cast<int>(sorted_tuples_.size())) {
    return nullptr;
  }
  current_tuple_ = sorted_tuples_[current_index_];
  return &current_tuple_;
}
