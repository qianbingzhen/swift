//===-------- AutoDiff.cpp - Routines for USR generation ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/AutoDiff.h"
#include "swift/AST/Types.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Range.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"

using namespace swift;

SILAutoDiffIndices::SILAutoDiffIndices(
    unsigned source, ArrayRef<unsigned> parameters) : source(source) {
  if (parameters.empty())
    return;

  auto max = *std::max_element(parameters.begin(), parameters.end());
  this->parameters.resize(max + 1);
  int last = -1;
  for (auto paramIdx : parameters) {
    assert((int)paramIdx > last && "Parameter indices must be ascending");
    last = paramIdx;
    this->parameters.set(paramIdx);
  }
}

bool SILAutoDiffIndices::operator==(
    const SILAutoDiffIndices &other) const {
  if (source != other.source)
    return false;

  // The parameters are the same when they have exactly the same set bit
  // indices, even if they have different sizes.
  llvm::SmallBitVector buffer(std::max(parameters.size(),
                                       other.parameters.size()));
  buffer ^= parameters;
  buffer ^= other.parameters;
  return buffer.none();
}

AutoDiffAssociatedFunctionKind::
AutoDiffAssociatedFunctionKind(StringRef string) {
  Optional<innerty> result =
      llvm::StringSwitch<Optional<innerty>>(string)
         .Case("jvp", JVP).Case("vjp", VJP);
  assert(result && "Invalid string");
  rawValue = *result;
}

Differentiability::Differentiability(AutoDiffMode mode,
                                     bool wrtSelf,
                                     llvm::SmallBitVector parameterIndices,
                                     llvm::SmallBitVector resultIndices)
    : mode(mode), wrtSelf(wrtSelf), parameterIndices(parameterIndices),
      resultIndices(resultIndices) {
}

Differentiability::Differentiability(AutoDiffMode mode,
                                     AnyFunctionType *type)
    : mode(mode), wrtSelf(type->getExtInfo().hasSelfParam()),
      // For now, we assume exactly one result until we figure out how to
      // model result selection.
      resultIndices(1) {
  // If function has self, it must be a curried method type.
  if (wrtSelf) {
    auto methodTy = type->getResult()->castTo<AnyFunctionType>();
    parameterIndices = llvm::SmallBitVector(methodTy->getNumParams());
  } else {
    parameterIndices = llvm::SmallBitVector(type->getNumParams());
  }
  parameterIndices.set();
  resultIndices.set();
}

unsigned autodiff::getOffsetForAutoDiffAssociatedFunction(
    unsigned order, AutoDiffAssociatedFunctionKind kind) {
  return (order - 1) * getNumAutoDiffAssociatedFunctions(order) + kind.rawValue;
}

unsigned
autodiff::getNumAutoDiffAssociatedFunctions(unsigned differentiationOrder) {
  return differentiationOrder * 2;
}

/// If `isMethod` is true, returns the non-self part of `functionType`. (e.g.
/// "(Self) -> (A, B) -> R" becomes "(A, B) -> R"). Otherwise, returns
/// `functionType` unmodified.
static AnyFunctionType *unwrapSelfParameter(AnyFunctionType *functionType,
                                            bool isMethod) {
  if (isMethod) {
    assert(functionType->getNumParams() == 1 &&
           "unexpected num params for method");
    return functionType->getResult()->castTo<AnyFunctionType>();
  }
  return functionType;
}

/// Allocates and initializes an empty `AutoDiffParameterIndices` for the
/// given `functionType`. `isMethod` specifies whether to treat the function
/// as a method.
AutoDiffParameterIndices *
AutoDiffParameterIndices::create(ASTContext &C, AnyFunctionType *functionType,
                                 bool isMethod, bool setAllParams) {
  // TODO(SR-9290): Note that the AutoDiffParameterIndices' destructor never
  // gets called, which causes a small memory leak in the case that the
  // SmallBitVector decides to allocate some heap space.
  void *mem = C.Allocate(sizeof(AutoDiffParameterIndices),
                         alignof(AutoDiffParameterIndices));
  unsigned paramCount =
      unwrapSelfParameter(functionType, isMethod)->getNumParams() +
      (isMethod ? 1 : 0);
  return
      ::new (mem) AutoDiffParameterIndices(paramCount, isMethod, setAllParams);
}

