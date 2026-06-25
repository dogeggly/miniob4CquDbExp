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
// Created by Wangyunlai on 2022/07/05.
//

#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "sql/expr/arithmetic_operator.hpp"
#include "common/type/vector_type.h"
#include "common/type/attr_type.h"

using namespace std;

RC FieldExpr::get_value(const Tuple &tuple, Value &value) const
{
  return tuple.find_cell(TupleCellSpec(table_name(), field_name()), value);
}

bool FieldExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::FIELD) {
    return false;
  }
  const auto &other_field_expr = static_cast<const FieldExpr &>(other);
  return table_name() == other_field_expr.table_name() && field_name() == other_field_expr.field_name();
}

// TODO: 在进行表达式计算时，`chunk` 包含了所有列，因此可以通过 `field_id` 获取到对应列。
// 后续可以优化成在 `FieldExpr` 中存储 `chunk` 中某列的位置信息。
RC FieldExpr::get_column(Chunk &chunk, Column &column)
{
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    column.reference(chunk.column(field().meta()->field_id()));
  }
  return RC::SUCCESS;
}

bool ValueExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::VALUE) {
    return false;
  }
  const auto &other_value_expr = static_cast<const ValueExpr &>(other);
  return value_.compare(other_value_expr.get_value()) == 0;
}

RC ValueExpr::get_value(const Tuple &tuple, Value &value) const
{
  value = value_;
  return RC::SUCCESS;
}

