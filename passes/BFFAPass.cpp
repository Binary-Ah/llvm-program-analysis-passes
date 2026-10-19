//===----------------------------------------------------------------------===//
// BFFAPass.cpp  —  Basic Functions for Function Analysis
//
//   1. Retrieving FAM (FunctionAnalysisManager) from a Module Pass
//   2. Dominator Tree analysis
//   3. Loop info analysis
//   4. SCEV (Scalar Evolution) analysis
//   5. Alias Analysis
//   6. Memory SSA
//   7. PHI Node / Reaching Definition
//   8. CFG traversal (successors/predecessors, RPO)
//
// build:
//   clang++ -shared -fPIC -o BFFAPass.dylib BFFAPass.cpp \
//     $(llvm-config --cxxflags --ldflags --libs) \
//     -Wl,-undefined,dynamic_lookup
//
// execution:
//   opt -load-pass-plugin=./BFFAPass.dylib \
//       -passes="print<bffa>" \
//       -disable-output sample.ll
//===----------------------------------------------------------------------===//

#include "llvm/ADT/BreadthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

struct FunctionInfo {
  std::string Name;

  // Basic counts
  unsigned NumBBs   = 0;
  unsigned NumInsts = 0;
  unsigned NumPhis  = 0;

  // PHI node / Reaching Definition info
  struct PhiInfo {
    std::string PhiName;
    SmallVector<std::pair<std::string /*BB*/, std::string /*Val*/>, 4> Incoming;
  };
  SmallVector<PhiInfo, 8> Phis;

  // Dominator Tree info
  struct DomInfo {
    std::string BB;
    std::string IDom;
  };
  SmallVector<DomInfo, 8> Doms;

  // Loop info
  struct LoopInfo_ {
    unsigned    Depth;
    std::string Header;
    std::string Preheader;  // "(none)" if null
    std::string Latch;      // "(none)" if null
    uint64_t    TripCount;  // 0 = unknown
  };
  SmallVector<LoopInfo_, 4> Loops;

  // Alias Analysis sample result
  std::string AliasResult;

  // Def-Use Chain info
  struct DefUseInfo {
    std::string DefName;   // name of the def instruction
    std::string DefBB;     // BB containing the def
    SmallVector<std::pair<std::string /*UserName*/, std::string /*UserBB*/>, 4> Users;
  };
  SmallVector<DefUseInfo, 16> DefUseChains;

  // CFG traversal order (RPO)
  SmallVector<std::string, 8> RPOOrder;

  void print(raw_ostream &OS) const {
    OS << "==================================================\n";
    OS << "Function: " << Name << "\n";
    OS << "  BBs=" << NumBBs
       << "  Insts=" << NumInsts
       << "  Phis=" << NumPhis << "\n";

    //--- PHI / Reaching Definition ---
    if (!Phis.empty()) {
      OS << "\n  [PHI Nodes - Reaching Definitions]\n";
      for (auto &P : Phis) {
        OS << "    %" << P.PhiName << " = phi ";
        for (auto &[BB, Val] : P.Incoming)
          OS << "[ " << Val << ", %" << BB << " ] ";
        OS << "\n";
      }
    }

    //--- Dominator Tree ---
    if (!Doms.empty()) {
      OS << "\n  [Dominator Tree]\n";
      for (auto &D : Doms)
        OS << "    %" << D.BB << "  idom: %" << D.IDom << "\n";
    }

    //--- Loop Info ---
    if (!Loops.empty()) {
      OS << "\n  [Loops]\n";
      for (auto &L : Loops) {
        OS << "    depth=" << L.Depth
           << "  header=%" << L.Header
           << "  preheader=" << L.Preheader
           << "  latch=" << L.Latch
           << "  tripCount=";
        if (L.TripCount) OS << L.TripCount;
        else             OS << "unknown";
        OS << "\n";
      }
    }

    //--- Alias Analysis ---
    if (!AliasResult.empty()) {
      OS << "\n  [Alias Analysis]\n";
      OS << "    " << AliasResult << "\n";
    }

    //--- Def-Use Chain ---
    if (!DefUseChains.empty()) {
      OS << "\n  [Def-Use Chains]\n";
      for (auto &DU : DefUseChains) {
        OS << "    %" << DU.DefName << " (%" << DU.DefBB << ") -> ";
        if (DU.Users.empty()) {
          OS << "(no users)";
        } else {
          for (unsigned i = 0; i < DU.Users.size(); ++i) {
            if (i) OS << ", ";
            OS << "%" << DU.Users[i].first << " (%" << DU.Users[i].second << ")";
          }
        }
        OS << "\n";
      }
    }

    //--- CFG RPO order ---
    if (!RPOOrder.empty()) {
      OS << "\n  [CFG Reverse Post-Order]\n";
      for (unsigned i = 0; i < RPOOrder.size(); ++i)
        OS << "    " << i << ": %" << RPOOrder[i] << "\n";
    }
  }
};

