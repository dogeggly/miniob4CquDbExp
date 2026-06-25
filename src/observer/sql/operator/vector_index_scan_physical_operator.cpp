/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/vector_index_scan_physical_operator.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "storage/index/ivfflat_index.h"
#include "storage/record/record.h"
#include "storage/table/table.h"

using namespace std;

VectorIndexScanPhysicalOperator::VectorIndexScanPhysicalOperator(
    Table *table, Index *index, const vector<float> &query_vector, int limit)
    : table_(table), index_(index), query_vector_(query_vector), limit_(limit)
{}

string VectorIndexScanPhysicalOperator::param() const
{
  return string("vector_index_scan(limit=") + to_string(limit_) + ")";
}

RC VectorIndexScanPhysicalOperator::open(Trx *trx)
{
  trx_ = trx;

  if (nullptr == table_ || nullptr == index_) {
    LOG_WARN("vector index scan: table or index is null");
    return RC::INTERNAL;
  }

  // 通过 IVF Flat 索引执行 ANN 搜索
  auto *ivfflat = dynamic_cast<IvfflatIndex *>(index_);
  if (ivfflat == nullptr) {
    LOG_WARN("vector index scan: index is not IvfflatIndex");
    return RC::INTERNAL;
  }

  size_t search_limit = limit_ > 0 ? static_cast<size_t>(limit_) : 100;
  candidate_rids_     = ivfflat->ann_search(query_vector_, search_limit);
  current_idx_        = 0;

  LOG_TRACE("vector index scan: found %zu candidates", candidate_rids_.size());

  // 设置 tuple schema
  tuple_.set_schema(table_, table_->table_meta().field_metas());

  return RC::SUCCESS;
}

RC VectorIndexScanPhysicalOperator::next()
{
  while (current_idx_ < candidate_rids_.size()) {
    RID rid = candidate_rids_[current_idx_++];

    RC rc = table_->get_record(rid, current_record_);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get record by rid: %s", rid.to_string().c_str());
      continue;
    }

    tuple_.set_record(&current_record_);

    // 应用额外谓词过滤
    bool result = true;
    rc          = filter(tuple_, result);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    if (result) {
      return RC::SUCCESS;
    }
  }

  return RC::RECORD_EOF;
}

RC VectorIndexScanPhysicalOperator::close()
{
  candidate_rids_.clear();
  current_idx_ = 0;
  return RC::SUCCESS;
}

Tuple *VectorIndexScanPhysicalOperator::current_tuple()
{
  return &tuple_;
}

void VectorIndexScanPhysicalOperator::set_predicates(vector<unique_ptr<Expression>> &&exprs)
{
  predicates_ = std::move(exprs);
}

RC VectorIndexScanPhysicalOperator::filter(RowTuple &tuple, bool &result)
{
  result = true;
  for (auto &expr : predicates_) {
    Value value;
    RC    rc = expr->get_value(tuple, value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    if (!value.get_boolean()) {
      result = false;
      return RC::SUCCESS;
    }
  }
  return RC::SUCCESS;
}
