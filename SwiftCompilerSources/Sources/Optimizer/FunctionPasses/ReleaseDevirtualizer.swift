//===--- ReleaseDevirtualizer.swift - Devirtualizes release-instructions --===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL

/// Devirtualizes release instructions which are known to destruct the object.
///
/// This means, it replaces a sequence of
///    %x = alloc_ref [stack] $X
///      ...
///    strong_release %x
///    dealloc_stack_ref %x
/// with
///    %x = alloc_ref [stack] $X
///      ...
///    set_deallocating %x
///    %d = function_ref @dealloc_of_X
///    %a = apply %d(%x)
///    dealloc_stack_ref %x
///
/// The optimization is only done for stack promoted objects because they are
/// known to have no associated objects (which are not explicitly released
/// in the deinit method).
let releaseDevirtualizerPass = FunctionPass(
  name: "release-devirtualizer", { function, context in
    let functionInfo = context.rcIdentityAnalysis.getFunctionInfo(function)

    for block in function.blocks {
      // The last `release_value`` or `strong_release`` instruction before the
      // deallocation.
      var lastRelease: RefCountingInst?

      for instruction in block.instructions {
        if let release = lastRelease {
          // We only do the optimization for stack promoted object, because for
          // these we know that they don't have associated objects, which are
          // _not_ released by the deinit method.
          if let deallocStackRef = instruction as? DeallocStackRefInst {
            devirtualizeReleaseOfObject(context, release, deallocStackRef, functionInfo)
            lastRelease = nil
            continue
          }
        }

        if instruction is ReleaseValueInst || instruction is StrongReleaseInst {
          lastRelease = instruction as? RefCountingInst
        } else if instruction.mayReleaseOrReadRefCount {
          lastRelease = nil
        }
      }
    }
  }
)

/// Devirtualize releases of array buffers.
private func devirtualizeReleaseOfObject(
  _ context: PassContext,
  _ release: RefCountingInst,
  _ deallocStackRef: DeallocStackRefInst,
  _ functionInfo: RCIdentityFunctionInfo
) {
  let allocRefInstruction = deallocStackRef.allocRef
  let rootObject = functionInfo.getRCIdentityRoot(release.operands[0].value)
  var root = release.operands[0].value
  while let newRoot = getRCIdentityPreservingCastOperand(root) {
    root = newRoot
  }

  precondition(rootObject === root)

  guard rootObject === allocRefInstruction else {
    return
  }

  guard rootObject === release.operands[0].value else {
    print(release.description)
    print(release.block.function.description)
    fatalError()
  }

  createDeallocCall(context, allocRefInstruction.type, release, allocRefInstruction)
}

/// Replaces the release-instruction `release` with an explicit call to
/// the deallocating destructor of `type` for `object`.
private func createDeallocCall(
  _ context: PassContext,
  _ type: Type,
  _ release: RefCountingInst,
  _ object: Value
) {
  guard let dealloc = context.getDestructor(ofClass: type) else {
    return
  }

  let substitutionMap = context.getContextSubstitutionMap(for: type)

  let builder = Builder(at: release, location: release.location, context)

  var object = object
  if object.type != type {
    object = builder.createUncheckedRefCast(object: object, type: type)
  }

  // Do what a release would do before calling the deallocator: set the object
  // in deallocating state, which means set the RC_DEALLOCATING_FLAG flag.
  builder.createSetDeallocating(operand: object, isAtomic: release.isAtomic)

  // Create the call to the destructor with the allocated object as self
  // argument.
  let functionRef = builder.createFunctionRef(dealloc)

  builder.createApply(function: functionRef, substitutionMap, arguments: [object])
  context.erase(instruction: release)
}

private func getRCIdentityPreservingCastOperand(_ value: Value) -> Value? {
  switch value {
  case let inst as Instruction where inst is UpcastInst ||
       inst is UncheckedRefCastInst ||
       inst is InitExistentialRefInst ||
       inst is OpenExistentialRefInst ||
       inst is RefToBridgeObjectInst ||
       inst is BridgeObjectToRefInst ||
       inst is ConvertFunctionInst:
    return inst.operands[0].value

  default:
    return nil
  }
}