struct BFFAResult {
  SmallVector<FunctionInfo, 16> Functions;

  void print(raw_ostream &OS) const {
    for (auto &FI : Functions)
      FI.print(OS);
  }
};


class BFFAPass : public AnalysisInfoMixin<BFFAPass> {
  friend AnalysisInfoMixin<BFFAPass>;
  static AnalysisKey Key;

public:
  using Result = BFFAResult;

  Result run(Module &M, ModuleAnalysisManager &MAM) {
    Result MR;

    // Borrow FAM via FunctionAnalysisManagerModuleProxy
    FunctionAnalysisManager &FAM =
        MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

    for (Function &F : M) {
      if (F.isDeclaration()) continue;  // skip external functions (declaration only)

      FunctionInfo FI;
      FI.Name = F.getName().str();

      collectBasicInfo(F, FI);
      collectPhiInfo(F, FI);                                               // PHI / Reaching Def
      collectDomInfo(F, FAM.getResult<DominatorTreeAnalysis>(F), FI);      // Dominator Tree
      collectLoopInfo(F, FAM.getResult<LoopAnalysis>(F),
                         FAM.getResult<ScalarEvolutionAnalysis>(F), FI);   // Loop + SCEV
      collectAliasInfo(F, FAM.getResult<AAManager>(F), FI);                // Alias Analysis
      collectMemSSAInfo(F, FAM.getResult<MemorySSAAnalysis>(F).getMSSA()); // Memory SSA
      collectDefUseInfo(F, FI);                                             // Def-Use Chain
      collectCFGOrder(F, FI);                                              // CFG RPO traversal

      MR.Functions.push_back(std::move(FI));
    }

    return MR;
  }

private:
  void collectBasicInfo(Function &F, FunctionInfo &FI) {
    for (BasicBlock &BB : F) {
      ++FI.NumBBs;
      for (Instruction &I : BB) {
        ++FI.NumInsts;
        if (isa<PHINode>(I)) ++FI.NumPhis;
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // PHI Node / Reaching Definition
  //===--------------------------------------------------------------------===//
  void collectPhiInfo(Function &F, FunctionInfo &FI) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *Phi = dyn_cast<PHINode>(&I);
        if (!Phi) continue;

        FunctionInfo::PhiInfo PI;
        PI.PhiName = Phi->hasName() ? Phi->getName().str() : "(unnamed)";

        for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
          std::string BBName = Phi->getIncomingBlock(i)->getName().str();
          Value      *InVal  = Phi->getIncomingValue(i);
          std::string ValName;

          if (auto *C = dyn_cast<ConstantInt>(InVal))
            ValName = std::to_string(C->getSExtValue());
          else if (InVal->hasName())
            ValName = "%" + InVal->getName().str();
          else
            ValName = "(undef)";

          PI.Incoming.push_back({BBName, ValName});
        }
        FI.Phis.push_back(std::move(PI));
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Dominator Tree
  //===--------------------------------------------------------------------===//
  void collectDomInfo(Function &F, DominatorTree &DT, FunctionInfo &FI) {
    for (BasicBlock &BB : F) {
      DomTreeNode *Node = DT.getNode(&BB);
      if (!Node) continue;

      DomTreeNode *IDomNode = Node->getIDom();
      if (!IDomNode) continue;  // entry block has no idom

      FI.Doms.push_back({
          BB.getName().str(),
          IDomNode->getBlock()->getName().str()
      });

      // Example: querying dominance relation directly
      // if (DT.dominates(A_BB, B_BB)) { ... }
    }
  }

  //===--------------------------------------------------------------------===//
  // Loop Info + SCEV (Scalar Evolution)
  //===--------------------------------------------------------------------===//
  void collectLoopInfo(Function &F, LoopInfo &LI,
                       ScalarEvolution &SE, FunctionInfo &FI) {
    // getLoopsInPreorder(): iterates all loops including nested ones
    for (Loop *L : LI.getLoopsInPreorder()) {
      FunctionInfo::LoopInfo_ LInfo;
      LInfo.Depth  = L->getLoopDepth();
      LInfo.Header = L->getHeader()->getName().str();
      LInfo.Preheader = L->getLoopPreheader()
                        ? "%" + L->getLoopPreheader()->getName().str()
                        : "(none)";
      LInfo.Latch = L->getLoopLatch()
                    ? "%" + L->getLoopLatch()->getName().str()
                    : "(none)";

      // SCEV: backedge taken count = trip count - 1
      LInfo.TripCount = 0;
      const SCEV *BTC = SE.getBackedgeTakenCount(L);
      if (auto *C = dyn_cast<SCEVConstant>(BTC))
        LInfo.TripCount = C->getValue()->getZExtValue() + 1;

      FI.Loops.push_back(std::move(LInfo));
    }
  }

  //===--------------------------------------------------------------------===//
  // Alias Analysis
  //===--------------------------------------------------------------------===//
  void collectAliasInfo(Function &F, AAResults &AA, FunctionInfo &FI) {
    // Find the first two loads in the function and check their alias relation
    SmallVector<LoadInst *, 2> Loads;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          Loads.push_back(LI);
          if (Loads.size() == 2) goto done;
        }
    done:
    if (Loads.size() < 2) {
      FI.AliasResult = "(fewer than 2 loads - cannot compare)";
      return;
    }

    auto AR = AA.alias(MemoryLocation::get(Loads[0]),
                       MemoryLocation::get(Loads[1]));
    std::string L0 = Loads[0]->hasName() ? Loads[0]->getName().str() : "load0";
    std::string L1 = Loads[1]->hasName() ? Loads[1]->getName().str() : "load1";

    std::string Rel;
    switch (AR) {
      case AliasResult::NoAlias:      Rel = "NoAlias";      break;
      case AliasResult::MayAlias:     Rel = "MayAlias";     break;
      case AliasResult::PartialAlias: Rel = "PartialAlias"; break;
      case AliasResult::MustAlias:    Rel = "MustAlias";    break;
    }
    FI.AliasResult = "%" + L0 + " vs %" + L1 + " -> " + Rel;
  }

  //===--------------------------------------------------------------------===//
  // Memory SSA
  //===--------------------------------------------------------------------===//
  void collectMemSSAInfo(Function &F, MemorySSA &MSSA) {
    errs() << "\n  [Memory SSA - " << F.getName() << "]\n";

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *MA = MSSA.getMemoryAccess(&I);
        if (!MA) continue;

        errs() << "    %" << I.getName() << " -> ";
        if (isa<MemoryDef>(MA))  errs() << "MemoryDef\n";
        if (isa<MemoryUse>(MA))  errs() << "MemoryUse\n";
        if (isa<MemoryPhi>(MA))  errs() << "MemoryPhi\n";
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // Def-Use Chain
  //===--------------------------------------------------------------------===//
  void collectDefUseInfo(Function &F, FunctionInfo &FI) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        // Void instructions (br, store, etc.) are not defs — skip them
        if (I.getType()->isVoidTy()) continue;

        FunctionInfo::DefUseInfo DU;
        DU.DefName = I.hasName() ? I.getName().str() : "(unnamed)";
        DU.DefBB   = BB.getName().str();

        for (User *U : I.users()) {
          if (auto *UserInst = dyn_cast<Instruction>(U)) {
            std::string UName = UserInst->hasName()
                                ? UserInst->getName().str()
                                : UserInst->getOpcodeName();
            std::string UBB = UserInst->getParent()->getName().str();
            DU.Users.push_back({UName, UBB});
          }
        }

        FI.DefUseChains.push_back(std::move(DU));
      }
    }
  }

  //===--------------------------------------------------------------------===//
  // CFG Traversal
  //===--------------------------------------------------------------------===//
  void collectCFGOrder(Function &F, FunctionInfo &FI) {
    // Collect RPO order
    ReversePostOrderTraversal<Function *> RPOT(&F);
    for (BasicBlock *BB : RPOT)
      FI.RPOOrder.push_back(BB->getName().str());

    // Example: successors / predecessors (log only)
    errs() << "\n  [CFG Edges - " << F.getName() << "]\n";
    for (BasicBlock &BB : F)
      for (BasicBlock *Succ : successors(&BB))
        errs() << "    %" << BB.getName()
               << " -> %" << Succ->getName() << "\n";
  }
};

