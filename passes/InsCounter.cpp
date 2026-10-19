#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <string>

using namespace llvm;

namespace {

struct InsCounterPass : public PassInfoMixin<InsCounterPass> {
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
        std::map<std::string, int> opcodeCounter;

        // 1. Module 내의 모든 Function 순회
        for (Function &F : M) {
            // 2. Function 내의 모든 BasicBlock 순회
            for (BasicBlock &BB : F) {
                // 3. BasicBlock 내의 모든 Instruction 순회
                for (Instruction &I : BB) {
                    // 명령어 이름(예: alloca, add, call 등)을 가져와 카운트
                    std::string name = I.getOpcodeName();
                    opcodeCounter[name]++;
                }
            }
        }

        // 통계 출력 (외부 스크립트 없이 Pass 내부에서 처리)
        errs() << "\n=== [LLVM Instruction Statistics] ===\n";
        for (auto const& [name, count] : opcodeCounter) {
            errs() << name << " : " << count << "\n";
        }
        errs() << "=====================================\n\n";

        return PreservedAnalyses::all();
    }
};

} // end anonymous namespace

// Pass 등록을 위한 외부 플러그인 인터페이스
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "InsCounterPass", LLVM_VERSION_STRING,
            [](PassBuilder &PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager &MPM,
                       ArrayRef<PassBuilder::PipelineElement>) {
                        if (Name == "ins-counter") {
                            MPM.addPass(InsCounterPass());
                            return true;
                        }
                        return false;
                    });
            }};
}