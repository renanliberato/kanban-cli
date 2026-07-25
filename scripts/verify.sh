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
echo "--- TUI smoke test ---"
python3 tests/smoke_tui.py ./bin/kanban

echo ""
echo "--- CRUD smoke test ---"
python3 tests/smoke_crud.py ./bin/kanban

echo ""
echo "--- verify: all green ---"
