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

#include "connector/curve_index.h"

#include <absl/strings/ascii.h>

#include <duckdb/common/vector/constant_vector.hpp>
#include <duckdb/common/vector/flat_vector.hpp>
#include <duckdb/common/vector/string_vector.hpp>
#include <duckdb/common/vector/unified_vector_format.hpp>
#include <duckdb/execution/expression_executor.hpp>
#include <duckdb/planner/expression/bound_constant_expression.hpp>
#include <duckdb/planner/expression/bound_function_expression.hpp>
#include <iresearch/utils/assert.hpp>
#include <iresearch/utils/pg/errcodes.hpp>
#include <iresearch/utils/pg/sql_exception_macro.hpp>

#include "connector/pg_logical_types.h"

namespace sdb::connector {
namespace {

constexpr std::string_view kMortonCurve = "morton";
constexpr std::string_view kHilbertCurve = "hilbert";

enum CurveOption : size_t {
  kKindOption,
  kMaxLevelOption,
  kLevelStepOption,
  kMaxCellsOption,
  kLowerOption,
  kUpperOption,
  kOptionCount,
};

constexpr std::string_view kCoordinateTypes =
  "an integer, FLOAT, DOUBLE, DATE, TIMESTAMP, TIMESTAMPTZ or INET";

constexpr uint8_t kInetV4 = 1;
constexpr uint8_t kInetV6 = 2;

std::optional<CurveFamily> CoordinateFamily(const duckdb::LogicalType& type) {
  using enum duckdb::LogicalTypeId;
  if (pg::IsInet(type)) {
    return CurveFamily::Inet;
  }
  switch (type.id()) {
    case SQLNULL:
      return CurveFamily::Null;
    case TINYINT:
    case SMALLINT:
    case INTEGER:
    case BIGINT:
      return CurveFamily::Signed;
    case UTINYINT:
    case USMALLINT:
    case UINTEGER:
    case UBIGINT:
      return CurveFamily::Unsigned;
    case FLOAT:
    case DOUBLE:
      return CurveFamily::Float;
    case DATE:
      return CurveFamily::Date;
    case TIMESTAMP:
      return CurveFamily::Timestamp;
    case TIMESTAMP_TZ:
      return CurveFamily::TimestampTz;
    default:
      return std::nullopt;
  }
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
    case DATE:
      return 32;
    default:
      return 64;
  }
}

uint64_t InetKey(uint8_t ip_type, duckdb::hugeint_t address) noexcept {
  if (ip_type == kInetV6) {
    return irs::curve::kSignBit |
           ((static_cast<uint64_t>(address.upper) ^ irs::curve::kSignBit) >> 1);
  }
  if (ip_type == kInetV4) {
    return address.lower << 31;
  }
  return 0;
}

struct RawBound {
  uint64_t key = 0;
  double value = 0;
  bool exact = false;
};

bool IsSignedInteger(duckdb::LogicalTypeId type) {
  using enum duckdb::LogicalTypeId;
  return type == TINYINT || type == SMALLINT || type == INTEGER ||
         type == BIGINT;
}

bool IsUnsignedInteger(duckdb::LogicalTypeId type) {
  using enum duckdb::LogicalTypeId;
  return type == UTINYINT || type == USMALLINT || type == UINTEGER ||
         type == UBIGINT;
}

std::optional<RawBound> ToRaw(CurveFamily family, const duckdb::Value& bound) {
  using enum duckdb::LogicalTypeId;
  const auto type = bound.type().id();
  const bool is_signed = IsSignedInteger(type);
  const bool is_unsigned = IsUnsignedInteger(type);
  switch (family) {
    case CurveFamily::Signed:
      if (is_signed) {
        return RawBound{
          .key = irs::curve::EncodeSigned(bound.GetValue<int64_t>()),
          .exact = true};
      }
      if (is_unsigned) {
        const auto value = bound.GetValue<uint64_t>();
        constexpr auto kMax =
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        return RawBound{.key = irs::curve::EncodeSigned(
                          static_cast<int64_t>(std::min(value, kMax))),
                        .exact = value <= kMax};
      }
      return std::nullopt;
    case CurveFamily::Unsigned:
      if (is_unsigned) {
        return RawBound{.key = bound.GetValue<uint64_t>(), .exact = true};
      }
      if (is_signed) {
        const auto value = bound.GetValue<int64_t>();
        return RawBound{
          .key = static_cast<uint64_t>(std::max<int64_t>(value, 0)),
          .exact = value >= 0};
      }
      return std::nullopt;
    case CurveFamily::Float:
      if (type == FLOAT || type == DOUBLE) {
        return RawBound{.value = bound.GetValue<double>(), .exact = true};
      }
      if (is_signed) {
        const auto value = bound.GetValue<int64_t>();
        const auto converted = static_cast<double>(value);
        return RawBound{.value = converted,
                        .exact = converted >= -0x1p63 && converted < 0x1p63 &&
                                 static_cast<int64_t>(converted) == value};
      }
      if (is_unsigned) {
        const auto value = bound.GetValue<uint64_t>();
        const auto converted = static_cast<double>(value);
        return RawBound{.value = converted,
                        .exact = converted < 0x1p64 &&
                                 static_cast<uint64_t>(converted) == value};
      }
      if (type == DECIMAL) {
        return RawBound{.value =
                          bound.DefaultCastAs(duckdb::LogicalType::DOUBLE)
                            .GetValue<double>()};
      }
      return std::nullopt;
    case CurveFamily::Date:
      if (type != DATE) {
        return std::nullopt;
      }
      return RawBound{
        .key = irs::curve::EncodeSigned(bound.GetValue<duckdb::date_t>().days),
        .exact = true};
    case CurveFamily::Timestamp:
      if (type != TIMESTAMP) {
        return std::nullopt;
      }
      return RawBound{.key = irs::curve::EncodeSigned(
                        bound.GetValue<duckdb::timestamp_t>().value),
                      .exact = true};
    case CurveFamily::TimestampTz:
      if (type != TIMESTAMP_TZ) {
        return std::nullopt;
      }
      return RawBound{.key = irs::curve::EncodeSigned(
                        bound.GetValue<duckdb::timestamp_tz_t>().value),
                      .exact = true};
    case CurveFamily::Inet: {
      if (!pg::IsInet(bound.type())) {
        return std::nullopt;
      }
      const auto& parts = duckdb::StructValue::GetChildren(bound);
      if (parts[0].IsNull() || parts[1].IsNull()) {
        return std::nullopt;
      }
      return RawBound{.key = InetKey(parts[0].GetValue<uint8_t>(),
                                     parts[1].GetValue<duckdb::hugeint_t>())};
    }
    case CurveFamily::Null:
      return std::nullopt;
  }
  return std::nullopt;
}

uint64_t MapKey(const CurveAxis& axis, uint64_t key) noexcept {
  return axis.normalized
           ? irs::curve::NormalizeKey(key, axis.key_lo, axis.key_hi)
           : key;
}

uint64_t MapDouble(const CurveAxis& axis, double value) noexcept {
  return axis.normalized ? irs::curve::NormalizeDouble(value, axis.lo, axis.hi)
                         : irs::curve::EncodeDouble(value);
}

template<typename T>
T Load(const duckdb::UnifiedVectorFormat& format, duckdb::idx_t index) {
  return duckdb::UnifiedVectorFormat::GetDataUnsafe<T>(format)[index];
}

bool IsValid(const CurveAxis& axis,
             const duckdb::RecursiveUnifiedVectorFormat& format,
             duckdb::idx_t row) {
  const auto& unified = format.unified;
  if (!unified.validity.RowIsValid(unified.sel->get_index(row))) {
    return false;
  }
  if (axis.family != CurveFamily::Inet) {
    return true;
  }
  for (const auto& child : {&format.children[0], &format.children[1]}) {
    const auto& part = child->unified;
    if (!part.validity.RowIsValid(part.sel->get_index(row))) {
      return false;
    }
  }
  return true;
}

uint64_t AxisKey(const CurveAxis& axis,
                 const duckdb::RecursiveUnifiedVectorFormat& format,
                 duckdb::idx_t row) {
  using enum duckdb::LogicalTypeId;
  const auto& unified = format.unified;
  const auto index = unified.sel->get_index(row);
  const auto signed_key = [&](int64_t value) {
    return MapKey(axis, irs::curve::EncodeSigned(value));
  };
  if (axis.family == CurveFamily::Inet) {
    const auto& type = format.children[0].unified;
    const auto& address = format.children[1].unified;
    return MapKey(axis, InetKey(Load<uint8_t>(type, type.sel->get_index(row)),
                                Load<duckdb::hugeint_t>(
                                  address, address.sel->get_index(row))));
  }
  switch (axis.type.id()) {
    case TINYINT:
      return signed_key(Load<int8_t>(unified, index));
    case SMALLINT:
      return signed_key(Load<int16_t>(unified, index));
    case INTEGER:
    case DATE:
      return signed_key(Load<int32_t>(unified, index));
    case BIGINT:
    case TIMESTAMP:
    case TIMESTAMP_TZ:
      return signed_key(Load<int64_t>(unified, index));
    case UTINYINT:
      return MapKey(axis, Load<uint8_t>(unified, index));
    case USMALLINT:
      return MapKey(axis, Load<uint16_t>(unified, index));
    case UINTEGER:
      return MapKey(axis, Load<uint32_t>(unified, index));
    case UBIGINT:
      return MapKey(axis, Load<uint64_t>(unified, index));
    case FLOAT:
      return MapDouble(axis, Load<float>(unified, index));
    case DOUBLE:
      return MapDouble(axis, Load<double>(unified, index));
    default:
      SDB_ASSERT(false);
      return 0;
  }
}

duckdb::Value OptionValue(duckdb::ClientContext& context,
                          duckdb::unique_ptr<duckdb::Expression>& argument,
                          std::string_view name) {
  if (!argument->IsFoldable() || argument->HasParameter()) {
    THROW_SQL_ERROR(
      ERR_CODE(ERRCODE_INVALID_PARAMETER_VALUE),
      ERR_MSG("curve(): option \"", name, "\" must be a constant"));
  }
  auto value = duckdb::ExpressionExecutor::EvaluateScalar(context, *argument);
  auto alias = argument->GetAlias();
  argument = duckdb::make_uniq<duckdb::BoundConstantExpression>(value);
  argument->SetAlias(std::move(alias));
  return value;
}

uint32_t LimitOption(const duckdb::Value& value, std::string_view name,
                     uint32_t min, uint32_t max) {
  if (!value.IsNull()) {
    const auto n = value.GetValue<int64_t>();
    if (n >= min && n <= max) {
      return static_cast<uint32_t>(n);
    }
  }
  THROW_SQL_ERROR(ERR_CODE(ERRCODE_INVALID_PARAMETER_VALUE),
                  ERR_MSG("curve(): option \"", name, "\" must be between ",
                          min, " and ", max, ", got ", value.ToString()));
}

const duckdb::Value* BoundComponent(const duckdb::Value& bounds,
                                    std::string_view name, uint32_t axis,
                                    uint32_t dimensions) {
  if (bounds.IsNull()) {
    return nullptr;
  }
  if (!duckdb::StructType::IsStruct(bounds.type()) ||
      pg::IsInet(bounds.type()) ||
      duckdb::StructType::GetChildCount(bounds.type()) != dimensions) {
    THROW_SQL_ERROR(
      ERR_CODE(ERRCODE_DATATYPE_MISMATCH),
      ERR_MSG("curve(): option \"", name, "\" must be a ROW of ", dimensions,
              " values, got ", bounds.type().ToString()));
  }
  const auto& component = duckdb::StructValue::GetChildren(bounds)[axis];
  return component.IsNull() ? nullptr : &component;
}

[[noreturn]] void ThrowBoundMismatch(std::string_view what,
                                     const duckdb::Value& bound, uint32_t axis,
                                     const CurveAxis& target) {
  THROW_SQL_ERROR(
    ERR_CODE(ERRCODE_DATATYPE_MISMATCH),
    ERR_MSG("curve(): ", what, " ", bound.type().ToString(),
            " does not match coordinate ", axis + 1, " of type ",
            target.type.ToString(), "; cast the bound explicitly"));
}

void Normalize(CurveAxis& axis, uint32_t index, const duckdb::Value* lower,
               const duckdb::Value* upper) {
  if (!lower && !upper) {
    return;
  }
  if (!lower || !upper) {
    THROW_SQL_ERROR(
      ERR_CODE(ERRCODE_INVALID_PARAMETER_VALUE),
      ERR_MSG("curve(): \"lower\" and \"upper\" must both bound coordinate ",
              index + 1, " or both leave it NULL"));
  }
  const auto lo = ToRaw(axis.family, *lower);
  if (!lo) {
    ThrowBoundMismatch("lower bound", *lower, index, axis);
  }
  const auto hi = ToRaw(axis.family, *upper);
  if (!hi) {
    ThrowBoundMismatch("upper bound", *upper, index, axis);
  }
  const bool ordered =
    axis.family == CurveFamily::Float
      ? std::isfinite(lo->value) && std::isfinite(hi->value) &&
          lo->value < hi->value && std::isfinite(hi->value - lo->value)
      : lo->key < hi->key;
  if (!ordered) {
    THROW_SQL_ERROR(ERR_CODE(ERRCODE_INVALID_PARAMETER_VALUE),
                    ERR_MSG("curve(): coordinate ", index + 1,
                            " needs finite bounds with lower < upper, got ",
                            lower->ToString(), " and ", upper->ToString()));
  }
  axis.normalized = true;
  axis.key_lo = lo->key;
  axis.key_hi = hi->key;
  axis.lo = lo->value;
  axis.hi = hi->value;
}

}  // namespace

