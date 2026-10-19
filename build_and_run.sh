#!/usr/bin/env bash
# build_and_run.sh
# ─────────────────────────────────────────────────────────────────────────────
# Builds the SignAnalysisPass plugin and runs it on a sample .ll file.
# Tested on Ubuntu 22.04 / 24.04 with LLVM 17 or 18 installed via apt.
#
# Prerequisites (run once):
#   sudo apt install llvm-18 llvm-18-dev clang-18 cmake
#   # Replace 18 with whatever version is available:  apt list | grep llvm-dev
#
# Usage:
#   chmod +x build_and_run.sh
#   ./build_and_run.sh [path/to/test.ll]
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── 1. Detect LLVM toolchain ──────────────────────────────────────────────────
# Try versioned binaries first (apt default names), then fall back to plain ones.
find_bin() {
    for v in 20 19 18 17 16 ""; do
        suffix="${v:+-$v}"        # "-18" or ""
        if command -v "$1${suffix}" &>/dev/null; then
            echo "$1${suffix}"
            return
        fi
    done
    echo "ERROR: $1 not found. Install with: sudo apt install llvm-18-dev clang-18" >&2
    exit 1
}

CLANG=$(find_bin clang)
OPT=$(find_bin opt)
LLVM_CONFIG=$(find_bin llvm-config)

echo "Using: clang=$CLANG  opt=$OPT  llvm-config=$LLVM_CONFIG"

LLVM_VERSION=$($LLVM_CONFIG --version | cut -d. -f1)
LLVM_CMAKE_DIR=$($LLVM_CONFIG --cmakedir)

# ── 2. Build the plugin ───────────────────────────────────────────────────────
BUILD_DIR="build"
mkdir -p "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR="$LLVM_CMAKE_DIR" \
    -DCMAKE_C_COMPILER="$CLANG" \
    -DCMAKE_CXX_COMPILER="${CLANG/clang/clang++}" \
    -G "Unix Makefiles"

cmake --build "$BUILD_DIR" -- -j"$(nproc)"

PLUGIN="$BUILD_DIR/SignAnalysisPass.so"
echo ""
echo "Plugin built: $PLUGIN"

# ── 3. Generate or use a test .ll file ───────────────────────────────────────
TEST_LL="${1:-tests/test_input.ll}"

if [[ ! -f "$TEST_LL" ]]; then
    echo ""
    echo "No test file supplied.  Generating $TEST_LL ..."
    cat > "$TEST_LL" << 'EOF'
; Sample IR: exercises add/sub/mul/sdiv/load/store/phi
define i32 @sample(i32 %a, i32 %b) {
entry:
  %x = alloca i32
  store i32 5, i32* %x          ; store positive constant
  %loaded = load i32, i32* %x   ; should be Positive
  %sum    = add  i32 %loaded, 3 ; Positive + Positive = Positive
  %diff   = sub  i32 %sum, %b   ; Positive - Top = Top
  %prod   = mul  i32 -2, -3     ; Negative * Negative = Positive
  %quot   = sdiv i32 %prod, 2   ; Positive / Positive = Positive
  ret i32 %quot
}
EOF
fi

# ── 4. Run sign analysis ──────────────────────────────────────────────────────
echo ""
echo "Running sign analysis on: $TEST_LL"
echo "────────────────────────────────────────"
"$OPT" \
    -load-pass-plugin "./$PLUGIN" \
    -passes="my-pass" \
    -disable-output \
    "$TEST_LL"
