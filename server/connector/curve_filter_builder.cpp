////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#include "connector/curve_filter_builder.h"

#include <duckdb/execution/expression_executor.hpp>
#include <duckdb/planner/expression/bound_between_expression.hpp>
#include <duckdb/planner/expression/bound_cast_expression.hpp>
#include <duckdb/planner/expression/bound_comparison_expression.hpp>
#include <duckdb/planner/expression/bound_conjunction_expression.hpp>
#include <duckdb/planner/expression/bound_function_expression.hpp>
#include <iresearch/utils/down_cast.hpp>

#include "connector/curve_index.h"

namespace sdb::connector {
namespace {

const duckdb::Expression& PeelCast(const duckdb::Expression& expr) {
  if (duckdb::BoundCastExpression::IsCast(expr)) {
    const auto& child = duckdb::BoundCastExpression::Child(
      expr.Cast<duckdb::BoundFunctionExpression>());
    if (child.GetReturnType() == expr.GetReturnType()) {
      return PeelCast(child);
    }
  }
  return expr;
}

uint32_t IntegerWidth(duckdb::LogicalTypeId type) {
  using enum duckdb::LogicalTypeId;
  switch (type) {
    case TINYINT:
    case UTINYINT:
      return 8;
    case SMALLINT:
    case USMALLINT:
      return 16;
    case INTEGER:
    case UINTEGER:
      return 32;
    case BIGINT:
    case UBIGINT:
      return 64;
    default:
      return 0;
  }
}

bool IsUnsigned(duckdb::LogicalTypeId type) {
  using enum duckdb::LogicalTypeId;
  return type == UTINYINT || type == USMALLINT || type == UINTEGER ||
         type == UBIGINT;
}

bool IsWideningCast(const duckdb::Expression& expression) {
  if (!duckdb::BoundCastExpression::IsCast(expression)) {
    return false;
  }
  const auto& child = duckdb::BoundCastExpression::Child(
    expression.Cast<duckdb::BoundFunctionExpression>());
  const auto source = child.GetReturnType().id();
  const auto target = expression.GetReturnType().id();
  return (source == duckdb::LogicalTypeId::FLOAT &&
          target == duckdb::LogicalTypeId::DOUBLE) ||
         (IntegerWidth(source) != 0 &&
          IntegerWidth(source) < IntegerWidth(target) &&
          IsUnsigned(source) == IsUnsigned(target));
}

bool Fold(duckdb::ClientContext& context, const duckdb::Expression& expression,
          duckdb::Value& value) {
  if (!expression.IsFoldable() || expression.HasParameter()) {
    return false;
  }
  auto copy = expression.Copy();
  return duckdb::ExpressionExecutor::TryEvaluateScalar(context, *copy, value) &&
         !value.IsNull();
}

bool IsClosedBox(const duckdb::Value& lower, const duckdb::Value& upper) {
  for (const auto* bound : {&lower, &upper}) {
    for (const auto& component : duckdb::StructValue::GetChildren(*bound)) {
      if (component.IsNull()) {
        return false;
      }
    }
  }
  return true;
}

void AddBoxCandidates(irs::BooleanFilter& root, const SearchColumnInfo& info,
                      const irs::curve::Box& box) {
  const auto& options =
    irs::utils::downCast<CurveTokenizer>(*info.tokenizer.analyzer).Options();
  const auto cells = irs::curve::CoverBox(box, options);
  if (cells.empty()) {
    root.Add(std::make_unique<irs::Empty>(), irs::Occur::Must);
    return;
  }
  auto terms = irs::curve::PointQueryTerms(cells, box, options);
  auto candidates = std::make_unique<irs::BooleanFilter>();
  candidates->SetScorer(&irs::ForceConstScore());
  for (const auto& term : terms) {
    AddTerm({candidates.get(), irs::Occur::Should}, info.field_id,
            irs::ViewCast<irs::byte_type>(std::string_view{term}),
            irs::kNoBoost, &irs::ForceConstScore());
  }
  SetMinMatch(*candidates, 1);
  root.Add(std::move(candidates), irs::Occur::Must);
}

void CollectRanges(const duckdb::Expression& expression,
                   absl::FunctionRef<duckdb::unique_ptr<duckdb::Expression>(
                     const duckdb::Expression&)>
                     normalize,
                   std::vector<CurveRange>& ranges) {
  using enum duckdb::ExpressionType;
  if (expression.GetExpressionType() == CONJUNCTION_AND) {
    for (const auto& child :
         expression.Cast<duckdb::BoundConjunctionExpression>().GetChildren()) {
      CollectRanges(*child, normalize, ranges);
    }
    return;
  }
  if (expression.GetExpressionClass() !=
        duckdb::ExpressionClass::BOUND_FUNCTION ||
      expression.HasParameter()) {
    return;
  }
  const auto add = [&](const duckdb::Expression& component,
                       const duckdb::Expression* lower,
                       const duckdb::Expression* upper) {
    if (auto normalized = normalize(PeelCast(component))) {
      ranges.push_back({std::move(normalized), lower, upper});
    }
  };
  const auto& function = expression.Cast<duckdb::BoundFunctionExpression>();
  if (expression.GetExpressionType() == COMPARE_BETWEEN) {
    if (duckdb::BoundBetweenExpression::HasValidBindData(function)) {
      add(duckdb::BoundBetweenExpression::Input(function),
          &duckdb::BoundBetweenExpression::LowerBound(function),
          &duckdb::BoundBetweenExpression::UpperBound(function));
    }
    return;
  }
  if (!duckdb::BoundComparisonExpression::IsComparison(expression)) {
    return;
  }
  const auto& left = duckdb::BoundComparisonExpression::Left(function);
  const auto& right = duckdb::BoundComparisonExpression::Right(function);
  switch (expression.GetExpressionType()) {
    case COMPARE_EQUAL:
      add(left, &right, &right);
      add(right, &left, &left);
      break;
    case COMPARE_GREATERTHAN:
    case COMPARE_GREATERTHANOREQUALTO:
      add(left, &right, nullptr);
      add(right, nullptr, &left);
      break;
    case COMPARE_LESSTHAN:
    case COMPARE_LESSTHANOREQUALTO:
      add(left, nullptr, &right);
      add(right, &left, nullptr);
      break;
    default:
      break;
  }
}

}  // namespace

bool AddCurveCandidates(irs::BooleanFilter& root,
                        const duckdb::Expression& expression,
                        const ColumnGetter& columns,
                        const ExpressionGetter& expressions,
                        duckdb::ClientContext& context) {
  if (expression.GetExpressionClass() !=
        duckdb::ExpressionClass::BOUND_FUNCTION ||
      expression.HasParameter()) {
    return false;
  }
  const auto& function = expression.Cast<duckdb::BoundFunctionExpression>();
  const auto& name = function.Function().GetName();
  const auto& args = function.GetChildren();
  if (name != "sdb_box_contains" || args.size() != 3) {
    return false;
  }
  const auto find =
    [&](const duckdb::Expression& argument) -> std::optional<SearchColumnInfo> {
    const auto& expr = PeelCast(argument);
    auto result =
      expr.GetExpressionClass() == duckdb::ExpressionClass::BOUND_COLUMN_REF
        ? columns(expr.Cast<duckdb::BoundColumnRefExpression>())
        : expressions(expr);
    if (!result || !result->tokenizer.analyzer ||
        result->tokenizer.analyzer->type() != irs::Type<CurveTokenizer>::id()) {
      return std::nullopt;
    }
    return result;
  };
  auto info = find(*args[0]);
  if (!info) {
    return false;
  }
  duckdb::Value lower, upper;
  if (!Fold(context, *args[1], lower) || !Fold(context, *args[2], upper)) {
    return false;
  }
  ValidateCurveBounds(info->logical_type, lower.type(), upper.type());
  if (!IsClosedBox(lower, upper)) {
    return false;
  }
  const auto box = CurveBox(info->logical_type, lower, upper);
  AddBoxCandidates(root, *info, box);
  return true;
}

std::vector<CurveRange> CollectCurveRanges(
  std::span<const duckdb::unique_ptr<duckdb::Expression>> conjuncts,
  absl::FunctionRef<
    duckdb::unique_ptr<duckdb::Expression>(const duckdb::Expression&)>
    normalize) {
  std::vector<CurveRange> ranges;
  for (const auto& conjunct : conjuncts) {
    CollectRanges(*conjunct, normalize, ranges);
  }
  return ranges;
}

bool AddCurveRangeCandidates(irs::BooleanFilter& root,
                             std::span<const CurveRange> ranges,
                             const duckdb::Expression& tuple,
                             const SearchColumnInfo& info,
                             duckdb::ClientContext& context) {
  const auto& point = PeelCast(tuple);
  if (point.GetExpressionClass() != duckdb::ExpressionClass::BOUND_FUNCTION) {
    return false;
  }
  const auto& function = point.Cast<duckdb::BoundFunctionExpression>();
  const auto& components = function.GetChildren();
  const auto dimensions = duckdb::StructType::GetChildCount(info.logical_type);
  if (function.Function().GetName() != "row" ||
      components.size() != dimensions) {
    return false;
  }
  irs::curve::Box box;
  for (uint32_t axis = 0; axis < dimensions; ++axis) {
    bool has_lower = false;
    bool has_upper = false;
    box.max[axis] = std::numeric_limits<uint64_t>::max();
    for (const auto& range : ranges) {
      auto* component = &PeelCast(*range.component);
      const auto& indexed = PeelCast(*components[axis]);
      bool matched = component->Equals(indexed);
      while (!matched && IsWideningCast(*component)) {
        component = &PeelCast(duckdb::BoundCastExpression::Child(
          component->Cast<duckdb::BoundFunctionExpression>()));
        matched = component->Equals(indexed);
      }
      if (!matched) {
        continue;
      }
      duckdb::vector<duckdb::Value> lower_values(dimensions);
      duckdb::vector<duckdb::Value> upper_values(dimensions);
      if ((range.lower && !Fold(context, *range.lower, lower_values[axis])) ||
          (range.upper && !Fold(context, *range.upper, upper_values[axis]))) {
        continue;
      }
      const auto& coordinate =
        duckdb::StructType::GetChildTypes(info.logical_type)[axis].second;
      if (!CurveBoundFits(coordinate, lower_values[axis].type()) ||
          !CurveBoundFits(coordinate, upper_values[axis].type())) {
        continue;
      }
      const auto lower = duckdb::Value::TUPLE(std::move(lower_values));
      const auto upper = duckdb::Value::TUPLE(std::move(upper_values));
      const auto limits = CurveBox(info.logical_type, lower, upper);
      if (range.lower) {
        box.min[axis] = std::max(box.min[axis], limits.min[axis]);
        has_lower = true;
      }
      if (range.upper) {
        box.max[axis] = std::min(box.max[axis], limits.max[axis]);
        has_upper = true;
      }
    }
    if (!has_lower || !has_upper) {
      return false;
    }
  }
  AddBoxCandidates(root, info, box);
  return true;
}

}  // namespace sdb::connector
