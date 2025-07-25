//===- VectorTransferSplitRewritePatterns.cpp - Transfer Split Rewrites ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements target-independent patterns to rewrite a vector.transfer
// op into a fully in-bounds part and a partial part.
//
//===----------------------------------------------------------------------===//

#include <optional>

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Utils/StructuredOpsUtils.h"

#include "mlir/Dialect/Vector/Transforms/VectorTransforms.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/VectorInterfaces.h"

#include "llvm/ADT/STLExtras.h"

#define DEBUG_TYPE "vector-transfer-split"

using namespace mlir;
using namespace mlir::vector;

/// Build the condition to ensure that a particular VectorTransferOpInterface
/// is in-bounds.
static Value createInBoundsCond(RewriterBase &b,
                                VectorTransferOpInterface xferOp) {
  assert(xferOp.getPermutationMap().isMinorIdentity() &&
         "Expected minor identity map");
  Value inBoundsCond;
  xferOp.zipResultAndIndexing([&](int64_t resultIdx, int64_t indicesIdx) {
    // Zip over the resulting vector shape and memref indices.
    // If the dimension is known to be in-bounds, it does not participate in
    // the construction of `inBoundsCond`.
    if (xferOp.isDimInBounds(resultIdx))
      return;
    // Fold or create the check that `index + vector_size` <= `memref_size`.
    Location loc = xferOp.getLoc();
    int64_t vectorSize = xferOp.getVectorType().getDimSize(resultIdx);
    OpFoldResult sum = affine::makeComposedFoldedAffineApply(
        b, loc, b.getAffineDimExpr(0) + b.getAffineConstantExpr(vectorSize),
        {xferOp.getIndices()[indicesIdx]});
    OpFoldResult dimSz =
        memref::getMixedSize(b, loc, xferOp.getBase(), indicesIdx);
    auto maybeCstSum = getConstantIntValue(sum);
    auto maybeCstDimSz = getConstantIntValue(dimSz);
    if (maybeCstSum && maybeCstDimSz && *maybeCstSum <= *maybeCstDimSz)
      return;
    Value cond =
        arith::CmpIOp::create(b, loc, arith::CmpIPredicate::sle,
                              getValueOrCreateConstantIndexOp(b, loc, sum),
                              getValueOrCreateConstantIndexOp(b, loc, dimSz));
    // Conjunction over all dims for which we are in-bounds.
    if (inBoundsCond)
      inBoundsCond = arith::AndIOp::create(b, loc, inBoundsCond, cond);
    else
      inBoundsCond = cond;
  });
  return inBoundsCond;
}

/// Split a vector.transfer operation into an in-bounds (i.e., no out-of-bounds
/// masking) fast path and a slow path.
/// If `ifOp` is not null and the result is `success, the `ifOp` points to the
/// newly created conditional upon function return.
/// To accommodate for the fact that the original vector.transfer indexing may
/// be arbitrary and the slow path indexes @[0...0] in the temporary buffer, the
/// scf.if op returns a view and values of type index.
/// At this time, only vector.transfer_read case is implemented.
///
/// Example (a 2-D vector.transfer_read):
/// ```
///    %1 = vector.transfer_read %0[...], %pad : memref<A...>, vector<...>
/// ```
/// is transformed into:
/// ```
///    %1:3 = scf.if (%inBounds) {
///      // fast path, direct cast
///      memref.cast %A: memref<A...> to compatibleMemRefType
///      scf.yield %view : compatibleMemRefType, index, index
///    } else {
///      // slow path, not in-bounds vector.transfer or linalg.copy.
///      memref.cast %alloc: memref<B...> to compatibleMemRefType
///      scf.yield %4 : compatibleMemRefType, index, index
//     }
///    %0 = vector.transfer_read %1#0[%1#1, %1#2] {in_bounds = [true ... true]}
/// ```
/// where `alloc` is a top of the function alloca'ed buffer of one vector.
///
/// Preconditions:
///  1. `xferOp.getPermutationMap()` must be a minor identity map
///  2. the rank of the `xferOp.memref()` and the rank of the
///     `xferOp.getVector()` must be equal. This will be relaxed in the future
///     but requires rank-reducing subviews.
static LogicalResult
splitFullAndPartialTransferPrecondition(VectorTransferOpInterface xferOp) {
  // TODO: support 0-d corner case.
  if (xferOp.getTransferRank() == 0)
    return failure();

  // TODO: expand support to these 2 cases.
  if (!xferOp.getPermutationMap().isMinorIdentity())
    return failure();
  // Must have some out-of-bounds dimension to be a candidate for splitting.
  if (!xferOp.hasOutOfBoundsDim())
    return failure();
  // Don't split transfer operations directly under IfOp, this avoids applying
  // the pattern recursively.
  // TODO: improve the filtering condition to make it more applicable.
  if (isa<scf::IfOp>(xferOp->getParentOp()))
    return failure();
  return success();
}