std::optional<CurveKey> CurveBoundKey(const CurveAxis& axis,
                                      const duckdb::Value& bound) {
  const auto raw = ToRaw(axis.family, bound);
  if (!raw) {
    return std::nullopt;
  }
  const bool injective = !axis.normalized && axis.family != CurveFamily::Inet;
  return CurveKey{.key = axis.family == CurveFamily::Float
                           ? MapDouble(axis, raw->value)
                           : MapKey(axis, raw->key),
                  .exact = raw->exact && injective};
}

CurveInterval CurveLimits(const CurveAxis& axis) noexcept {
  constexpr auto kMax = std::numeric_limits<uint64_t>::max();
  if (axis.normalized) {
    return {.max = kMax};
  }
  switch (axis.family) {
    case CurveFamily::Float:
      return {
        .min =
          irs::curve::EncodeDouble(-std::numeric_limits<double>::infinity()),
        .max =
          irs::curve::EncodeDouble(std::numeric_limits<double>::quiet_NaN()),
      };
    case CurveFamily::Signed:
    case CurveFamily::Date: {
      const auto width = IntegerWidth(axis.type.id());
      if (width == 64) {
        return {.max = kMax};
      }
      const auto half = uint64_t{1} << (width - 1);
      return {
        .min = irs::curve::kSignBit - half,
        .max = irs::curve::kSignBit + half - 1,
      };
    }
    case CurveFamily::Unsigned: {
      const auto width = IntegerWidth(axis.type.id());
      return {.max = width == 64 ? kMax : (uint64_t{1} << width) - 1};
    }
    default:
      return {.max = kMax};
  }
}

