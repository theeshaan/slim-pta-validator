# Good testcases

These examples are expected to run without any `UNSOUND` diagnostics when
instrumented with matching points-to facts.

## Build IR

```bash
clang-20 -O0 -S -emit-llvm -g -fno-discard-value-names -o <name>.ll <name>.c
```

## Instrument + run

```bash
./build/pta-validator <name>.ll <name>.pta -o <name>.inst.ll
clang-20 <name>.inst.ll ./build/libpta_runtime.a -o <name>.bin
./<name>.bin
```