/// Given two MemRefTypes `aT` and `bT`, return a MemRefType to which both can
/// be cast. If the MemRefTypes don't have the same rank or are not strided,
/// return null; otherwise:
///   1. if `aT` and `bT` are cast-compatible, return `aT`.
///   2. else return a new MemRefType obtained by iterating over the shape and
///   strides and:
///     a. keeping the ones that are static and equal across `aT` and `bT`.
///     b. using a dynamic shape and/or stride for the dimensions that don't
///        agree.
static MemRefType getCastCompatibleMemRefType(MemRefType aT, MemRefType bT) {
  if (memref::CastOp::areCastCompatible(aT, bT))
    return aT;
  if (aT.getRank() != bT.getRank())
    return MemRefType();
  int64_t aOffset, bOffset;
  SmallVector<int64_t, 4> aStrides, bStrides;
  if (failed(aT.getStridesAndOffset(aStrides, aOffset)) ||
      failed(bT.getStridesAndOffset(bStrides, bOffset)) ||
      aStrides.size() != bStrides.size())
    return MemRefType();

  ArrayRef<int64_t> aShape = aT.getShape(), bShape = bT.getShape();
  int64_t resOffset;
  SmallVector<int64_t, 4> resShape(aT.getRank(), 0),
      resStrides(bT.getRank(), 0);
  for (int64_t idx = 0, e = aT.getRank(); idx < e; ++idx) {
    resShape[idx] =
        (aShape[idx] == bShape[idx]) ? aShape[idx] : ShapedType::kDynamic;
    resStrides[idx] =
        (aStrides[idx] == bStrides[idx]) ? aStrides[idx] : ShapedType::kDynamic;
  }
  resOffset = (aOffset == bOffset) ? aOffset : ShapedType::kDynamic;
  return MemRefType::get(
      resShape, aT.getElementType(),
      StridedLayoutAttr::get(aT.getContext(), resOffset, resStrides));
}

/// Casts the given memref to a compatible memref type. If the source memref has
/// a different address space than the target type, a `memref.memory_space_cast`
/// is first inserted, followed by a `memref.cast`.
static Value castToCompatibleMemRefType(OpBuilder &b, Value memref,
                                        MemRefType compatibleMemRefType) {
  MemRefType sourceType = cast<MemRefType>(memref.getType());
  Value res = memref;
  if (sourceType.getMemorySpace() != compatibleMemRefType.getMemorySpace()) {
    sourceType = MemRefType::get(
        sourceType.getShape(), sourceType.getElementType(),
        sourceType.getLayout(), compatibleMemRefType.getMemorySpace());
    res =
        memref::MemorySpaceCastOp::create(b, memref.getLoc(), sourceType, res);
  }
  if (sourceType == compatibleMemRefType)
    return res;
  return memref::CastOp::create(b, memref.getLoc(), compatibleMemRefType, res);
}

