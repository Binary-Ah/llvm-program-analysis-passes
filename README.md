# LLVM Program Analysis Passes

시스템보안·컴파일러 수업에서 작성한 LLVM new pass manager 기반 out-of-tree 플러그인 3종.

| 패스 | 파일 | 파이프라인 이름 | 내용 |
|---|---|---|---|
| Sign Analysis | `passes/sign_analysis.cpp` | `my-pass` | **추상 해석(abstract interpretation)** 으로 정수 변수의 부호를 추론. 격자 {Bottom, Negative, Zero, Positive, Top}, join(LUB), add/sub/mul/sdiv/srem/neg 전이 함수, worklist 고정점 반복 |
| BFFA | `passes/BFFAPass.cpp` | `print<bffa>` | Module pass에서 FunctionAnalysisManager를 받아 Dominator Tree, LoopInfo, SCEV, Alias Analysis, MemorySSA, PHI/reaching definition, CFG 순회(RPO)를 한 번에 출력하는 분석 모음 |
| Instruction Counter | `passes/InsCounter.cpp` | `ins-counter` | 모듈 전체의 opcode별 명령어 수 집계 |

`tests/`에는 상수 전파·alias 분석 확인용 C 코드(`security_lab.c`, `alias_test.c`)와 sign analysis 입력 `.ll`이 있다.

## 실행
```bash
sudo apt install llvm-18 llvm-18-dev clang-18 cmake
./build_and_run.sh                      # SignAnalysisPass 빌드 후 tests/test_input.ll 분석

# 다른 패스
clang-18 -S -emit-llvm -O0 -Xclang -disable-O0-optnone tests/security_lab.c -o /tmp/s.ll
opt-18 -load-pass-plugin build/InsCounter.so -passes=ins-counter -disable-output /tmp/s.ll
opt-18 -load-pass-plugin build/BFFAPass.so  -passes='print<bffa>' -disable-output /tmp/s.ll
```
