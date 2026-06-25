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
// Created by Wangyunlai on 2023/4/25.
//

#pragma once

#include "sql/stmt/stmt.h"

struct CreateIndexSqlNode;
class Table;
class FieldMeta;

/**
 * @brief 创建索引的语句
 * @ingroup Statement
 */
class CreateIndexStmt : public Stmt
{
public:
  CreateIndexStmt(Table *table, const FieldMeta *field_meta, const string &index_name)
      : table_(table), field_meta_(field_meta), index_name_(index_name)
  {}

  virtual ~CreateIndexStmt() = default;

  StmtType type() const override
  {
    return is_vector_index_ ? StmtType::CREATE_VECTOR_INDEX : StmtType::CREATE_INDEX;
  }

  Table           *table() const { return table_; }
  const FieldMeta *field_meta() const { return field_meta_; }
  const string    &index_name() const { return index_name_; }

  // 向量索引相关属性
  bool        is_vector_index() const { return is_vector_index_; }
  const char *distance_method() const { return distance_method_.c_str(); }
  const char *index_type_str() const { return index_type_str_.c_str(); }
  int         lists() const { return lists_; }
  int         probes() const { return probes_; }

  void set_vector_options(const char *index_type_str, const char *distance_method, int lists, int probes)
  {
    is_vector_index_ = true;
    index_type_str_  = index_type_str;
    distance_method_ = distance_method;
    lists_           = lists;
    probes_          = probes;
  }

public:
  static RC create(Db *db, const CreateIndexSqlNode &create_index, bool is_vector_command, Stmt *&stmt);

private:
  Table           *table_      = nullptr;
  const FieldMeta *field_meta_ = nullptr;
  string           index_name_;

  // 向量索引专用属性
  bool   is_vector_index_ = false;
  string index_type_str_;
  string distance_method_;
  int    lists_  = 0;
  int    probes_ = 0;
};