/// Operates under a scoped context to build the intersection between the
/// view `xferOp.getbase()` @ `xferOp.getIndices()` and the view `alloc`.
// TODO: view intersection/union/differences should be a proper std op.
static std::pair<Value, Value>
createSubViewIntersection(RewriterBase &b, VectorTransferOpInterface xferOp,
                          Value alloc) {
  Location loc = xferOp.getLoc();
  int64_t memrefRank = xferOp.getShapedType().getRank();
  // TODO: relax this precondition, will require rank-reducing subviews.
  assert(memrefRank == cast<MemRefType>(alloc.getType()).getRank() &&
         "Expected memref rank to match the alloc rank");
  ValueRange leadingIndices =
      xferOp.getIndices().take_front(xferOp.getLeadingShapedRank());
  SmallVector<OpFoldResult, 4> sizes;
  sizes.append(leadingIndices.begin(), leadingIndices.end());
  auto isaWrite = isa<vector::TransferWriteOp>(xferOp);
  xferOp.zipResultAndIndexing([&](int64_t resultIdx, int64_t indicesIdx) {
    using MapList = ArrayRef<ArrayRef<AffineExpr>>;
    Value dimMemRef =
        memref::DimOp::create(b, xferOp.getLoc(), xferOp.getBase(), indicesIdx);
    Value dimAlloc = memref::DimOp::create(b, loc, alloc, resultIdx);
    Value index = xferOp.getIndices()[indicesIdx];
    AffineExpr i, j, k;
    bindDims(xferOp.getContext(), i, j, k);
    SmallVector<AffineMap, 4> maps =
        AffineMap::inferFromExprList(MapList{{i - j, k}}, b.getContext());
    // affine_min(%dimMemRef - %index, %dimAlloc)
    Value affineMin =
        affine::AffineMinOp::create(b, loc, index.getType(), maps[0],
                                    ValueRange{dimMemRef, index, dimAlloc});
    sizes.push_back(affineMin);
  });

  SmallVector<OpFoldResult> srcIndices = llvm::to_vector<4>(llvm::map_range(
      xferOp.getIndices(), [](Value idx) -> OpFoldResult { return idx; }));
  SmallVector<OpFoldResult> destIndices(memrefRank, b.getIndexAttr(0));
  SmallVector<OpFoldResult> strides(memrefRank, b.getIndexAttr(1));
  auto copySrc = memref::SubViewOp::create(
      b, loc, isaWrite ? alloc : xferOp.getBase(), srcIndices, sizes, strides);
  auto copyDest = memref::SubViewOp::create(
      b, loc, isaWrite ? xferOp.getBase() : alloc, destIndices, sizes, strides);
  return std::make_pair(copySrc, copyDest);
}

/// Given an `xferOp` for which:
///   1. `inBoundsCond` and a `compatibleMemRefType` have been computed.
///   2. a memref of single vector `alloc` has been allocated.
/// Produce IR resembling:
/// ```
///    %1:3 = scf.if (%inBounds) {
///      (memref.memory_space_cast %A: memref<A..., addr_space> to memref<A...>)
///      %view = memref.cast %A: memref<A...> to compatibleMemRefType
///      scf.yield %view, ... : compatibleMemRefType, index, index
///    } else {
///      %2 = linalg.fill(%pad, %alloc)
///      %3 = subview %view [...][...][...]
///      %4 = subview %alloc [0, 0] [...] [...]
///      linalg.copy(%3, %4)
///      %5 = memref.cast %alloc: memref<B...> to compatibleMemRefType
///      scf.yield %5, ... : compatibleMemRefType, index, index
///   }
/// ```
/// Return the produced scf::IfOp.
static scf::IfOp
createFullPartialLinalgCopy(RewriterBase &b, vector::TransferReadOp xferOp,
                            TypeRange returnTypes, Value inBoundsCond,
                            MemRefType compatibleMemRefType, Value alloc) {
  Location loc = xferOp.getLoc();
  Value zero = arith::ConstantIndexOp::create(b, loc, 0);
  Value memref = xferOp.getBase();
  return scf::IfOp::create(
      b, loc, inBoundsCond,
      [&](OpBuilder &b, Location loc) {
        Value res = castToCompatibleMemRefType(b, memref, compatibleMemRefType);
        scf::ValueVector viewAndIndices{res};
        llvm::append_range(viewAndIndices, xferOp.getIndices());
        scf::YieldOp::create(b, loc, viewAndIndices);
      },
      [&](OpBuilder &b, Location loc) {
        linalg::FillOp::create(b, loc, ValueRange{xferOp.getPadding()},
                               ValueRange{alloc});
        // Take partial subview of memref which guarantees no dimension
        // overflows.
        IRRewriter rewriter(b);
        std::pair<Value, Value> copyArgs = createSubViewIntersection(
            rewriter, cast<VectorTransferOpInterface>(xferOp.getOperation()),
            alloc);
        memref::CopyOp::create(b, loc, copyArgs.first, copyArgs.second);
        Value casted =
            castToCompatibleMemRefType(b, alloc, compatibleMemRefType);
        scf::ValueVector viewAndIndices{casted};
        viewAndIndices.insert(viewAndIndices.end(), xferOp.getTransferRank(),
                              zero);
        scf::YieldOp::create(b, loc, viewAndIndices);
      });
}

