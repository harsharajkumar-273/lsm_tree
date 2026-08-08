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

echo "Running Unit Tests..."
./build/test_skip_list
./build/test_bloom_filter
./build/test_wal_recovery
./build/test_sstable
./build/test_compaction_streaming

echo "Running Benchmarks..."
"$BUILD_DIR/bench_write"
"$BUILD_DIR/bench_read"

echo "==========================================="
echo "  All tests and benchmarks completed!      "
echo "==========================================="
