# A Points-To Analysis Validator for LLVM IR

This project instruments LLVM IR and validates points-to analysis facts at run time.
It supports both flow-insensitive (FI) and flow-sensitive (FS) validation.

## Build

```bash
cmake -S . -B build -DLLVM_DIR=$(llvm-config-18 --cmakedir)
cmake --build build -j
```

LLVM 17+ is required.

## Compile input IR

Generate LLVM IR:

```bash
clang -O0 -S -emit-llvm -g -fno-discard-value-names \
  -o input.ll input.c
```

The validator expects unoptimized IR with debug info and preserved value names:

- `-O0`
- `-g`
- `-fno-discard-value-names`

## Instrumentation usage

FI mode is the default:

```bash
./build/pta-validator input.ll points_to.pta -o instrumented.ll
```


FS mode is selected explicitly:

```bash
./build/pta-validator --pta-mode=fs input.ll points_to.pta -o instrumented.ll
```

Then link the instrumented IR with the runtime, and run the instrumented program:

```bash
clang instrumented.ll build/libpta_runtime.a -o program_instrumented
./program_instrumented
```

The runtime prints a summary to `stderr`:

- `SUCCESS` when no unsound dereference was observed
- `FAILURE` when one or more unsound dereferences were observed

## FI points-to file format

Use one entry per pointer variable:

```text
# <pointer_var> -> <pointee1> <pointee2> ...
%p -> %x %y
%q -> %z
@gptr -> @gx
```

Notes:

- Names must exactly match LLVM IR names (use `%` for locals, `@` for globals).
- Lines starting with `#` are treated as comments and ignored.
- Empty RHS is valid and is treated as an empty pointee set:

```text
%p ->
```

## FS points-to file format

Each fact is attached to an IR-derived program point:

```text
# @<function>:<basic-block>:<instruction-index> <ptr_name> -> <pointee1> ...
@main:entry:12 %p -> %a %b
@main:entry:15 %p -> %b
@main:entry:18 %p ->
```

FS semantics:

- Facts are keyed by exact `(@program_point, ptr_name)`.
- If the same key appears multiple times, the last entry in file order wins.
- Empty RHS means the active points-to set is empty.
- Missing or empty FS facts at a dereference are treated as `UNSOUND`.
- FS mode uses only the FS file provided for that run. There is no FI fallback.

Program-point components:

- `function`: LLVM function name
- `basic-block`: LLVM basic block label, or `bb<N>` when the block is unnamed
- `instruction-index`: 0-based index of the dereference instruction within that basic block, counting all instructions in block order

The validator derives the same key directly from the input LLVM IR. To inspect
the exact keys used for instrumented checks, look for calls to
`__pta_check_deref(..., ptr @<program-point-string>)` in the emitted `.ll`
file, or inspect the associated constant strings.

## Example commands

FI example:

```bash
clang -O0 -S -emit-llvm -g -fno-discard-value-names \
  -o build/branching_deref.ll testcases/branching_deref.c
./build/pta-validator build/branching_deref.ll testcases/branching_deref.pta \
  -o build/branching_deref.inst.ll
clang build/branching_deref.inst.ll build/libpta_runtime.a \
  -o build/branching_deref.bin
./build/branching_deref.bin
```

FS example:

```bash
clang -O0 -S -emit-llvm -g -fno-discard-value-names \
  -o build/fs_last_entry_wins.ll testcases/fs_last_entry_wins.c
./build/pta-validator --pta-mode=fs \
  build/fs_last_entry_wins.ll testcases/fs_last_entry_wins.pta \
  -o build/fs_last_entry_wins.inst.ll
clang build/fs_last_entry_wins.inst.ll build/libpta_runtime.a \
  -o build/fs_last_entry_wins.bin
./build/fs_last_entry_wins.bin
```