/// Given an `xferOp` for which:
///   1. `inBoundsCond` and a `compatibleMemRefType` have been computed.
///   2. a memref of single vector `alloc` has been allocated.
/// Produce IR resembling:
/// ```
///    %1:3 = scf.if (%inBounds) {
///      (memref.memory_space_cast %A: memref<A..., addr_space> to memref<A...>)
///      memref.cast %A: memref<A...> to compatibleMemRefType
///      scf.yield %view, ... : compatibleMemRefType, index, index
///    } else {
///      %2 = vector.transfer_read %view[...], %pad : memref<A...>, vector<...>
///      %3 = vector.type_cast %extra_alloc :
///        memref<...> to memref<vector<...>>
///      store %2, %3[] : memref<vector<...>>
///      %4 = memref.cast %alloc: memref<B...> to compatibleMemRefType
///      scf.yield %4, ... : compatibleMemRefType, index, index
///   }
/// ```
/// Return the produced scf::IfOp.
static scf::IfOp createFullPartialVectorTransferRead(
    RewriterBase &b, vector::TransferReadOp xferOp, TypeRange returnTypes,
    Value inBoundsCond, MemRefType compatibleMemRefType, Value alloc) {
  Location loc = xferOp.getLoc();
  scf::IfOp fullPartialIfOp;
  Value zero = arith::ConstantIndexOp::create(b, loc, 0);
  Value memref = xferOp.getBase();
  return scf::IfOp::create(
      b, loc, inBoundsCond,
      [&](OpBuilder &b, Location loc) {
        Value res = castToCompatibleMemRefType(b, memref, compatibleMemRefType);
        scf::ValueVector viewAndIndices{res};
        llvm::append_range(viewAndIndices, xferOp.getIndices());
        scf::YieldOp::create(b, loc, viewAndIndices);
      },
      [&](OpBuilder &b, Location loc) {
        Operation *newXfer = b.clone(*xferOp.getOperation());
        Value vector = cast<VectorTransferOpInterface>(newXfer).getVector();
        memref::StoreOp::create(
            b, loc, vector,
            vector::TypeCastOp::create(
                b, loc, MemRefType::get({}, vector.getType()), alloc));

        Value casted =
            castToCompatibleMemRefType(b, alloc, compatibleMemRefType);
        scf::ValueVector viewAndIndices{casted};
        viewAndIndices.insert(viewAndIndices.end(), xferOp.getTransferRank(),
                              zero);
        scf::YieldOp::create(b, loc, viewAndIndices);
      });
}

/// Given an `xferOp` for which:
///   1. `inBoundsCond` and a `compatibleMemRefType` have been computed.
///   2. a memref of single vector `alloc` has been allocated.
/// Produce IR resembling:
/// ```
///    %1:3 = scf.if (%inBounds) {
///      memref.cast %A: memref<A...> to compatibleMemRefType
///      scf.yield %view, ... : compatibleMemRefType, index, index
///    } else {
///      %3 = vector.type_cast %extra_alloc :
///        memref<...> to memref<vector<...>>
///      %4 = memref.cast %alloc: memref<B...> to compatibleMemRefType
///      scf.yield %4, ... : compatibleMemRefType, index, index
///   }
/// ```
static ValueRange
getLocationToWriteFullVec(RewriterBase &b, vector::TransferWriteOp xferOp,
                          TypeRange returnTypes, Value inBoundsCond,
                          MemRefType compatibleMemRefType, Value alloc) {
  Location loc = xferOp.getLoc();
  Value zero = arith::ConstantIndexOp::create(b, loc, 0);
  Value memref = xferOp.getBase();
  return b
      .create<scf::IfOp>(
          loc, inBoundsCond,
          [&](OpBuilder &b, Location loc) {
            Value res =
                castToCompatibleMemRefType(b, memref, compatibleMemRefType);
            scf::ValueVector viewAndIndices{res};
            llvm::append_range(viewAndIndices, xferOp.getIndices());
            scf::YieldOp::create(b, loc, viewAndIndices);
          },
          [&](OpBuilder &b, Location loc) {
            Value casted =
                castToCompatibleMemRefType(b, alloc, compatibleMemRefType);
            scf::ValueVector viewAndIndices{casted};
            viewAndIndices.insert(viewAndIndices.end(),
                                  xferOp.getTransferRank(), zero);
            scf::YieldOp::create(b, loc, viewAndIndices);
          })
      ->getResults();
}

