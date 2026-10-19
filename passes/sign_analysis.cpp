#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <queue>
#include <set>
#include <string>

using namespace llvm;

namespace {

// Abstract sign domain:
//   Bottom  = no information yet (uninitialized)
//   Negative, Zero, Positive = definite sign
//   Top     = unknown / could be anything
enum class Sign { Bottom, Negative, Zero, Positive, Top };

static const char *signToString(Sign s) {
  switch (s) {
  case Sign::Bottom:
    return "Bottom";
  case Sign::Negative:
    return "Negative";
  case Sign::Zero:
    return "Zero";
  case Sign::Positive:
    return "Positive";
  case Sign::Top:
    return "Top";
  }
  return "?";
}

// Lattice join: merge two signs (least upper bound).
static Sign join(Sign a, Sign b) {
  if (a == Sign::Bottom)
    return b;
  if (b == Sign::Bottom)
    return a;
  if (a == b)
    return a;
  return Sign::Top;
}

// Sign of a constant integer.
static Sign signOfConstant(int64_t val) {
  if (val > 0)
    return Sign::Positive;
  if (val < 0)
    return Sign::Negative;
  return Sign::Zero;
}

// Abstract transfer functions for arithmetic.
static Sign transferNeg(Sign s);

static Sign transferAdd(Sign lhs, Sign rhs) {
  if (lhs == Sign::Bottom || rhs == Sign::Bottom)
    return Sign::Bottom;
  if (lhs == Sign::Top || rhs == Sign::Top)
    return Sign::Top;
  // Zero is the identity.
  if (lhs == Sign::Zero)
    return rhs;
  if (rhs == Sign::Zero)
    return lhs;
  // Same sign is preserved.
  if (lhs == rhs)
    return lhs;
  // Pos + Neg or Neg + Pos => unknown.
  return Sign::Top;
}

static Sign transferSub(Sign lhs, Sign rhs) {
  // a - b == a + (-b), so negate rhs then reuse transferAdd.
  return transferAdd(lhs, transferNeg(rhs));
}

static Sign transferMul(Sign lhs, Sign rhs) {
  if (lhs == Sign::Bottom || rhs == Sign::Bottom)
    return Sign::Bottom;
  if (lhs == Sign::Zero || rhs == Sign::Zero)
    return Sign::Zero;
  if (lhs == Sign::Top || rhs == Sign::Top)
    return Sign::Top;
  if (lhs == rhs)
    return Sign::Positive; // same sign => positive
  return Sign::Negative;   // different signs => negative
}

static Sign transferSDiv(Sign lhs, Sign rhs) {
  // Division by zero is UB in LLVM IR; treat conservatively as Top.
  if (lhs == Sign::Bottom || rhs == Sign::Bottom)
    return Sign::Bottom;
  if (rhs == Sign::Zero)
    return Sign::Top; // UB / conservatively Top
  if (lhs == Sign::Zero)
    return Sign::Zero; // 0 / anything (non-zero) = 0
  if (lhs == Sign::Top || rhs == Sign::Top)
    return Sign::Top;
  // Same sign => positive quotient; different signs => negative quotient.
  if (lhs == rhs)
    return Sign::Positive;
  return Sign::Negative;
}

static Sign transferSRem(Sign lhs, Sign rhs) {
  // Result sign follows the dividend in LLVM's srem.
  if (lhs == Sign::Bottom || rhs == Sign::Bottom)
    return Sign::Bottom;
  if (rhs == Sign::Zero)
    return Sign::Top;
  if (lhs == Sign::Zero)
    return Sign::Zero;
  if (lhs == Sign::Top || rhs == Sign::Top)
    return Sign::Top;
  return lhs; // sign of remainder = sign of dividend
}

static Sign transferNeg(Sign s) {
  if (s == Sign::Bottom)
    return Sign::Bottom;
  if (s == Sign::Positive)
    return Sign::Negative;
  if (s == Sign::Negative)
    return Sign::Positive;
  if (s == Sign::Zero)
    return Sign::Zero;
  return Sign::Top;
}

struct SignAnalysisPass : public PassInfoMixin<SignAnalysisPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &) {
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      analyzeFunction(F);
    }
    return PreservedAnalyses::all();
  }

  void analyzeFunction(Function &F) {
    errs() << "========================================\n";
    errs() << "Sign Analysis for Function: " << F.getName() << "\n";
    errs() << "========================================\n";

    // Map from Value* to its abstract sign.
    std::map<Value *, Sign> signState;

    // Initialize function arguments as Top (unknown).
    for (Argument &A : F.args()) {
      if (A.getType()->isIntegerTy())
        signState[&A] = Sign::Top;
    }

    // Look up sign of a Value, handling constants directly.
    auto getSign = [&](Value *V) -> Sign {
      if (auto *CI = dyn_cast<ConstantInt>(V))
        return signOfConstant(CI->getSExtValue());
      if (isa<UndefValue>(V))
        return Sign::Bottom;
      auto it = signState.find(V);
      if (it != signState.end())
        return it->second;
      return Sign::Bottom;
    };

    // Worklist: set of instructions to (re)evaluate.
    std::queue<Instruction *> worklist;
    std::set<Instruction *> inWorklist;

    // Seed the worklist with all instructions.
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      worklist.push(&*I);
      inWorklist.insert(&*I);
    }

    // Fixed-point iteration.
    while (!worklist.empty()) {
      Instruction *I = worklist.front();
      worklist.pop();
      inWorklist.erase(I);

      // Only analyze integer-typed instructions.
      if (!I->getType()->isIntegerTy())
        continue;

      Sign newSign = Sign::Bottom;

      switch (I->getOpcode()) {
      case Instruction::Add:
        newSign =
            transferAdd(getSign(I->getOperand(0)), getSign(I->getOperand(1)));
        break;
      case Instruction::Sub:
        newSign =
            transferSub(getSign(I->getOperand(0)), getSign(I->getOperand(1)));
        break;
      case Instruction::Mul:
        newSign =
            transferMul(getSign(I->getOperand(0)), getSign(I->getOperand(1)));
        break;
      case Instruction::SDiv:
        newSign =
            transferSDiv(getSign(I->getOperand(0)), getSign(I->getOperand(1)));
        break;
      case Instruction::SRem:
        newSign =
            transferSRem(getSign(I->getOperand(0)), getSign(I->getOperand(1)));
        break;
      case Instruction::UDiv:
      case Instruction::URem:
        // Unsigned: result is non-negative. But we treat as Top unless
        // both operands are known non-negative constants.
        newSign = Sign::Top;
        break;

      case Instruction::Alloca:
        // Alloca returns a pointer, skip (already filtered by isIntegerTy).
        continue;

      case Instruction::Load: {
        // For loads: look through store→load pairs via constant propagation.
        // Strategy: find the pointer operand (typically an alloca), then scan
        // all store instructions that write to that same pointer.  Join the
        // signs of every stored value to get a sound over-approximation.
        // If no matching store is found at all, fall back to Top (unknown).
        Value *ptr = I->getOperand(0); // pointer being loaded from
        Sign merged = Sign::Bottom;
        bool foundAnyStore = false;

        for (User *U : ptr->users()) {
          if (auto *SI = dyn_cast<StoreInst>(U)) {
            // SI->getPointerOperand() must match ptr.
            if (SI->getPointerOperand() == ptr) {
              foundAnyStore = true;
              merged = join(merged, getSign(SI->getValueOperand()));
            }
          }
        }

        // If we never saw a store, the value is unknown (e.g. pointer came
        // from a function argument or we can't track it statically).
        newSign = foundAnyStore ? merged : Sign::Top;
        break;
      }

      case Instruction::Store:
        // Store doesn't produce an integer result.
        continue;

      case Instruction::ICmp:
        // Comparison result is i1; treat as Top (0 or 1).
        newSign = Sign::Top;
        break;

      case Instruction::PHI: {
        auto *PN = cast<PHINode>(I);
        Sign merged = Sign::Bottom;
        for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
          merged = join(merged, getSign(PN->getIncomingValue(i)));
        newSign = merged;
        break;
      }

      case Instruction::Select: {
        // select cond, true_val, false_val → join of both branches
        newSign = join(getSign(I->getOperand(1)), getSign(I->getOperand(2)));
        break;
      }

      case Instruction::SExt:
      case Instruction::Trunc:
        // Sign-extending or truncating preserves sign.
        newSign = getSign(I->getOperand(0));
        break;

      case Instruction::ZExt:
        // Zero-extension: non-negative result.
        {
          Sign src = getSign(I->getOperand(0));
          if (src == Sign::Negative)
            newSign = Sign::Top; // Reinterpretation
          else
            newSign = src;
        }
        break;

      case Instruction::Call: {
        // Return value of a call is unknown.
        newSign = Sign::Top;
        break;
      }

      default:
        // For any other integer-producing instruction, be conservative.
        newSign = Sign::Top;
        break;
      }

      // Check if the sign changed; if so, update and add users to worklist.
      Sign &oldSign = signState[I];
      if (newSign != oldSign) {
        oldSign = newSign;
        // Propagate to all users of this instruction.
        for (User *U : I->users()) {
          if (auto *UI = dyn_cast<Instruction>(U)) {
            if (inWorklist.find(UI) == inWorklist.end()) {
              worklist.push(UI);
              inWorklist.insert(UI);
            }
          }
        }
      }
    }

    // Print results.
    errs() << "\n--- Results ---\n";
    for (BasicBlock &BB : F) {
      errs() << "BasicBlock ";
      BB.printAsOperand(errs(), false);
      errs() << ":\n";
      for (Instruction &I : BB) {
        if (!I.getType()->isIntegerTy()) {
          errs() << "  " << I << "\n";
          continue;
        }
        Sign s = Sign::Bottom;
        if (auto *CI = dyn_cast<ConstantInt>(&I))
          s = signOfConstant(CI->getSExtValue());
        else {
          auto it = signState.find(&I);
          if (it != signState.end())
            s = it->second;
        }
        errs() << "  " << I << "\n";
        errs() << "      -> sign: " << signToString(s) << "\n";
      }
    }

    // Also print argument signs.
    if (F.arg_size() > 0) {
      errs() << "\nArgument signs:\n";
      for (Argument &A : F.args()) {
        if (A.getType()->isIntegerTy())
          errs() << "  " << A.getName() << " (" << A
                 << "): " << signToString(signState[&A]) << "\n";
      }
    }
    errs() << "\n";
  }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "SignAnalysisPass", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "my-pass") {
                    MPM.addPass(SignAnalysisPass());
                    return true;
                  }
                  return false;
                });
          }};
}