RC ValueExpr::get_column(Chunk &chunk, Column &column)
{
  column.init(value_, chunk.rows());
  return RC::SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////
CastExpr::CastExpr(unique_ptr<Expression> child, AttrType cast_type) : child_(std::move(child)), cast_type_(cast_type)
{}

CastExpr::~CastExpr() {}

RC CastExpr::cast(const Value &value, Value &cast_value) const
{
  RC rc = RC::SUCCESS;
  if (this->value_type() == value.attr_type()) {
    cast_value = value;
    return rc;
  }
  rc = Value::cast_to(value, cast_type_, cast_value);
  return rc;
}

RC CastExpr::get_value(const Tuple &tuple, Value &result) const
{
  Value value;
  RC rc = child_->get_value(tuple, value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

RC CastExpr::get_column(Chunk &chunk, Column &column)
{
  Column child_column;
  RC rc = child_->get_column(chunk, child_column);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  column.init(cast_type_, child_column.attr_len());
  for (int i = 0; i < child_column.count(); ++i) {
    Value value = child_column.get_value(i);
    Value cast_value;
    rc = cast(value, cast_value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    column.append_value(cast_value);
  }
  return rc;
}

RC CastExpr::try_get_value(Value &result) const
{
  Value value;
  RC rc = child_->try_get_value(value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

////////////////////////////////////////////////////////////////////////////////

ComparisonExpr::ComparisonExpr(CompOp comp, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : comp_(comp), left_(std::move(left)), right_(std::move(right))
{
}

ComparisonExpr::~ComparisonExpr() {}

RC ComparisonExpr::compare_value(const Value &left, const Value &right, bool &result) const
{
  RC  rc         = RC::SUCCESS;
  int cmp_result = left.compare(right);
  result         = false;
  switch (comp_) {
    case EQUAL_TO: {
      result = (0 == cmp_result);
    } break;
    case LESS_EQUAL: {
      result = (cmp_result <= 0);
    } break;
    case NOT_EQUAL: {
      result = (cmp_result != 0);
    } break;
    case LESS_THAN: {
      result = (cmp_result < 0);
    } break;
    case GREAT_EQUAL: {
      result = (cmp_result >= 0);
    } break;
    case GREAT_THAN: {
      result = (cmp_result > 0);
    } break;
    default: {
      LOG_WARN("unsupported comparison. %d", comp_);
      rc = RC::INTERNAL;
    } break;
  }

  return rc;
}

RC ComparisonExpr::try_get_value(Value &cell) const
{
  if (left_->type() == ExprType::VALUE && right_->type() == ExprType::VALUE) {
    ValueExpr *  left_value_expr  = static_cast<ValueExpr *>(left_.get());
    ValueExpr *  right_value_expr = static_cast<ValueExpr *>(right_.get());
    const Value &left_cell        = left_value_expr->get_value();
    const Value &right_cell       = right_value_expr->get_value();

    bool value = false;
    RC   rc    = compare_value(left_cell, right_cell, value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
    } else {
      cell.set_boolean(value);
    }
    return rc;
  }

  return RC::INVALID_ARGUMENT;
}

RC ComparisonExpr::get_value(const Tuple &tuple, Value &value) const
{
  Value left_value;
  Value right_value;

  RC rc = left_->get_value(tuple, left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_value(tuple, right_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }

  bool bool_value = false;

  rc = compare_value(left_value, right_value, bool_value);
  if (rc == RC::SUCCESS) {
    value.set_boolean(bool_value);
  }
  return rc;
}

RC ComparisonExpr::eval(Chunk &chunk, vector<uint8_t> &select)
{
  RC     rc = RC::SUCCESS;
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_column(chunk, right_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }
  if (left_column.attr_type() != right_column.attr_type()) {
    LOG_WARN("cannot compare columns with different types");
    return RC::INTERNAL;
  }
  if (left_column.attr_type() == AttrType::INTS) {
    rc = compare_column<int>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::FLOATS) {
    rc = compare_column<float>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::CHARS || left_column.attr_type() == AttrType::VECTORS) {
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      Value left_val = left_column.get_value(i);
      Value right_val = right_column.get_value(i);
      bool        result   = false;
      rc                   = compare_value(left_val, right_val, result);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
        return rc;
      }
      select[i] &= result ? 1 : 0;
    }

  } else {
    LOG_WARN("unsupported data type %d", left_column.attr_type());
    return RC::INTERNAL;
  }
  return rc;
}

template <typename T>
RC ComparisonExpr::compare_column(const Column &left, const Column &right, vector<uint8_t> &result) const
{
  RC rc = RC::SUCCESS;

  bool left_const  = left.column_type() == Column::Type::CONSTANT_COLUMN;
  bool right_const = right.column_type() == Column::Type::CONSTANT_COLUMN;
  if (left_const && right_const) {
    compare_result<T, true, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else if (left_const && !right_const) {
    compare_result<T, true, false>((T *)left.data(), (T *)right.data(), right.count(), result, comp_);
  } else if (!left_const && right_const) {
    compare_result<T, false, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else {
    compare_result<T, false, false>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
ConjunctionExpr::ConjunctionExpr(Type type, vector<unique_ptr<Expression>> &children)
    : conjunction_type_(type), children_(std::move(children))
{}

RC ConjunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;
  if (children_.empty()) {
    value.set_boolean(true);
    return rc;
  }

  Value tmp_value;
  for (const unique_ptr<Expression> &expr : children_) {
    rc = expr->get_value(tuple, tmp_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value by child expression. rc=%s", strrc(rc));
      return rc;
    }
    bool bool_value = tmp_value.get_boolean();
    if ((conjunction_type_ == Type::AND && !bool_value) || (conjunction_type_ == Type::OR && bool_value)) {
      value.set_boolean(bool_value);
      return rc;
    }
  }

  bool default_value = (conjunction_type_ == Type::AND);
  value.set_boolean(default_value);
  return rc;
}

////////////////////////////////////////////////////////////////////////////////

ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, Expression *left, Expression *right)
    : arithmetic_type_(type), left_(left), right_(right)
{}
ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : arithmetic_type_(type), left_(std::move(left)), right_(std::move(right))
{}

bool ArithmeticExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (type() != other.type()) {
    return false;
  }
  auto &other_arith_expr = static_cast<const ArithmeticExpr &>(other);
  return arithmetic_type_ == other_arith_expr.arithmetic_type() && left_->equal(*other_arith_expr.left_) &&
         right_->equal(*other_arith_expr.right_);
}
AttrType ArithmeticExpr::value_type() const
{
  if (!right_) {
    return left_->value_type();
  }

  if ((left_->value_type() == AttrType::INTS) &&
   (right_->value_type() == AttrType::INTS) &&
      arithmetic_type_ != Type::DIV) {
    return AttrType::INTS;
  }

  return AttrType::FLOATS;
}

RC ArithmeticExpr::calc_value(const Value &left_value, const Value &right_value, Value &value) const
{
  RC rc = RC::SUCCESS;

  const AttrType target_type = value_type();
  value.set_type(target_type);

  switch (arithmetic_type_) {
    case Type::ADD: {
      Value::add(left_value, right_value, value);
    } break;

    case Type::SUB: {
      Value::subtract(left_value, right_value, value);
    } break;

    case Type::MUL: {
      Value::multiply(left_value, right_value, value);
    } break;

    case Type::DIV: {
      Value::divide(left_value, right_value, value);
    } break;

    case Type::NEGATIVE: {
      Value::negative(left_value, value);
    } break;

    default: {
      rc = RC::INTERNAL;
      LOG_WARN("unsupported arithmetic type. %d", arithmetic_type_);
    } break;
  }
  return rc;
}

template <bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
RC ArithmeticExpr::execute_calc(
    const Column &left, const Column &right, Column &result, Type type, AttrType attr_type) const
{
  RC rc = RC::SUCCESS;
  switch (type) {
    case Type::ADD: {
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, AddOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, AddOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
    } break;
    case Type::SUB:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, SubtractOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, SubtractOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::MUL:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, MultiplyOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, MultiplyOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::DIV:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, DivideOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, DivideOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::NEGATIVE:
      if (attr_type == AttrType::INTS) {
        unary_operator<LEFT_CONSTANT, int, NegateOperator>((int *)left.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        unary_operator<LEFT_CONSTANT, float, NegateOperator>(
            (float *)left.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    default: rc = RC::UNIMPLEMENTED; break;
  }
  if (rc == RC::SUCCESS) {
    result.set_count(result.capacity());
  }
  return rc;
}

RC ArithmeticExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->get_value(tuple, left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_value(tuple, right_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }
  return calc_value(left_value, right_value, value);
}

RC ArithmeticExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
    return rc;
  }
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get column of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_column(chunk, right_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get column of right expression. rc=%s", strrc(rc));
    return rc;
  }
  return calc_column(left_column, right_column, column);
}

RC ArithmeticExpr::calc_column(const Column &left_column, const Column &right_column, Column &column) const
{
  RC rc = RC::SUCCESS;

  const AttrType target_type = value_type();
  column.init(target_type, left_column.attr_len(), max(left_column.count(), right_column.count()));
  bool left_const  = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
  bool right_const = right_column.column_type() == Column::Type::CONSTANT_COLUMN;
  if (left_const && right_const) {
    column.set_column_type(Column::Type::CONSTANT_COLUMN);
    rc = execute_calc<true, true>(left_column, right_column, column, arithmetic_type_, target_type);
  } else if (left_const && !right_const) {
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    rc = execute_calc<true, false>(left_column, right_column, column, arithmetic_type_, target_type);
  } else if (!left_const && right_const) {
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    rc = execute_calc<false, true>(left_column, right_column, column, arithmetic_type_, target_type);
  } else {
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    rc = execute_calc<false, false>(left_column, right_column, column, arithmetic_type_, target_type);
  }
  return rc;
}

RC ArithmeticExpr::try_get_value(Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->try_get_value(left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }

  if (right_) {
    rc = right_->try_get_value(right_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_value(left_value, right_value, value);
}

////////////////////////////////////////////////////////////////////////////////

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, Expression *child)
    : aggregate_name_(aggregate_name), child_(child)
{}

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, unique_ptr<Expression> child)
    : aggregate_name_(aggregate_name), child_(std::move(child))
{}

////////////////////////////////////////////////////////////////////////////////
AggregateExpr::AggregateExpr(Type type, Expression *child) : aggregate_type_(type), child_(child) {}

AggregateExpr::AggregateExpr(Type type, unique_ptr<Expression> child) : aggregate_type_(type), child_(std::move(child))
{}

RC AggregateExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    rc = RC::INTERNAL;
  }
  return rc;
}

bool AggregateExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != type()) {
    return false;
  }
  const AggregateExpr &other_aggr_expr = static_cast<const AggregateExpr &>(other);
  return aggregate_type_ == other_aggr_expr.aggregate_type() && child_->equal(*other_aggr_expr.child());
}

unique_ptr<Aggregator> AggregateExpr::create_aggregator() const
{
  unique_ptr<Aggregator> aggregator;
  switch (aggregate_type_) {
    case Type::SUM: {
      aggregator = make_unique<SumAggregator>();
      break;
    }
    default: {
      ASSERT(false, "unsupported aggregate type");
      break;
    }
  }
  return aggregator;
}

RC AggregateExpr::get_value(const Tuple &tuple, Value &value) const
{
  return tuple.find_cell(TupleCellSpec(name()), value);
}

RC AggregateExpr::type_from_string(const char *type_str, AggregateExpr::Type &type)
{
  RC rc = RC::SUCCESS;
  if (0 == strcasecmp(type_str, "count")) {
    type = Type::COUNT;
  } else if (0 == strcasecmp(type_str, "sum")) {
    type = Type::SUM;
  } else if (0 == strcasecmp(type_str, "avg")) {
    type = Type::AVG;
  } else if (0 == strcasecmp(type_str, "max")) {
    type = Type::MAX;
  } else if (0 == strcasecmp(type_str, "min")) {
    type = Type::MIN;
  } else {
    rc = RC::INVALID_ARGUMENT;
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
// UnboundFunctionExpr

UnboundFunctionExpr::UnboundFunctionExpr(const char *function_name, vector<unique_ptr<Expression>> children)
    : function_name_(function_name), children_(std::move(children))
{}

UnboundFunctionExpr::UnboundFunctionExpr(const char *function_name, unique_ptr<Expression> child)
    : function_name_(function_name)
{
  children_.emplace_back(std::move(child));
}

unique_ptr<Expression> UnboundFunctionExpr::copy() const
{
  vector<unique_ptr<Expression>> children;
  for (auto &child : children_) {
    children.emplace_back(child->copy());
  }
  return make_unique<UnboundFunctionExpr>(function_name_.c_str(), std::move(children));
}

////////////////////////////////////////////////////////////////////////////////
// FunctionExpr

FunctionExpr::FunctionExpr(Type type, vector<unique_ptr<Expression>> children)
    : function_type_(type), children_(std::move(children))
{}

unique_ptr<Expression> FunctionExpr::copy() const
{
  vector<unique_ptr<Expression>> children;
  for (auto &child : children_) {
    children.emplace_back(child->copy());
  }
  return make_unique<FunctionExpr>(function_type_, std::move(children));
}

bool FunctionExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::FUNCTION) {
    return false;
  }
  const auto &other_func = static_cast<const FunctionExpr &>(other);
  if (function_type_ != other_func.function_type_) {
    return false;
  }
  if (children_.size() != other_func.children_.size()) {
    return false;
  }
  for (size_t i = 0; i < children_.size(); i++) {
    if (!children_[i]->equal(*other_func.children_[i])) {
      return false;
    }
  }
  return true;
}

AttrType FunctionExpr::value_type() const
{
  switch (function_type_) {
    case Type::DISTANCE:
      return AttrType::FLOATS;
    case Type::VECTOR_TO_STRING:
      return AttrType::CHARS;
    case Type::STRING_TO_VECTOR:
      return AttrType::VECTORS;
    default:
      return AttrType::UNDEFINED;
  }
}

int FunctionExpr::value_length() const
{
  switch (function_type_) {
    case Type::DISTANCE:
      return static_cast<int>(sizeof(float));
    case Type::VECTOR_TO_STRING:
      return -1;  // 变长字符串
    case Type::STRING_TO_VECTOR:
      return VectorType::DEFAULT_DIM * static_cast<int>(sizeof(float));
    default:
      return -1;
  }
}

RC FunctionExpr::type_from_string(const char *type_str, FunctionExpr::Type &type)
{
  RC rc = RC::SUCCESS;
  if (0 == strcasecmp(type_str, "distance")) {
    type = Type::DISTANCE;
  } else if (0 == strcasecmp(type_str, "string_to_vector")) {
    type = Type::STRING_TO_VECTOR;
  } else if (0 == strcasecmp(type_str, "vector_to_string")) {
    type = Type::VECTOR_TO_STRING;
  } else {
    rc = RC::INVALID_ARGUMENT;
  }
  return rc;
}

RC FunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  switch (function_type_) {
    case Type::DISTANCE:
      return eval_distance(tuple, value);
    case Type::STRING_TO_VECTOR:
      return eval_string_to_vector(tuple, value);
    case Type::VECTOR_TO_STRING:
      return eval_vector_to_string(tuple, value);
    default:
      LOG_WARN("unknown function type: %d", static_cast<int>(function_type_));
      return RC::INTERNAL;
  }
}

RC FunctionExpr::try_get_value(Value &value) const
{
  // 获取所有子表达式的常量值
  vector<Value> child_values;
  for (auto &child : children_) {
    Value child_val;
    RC    rc = child->try_get_value(child_val);
    if (OB_FAIL(rc)) {
      return rc;
    }
    child_values.push_back(std::move(child_val));
  }

  switch (function_type_) {
    case Type::DISTANCE: {
      if (child_values.size() != 3) {
        return RC::INVALID_ARGUMENT;
      }
      if (child_values[0].attr_type() != AttrType::VECTORS || child_values[1].attr_type() != AttrType::VECTORS) {
        return RC::INVALID_ARGUMENT;
      }
      if (child_values[0].length() != child_values[1].length()) {
        return RC::INVALID_ARGUMENT;
      }

      const float *data1     = reinterpret_cast<const float *>(child_values[0].data());
      const float *data2     = reinterpret_cast<const float *>(child_values[1].data());
      int          dim       = child_values[0].length();
      string       method_str = child_values[2].get_string();
      float        dist      = 0.0f;

      RC rc;
      if (0 == strcasecmp(method_str.c_str(), "cosine")) {
        rc = VectorType::cosine_distance(data1, data2, dim, dist);
      } else if (0 == strcasecmp(method_str.c_str(), "dot")) {
        rc = VectorType::dot_product(data1, data2, dim, dist);
      } else if (0 == strcasecmp(method_str.c_str(), "euclidean") || 0 == strcasecmp(method_str.c_str(), "l2")) {
        rc = VectorType::euclidean_distance(data1, data2, dim, dist);
      } else {
        return RC::INVALID_ARGUMENT;
      }
      if (OB_FAIL(rc)) {
        return rc;
      }
      value.set_float(dist);
      return RC::SUCCESS;
    }

    case Type::STRING_TO_VECTOR: {
      if (child_values.size() != 1) {
        return RC::INVALID_ARGUMENT;
      }
      string str = child_values[0].get_string();
      value.set_type(AttrType::VECTORS);
      return DataType::type_instance(AttrType::VECTORS)->set_value_from_str(value, str);
    }

    case Type::VECTOR_TO_STRING: {
      if (child_values.size() != 1) {
        return RC::INVALID_ARGUMENT;
      }
      if (child_values[0].attr_type() != AttrType::VECTORS) {
        return RC::INVALID_ARGUMENT;
      }
      string out;
      RC     rc = DataType::type_instance(AttrType::VECTORS)->to_string(child_values[0], out);
      if (OB_FAIL(rc)) {
        return rc;
      }
      value.set_string(out.c_str(), static_cast<int>(out.size()));
      return RC::SUCCESS;
    }

    default:
      return RC::INTERNAL;
  }
}

RC FunctionExpr::get_column(Chunk &chunk, Column &column)
{
  // 标量函数暂不支持向量化执行，返回 UNIMPLEMENTED
  return RC::UNIMPLEMENTED;
}

RC FunctionExpr::eval_distance(const Tuple &tuple, Value &result) const
{
  if (children_.size() != 3) {
    LOG_WARN("DISTANCE requires exactly 3 arguments, got %d", static_cast<int>(children_.size()));
    return RC::INVALID_ARGUMENT;
  }

  // 求值向量1 (参数0)
  Value vec1;
  RC    rc = children_[0]->get_value(tuple, vec1);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (vec1.attr_type() != AttrType::VECTORS) {
    LOG_WARN("DISTANCE first argument must be VECTOR, got %s", attr_type_to_string(vec1.attr_type()));
    return RC::INVALID_ARGUMENT;
  }

  // 求值向量2 (参数1)
  Value vec2;
  rc = children_[1]->get_value(tuple, vec2);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (vec2.attr_type() != AttrType::VECTORS) {
    LOG_WARN("DISTANCE second argument must be VECTOR, got %s", attr_type_to_string(vec2.attr_type()));
    return RC::INVALID_ARGUMENT;
  }

  // 校验维度匹配
  if (vec1.length() != vec2.length()) {
    LOG_WARN("DISTANCE dimension mismatch: %d vs %d", vec1.length(), vec2.length());
    return RC::INVALID_ARGUMENT;
  }

  // 求值距离方法字符串 (参数2)
  Value method_val;
  rc = children_[2]->get_value(tuple, method_val);
  if (OB_FAIL(rc)) {
    return rc;
  }
  string method_str = method_val.get_string();

  const float *data1 = reinterpret_cast<const float *>(vec1.data());
  const float *data2 = reinterpret_cast<const float *>(vec2.data());
  int          dim   = vec1.length();
  float        dist  = 0.0f;

  if (0 == strcasecmp(method_str.c_str(), "cosine")) {
    rc = VectorType::cosine_distance(data1, data2, dim, dist);
  } else if (0 == strcasecmp(method_str.c_str(), "dot")) {
    rc = VectorType::dot_product(data1, data2, dim, dist);
  } else if (0 == strcasecmp(method_str.c_str(), "euclidean") || 0 == strcasecmp(method_str.c_str(), "l2")) {
    rc = VectorType::euclidean_distance(data1, data2, dim, dist);
  } else {
    LOG_WARN("DISTANCE unsupported method: %s", method_str.c_str());
    return RC::INVALID_ARGUMENT;
  }
  if (OB_FAIL(rc)) {
    return rc;
  }

  result.set_float(dist);
  return RC::SUCCESS;
}

RC FunctionExpr::eval_string_to_vector(const Tuple &tuple, Value &result) const
{
  if (children_.size() != 1) {
    LOG_WARN("STRING_TO_VECTOR requires exactly 1 argument, got %d", static_cast<int>(children_.size()));
    return RC::INVALID_ARGUMENT;
  }

  Value str_val;
  RC    rc = children_[0]->get_value(tuple, str_val);
  if (OB_FAIL(rc)) {
    return rc;
  }

  string str = str_val.get_string();
  result.set_type(AttrType::VECTORS);
  return DataType::type_instance(AttrType::VECTORS)->set_value_from_str(result, str);
}

RC FunctionExpr::eval_vector_to_string(const Tuple &tuple, Value &result) const
{
  if (children_.size() != 1) {
    LOG_WARN("VECTOR_TO_STRING requires exactly 1 argument, got %d", static_cast<int>(children_.size()));
    return RC::INVALID_ARGUMENT;
  }

  Value vec_val;
  RC    rc = children_[0]->get_value(tuple, vec_val);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (vec_val.attr_type() != AttrType::VECTORS) {
    LOG_WARN("VECTOR_TO_STRING argument must be VECTOR, got %s", attr_type_to_string(vec_val.attr_type()));
    return RC::INVALID_ARGUMENT;
  }

  string out;
  rc = DataType::type_instance(AttrType::VECTORS)->to_string(vec_val, out);
  if (OB_FAIL(rc)) {
    return rc;
  }

  result.set_string(out.c_str(), static_cast<int>(out.size()));
  return RC::SUCCESS;
}