/// Given an `xferOp` for which:
///   1. `inBoundsCond` has been computed.
///   2. a memref of single vector `alloc` has been allocated.
///   3. it originally wrote to %view
/// Produce IR resembling:
/// ```
///    %notInBounds = arith.xori %inBounds, %true
///    scf.if (%notInBounds) {
///      %3 = subview %alloc [...][...][...]
///      %4 = subview %view [0, 0][...][...]
///      linalg.copy(%3, %4)
///   }
/// ```
static void createFullPartialLinalgCopy(RewriterBase &b,
                                        vector::TransferWriteOp xferOp,
                                        Value inBoundsCond, Value alloc) {
  Location loc = xferOp.getLoc();
  auto notInBounds = arith::XOrIOp::create(
      b, loc, inBoundsCond, arith::ConstantIntOp::create(b, loc, true, 1));
  scf::IfOp::create(b, loc, notInBounds, [&](OpBuilder &b, Location loc) {
    IRRewriter rewriter(b);
    std::pair<Value, Value> copyArgs = createSubViewIntersection(
        rewriter, cast<VectorTransferOpInterface>(xferOp.getOperation()),
        alloc);
    memref::CopyOp::create(b, loc, copyArgs.first, copyArgs.second);
    scf::YieldOp::create(b, loc, ValueRange{});
  });
}

/// Given an `xferOp` for which:
///   1. `inBoundsCond` has been computed.
///   2. a memref of single vector `alloc` has been allocated.
///   3. it originally wrote to %view
/// Produce IR resembling:
/// ```
///    %notInBounds = arith.xori %inBounds, %true
///    scf.if (%notInBounds) {
///      %2 = load %alloc : memref<vector<...>>
///      vector.transfer_write %2, %view[...] : memref<A...>, vector<...>
///   }
/// ```
static void createFullPartialVectorTransferWrite(RewriterBase &b,
                                                 vector::TransferWriteOp xferOp,
                                                 Value inBoundsCond,
                                                 Value alloc) {
  Location loc = xferOp.getLoc();
  auto notInBounds = arith::XOrIOp::create(
      b, loc, inBoundsCond, arith::ConstantIntOp::create(b, loc, true, 1));
  scf::IfOp::create(b, loc, notInBounds, [&](OpBuilder &b, Location loc) {
    IRMapping mapping;
    Value load = memref::LoadOp::create(
        b, loc,
        vector::TypeCastOp::create(
            b, loc, MemRefType::get({}, xferOp.getVector().getType()), alloc),
        ValueRange());
    mapping.map(xferOp.getVector(), load);
    b.clone(*xferOp.getOperation(), mapping);
    scf::YieldOp::create(b, loc, ValueRange{});
  });
}

// TODO: Parallelism and threadlocal considerations with a ParallelScope trait.
static Operation *getAutomaticAllocationScope(Operation *op) {
  // Find the closest surrounding allocation scope that is not a known looping
  // construct (putting alloca's in loops doesn't always lower to deallocation
  // until the end of the loop).
  Operation *scope = nullptr;
  for (Operation *parent = op->getParentOp(); parent != nullptr;
       parent = parent->getParentOp()) {
    if (parent->hasTrait<OpTrait::AutomaticAllocationScope>())
      scope = parent;
    if (!isa<scf::ForOp, affine::AffineForOp>(parent))
      break;
  }
  assert(scope && "Expected op to be inside automatic allocation scope");
  return scope;
}

