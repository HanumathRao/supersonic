// Copyright 2010 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: onufry@google.com (Onufry Wojtaszczyk)

#include "supersonic/expression/core/string_expressions.h"

#include <string>
using std::string;

#include "supersonic/utils/macros.h"
#include "supersonic/utils/scoped_ptr.h"
#include "supersonic/utils/stringprintf.h"
#include "supersonic/base/exception/exception.h"
#include "supersonic/base/exception/exception_macros.h"
#include "supersonic/base/exception/result.h"
#include "supersonic/base/infrastructure/types.h"
#include "supersonic/expression/base/expression.h"
#include "supersonic/expression/core/string_bound_expressions.h"
#include "supersonic/expression/core/string_bound_expressions_internal.h"
#include "supersonic/expression/core/string_evaluators.h"  // IWYU pragma: keep
#include "supersonic/expression/infrastructure/basic_expressions.h"
#include "supersonic/expression/infrastructure/expression_utils.h"
#include "supersonic/expression/proto/operators.pb.h"
#include "supersonic/expression/templated/abstract_expressions.h"
#include "supersonic/expression/vector/expression_traits.h"
#include "supersonic/proto/supersonic.pb.h"
#include "supersonic/utils/strings/join.h"

namespace supersonic {

class BufferAllocator;
class TupleSchema;

namespace {

// The concatenation expression, extending expression. It doesn't fit into the
// general scheme of abstract_expressions.h as it has an arbitrary number of
// arguments.
class ConcatExpression : public Expression {
 public:
  explicit ConcatExpression(const ExpressionList* const list) : args_(list) {}

 private:
  virtual FailureOrOwned<BoundExpression> DoBind(
      const TupleSchema& input_schema,
      BufferAllocator* allocator,
      rowcount_t max_row_count) const {
    FailureOrOwned<BoundExpressionList> args = args_->DoBind(
        input_schema, allocator, max_row_count);
    PROPAGATE_ON_FAILURE(args);
    return BoundConcat(args.release(), allocator, max_row_count);
  }

  virtual string ToString(bool verbose) const {
    return StrCat("CONCAT(", args_.get()->ToString(verbose), ")");
  }

  const scoped_ptr<const ExpressionList> args_;
};

// The RegExp expressions. They differ from the standard abstract setting in
// having a state - the regexp pattern. This is not an argument (that is - its
// not an expression, as we do not want to compile the pattern each time), but
// really a state.
// TODO(onufry): This could be done through expressions traits, by making them
// stateful and passing an instance of the traits through the building of an
// expression. But that looks like a giant piece of work, and probably isn't
// worth it unless we hit a large number of stateful expressions.
template<OperatorId operation_type>
class RegexpExpression : public UnaryExpression {
 public:
  RegexpExpression(const Expression* const arg,
                   const StringPiece& pattern)
      : UnaryExpression(arg),
        pattern_(pattern.data(), pattern.length()) {}

  virtual string ToString(bool verbose) const {
    return UnaryExpressionTraits<operation_type>::FormatDescription(
        child_expression_->ToString(verbose));
  }

 private:
  virtual FailureOrOwned<BoundExpression> CreateBoundUnaryExpression(
      const TupleSchema& schema,
      BufferAllocator* const allocator,
      rowcount_t row_capacity,
      BoundExpression* child) const {
    scoped_ptr<BoundExpression> child_ptr(child);
    DataType child_type = GetExpressionType(child);
    if (child_type != STRING) {
      THROW(new Exception(ERROR_ATTRIBUTE_TYPE_MISMATCH,
          StringPrintf("Invalid argument type (%s) to %s, STRING expected",
                       GetTypeInfo(child_type).name().c_str(),
                       ToString(true).c_str())));
    }
    return BoundGeneralRegexp<operation_type>(
        child_ptr.release(), pattern_, allocator, row_capacity);
  }

  string pattern_;

  DISALLOW_COPY_AND_ASSIGN(RegexpExpression);
};

class RegexpExtractExpression : public UnaryExpression {
 public:
  RegexpExtractExpression(const Expression* const arg,
                          const StringPiece& pattern)
      : UnaryExpression(arg),
        pattern_(pattern.data(), pattern.length()) {}

  virtual string ToString(bool verbose) const {
    return StringPrintf("REGEXP_EXTRACT(%s)",
                        child_expression_->ToString(verbose).c_str());
  }

 private:
  virtual FailureOrOwned<BoundExpression> CreateBoundUnaryExpression(
      const TupleSchema& schema,
      BufferAllocator* const allocator,
      rowcount_t row_capacity,
      BoundExpression* child) const {
    scoped_ptr<BoundExpression> child_ptr(child);
    DataType child_type = GetExpressionType(child);
    if (child_type != STRING) {
      THROW(new Exception(ERROR_ATTRIBUTE_TYPE_MISMATCH,
          StringPrintf("Invalid argument type (%s) to %s, STRING expected",
                       GetTypeInfo(child_type).name().c_str(),
                       ToString(true).c_str())));
    }
    return BoundRegexpExtract(child_ptr.release(), pattern_, allocator,
                              row_capacity);
  }

  string pattern_;

  DISALLOW_COPY_AND_ASSIGN(RegexpExtractExpression);
};

class RegexpReplaceExpression : public BinaryExpression {
 public:
  RegexpReplaceExpression(const Expression* const haystack,
                          const StringPiece& needle,
                          const Expression* const substitute)
      : BinaryExpression(haystack, substitute),
        pattern_(needle.data(), needle.length()) {}

