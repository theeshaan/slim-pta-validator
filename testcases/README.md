# PTA validator testcases

All testcases in this directory are intended to be **sound** with the paired
`.pta` facts unless explicitly marked otherwise.

Unsound FS coverage in this directory:

- `fs_replace_semantics.*`
- `fs_empty_set_unsound.*`
- `fs_missing_fact_unsound.*`

## Run one testcase

```bash
clang-18 -O0 -S -emit-llvm -g -fno-discard-value-names -o <name>.ll testcases/<name>.c
./build/pta-validator <name>.ll testcases/<name>.pta -o <name>.inst.ll
clang-18 <name>.inst.ll ./build/libpta_runtime.a -o <name>.bin
./<name>.bin
```

## Run one FS testcase

```bash
clang-18 -O0 -S -emit-llvm -g -fno-discard-value-names -o <name>.ll testcases/<name>.c
./build/pta-validator --pta-mode=fs <name>.ll testcases/<name>.pta -o <name>.inst.ll
clang-18 <name>.inst.ll ./build/libpta_runtime.a -o <name>.bin
./<name>.bin
```

FS testcase files use:

```text
@<function>:<basic-block>:<instruction-index> <ptr_name> -> <pointees...>
```

The instruction index is 0-based within the basic block and counts all
instructions in block order.

## Expected runtime summary
- `[PtaRuntime] SUCCESS: ...` for sound testcase/PTA pairs
- `[PtaRuntime] FAILURE: ...` for unsound testcase/PTA pairs