/// Split a vector.transfer operation into an in-bounds (i.e., no out-of-bounds
/// masking) fastpath and a slowpath.
///
/// For vector.transfer_read:
/// If `ifOp` is not null and the result is `success, the `ifOp` points to the
/// newly created conditional upon function return.
/// To accomodate for the fact that the original vector.transfer indexing may be
/// arbitrary and the slow path indexes @[0...0] in the temporary buffer, the
/// scf.if op returns a view and values of type index.
///
/// Example (a 2-D vector.transfer_read):
/// ```
///    %1 = vector.transfer_read %0[...], %pad : memref<A...>, vector<...>
/// ```
/// is transformed into:
/// ```
///    %1:3 = scf.if (%inBounds) {
///      // fastpath, direct cast
///      memref.cast %A: memref<A...> to compatibleMemRefType
///      scf.yield %view : compatibleMemRefType, index, index
///    } else {
///      // slowpath, not in-bounds vector.transfer or linalg.copy.
///      memref.cast %alloc: memref<B...> to compatibleMemRefType
///      scf.yield %4 : compatibleMemRefType, index, index
//     }
///    %0 = vector.transfer_read %1#0[%1#1, %1#2] {in_bounds = [true ... true]}
/// ```
/// where `alloc` is a top of the function alloca'ed buffer of one vector.
///
/// For vector.transfer_write:
/// There are 2 conditional blocks. First a block to decide which memref and
/// indices to use for an unmasked, inbounds write. Then a conditional block to
/// further copy a partial buffer into the final result in the slow path case.
///
/// Example (a 2-D vector.transfer_write):
/// ```
///    vector.transfer_write %arg, %0[...], %pad : memref<A...>, vector<...>
/// ```
/// is transformed into:
/// ```
///    %1:3 = scf.if (%inBounds) {
///      memref.cast %A: memref<A...> to compatibleMemRefType
///      scf.yield %view : compatibleMemRefType, index, index
///    } else {
///      memref.cast %alloc: memref<B...> to compatibleMemRefType
///      scf.yield %4 : compatibleMemRefType, index, index
///     }
///    %0 = vector.transfer_write %arg, %1#0[%1#1, %1#2] {in_bounds = [true ...
///                                                                    true]}
///    scf.if (%notInBounds) {
///      // slowpath: not in-bounds vector.transfer or linalg.copy.
///    }
/// ```
/// where `alloc` is a top of the function alloca'ed buffer of one vector.
///
/// Preconditions:
///  1. `xferOp.getPermutationMap()` must be a minor identity map
///  2. the rank of the `xferOp.getBase()` and the rank of the
///     `xferOp.getVector()` must be equal. This will be relaxed in the future
///     but requires rank-reducing subviews.
LogicalResult mlir::vector::splitFullAndPartialTransfer(
    RewriterBase &b, VectorTransferOpInterface xferOp,
    VectorTransformsOptions options, scf::IfOp *ifOp) {
  if (options.vectorTransferSplit == VectorTransferSplit::None)
    return failure();

  SmallVector<bool, 4> bools(xferOp.getTransferRank(), true);
  auto inBoundsAttr = b.getBoolArrayAttr(bools);
  if (options.vectorTransferSplit == VectorTransferSplit::ForceInBounds) {
    b.modifyOpInPlace(xferOp, [&]() {
      xferOp->setAttr(xferOp.getInBoundsAttrName(), inBoundsAttr);
    });
    return success();
  }

  // Assert preconditions. Additionally, keep the variables in an inner scope to
  // ensure they aren't used in the wrong scopes further down.
  {
    assert(succeeded(splitFullAndPartialTransferPrecondition(xferOp)) &&
           "Expected splitFullAndPartialTransferPrecondition to hold");

    auto xferReadOp = dyn_cast<vector::TransferReadOp>(xferOp.getOperation());
    auto xferWriteOp = dyn_cast<vector::TransferWriteOp>(xferOp.getOperation());

    if (!(xferReadOp || xferWriteOp))
      return failure();
    if (xferWriteOp && xferWriteOp.getMask())
      return failure();
    if (xferReadOp && xferReadOp.getMask())
      return failure();
  }

  RewriterBase::InsertionGuard guard(b);
  b.setInsertionPoint(xferOp);
  Value inBoundsCond = createInBoundsCond(
      b, cast<VectorTransferOpInterface>(xferOp.getOperation()));
  if (!inBoundsCond)
    return failure();

  // Top of the function `alloc` for transient storage.
  Value alloc;
  {
    RewriterBase::InsertionGuard guard(b);
    Operation *scope = getAutomaticAllocationScope(xferOp);
    assert(scope->getNumRegions() == 1 &&
           "AutomaticAllocationScope with >1 regions");
    b.setInsertionPointToStart(&scope->getRegion(0).front());
    auto shape = xferOp.getVectorType().getShape();
    Type elementType = xferOp.getVectorType().getElementType();
    alloc = memref::AllocaOp::create(b, scope->getLoc(),
                                     MemRefType::get(shape, elementType),
                                     ValueRange{}, b.getI64IntegerAttr(32));
  }

  MemRefType compatibleMemRefType =
      getCastCompatibleMemRefType(cast<MemRefType>(xferOp.getShapedType()),
                                  cast<MemRefType>(alloc.getType()));
  if (!compatibleMemRefType)
    return failure();

  SmallVector<Type, 4> returnTypes(1 + xferOp.getTransferRank(),
                                   b.getIndexType());
  returnTypes[0] = compatibleMemRefType;

  if (auto xferReadOp =
          dyn_cast<vector::TransferReadOp>(xferOp.getOperation())) {
    // Read case: full fill + partial copy -> in-bounds vector.xfer_read.
    scf::IfOp fullPartialIfOp =
        options.vectorTransferSplit == VectorTransferSplit::VectorTransfer
            ? createFullPartialVectorTransferRead(b, xferReadOp, returnTypes,
                                                  inBoundsCond,
                                                  compatibleMemRefType, alloc)
            : createFullPartialLinalgCopy(b, xferReadOp, returnTypes,
                                          inBoundsCond, compatibleMemRefType,
                                          alloc);
    if (ifOp)
      *ifOp = fullPartialIfOp;

    // Set existing read op to in-bounds, it always reads from a full buffer.
    for (unsigned i = 0, e = returnTypes.size(); i != e; ++i)
      xferReadOp.setOperand(i, fullPartialIfOp.getResult(i));

    b.modifyOpInPlace(xferOp, [&]() {
      xferOp->setAttr(xferOp.getInBoundsAttrName(), inBoundsAttr);
    });

    return success();
  }

  auto xferWriteOp = cast<vector::TransferWriteOp>(xferOp.getOperation());

  // Decide which location to write the entire vector to.
  auto memrefAndIndices = getLocationToWriteFullVec(
      b, xferWriteOp, returnTypes, inBoundsCond, compatibleMemRefType, alloc);

  // Do an in bounds write to either the output or the extra allocated buffer.
  // The operation is cloned to prevent deleting information needed for the
  // later IR creation.
  IRMapping mapping;
  mapping.map(xferWriteOp.getBase(), memrefAndIndices.front());
  mapping.map(xferWriteOp.getIndices(), memrefAndIndices.drop_front());
  auto *clone = b.clone(*xferWriteOp, mapping);
  clone->setAttr(xferWriteOp.getInBoundsAttrName(), inBoundsAttr);

  // Create a potential copy from the allocated buffer to the final output in
  // the slow path case.
  if (options.vectorTransferSplit == VectorTransferSplit::VectorTransfer)
    createFullPartialVectorTransferWrite(b, xferWriteOp, inBoundsCond, alloc);
  else
    createFullPartialLinalgCopy(b, xferWriteOp, inBoundsCond, alloc);

  b.eraseOp(xferOp);

  return success();
}

