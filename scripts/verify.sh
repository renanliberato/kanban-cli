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
echo "--- BDD tests ---"
python3 -m behave tests/bdd -D binary=./bin/kanban

echo ""
echo "--- verify: all green ---"
