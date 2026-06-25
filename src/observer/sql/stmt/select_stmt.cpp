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
// Created by Wangyunlai on 2022/6/6.
//

#include "sql/stmt/select_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"

using namespace std;
using namespace common;

SelectStmt::~SelectStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC SelectStmt::create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  BinderContext binder_context;

  // collect tables in `from` statement
  vector<Table *>                tables;
  unordered_map<string, Table *> table_map;
  for (size_t i = 0; i < select_sql.relations.size(); i++) {
    const char *table_name = select_sql.relations[i].c_str();
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    binder_context.add_table(table);
    tables.push_back(table);
    table_map.insert({table_name, table});
  }

  // collect query fields in `select` statement
  vector<unique_ptr<Expression>> bound_expressions;
  ExpressionBinder expression_binder(binder_context);
  
  for (unique_ptr<Expression> &expression : select_sql.expressions) {
    RC rc = expression_binder.bind_expression(expression, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  vector<unique_ptr<Expression>> group_by_expressions;
  for (unique_ptr<Expression> &expression : select_sql.group_by) {
    RC rc = expression_binder.bind_expression(expression, group_by_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  Table *default_table = nullptr;
  if (tables.size() == 1) {
    default_table = tables[0];
  }

  // create filter statement in `where` statement
  FilterStmt *filter_stmt = nullptr;
  RC          rc          = FilterStmt::create(db,
      default_table,
      &table_map,
      select_sql.conditions.data(),
      static_cast<int>(select_sql.conditions.size()),
      filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("cannot construct filter stmt");
    return rc;
  }

  // build alias map from SELECT expressions for ORDER BY resolution
  unordered_map<string, unique_ptr<Expression> *> alias_map;
  for (size_t i = 0; i < bound_expressions.size(); i++) {
    const char *name = bound_expressions[i]->name();
    if (name != nullptr && strlen(name) > 0) {
      alias_map[name] = &bound_expressions[i];
    }
  }

  // bind ORDER BY expressions
  vector<unique_ptr<Expression>> order_by_expressions;
  vector<bool>                   order_by_asc;
  for (auto &order_item : select_sql.order_by) {
    bool matched_alias = false;
    if (order_item.expression->type() == ExprType::UNBOUND_FIELD) {
      auto       unbound    = static_cast<UnboundFieldExpr *>(order_item.expression.get());
      const char *table_name = unbound->table_name();
      const char *field_name = unbound->field_name();

      // only match aliases when no explicit table name is given
      if (common::is_blank(table_name)) {
        auto it = alias_map.find(field_name);
        if (it != alias_map.end()) {
          // replace with a copy of the SELECT expression
          order_by_expressions.emplace_back((*it->second)->copy());
          order_by_asc.push_back(order_item.is_asc);
          matched_alias = true;
        }
      }
    }

    if (!matched_alias) {
      // normal binding against table context
      RC rc = expression_binder.bind_expression(order_item.expression, order_by_expressions);
      if (OB_FAIL(rc)) {
        LOG_INFO("bind order by expression failed. rc=%s", strrc(rc));
        if (filter_stmt != nullptr) {
          delete filter_stmt;
        }
        return rc;
      }
      // each ORDER BY item should produce exactly one expression after binding
      while (order_by_asc.size() < order_by_expressions.size()) {
        order_by_asc.push_back(order_item.is_asc);
      }
    }
  }

  // everything alright
  SelectStmt *select_stmt = new SelectStmt();

  select_stmt->tables_.swap(tables);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->group_by_.swap(group_by_expressions);
  select_stmt->order_by_.swap(order_by_expressions);
  select_stmt->order_by_asc_ = std::move(order_by_asc);
  stmt                      = select_stmt;
  return RC::SUCCESS;
}
