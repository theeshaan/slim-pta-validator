# PTA validator testcases

All testcases in this directory are intended to be **sound** with the paired
`.pta` facts unless explicitly marked otherwise.

## Run one testcase

```bash
clang-20 -O0 -S -emit-llvm -g -fno-discard-value-names -o <name>.ll testcases/<name>.c
./build/pta-validator <name>.ll testcases/<name>.pta -o <name>.inst.ll
clang-20 <name>.inst.ll ./build/libpta_runtime.a -o <name>.bin
./<name>.bin
```

## Expected runtime summary
- `[PtaRuntime] SUCCESS: ...` for sound testcase/PTA pairs
- `[PtaRuntime] FAILURE: ...` for unsound testcase/PTA pairs