AnalysisKey BFFAPass::Key;

//===----------------------------------------------------------------------===//
class BFFAPrinterPass : public PassInfoMixin<BFFAPrinterPass> {
  raw_ostream &OS;
public:
  explicit BFFAPrinterPass(raw_ostream &OS) : OS(OS) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    // Retrieve cached analysis result (no recomputation)
    auto &Res = MAM.getResult<BFFAPass>(M);
    Res.print(OS);
    return PreservedAnalyses::all();  // no IR modification
  }
};

extern "C" LLVM_ATTRIBUTE_WEAK
::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {
    LLVM_PLUGIN_API_VERSION, "bffa", LLVM_VERSION_STRING,
    [](PassBuilder &PB) {

      // Register Analysis Pass
      PB.registerAnalysisRegistrationCallback(
          [](ModuleAnalysisManager &MAM) {
            MAM.registerPass([&] { return BFFAPass(); });
          });

      // Register Printer Pass → opt -passes="print<bffa>"
      PB.registerPipelineParsingCallback(
          [](StringRef Name, ModulePassManager &MPM,
             ArrayRef<PassBuilder::PipelineElement>) -> bool {
            if (Name == "print<bffa>") {
              MPM.addPass(BFFAPrinterPass(errs()));
              return true;
            }
            return false;
          });
    }
  };
}
