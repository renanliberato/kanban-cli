#!/usr/bin/env bash
set -euo pipefail

echo "=== verify.sh ==="

echo ""
echo "--- clean ---"
make clean

echo ""
echo "--- build ---"
make

echo ""
echo "--- unit tests ---"
make test

echo ""
echo "--- verify: all green ---"
