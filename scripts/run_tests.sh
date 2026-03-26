#!/usr/bin/env bash
# run_tests.sh — Build + run all tests on Linux/WSL2 (GCC/Clang)
# Usage: bash scripts/run_tests.sh [--rebuild] [--coverage]
# From repo root: bash scripts/run_tests.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/cmake-build-test-linux"
REBUILD=false
COVERAGE=false

for arg in "$@"; do
    case $arg in
        --rebuild)  REBUILD=true ;;
        --coverage) COVERAGE=true ;;
    esac
done

# Clean if rebuild requested
if $REBUILD && [ -d "$BUILD_DIR" ]; then
    echo "[cmake] Cleaning build dir..."
    rm -rf "$BUILD_DIR"
fi

# Configure
if [ ! -d "$BUILD_DIR" ]; then
    echo "[cmake] Configuring..."
    CMAKE_EXTRA=""
    if $COVERAGE; then
        CMAKE_EXTRA="-DENABLE_COVERAGE=ON"
    fi
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_TESTS=ON \
        $CMAKE_EXTRA
fi

# Build
echo "[cmake] Building MiddlewareTests..."
cmake --build "$BUILD_DIR" --target MiddlewareTests -- -j"$(nproc)"

# Run tests
TEST_EXE="$BUILD_DIR/tests/MiddlewareTests"
if [ ! -f "$TEST_EXE" ]; then
    echo "[ERROR] Test executable not found at: $TEST_EXE"
    exit 1
fi

echo ""
echo "[tests] Running all tests..."
echo "------------------------------------------------------------"
"$TEST_EXE" --gtest_color=yes
EXIT_CODE=$?
echo "------------------------------------------------------------"

if [ $EXIT_CODE -eq 0 ]; then
    echo "[PASS] All tests passed."
else
    echo "[FAIL] Some tests failed (exit code $EXIT_CODE)."
fi

# Coverage report (requires lcov)
if $COVERAGE && [ $EXIT_CODE -eq 0 ]; then
    echo ""
    echo "[coverage] Generating HTML report..."
    COVERAGE_DIR="$BUILD_DIR/coverage"
    mkdir -p "$COVERAGE_DIR"
    lcov --capture --directory "$BUILD_DIR" --output-file "$COVERAGE_DIR/coverage.info" --quiet
    lcov --remove "$COVERAGE_DIR/coverage.info" '*/googletest/*' '/usr/*' --output-file "$COVERAGE_DIR/coverage.info" --quiet
    genhtml "$COVERAGE_DIR/coverage.info" --output-directory "$COVERAGE_DIR/html" --quiet
    echo "[coverage] Report: $COVERAGE_DIR/html/index.html"
fi

exit $EXIT_CODE
