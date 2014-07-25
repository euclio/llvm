//=-- CoverageMappingWriter.cpp - Code coverage mapping writer -------------=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains support for writing coverage mapping data for
// instrumentation based coverage.
//
//===----------------------------------------------------------------------===//

#include "llvm/ProfileData/CoverageMappingWriter.h"
#include "llvm/Support/LEB128.h"

using namespace llvm;
using namespace coverage;

void CoverageFilenamesSectionWriter::write(raw_ostream &OS) {
  encodeULEB128(Filenames.size(), OS);
  for (const auto &Filename : Filenames) {
    encodeULEB128(Filename.size(), OS);
    OS << Filename;
  }
}

namespace {
/// \brief Gather only the expressions that are used by the mapping
/// regions in this function.
class CounterExpressionsMinimizer {
  ArrayRef<CounterExpression> Expressions;
  llvm::SmallVector<CounterExpression, 16> UsedExpressions;
  std::vector<unsigned> AdjustedExpressionIDs;

public:
  void mark(Counter C) {
    if (!C.isExpression())
      return;
    unsigned ID = C.getExpressionID();
    AdjustedExpressionIDs[ID] = 1;
    mark(Expressions[ID].LHS);
    mark(Expressions[ID].RHS);
  }

  void gatherUsed(Counter C) {
    if (!C.isExpression() || !AdjustedExpressionIDs[C.getExpressionID()])
      return;
    AdjustedExpressionIDs[C.getExpressionID()] = UsedExpressions.size();
    const auto &E = Expressions[C.getExpressionID()];
    UsedExpressions.push_back(E);
    gatherUsed(E.LHS);
    gatherUsed(E.RHS);
  }

  CounterExpressionsMinimizer(ArrayRef<CounterExpression> Expressions,
                              ArrayRef<CounterMappingRegion> MappingRegions)
      : Expressions(Expressions) {
    AdjustedExpressionIDs.resize(Expressions.size(), 0);
    for (const auto &I : MappingRegions)
      mark(I.Count);
    for (const auto &I : MappingRegions)
      gatherUsed(I.Count);
  }

  ArrayRef<CounterExpression> getExpressions() const { return UsedExpressions; }

  /// \brief Adjust the given counter to correctly transition from the old
  /// expression ids to the new expression ids.
  Counter adjust(Counter C) const {
    if (C.isExpression())
      C = Counter::getExpression(AdjustedExpressionIDs[C.getExpressionID()]);
    return C;
  }
};
}

/// \brief Return the number of regions that have the given FileID.
static unsigned countFileIDs(ArrayRef<CounterMappingRegion> Regions,
                             unsigned FileID) {
  unsigned Result = 0;
  for (const auto &I : Regions) {
    if (I.FileID == FileID)
      ++Result;
    if (I.FileID > FileID)
      break;
  }
  return Result;
}

/// \brief Encode the counter.
///
/// The encoding uses the following format:
/// Low 2 bits - Tag:
///   Counter::Zero(0) - A Counter with kind Counter::Zero
///   Counter::CounterValueReference(1) - A counter with kind
///     Counter::CounterValueReference
///   Counter::Expression(2) + CounterExpression::Subtract(0) -
///     A counter with kind Counter::Expression and an expression
///     with kind CounterExpression::Subtract
///   Counter::Expression(2) + CounterExpression::Add(1) -
///     A counter with kind Counter::Expression and an expression
///     with kind CounterExpression::Add
/// Remaining bits - Counter/Expression ID.
unsigned encodeCounter(ArrayRef<CounterExpression> Expressions, Counter C) {
  unsigned Tag = unsigned(C.getKind());
  if (C.isExpression())
    Tag += Expressions[C.getExpressionID()].Kind;
  unsigned ID = C.getCounterID();
  assert(ID <=
         (std::numeric_limits<unsigned>::max() >> Counter::EncodingTagBits));
  return Tag | (ID << Counter::EncodingTagBits);
}

static void writeCounter(ArrayRef<CounterExpression> Expressions, Counter C,
                         raw_ostream &OS) {
  encodeULEB128(encodeCounter(Expressions, C), OS);
}

void CoverageMappingWriter::write(raw_ostream &OS) {
  // Sort the regions in an ascending order by the file id and the starting
  // location.
  std::sort(MappingRegions.begin(), MappingRegions.end());

  // Write out the fileid -> filename mapping.
  encodeULEB128(VirtualFileMapping.size(), OS);
  for (const auto &FileID : VirtualFileMapping)
    encodeULEB128(FileID, OS);

  // Write out the expressions.
  CounterExpressionsMinimizer Minimizer(Expressions, MappingRegions);
  auto MinExpressions = Minimizer.getExpressions();
  encodeULEB128(MinExpressions.size(), OS);
  for (const auto &E : MinExpressions) {
    writeCounter(MinExpressions, Minimizer.adjust(E.LHS), OS);
    writeCounter(MinExpressions, Minimizer.adjust(E.RHS), OS);
  }

  // Write out the mapping regions.
  // Split the regions into subarrays where each region in a
  // subarray has a fileID which is the index of that subarray.
  unsigned PrevLineStart = 0;
  unsigned CurrentFileID = MappingRegions.front().FileID;
  assert(CurrentFileID == 0);
  encodeULEB128(countFileIDs(MappingRegions, CurrentFileID), OS);
  for (const auto &I : MappingRegions) {
    if (I.FileID != CurrentFileID) {
      // Ensure that all file ids have at least one mapping region.
      assert(I.FileID == (CurrentFileID + 1));
      // Start a new region sub-array.
      CurrentFileID = I.FileID;
      encodeULEB128(countFileIDs(MappingRegions, CurrentFileID), OS);
      PrevLineStart = 0;
    }
    Counter Count = Minimizer.adjust(I.Count);
    switch (I.Kind) {
    case CounterMappingRegion::CodeRegion:
      writeCounter(MinExpressions, Count, OS);
      break;
    case CounterMappingRegion::ExpansionRegion: {
      assert(Count.isZero());
      assert(I.ExpandedFileID <=
             (std::numeric_limits<unsigned>::max() >>
              Counter::EncodingCounterTagAndExpansionRegionTagBits));
      // Mark an expansion region with a set bit that follows the counter tag,
      // and pack the expanded file id into the remaining bits.
      unsigned EncodedTagExpandedFileID =
          (1 << Counter::EncodingTagBits) |
          (I.ExpandedFileID
           << Counter::EncodingCounterTagAndExpansionRegionTagBits);
      encodeULEB128(EncodedTagExpandedFileID, OS);
      break;
    }
    case CounterMappingRegion::EmptyRegion:
      encodeULEB128(unsigned(I.Kind)
                        << Counter::EncodingCounterTagAndExpansionRegionTagBits,
                    OS);
      break;
    case CounterMappingRegion::SkippedRegion:
      assert(Count.isZero());
      encodeULEB128(unsigned(I.Kind)
                        << Counter::EncodingCounterTagAndExpansionRegionTagBits,
                    OS);
      break;
    }
    assert(I.LineStart >= PrevLineStart);
    encodeULEB128(I.LineStart - PrevLineStart, OS);
    encodeULEB128(I.ColumnStart, OS);
    assert(I.LineEnd >= I.LineStart);
    encodeULEB128(I.LineEnd - I.LineStart, OS);
    encodeULEB128(I.ColumnEnd, OS);
    PrevLineStart = I.LineStart;
  }
  // Ensure that all file ids have at least one mapping region.
  assert(CurrentFileID == (VirtualFileMapping.size() - 1));
}