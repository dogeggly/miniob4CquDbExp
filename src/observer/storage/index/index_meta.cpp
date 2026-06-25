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
// Created by Wangyunlai.wyl on 2021/5/18.
//

#include "storage/index/index_meta.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/field/field_meta.h"
#include "storage/table/table_meta.h"
#include "json/json.h"

const static Json::StaticString FIELD_NAME("name");
const static Json::StaticString FIELD_FIELD_NAME("field_name");
const static Json::StaticString FIELD_INDEX_TYPE("index_type");
const static Json::StaticString FIELD_DISTANCE_METHOD("distance_method");
const static Json::StaticString FIELD_LISTS("lists");
const static Json::StaticString FIELD_PROBES("probes");

static const Json::StaticString INDEX_TYPE_BPLUS_TREE("bplus_tree");
static const Json::StaticString INDEX_TYPE_IVFFLAT("ivfflat");

RC IndexMeta::init(const char *name, const FieldMeta &field)
{
  return init(name, field, IndexType::BPLUS_TREE);
}

RC IndexMeta::init(const char *name, const FieldMeta &field, IndexType type)
{
  if (common::is_blank(name)) {
    LOG_ERROR("Failed to init index, name is empty.");
    return RC::INVALID_ARGUMENT;
  }

  name_       = name;
  field_      = field.name();
  index_type_ = type;
  return RC::SUCCESS;
}

void IndexMeta::to_json(Json::Value &json_value) const
{
  json_value[FIELD_NAME]       = name_;
  json_value[FIELD_FIELD_NAME] = field_;

  // 索引类型
  switch (index_type_) {
    case IndexType::BPLUS_TREE: json_value[FIELD_INDEX_TYPE] = INDEX_TYPE_BPLUS_TREE; break;
    case IndexType::IVFFLAT: json_value[FIELD_INDEX_TYPE] = INDEX_TYPE_IVFFLAT; break;
  }

  // 向量索引属性
  if (!distance_method_.empty()) {
    json_value[FIELD_DISTANCE_METHOD] = distance_method_;
  }
  if (lists_ > 0) {
    json_value[FIELD_LISTS] = lists_;
  }
  if (probes_ > 0) {
    json_value[FIELD_PROBES] = probes_;
  }
}

RC IndexMeta::from_json(const TableMeta &table, const Json::Value &json_value, IndexMeta &index)
{
  const Json::Value &name_value  = json_value[FIELD_NAME];
  const Json::Value &field_value = json_value[FIELD_FIELD_NAME];
  if (!name_value.isString()) {
    LOG_ERROR("Index name is not a string. json value=%s", name_value.toStyledString().c_str());
    return RC::INTERNAL;
  }

  if (!field_value.isString()) {
    LOG_ERROR("Field name of index [%s] is not a string. json value=%s",
        name_value.asCString(), field_value.toStyledString().c_str());
    return RC::INTERNAL;
  }

  const FieldMeta *field = table.field(field_value.asCString());
  if (nullptr == field) {
    LOG_ERROR("Deserialize index [%s]: no such field: %s", name_value.asCString(), field_value.asCString());
    return RC::SCHEMA_FIELD_MISSING;
  }

  // 解析索引类型
  IndexType idx_type = IndexType::BPLUS_TREE;
  const Json::Value &type_value = json_value[FIELD_INDEX_TYPE];
  if (type_value.isString()) {
    if (type_value == INDEX_TYPE_IVFFLAT) {
      idx_type = IndexType::IVFFLAT;
    }
  }

  RC rc = index.init(name_value.asCString(), *field, idx_type);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 解析向量索引属性
  const Json::Value &dist_value = json_value[FIELD_DISTANCE_METHOD];
  if (dist_value.isString()) {
    index.set_distance_method(dist_value.asCString());
  }

  const Json::Value &lists_value = json_value[FIELD_LISTS];
  if (lists_value.isInt()) {
    index.set_lists(lists_value.asInt());
  }

  const Json::Value &probes_value = json_value[FIELD_PROBES];
  if (probes_value.isInt()) {
    index.set_probes(probes_value.asInt());
  }

  return RC::SUCCESS;
}

const char *IndexMeta::name() const { return name_.c_str(); }

const char *IndexMeta::field() const { return field_.c_str(); }

void IndexMeta::desc(ostream &os) const { os << "index name=" << name_ << ", field=" << field_; }