  virtual string ToString(bool verbose) const {
    return BinaryExpressionTraits<OPERATOR_REGEXP_REPLACE>::FormatDescription(
        left_->ToString(verbose), right_->ToString(verbose));
  }

 private:
  virtual FailureOrOwned<BoundExpression> CreateBoundBinaryExpression(
      const TupleSchema& schema,
      BufferAllocator* const allocator,
      rowcount_t row_capacity,
      BoundExpression* left,
      BoundExpression* right) const {
    scoped_ptr<BoundExpression> left_ptr(left);
    scoped_ptr<BoundExpression> right_ptr(right);
    DataType left_type = GetExpressionType(left);
    if (left_type != STRING) {
      THROW(new Exception(ERROR_ATTRIBUTE_TYPE_MISMATCH,
          StringPrintf("Invalid argument type (%s) as first argument to %s, "
                       "STRING expected",
                       GetTypeInfo(left_type).name().c_str(),
                       ToString(true).c_str())));
    }
    DataType right_type = GetExpressionType(right);
    if (right_type != STRING) {
      THROW(new Exception(ERROR_ATTRIBUTE_TYPE_MISMATCH,
          StringPrintf("Invalid argument type (%s) as last argument to %s, "
                       "STRING expected",
                       GetTypeInfo(right_type).name().c_str(),
                       ToString(true).c_str())));
    }
    return BoundRegexpReplace(left_ptr.release(), pattern_, right_ptr.release(),
                              allocator, row_capacity);
  }

  string pattern_;

  DISALLOW_COPY_AND_ASSIGN(RegexpReplaceExpression);
};

}  // namespace

const Expression* Concat(const ExpressionList* const args) {
  return new ConcatExpression(args);
}

const Expression* Length(const Expression* const str) {
  return CreateExpressionForExistingBoundFactory(str, &BoundLength,
                                                 "LENGTH($0)");
}

const Expression* Ltrim(const Expression* const str) {
  return CreateExpressionForExistingBoundFactory(str, &BoundLtrim, "LTRIM($0)");
}

const Expression* RegexpPartialMatch(const Expression* const str,
                                     const StringPiece& pattern) {
  return new RegexpExpression<OPERATOR_REGEXP_PARTIAL>(str, pattern);
}

const Expression* RegexpFullMatch(const Expression* const str,
                                  const StringPiece& pattern) {
  return new RegexpExpression<OPERATOR_REGEXP_FULL>(str, pattern);
}

const Expression* RegexpExtract(const Expression* const str,
                                const StringPiece& pattern) {
  return new RegexpExtractExpression(str, pattern);
}

const Expression* RegexpReplace(const Expression* const haystack,
                                const StringPiece& needle,
                                const Expression* const substitute) {
  return new RegexpReplaceExpression(haystack, needle, substitute);
}

const Expression* Rtrim(const Expression* const str) {
  return CreateExpressionForExistingBoundFactory(str, &BoundRtrim, "RTRIM($0)");
}

const Expression* StringContains(const Expression* const haystack,
                                 const Expression* const needle) {
  return CreateExpressionForExistingBoundFactory(haystack, needle,
      &BoundContains, "CONTAINS($0, $1)");
}

const Expression* StringContainsCI(const Expression* const haystack,
                                   const Expression* const needle) {
  return CreateExpressionForExistingBoundFactory(haystack, needle,
      &BoundContainsCI, "CONTAINS_CI($0, $1)");
}

const Expression* StringOffset(const Expression* const haystack,
                               const Expression* const needle) {
  return CreateExpressionForExistingBoundFactory(
      haystack, needle, &BoundStringOffset, "STRING_OFFSET($0, $1)");
}

const Expression* StringReplace(const Expression* const haystack,
                                const Expression* const needle,
                                const Expression* const substitute) {
  return CreateExpressionForExistingBoundFactory(
      haystack, needle, substitute, &BoundStringReplace,
      "STRING_REPLACE($0, $1, $2)");
}

const Expression* Substring(const Expression* const str,
                            const Expression* const pos,
                            const Expression* const length) {
  return CreateExpressionForExistingBoundFactory(
      str, pos, length, &BoundSubstring, "SUBSTRING($0, $1, $2)");
}

const Expression* ToLower(const Expression* const str) {
  return CreateExpressionForExistingBoundFactory(str, &BoundToLower,
                                                 "TO_LOWER($0)");
}

const Expression* ToString(const Expression* const expr) {
  return CreateExpressionForExistingBoundFactory(expr, &BoundToString,
                                                 "TO_STRING($0)");
}

const Expression* ToUpper(const Expression* const str) {
  return CreateExpressionForExistingBoundFactory(str, &BoundToUpper,
                                                 "TO_UPPER($0)");
}

const Expression* TrailingSubstring(const Expression* const str,
                                    const Expression* const pos) {
  return CreateExpressionForExistingBoundFactory(
      str, pos, &BoundTrailingSubstring, "SUBSTRING($0, $1)");
}

const Expression* Trim(const Expression* const str) {
  return CreateExpressionForExistingBoundFactory(str, &BoundTrim, "TRIM($0)");
}

}  // namespace supersonic
