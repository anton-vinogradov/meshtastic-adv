#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

"${CXX:-c++}" \
  -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_dir/overlay/src/advui" \
  "$repo_dir/overlay/src/advui/AdvUtf8.cpp" \
  "$repo_dir/overlay/src/advui/AdvStorage.cpp" \
  "$repo_dir/tests/native/test_hardening.cpp" \
  -o "$build_dir/test_hardening"

"$build_dir/test_hardening"
echo "host hardening tests: OK"
