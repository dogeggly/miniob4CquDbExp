/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/limit_physical_operator.h"

LimitPhysicalOperator::LimitPhysicalOperator(int limit_num) : limit_num_(limit_num) {}

RC LimitPhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::INVALID_ARGUMENT;
  }
  count_ = 0;
  return children_[0]->open(trx);
}

RC LimitPhysicalOperator::next()
{
  if (limit_num_ >= 0 && count_ >= limit_num_) {
    return RC::RECORD_EOF;
  }

  RC rc = children_[0]->next();
  if (rc == RC::SUCCESS) {
    count_++;
    child_tuple_ = children_[0]->current_tuple();
  }
  return rc;
}

RC LimitPhysicalOperator::close()
{
  count_ = 0;
  if (!children_.empty()) {
    return children_[0]->close();
  }
  return RC::SUCCESS;
}

Tuple *LimitPhysicalOperator::current_tuple()
{
  return child_tuple_;
}