/// Allocates and initializes an `AutoDiffParameterIndices` corresponding to
/// the given `string` generated by `getString()`. If the string is invalid,
/// returns nullptr.
AutoDiffParameterIndices *
AutoDiffParameterIndices::create(ASTContext &C, StringRef string) {
  if (string.size() < 2)
    return nullptr;

  bool isMethod = false;
  llvm::SmallBitVector indices(string.size() - 1);
  if (string[0] == 'M')
    isMethod = true;
  else if (string[0] != 'F')
    return nullptr;
  for (unsigned i : range(indices.size())) {
    if (string[i + 1] == 'S')
      indices.set(i);
    else if (string[i + 1] != 'U')
      return nullptr;
  }

  // TODO(SR-9290): Note that the AutoDiffParameterIndices' destructor never
  // gets called, which causes a small memory leak in the case that the
  // SmallBitVector decides to allocate some heap space.
  void *mem = C.Allocate(sizeof(AutoDiffParameterIndices),
                         alignof(AutoDiffParameterIndices));
  return ::new (mem) AutoDiffParameterIndices(indices, isMethod);
}

/// Returns a textual string description of these indices,
///
///   [FM][SU]+
///
/// "F" means that `isMethodFlag` is false
/// "M" means that `isMethodFlag` is true
/// "S" means that the corresponding index is set
/// "U" means that the corresponding index is unset
std::string AutoDiffParameterIndices::getString() const {
  std::string result;
  if (isMethodFlag)
    result += "M";
  else
    result += "F";
  for (unsigned i : range(indices.size())) {
    if (indices[i])
      result += "S";
    else
      result += "U";
  }
  return result;
}

unsigned AutoDiffParameterIndices::getNumNonSelfParameters() const {
  return indices.size() - (isMethodFlag ? 1 : 0);
}

/// Adds the indexed parameter to the set. When `isMethodFlag` is not set, the
/// indices index into the first parameter list. For example,
///
///   functionType = (A, B, C) -> R
///   paramIndex = 0
///   ==> adds "A" to the set.
///
/// When `isMethodFlag` is set, the indices index into the first non-self
/// parameter list. For example,
///
///   functionType = (Self) -> (A, B, C) -> R
///   paramIndex = 0
///   ==> adds "A" to the set.
///
void AutoDiffParameterIndices::setNonSelfParameter(unsigned paramIndex) {
  assert(paramIndex < getNumNonSelfParameters() && "paramIndex out of bounds");
  indices.set(paramIndex);
}

/// Adds all the paramaters from the first non-self parameter list to the set.
/// For example,
///
///   functionType = (A, B, C) -> R
///   ==> adds "A", B", and "C" to the set.
///
///   functionType = (Self) -> (A, B, C) -> R
///   ==> adds "A", B", and "C" to the set.
///
void AutoDiffParameterIndices::setAllNonSelfParameters() {
  indices.set(0, getNumNonSelfParameters());
}

/// Adds the self parameter to the set. `isMethodFlag` must be set. For
/// example,
///
///   functionType = (Self) -> (A, B, C) -> R
///   ==> adds "Self" to the set
///
void AutoDiffParameterIndices::setSelfParameter() {
  assert(isMethodFlag &&
         "trying to add self param to non-method parameter indices");
  indices.set(indices.size() - 1);
}

/// Pushes the subset's parameter's types to `paramTypes`, in the order in
/// which they appear in the function type. For example,
///
///   functionType = (A, B, C) -> R
///   if "A" and "C" are in the set,
///   ==> pushes {A, C} to `paramTypes`.
///
///   functionType = (Self) -> (A, B, C) -> R
///   if "Self" and "C" are in the set,
///   ==> pushes {Self, C} to `paramTypes`.
///
/// Pass `selfUncurried = true` when the function type is for a method whose
/// self parameter has been uncurried as in (A, B, C, Self) -> R.
///
void AutoDiffParameterIndices::getSubsetParameterTypes(
    AnyFunctionType *functionType, SmallVectorImpl<Type> &paramTypes,
    bool selfUncurried) const {
  if (selfUncurried && isMethodFlag) {
    if (isMethodFlag && indices[indices.size() - 1])
      paramTypes.push_back(functionType->getParams()[functionType->getNumParams() - 1].getPlainType());
    for (unsigned paramIndex : range(functionType->getNumParams() - 1))
      if (indices[paramIndex])
        paramTypes.push_back(functionType->getParams()[paramIndex].getPlainType());
  } else {
    AnyFunctionType *unwrapped = unwrapSelfParameter(functionType, isMethodFlag);
    if (isMethodFlag && indices[indices.size() - 1])
      paramTypes.push_back(functionType->getParams()[0].getPlainType());
    for (unsigned paramIndex : range(unwrapped->getNumParams()))
      if (indices[paramIndex])
        paramTypes.push_back(unwrapped->getParams()[paramIndex].getPlainType());
  }
}

