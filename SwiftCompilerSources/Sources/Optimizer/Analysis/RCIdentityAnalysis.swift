//===--- CalleeAnalysis.swift - the callee analysis -----------------------===//
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

import OptimizerBridging
import SIL

struct RCIdentityAnalysis {
  let bridged: BridgedRCIdentityAnalysis

  func getFunctionInfo(_ function: Function) -> RCIdentityFunctionInfo {
    RCIdentityFunctionInfo(bridged: RCIdentityAnalysis_getFunctionInfo(bridged, function.bridged))
  }
}

struct RCIdentityFunctionInfo {
  let bridged: BridgedRCIdentityFunctionInfo

  func getRCIdentityRoot(_ value: Value) -> AnyObject {
    RCIdentityFunctionInfo_getRCIdentityRoot(bridged, value.bridged).getAs(AnyObject.self)
  }
}
