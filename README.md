# A Points-To Analysis Validator for LLVM IR
This project visualizes a system that takes points-to information computed for the program and instruments the IR to validate the correctness of the points-to information at run-time.

## Build & Usage
```bash
# Build
mkdir build && cd build
cmake .. -DLLVM_DIR=$(llvm-config --cmakedir)
make

# Instrument
./pta-validator input.ll points_to.txt -o instrumented.ll

# Compile instrumented IR + link runtime
clang instrumented.ll libpta_runtime.a -o program_instrumented

# Run — violations are printed to stderr and exit(1)
./program_instrumented
```

## Expected input format
```python
# points-to.txt
# Format: <pointer_var> -> <pointee1> <pointee2> ...
# Names must exactly match LLVM IR value names (use % for locals, @ for globals)

# Example: pointer %p may point to %x or %y
%p -> %x %y

# Example: pointer %q points only to %z
%q -> %z

# Example: global pointer @gptr points to global @gx
@gptr -> @gx
```

The LLVM IR (`.ll`) file is expected to have been compiled with the following options:<br>
- `-O0`
- `-g`
- `-fno-discard-value-names`

LLVM version 17+ is to be used for the compilation.