static unsigned countNumFlattenedElementTypes(Type type) {
  if (auto *tupleTy = type->getCanonicalType()->getAs<TupleType>())
    return accumulate(tupleTy->getElementTypes(), 0,
                      [&](unsigned num, Type type) {
                        return num + countNumFlattenedElementTypes(type);
                      });
  return 1;
}

/// Returns a bitvector for the SILFunction parameters corresponding to the
/// parameters in this set. In particular, this explodes tuples and puts the
/// method self parameter at the end. For example,
///
///   functionType = (A, B, C) -> R
///   if "A" and "C" are in the set,
///   ==> returns 101
///   (because the lowered SIL type is (A, B, C) -> R)
///
///   functionType = (Self) -> (A, B, C) -> R
///   if "Self" and "C" are in the set,
///   ==> returns 0011
///   (because the lowered SIL type is (A, B, C, Self) -> R)
///
///   functionType = (A, (B, C), D) -> R
///   if "A" and "(B, C)" are in the set,
///   ==> returns 1110
///   (because the lowered SIL type is (A, B, C, D) -> R)
///
/// Pass `selfUncurried = true` when the function type is a for method whose
/// self parameter has been uncurried as in (A, B, C, Self) -> R.
///
llvm::SmallBitVector
AutoDiffParameterIndices::getLowered(AnyFunctionType *functionType,
                                     bool selfUncurried) const {
  // Calculate the lowered sizes of all the parameters.
  AnyFunctionType *unwrapped = selfUncurried
      ? functionType
      : unwrapSelfParameter(functionType, isMethodFlag);
  SmallVector<unsigned, 8> paramLoweredSizes;
  unsigned totalLoweredSize = 0;
  auto addLoweredParamInfo = [&](Type type) {
    unsigned paramLoweredSize = countNumFlattenedElementTypes(type);
    paramLoweredSizes.push_back(paramLoweredSize);
    totalLoweredSize += paramLoweredSize;
  };
  for (auto &param : unwrapped->getParams())
    addLoweredParamInfo(param.getPlainType());
  if (isMethodFlag && !selfUncurried)
    addLoweredParamInfo(functionType->getParams()[0].getPlainType());

  // Construct the result by setting each range of bits that corresponds to each
  // "on" parameter.
  llvm::SmallBitVector result(totalLoweredSize);
  unsigned currentBitIndex = 0;
  for (unsigned i : range(indices.size())) {
    auto paramLoweredSize = paramLoweredSizes[i];
    if (indices[i])
      result.set(currentBitIndex, currentBitIndex + paramLoweredSize);
    currentBitIndex += paramLoweredSize;
  }

  return result;
}

AutoDiffAssociatedFunctionIdentifier *
AutoDiffAssociatedFunctionIdentifier::get(
      AutoDiffAssociatedFunctionKind kind, unsigned differentiationOrder,
      AutoDiffParameterIndices *parameterIndices, ASTContext &C) {
  auto *newAutoDiffAssociatedFunctionIdentifier =
      C.Allocate<AutoDiffAssociatedFunctionIdentifier>();
  newAutoDiffAssociatedFunctionIdentifier->kind = kind;
  newAutoDiffAssociatedFunctionIdentifier->differentiationOrder =
      differentiationOrder;
  newAutoDiffAssociatedFunctionIdentifier->parameterIndices = parameterIndices;
  return newAutoDiffAssociatedFunctionIdentifier;
}