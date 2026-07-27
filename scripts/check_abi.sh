#!/usr/bin/env bash
# scripts/check_abi.sh - Verify exported API symbols stability
set -euo pipefail

SYMBOLS_FILE=".cmem_exported_symbols.txt"
BUILD_DIR="${1:-build_cmake}"

if [ ! -f "$BUILD_DIR/libcmem.a" ]; then
    echo "ERROR: $BUILD_DIR/libcmem.a not found. Build the library first."
    exit 1
fi

nm -g "$BUILD_DIR/libcmem.a" | grep " T " | awk '{print $3}' | sort > current_symbols.txt

if [ ! -f "$SYMBOLS_FILE" ]; then
    echo "Initializing exported symbols baseline..."
    mv current_symbols.txt "$SYMBOLS_FILE"
    echo "Baseline saved to $SYMBOLS_FILE"
    exit 0
fi

echo "Checking exported symbols against baseline..."
if diff -q "$SYMBOLS_FILE" current_symbols.txt > /dev/null; then
    echo "OK: Exported symbols unchanged."
    rm -f current_symbols.txt
    exit 0
else
    echo "ERROR: Exported symbols have changed!"
    echo "Diff:"
    diff -u "$SYMBOLS_FILE" current_symbols.txt || true
    rm -f current_symbols.txt
    exit 1
fi