duckdb::unique_ptr<duckdb::FunctionData> CurveSpec::Copy() const {
  return duckdb::make_uniq<CurveSpec>(*this);
}

bool CurveSpec::Equals(const duckdb::FunctionData& other) const {
  const auto& spec = other.Cast<CurveSpec>();
  return options == spec.options && axes == spec.axes;
}

duckdb::unique_ptr<CurveSpec> BindCurveSpec(
  duckdb::ClientContext& context,
  duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>& args) {
  SDB_ASSERT(args.size() >= kOptionCount);
  const auto dimensions = args.size() - kOptionCount;
  if (dimensions < 2 || dimensions > irs::curve::kMaxDimensions) {
    THROW_SQL_ERROR(ERR_CODE(ERRCODE_INVALID_PARAMETER_VALUE),
                    ERR_MSG("curve(): takes 2 to ", irs::curve::kMaxDimensions,
                            " coordinates, got ", dimensions));
  }
  auto spec = duckdb::make_uniq<CurveSpec>();
  auto& options = spec->options;
  options.dimensions = static_cast<uint32_t>(dimensions);
  for (uint32_t axis = 0; axis < options.dimensions; ++axis) {
    const auto& type = args[axis]->GetReturnType();
    const auto family = CoordinateFamily(type);
    if (!family) {
      THROW_SQL_ERROR(ERR_CODE(ERRCODE_DATATYPE_MISMATCH),
                      ERR_MSG("curve(): coordinate ", axis + 1, " must be ",
                              kCoordinateTypes, ", got ", type.ToString()));
    }
    spec->axes[axis].type = type;
    spec->axes[axis].family = *family;
  }
  const auto option = [&](CurveOption index, std::string_view name) {
    return OptionValue(context, args[dimensions + index], name);
  };

  const auto kind = option(kKindOption, "kind");
  const auto curve = kind.IsNull()
                       ? std::string{kMortonCurve}
                       : absl::AsciiStrToLower(duckdb::StringValue::Get(kind));
  if (curve != kMortonCurve && curve != kHilbertCurve) {
    THROW_SQL_ERROR(ERR_CODE(ERRCODE_INVALID_PARAMETER_VALUE),
                    ERR_MSG("curve(): option \"kind\" must be '", kMortonCurve,
                            "' or '", kHilbertCurve, "', got '", curve, "'"));
  }
  options.hilbert = curve == kHilbertCurve;
  options.max_level = LimitOption(option(kMaxLevelOption, "max_level"),
                                  "max_level", 0, irs::curve::kMaxLevel);
  const auto step = option(kLevelStepOption, "level_step");
  options.level_step =
    step.IsNull() ? irs::curve::DefaultLevelStep(options.dimensions)
                  : LimitOption(step, "level_step", 1,
                                irs::curve::MaxLevelStep(options.dimensions));
  options.max_cells = LimitOption(option(kMaxCellsOption, "max_cells"),
                                  "max_cells", 1, irs::curve::kMaxCells);

  const auto lower = option(kLowerOption, "lower");
  const auto upper = option(kUpperOption, "upper");
  for (uint32_t axis = 0; axis < options.dimensions; ++axis) {
    Normalize(spec->axes[axis], axis,
              BoundComponent(lower, "lower", axis, options.dimensions),
              BoundComponent(upper, "upper", axis, options.dimensions));
  }
  return spec;
}

