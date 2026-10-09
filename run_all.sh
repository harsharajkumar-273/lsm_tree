#!/bin/bash
set -e

# Dynamically resolve project root directory containing run_all.sh
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="${BUILD_DIR:-"$SCRIPT_DIR/build"}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory '$BUILD_DIR' not found. Please build the project first."
    exit 1
fi

echo "==========================================="
echo "  LSM-Tree Key-Value Store — Test Runner  "
echo "==========================================="

echo "Running all registered tests (ctest)..."
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 300

echo "Running Benchmarks..."
"$BUILD_DIR/bench_write"
"$BUILD_DIR/bench_read"
"$BUILD_DIR/bench_durability" --quick

echo "==========================================="
echo "  All tests and benchmarks completed!      "
echo "==========================================="
