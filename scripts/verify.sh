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
echo "--- e2e tests ---"
python3 tests/e2e.py ./bin/kanban

echo ""
echo "--- verify: all green ---"