const CurveSpec* TryGetCurveSpec(const duckdb::Expression& expression) {
  if (expression.GetExpressionClass() !=
      duckdb::ExpressionClass::BOUND_FUNCTION) {
    return nullptr;
  }
  const auto& function = expression.Cast<duckdb::BoundFunctionExpression>();
  if (function.Function().GetName().GetIdentifierName() != kCurveFunction ||
      !function.BindInfo()) {
    return nullptr;
  }
  return &function.BindInfo()->Cast<CurveSpec>();
}

void EncodeCurveCodes(const CurveSpec& spec, duckdb::DataChunk& args,
                      duckdb::Vector& result) {
  const auto count = args.size();
  const auto dimensions = spec.options.dimensions;
  std::array<duckdb::RecursiveUnifiedVectorFormat, irs::curve::kMaxDimensions>
    formats;
  for (uint32_t axis = 0; axis < dimensions; ++axis) {
    if (spec.axes[axis].family == CurveFamily::Null) {
      result.SetVectorType(duckdb::VectorType::CONSTANT_VECTOR);
      duckdb::ConstantVector::SetNull(result, true);
      return;
    }
    duckdb::Vector::RecursiveToUnifiedFormat(args.data[axis], formats[axis]);
  }
  result.SetVectorType(duckdb::VectorType::FLAT_VECTOR);
  auto* out = duckdb::FlatVector::GetDataMutable<duckdb::string_t>(result);
  irs::curve::Point point;
  point.fill(0);
  irs::curve::Code code;
  for (duckdb::idx_t row = 0; row < count; ++row) {
    bool valid = true;
    for (uint32_t axis = 0; axis < dimensions && valid; ++axis) {
      valid = IsValid(spec.axes[axis], formats[axis], row);
    }
    if (!valid) {
      duckdb::FlatVector::SetNull(result, row, true);
      continue;
    }
    for (uint32_t axis = 0; axis < dimensions; ++axis) {
      point[axis] = AxisKey(spec.axes[axis], formats[axis], row);
    }
    const auto size = irs::curve::LeafCode(point, spec.options, code.data());
    out[row] = duckdb::StringVector::AddStringOrBlob(
      result, reinterpret_cast<const char*>(code.data()), size);
  }
  if (args.AllConstant()) {
    result.SetVectorType(duckdb::VectorType::CONSTANT_VECTOR);
  }
}

}  // namespace sdb::connector