namespace {
/// Apply `splitFullAndPartialTransfer` selectively via a pattern. This pattern
/// may take an extra filter to perform selection at a finer granularity.
struct VectorTransferFullPartialRewriter : public RewritePattern {
  using FilterConstraintType =
      std::function<LogicalResult(VectorTransferOpInterface op)>;

  explicit VectorTransferFullPartialRewriter(
      MLIRContext *context,
      VectorTransformsOptions options = VectorTransformsOptions(),
      FilterConstraintType filter =
          [](VectorTransferOpInterface op) { return success(); },
      PatternBenefit benefit = 1)
      : RewritePattern(MatchAnyOpTypeTag(), benefit, context), options(options),
        filter(std::move(filter)) {}

  /// Performs the rewrite.
  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override;

private:
  VectorTransformsOptions options;
  FilterConstraintType filter;
};

} // namespace

LogicalResult VectorTransferFullPartialRewriter::matchAndRewrite(
    Operation *op, PatternRewriter &rewriter) const {
  auto xferOp = dyn_cast<VectorTransferOpInterface>(op);
  if (!xferOp || failed(splitFullAndPartialTransferPrecondition(xferOp)) ||
      failed(filter(xferOp)))
    return failure();
  return splitFullAndPartialTransfer(rewriter, xferOp, options);
}

void mlir::vector::populateVectorTransferFullPartialPatterns(
    RewritePatternSet &patterns, const VectorTransformsOptions &options) {
  patterns.add<VectorTransferFullPartialRewriter>(patterns.getContext(),
                                                  options);
}
