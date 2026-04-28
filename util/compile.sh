#!/usr/bin/env bash

# Check if an argument is provided
if [ $# -ne 1 ]; then
    echo "Usage: $0 <source.c>"
    exit 1
fi

input="$1"

# Ensure the file exists
if [ ! -f "$input" ]; then
    echo "Error: File '$input' not found."
    exit 1
fi

# Derive output filename (replace .c with .ll)
output="${input%.c}.ll"

# Run clang
clang-18 -O0 -S -emit-llvm -g -fno-discard-value-names -o "$output" "$input"

# Check result
if [ $? -eq 0 ]; then
    echo "Generated LLVM IR: $output"
else
    echo "Compilation failed."
    exit 1
fi