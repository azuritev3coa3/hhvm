/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/opt.h"

#include "hphp/runtime/vm/jit/check.h"
#include "hphp/runtime/vm/jit/guard-relaxation.h"
#include "hphp/runtime/vm/jit/ir-builder.h"
#include "hphp/runtime/vm/jit/ir-unit.h"
#include "hphp/runtime/vm/jit/mutation.h"
#include "hphp/runtime/vm/jit/print.h"
#include "hphp/runtime/vm/jit/simplify.h"
#include "hphp/runtime/vm/jit/timer.h"
#include "hphp/runtime/vm/jit/dce.h"

#include "hphp/util/trace.h"

#include <boost/dynamic_bitset.hpp>

namespace HPHP { namespace jit {

namespace {

//////////////////////////////////////////////////////////////////////

enum class DCE { None, Minimal, Full };

template<class PassFN>
void doPass(IRUnit& unit, PassFN fn, DCE dce) {
  fn(unit);
  switch (dce) {
  case DCE::Minimal:  mandatoryDCE(unit); break;
  case DCE::Full:     fullDCE(unit); // fallthrough
  case DCE::None:     assertx(checkEverything(unit)); break;
  }
}

//////////////////////////////////////////////////////////////////////

}

void optimize(IRUnit& unit, IRBuilder& irBuilder, TransKind kind) {
  Timer timer(Timer::optimize);

  assertx(checkEverything(unit));

  auto const hasLoop = RuntimeOption::EvalJitLoops && cfgHasLoop(unit);
  auto const func = unit.entry()->front().marker().func();
  auto const regionMode = pgoRegionMode(*func);
  auto const traceMode = kind != TransKind::Optimize ||
                         regionMode == PGORegionMode::Hottrace;

  // TODO (#5792564): Guard relaxation doesn't work with loops.
  // TODO (#6599498): Guard relaxation is broken in wholecfg mode.
  if (shouldHHIRRelaxGuards() && !hasLoop && traceMode) {
    Timer _t(Timer::optimize_relaxGuards);
    const bool simple = kind == TransKind::Profile &&
                        (RuntimeOption::EvalJitRegionSelector == "tracelet" ||
                         RuntimeOption::EvalJitRegionSelector == "method");
    RelaxGuardsFlags flags = (RelaxGuardsFlags)
      (RelaxReflow | (simple ? RelaxSimple : RelaxNormal));
    auto changed = relaxGuards(unit, *irBuilder.guards(), flags);
    if (changed) {
      printUnit(6, unit, "after guard relaxation");
      mandatoryDCE(unit);  // relaxGuards can leave unreachable preds.
    }

    if (RuntimeOption::EvalHHIRSimplification) {
      doPass(unit, simplifyPass, DCE::Minimal);
      doPass(unit, cleanCfg, DCE::None);
    }
  }

  fullDCE(unit);
  printUnit(6, unit, "after initial DCE");
  assertx(checkEverything(unit));

  if (RuntimeOption::EvalHHIRPredictionOpts) {
    doPass(unit, optimizePredictions, DCE::None);
  }

  if (RuntimeOption::EvalHHIRSimplification) {
    doPass(unit, simplifyPass, DCE::Full);
    doPass(unit, cleanCfg, DCE::None);
  }

  if (RuntimeOption::EvalHHIRGlobalValueNumbering) {
    doPass(unit, gvn, DCE::Full);
  }

  if (kind != TransKind::Profile && RuntimeOption::EvalHHIRMemoryOpts) {
    doPass(unit, optimizeLoads, DCE::Full);
  }

  if (kind != TransKind::Profile && RuntimeOption::EvalHHIRMemoryOpts) {
    doPass(unit, optimizeStores, DCE::Full);
  }

  if (kind != TransKind::Profile && RuntimeOption::EvalHHIRRefcountOpts) {
    doPass(unit, optimizeRefcounts2, DCE::Full);
  }

  if (RuntimeOption::EvalHHIRGenerateAsserts) {
    doPass(unit, insertAsserts, DCE::None);
  }
}

//////////////////////////////////////////////////////////////////////

}}
