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

#include "sql/expr/tuple.h"
#include "sql/operator/physical_operator.h"
#include "storage/record/record_manager.h"

class Index;
class Table;

/**
 * @brief 向量索引扫描物理算子
 * @ingroup PhysicalOperator
 * @details 使用 IVF Flat 向量索引进行近似最近邻搜索。
 * 在 open() 时调用索引的 ann_search() 获取候选 RID 列表，
 * 然后逐个获取记录并通过 next() 返回。
 */
class VectorIndexScanPhysicalOperator : public PhysicalOperator
{
public:
  VectorIndexScanPhysicalOperator(Table *table, Index *index, const vector<float> &query_vector, int limit);

  virtual ~VectorIndexScanPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::VECTOR_INDEX_SCAN; }

  string param() const override;

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;

  void set_predicates(vector<unique_ptr<Expression>> &&exprs);

private:
  RC filter(RowTuple &tuple, bool &result);

private:
  Trx           *trx_   = nullptr;
  Table         *table_ = nullptr;
  Index         *index_ = nullptr;
  vector<float>  query_vector_;
  int            limit_;

  /// ANN 搜索结果 RID 列表
  vector<RID> candidate_rids_;
  size_t      current_idx_ = 0;

  Record   current_record_;
  RowTuple tuple_;

  vector<unique_ptr<Expression>> predicates_;
};
