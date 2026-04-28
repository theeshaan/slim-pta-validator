#!/usr/bin/env bash
set -euo pipefail

pta_mode=""
pp_format=""

usage() {
  echo "Usage: $0 [--pta-mode=fi|fs] [--program-point-format=ir|source-line] <input.c|input.ll> <pta_file>"
  exit 1
}

# Parse options
while [[ $# -gt 0 ]]; do
  case "$1" in
    --pta-mode=*)
      pta_mode="${1#*=}"
      if [[ "$pta_mode" != "fi" && "$pta_mode" != "fs" ]]; then
        echo "Invalid --pta-mode: '$pta_mode'"
        echo "Valid values: fi, fs"
        exit 1
      fi
      shift
      ;;
    --program-point-format=*)
      pp_format="${1#*=}"
      if [[ "$pp_format" != "ir" && "$pp_format" != "source-line" ]]; then
        echo "Invalid --program-point-format: '$pp_format'"
        echo "Valid values: ir, source-line"
        exit 1
      fi
      shift
      ;;
    -*)
      echo "Unknown option: $1"
      usage
      ;;
    *)
      break
      ;;
  esac
done

[[ $# -eq 2 ]] || usage

input="$1"
pta_file="$2"

cleanup_files=()

# Handle input file
if [[ "$input" == *.c ]]; then
  base="${input%.c}"
  ll_file="${base}.ll"
  util/compile.sh "$input"
  cleanup_files+=("$ll_file")
elif [[ "$input" == *.ll ]]; then
  ll_file="$input"
else
  echo "First input must be .c or .ll"
  exit 1
fi

# Build command
cmd="./build/pta-validator"
[[ -n "$pta_mode" ]] && cmd+=" --pta-mode=$pta_mode"
[[ -n "$pp_format" ]] && cmd+=" --program-point-format=$pp_format"
cmd+=" $ll_file $pta_file -o instrumented.ll"

# Run
eval "$cmd"
cleanup_files+=("instrumented.ll")

clang-18 instrumented.ll build/libpta_runtime.a -o program_instrumented
cleanup_files+=("program_instrumented")

./program_instrumented > /dev/null

# Cleanup
for f in "${cleanup_files[@]}"; do
  [[ -f "$f" ]] && rm -f "$f"
done