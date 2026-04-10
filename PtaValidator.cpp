/**
 * PtaValidator.cpp
 *
 * Standalone LLVM-based instrumentation tool that validates a points-to
 * analysis by inserting runtime checks into LLVM IR.
 *
 * Targets: LLVM 17+ with New Pass Manager
 *
 * Build: see CMakeLists.txt
 *
 * Usage:
 *   ./pta-validator <input.ll> <points-to.txt> -o <output.ll>
 *
 * Points-to file format (one entry per line):
 *   <pointer_var> -> <pointee1> <pointee2> ...
 *   e.g.  %p -> %x %y
 *         @gptr -> @gx
 */

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h" // appendToGlobalCtors

#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace llvm;

// ---------------------------------------------------------------------------
// Command-line arguments
// ---------------------------------------------------------------------------

static cl::opt<std::string> InputIR(cl::Positional, cl::Required,
                                     cl::desc("<input .ll file>"));

static cl::opt<std::string> PtaFile(cl::Positional, cl::Required,
                                     cl::desc("<points-to analysis file>"));

static cl::opt<std::string> OutputIR("o", cl::desc("Output IR file"),
                                      cl::value_desc("filename"),
                                      cl::init("instrumented.ll"));

// ---------------------------------------------------------------------------
// Points-to file parser
// ---------------------------------------------------------------------------

// Represents the static points-to map: pointer name -> set of pointee names
using PtaMap = std::map<std::string, std::set<std::string>>;

// Parse lines of the form:
//   %p -> %x %y %z
//   @gptr -> @ga @gb
static PtaMap parsePtaFile(const std::string &path) {
  PtaMap result;
  std::ifstream f(path);
  if (!f.is_open()) {
    errs() << "[PtaValidator] ERROR: Cannot open points-to file: " << path
           << "\n";
    std::exit(1);
  }

  std::string line;
  while (std::getline(f, line)) {
    // Skip empty lines and comments
    if (line.empty() || line[0] == '#')
      continue;

    // Split on "->"
    auto arrowPos = line.find("->");
    if (arrowPos == std::string::npos)
      continue;

    std::string lhs = line.substr(0, arrowPos);
    std::string rhs = line.substr(arrowPos + 2);

    // Trim whitespace from lhs
    auto ltrim = [](std::string &s) {
      s.erase(0, s.find_first_not_of(" \t\r\n"));
    };
    auto rtrim = [](std::string &s) {
      s.erase(s.find_last_not_of(" \t\r\n") + 1);
    };
    ltrim(lhs);
    rtrim(lhs);
    if (lhs.empty())
      continue;

    result[lhs];

    // Parse space-separated pointees from rhs
    std::istringstream iss(rhs);
    std::string token;
    while (iss >> token) {
      result[lhs].insert(token);
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// Runtime function declarations
//
// These functions are defined in runtime.c and linked with the instrumented
// program. We declare them here so IRBuilder can emit calls to them.
//
//   void __pta_init(const char *pta_file_path);
//   void __pta_record_alloc(const char *abstract_name, void *ptr, size_t size);
//   void __pta_record_free(void *ptr);
//   void __pta_check_deref(const char *ptr_var_name, void *addr);
// ---------------------------------------------------------------------------

static FunctionCallee getOrDeclareFunc(Module &M, const std::string &name,
                                        FunctionType *FTy) {
  return M.getOrInsertFunction(name, FTy);
}

struct RuntimeFuncs {
  FunctionCallee PtaInit;        // __pta_init(const char*)
  FunctionCallee RecordAlloc;    // __pta_record_alloc(const char*, void*, size_t)
  FunctionCallee RecordFree;     // __pta_record_free(void*)
  FunctionCallee CheckDeref;     // __pta_check_deref(const char*, void*)
};

static RuntimeFuncs declareRuntimeFunctions(Module &M, LLVMContext &Ctx) {
  Type *VoidTy = Type::getVoidTy(Ctx);
  Type *I8PtrTy = PointerType::getUnqual(Ctx); // opaque ptr (LLVM 17+)
  Type *SizeTy = Type::getInt64Ty(Ctx);

  RuntimeFuncs RF;

  RF.PtaInit = getOrDeclareFunc(
      M, "__pta_init",
      FunctionType::get(VoidTy, {I8PtrTy}, false));

  RF.RecordAlloc = getOrDeclareFunc(
      M, "__pta_record_alloc",
      FunctionType::get(VoidTy, {I8PtrTy, I8PtrTy, SizeTy}, false));

  RF.RecordFree = getOrDeclareFunc(
      M, "__pta_record_free",
      FunctionType::get(VoidTy, {I8PtrTy}, false));

  RF.CheckDeref = getOrDeclareFunc(
      M, "__pta_check_deref",
      FunctionType::get(VoidTy, {I8PtrTy, I8PtrTy}, false));

  return RF;
}

// ---------------------------------------------------------------------------
// Helper: get a stable string name for a Value
//
// For named values (e.g. %p, @g) this returns the LLVM name.
// For unnamed values we fall back to a string representation.
// ---------------------------------------------------------------------------

static std::string getValueName(const Value *V) {
  if (V->hasName())
    return (isa<GlobalValue>(V) ? "@" : "%") + V->getName().str();
  return "<unnamed>";
}

// ---------------------------------------------------------------------------
// Helper: create a global constant string and return an i8* to it
// ---------------------------------------------------------------------------

static Value *makeStringPtr(IRBuilder<> &B, Module &M, const std::string &s) {
  return B.CreateGlobalStringPtr(s);
}

// ---------------------------------------------------------------------------
// The Instrumentation Pass
// ---------------------------------------------------------------------------

class PtaValidatorPass : public PassInfoMixin<PtaValidatorPass> {
public:
  PtaValidatorPass(PtaMap ptaMap, std::string ptaFilePath)
      : PtaMap_(std::move(ptaMap)), PtaFilePath_(std::move(ptaFilePath)) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    LLVMContext &Ctx = M.getContext();
    IRBuilder<> B(Ctx);
    const DataLayout &DL = M.getDataLayout();

    RuntimeFuncs RF = declareRuntimeFunctions(M, Ctx);

    // ------------------------------------------------------------------
    // 1. Inject __pta_init call as a global constructor (priority 0 so it
    //    runs before any other constructors).
    // ------------------------------------------------------------------
    injectInitCall(M, B, Ctx, RF);

    // ------------------------------------------------------------------
    // 2. Instrument global variables: record their address + size.
    //    We do this inside the same constructor function.
    // ------------------------------------------------------------------
    instrumentGlobals(M, B, Ctx, DL, RF);

    // ------------------------------------------------------------------
    // 3. Walk all functions, instrument allocas, calls, loads, stores.
    // ------------------------------------------------------------------
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      instrumentFunction(F, M, B, Ctx, DL, RF);
    }

    return PreservedAnalyses::none();
  }

private:
  PtaMap PtaMap_;
  std::string PtaFilePath_;

  // The global constructor function we inject; created once, reused for globals
  Function *CtorFn_ = nullptr;
  BasicBlock *CtorBB_ = nullptr;

  // ----------------------------------------------------------------
  // Create (or return) the module constructor function
  // ----------------------------------------------------------------
  void ensureCtorFunction(Module &M, LLVMContext &Ctx) {
    if (CtorFn_)
      return;

    FunctionType *FTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    CtorFn_ = Function::Create(FTy, GlobalValue::InternalLinkage,
                                "__pta_module_init", M);
    CtorBB_ = BasicBlock::Create(Ctx, "entry", CtorFn_);
    // We'll add the ret later; for now keep the block open.
  }

  void finalizeCtorFunction(LLVMContext &Ctx) {
    if (!CtorFn_)
      return;
    IRBuilder<> B(CtorBB_);
    B.CreateRetVoid();
  }

  // ----------------------------------------------------------------
  // 1. Inject __pta_init(pta_file_path) into the constructor
  // ----------------------------------------------------------------
  void injectInitCall(Module &M, IRBuilder<> &B, LLVMContext &Ctx,
                       RuntimeFuncs &RF) {
    ensureCtorFunction(M, Ctx);

    // Insert __pta_init call at the start of the ctor block (before ret)
    B.SetInsertPoint(CtorBB_);
    Value *PathStr = B.CreateGlobalStringPtr(PtaFilePath_, "__pta_filepath");
    B.CreateCall(RF.PtaInit, {PathStr});

    // Register this function as a global constructor (priority 0)
    appendToGlobalCtors(M, CtorFn_, 0);
  }

  // ----------------------------------------------------------------
  // 2. Instrument global variables
  // ----------------------------------------------------------------
  void instrumentGlobals(Module &M, IRBuilder<> &B, LLVMContext &Ctx,
                          const DataLayout &DL, RuntimeFuncs &RF) {
    B.SetInsertPoint(CtorBB_);

    for (GlobalVariable &GV : M.globals()) {
      // Skip LLVM-internal globals (llvm.*, __pta_*)
      if (GV.getName().starts_with("llvm.") ||
          GV.getName().starts_with("__pta"))
        continue;

      std::string name = getValueName(&GV);

      // Only instrument if this global appears in the points-to map
      // as a pointee (i.e. some pointer may point to it)
      if (!isKnownPointee(name))
        continue;

      TypeSize ts = DL.getTypeAllocSize(GV.getValueType());
      Value *NameStr = B.CreateGlobalStringPtr(name);
      Value *Ptr = B.CreateBitCast(&GV, PointerType::getUnqual(Ctx));
      Value *Size = ConstantInt::get(Type::getInt64Ty(Ctx), ts.getFixedValue());
      B.CreateCall(RF.RecordAlloc, {NameStr, Ptr, Size});
    }

    // Finalize ctor with ret void
    finalizeCtorFunction(Ctx);
  }

  // ----------------------------------------------------------------
  // 3. Instrument a single function
  // ----------------------------------------------------------------
  void instrumentFunction(Function &F, Module &M, IRBuilder<> &B,
                           LLVMContext &Ctx, const DataLayout &DL,
                           RuntimeFuncs &RF) {
    // Collect instructions first to avoid iterator invalidation
    std::vector<AllocaInst *> Allocas;
    std::vector<CallInst *> Calls;
    std::vector<LoadInst *> Loads;
    std::vector<StoreInst *> Stores;

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *AI = dyn_cast<AllocaInst>(&I))
          Allocas.push_back(AI);
        else if (auto *CI = dyn_cast<CallInst>(&I))
          Calls.push_back(CI);
        else if (auto *LI = dyn_cast<LoadInst>(&I))
          Loads.push_back(LI);
        else if (auto *SI = dyn_cast<StoreInst>(&I))
          Stores.push_back(SI);
      }
    }

    instrumentAllocas(Allocas, M, B, Ctx, DL, RF);
    instrumentCalls(Calls, M, B, Ctx, RF);
    instrumentLoadsStores(Loads, Stores, M, B, Ctx, RF);
  }

  // ----------------------------------------------------------------
  // 3a. Instrument alloca instructions
  //     After each alloca, call __pta_record_alloc(name, ptr, size)
  // ----------------------------------------------------------------
  void instrumentAllocas(std::vector<AllocaInst *> &Allocas, Module &M,
                          IRBuilder<> &B, LLVMContext &Ctx,
                          const DataLayout &DL, RuntimeFuncs &RF) {
    for (AllocaInst *AI : Allocas) {
      std::string name = getValueName(AI);
      if (!isKnownPointee(name))
        continue;

      // Insert *after* the alloca
      B.SetInsertPoint(AI->getNextNode());

      TypeSize ts = DL.getTypeAllocSize(AI->getAllocatedType());
      Value *NameStr = makeStringPtr(B, M, name);
      Value *Ptr = AI; // alloca already returns a pointer
      Value *Size = ConstantInt::get(Type::getInt64Ty(Ctx), ts.getFixedValue());

      // If alloca has a dynamic array size, multiply
      if (AI->isArrayAllocation()) {
        Value *ArrSize = AI->getArraySize();
        if (ArrSize->getType() != Type::getInt64Ty(Ctx))
          ArrSize = B.CreateZExt(ArrSize, Type::getInt64Ty(Ctx));
        Size = B.CreateMul(Size, ArrSize);
      }

      B.CreateCall(RF.RecordAlloc, {NameStr, Ptr, Size});
    }
  }

  // ----------------------------------------------------------------
  // 3b. Instrument call instructions
  //     - malloc/calloc: record the returned pointer + size
  //     - free: remove the entry
  // ----------------------------------------------------------------
  void instrumentCalls(std::vector<CallInst *> &Calls, Module &M,
                        IRBuilder<> &B, LLVMContext &Ctx, RuntimeFuncs &RF) {
    for (CallInst *CI : Calls) {
      Function *Callee = CI->getCalledFunction();
      if (!Callee)
        continue;

      StringRef FName = Callee->getName();

      if (FName == "malloc") {
        // malloc(size) -> ptr
        // Only instrument if this call result has a name in the pta map
        std::string name = getValueName(CI);
        if (!isKnownPointee(name))
          continue;

        B.SetInsertPoint(CI->getNextNode());
        Value *NameStr = makeStringPtr(B, M, name);
        Value *Size = CI->getArgOperand(0);
        if (Size->getType() != Type::getInt64Ty(Ctx))
          Size = B.CreateZExt(Size, Type::getInt64Ty(Ctx));
        B.CreateCall(RF.RecordAlloc, {NameStr, CI, Size});

      } else if (FName == "calloc") {
        // calloc(nmemb, size) -> ptr
        std::string name = getValueName(CI);
        if (!isKnownPointee(name))
          continue;

        B.SetInsertPoint(CI->getNextNode());
        Value *NameStr = makeStringPtr(B, M, name);
        Value *N = CI->getArgOperand(0);
        Value *S = CI->getArgOperand(1);
        if (N->getType() != Type::getInt64Ty(Ctx))
          N = B.CreateZExt(N, Type::getInt64Ty(Ctx));
        if (S->getType() != Type::getInt64Ty(Ctx))
          S = B.CreateZExt(S, Type::getInt64Ty(Ctx));
        Value *Size = B.CreateMul(N, S);
        B.CreateCall(RF.RecordAlloc, {NameStr, CI, Size});

      } else if (FName == "free") {
        // free(ptr) — remove before the call
        B.SetInsertPoint(CI);
        Value *Ptr = CI->getArgOperand(0);
        B.CreateCall(RF.RecordFree, {Ptr});
      }
    }
  }

  // ----------------------------------------------------------------
  // 3c. Instrument loads and stores
  //     Before each load/store, call __pta_check_deref(ptr_name, addr)
  //     only if the pointer variable appears in the points-to map as a key.
  // ----------------------------------------------------------------
  void instrumentLoadsStores(std::vector<LoadInst *> &Loads,
                               std::vector<StoreInst *> &Stores, Module &M,
                               IRBuilder<> &B, LLVMContext &Ctx,
                               RuntimeFuncs &RF) {
    // Loads: load T, ptr %p  — the pointer operand is what we check
    for (LoadInst *LI : Loads) {
      Value *PtrOp = LI->getPointerOperand();
      std::optional<std::string> PtrName = inferPointerNameForDeref(PtrOp);
      if (!PtrName)
        continue;

      B.SetInsertPoint(LI);
      Value *NameStr = makeStringPtr(B, M, *PtrName);
      B.CreateCall(RF.CheckDeref, {NameStr, PtrOp});
    }

    // Stores: store T val, ptr %p  — the pointer operand is the destination
    for (StoreInst *SI : Stores) {
      Value *PtrOp = SI->getPointerOperand();
      std::optional<std::string> PtrName = inferPointerNameForDeref(PtrOp);
      if (!PtrName)
        continue;

      B.SetInsertPoint(SI);
      Value *NameStr = makeStringPtr(B, M, *PtrName);
      B.CreateCall(RF.CheckDeref, {NameStr, PtrOp});
    }
  }

  // Infer the abstract pointer variable name that should be used for a
  // dereference check at address `DerefAddr`.
  //
  // Key distinction:
  //   - Accessing `%p` itself (where `%p` is an alloca/global slot storing a
  //     pointer) is NOT a dereference through pointer `%p`.
  //   - Accessing the loaded pointer value from `%p` IS a dereference through
  //     `%p`.
  //
  // We therefore map:
  //   - direct pointer values (arguments, phi, etc.) that are known pointers
  //   - and loaded values where the load source is a known pointer variable
  // to the corresponding `%p` / `@gptr` name.
  std::optional<std::string> inferPointerNameForDeref(Value *DerefAddr) const {
    Value *V = DerefAddr->stripPointerCasts();

    // Direct pointer value (e.g. function parameter `%p`) — but never treat
    // an alloca/global pointer slot itself as a dereference target.
    std::string DirectName = getValueName(V);
    if (isKnownPointer(DirectName) && !isa<AllocaInst>(V) &&
        !isa<GlobalVariable>(V)) {
      return DirectName;
    }

    // Typical lowered C pattern:
    //   %tmp = load ptr, ptr %p
    //   load/store ..., ptr %tmp
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      Value *Source = LI->getPointerOperand()->stripPointerCasts();
      std::string SourceName = getValueName(Source);
      if (isKnownPointer(SourceName))
        return SourceName;
    }

    return std::nullopt;
  }

  // ----------------------------------------------------------------
  // Helpers: check membership in the static PTA map
  // ----------------------------------------------------------------

  // Is `name` a pointer variable (LHS of some entry)?
  bool isKnownPointer(const std::string &name) const {
    return PtaMap_.count(name) > 0;
  }

  // Is `name` a pointee (RHS of any entry)?
  bool isKnownPointee(const std::string &name) const {
    for (const auto &kv : PtaMap_)
      if (kv.second.count(name))
        return true;
    return false;
  }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "PTA Validator — instruments LLVM IR "
                                          "to validate points-to analysis\n");

  // 1. Parse the points-to file
  PtaMap ptaMap = parsePtaFile(PtaFile);
  if (ptaMap.empty()) {
    errs() << "[PtaValidator] WARNING: points-to map is empty. Nothing to "
              "validate.\n";
  }

  outs() << "[PtaValidator] Loaded " << ptaMap.size()
         << " pointer entries from " << PtaFile << "\n";
  for (const auto &kv : ptaMap) {
    outs() << "  " << kv.first << " -> {";
    for (const auto &pt : kv.second)
      outs() << " " << pt;
    outs() << " }\n";
  }

  // 2. Load the LLVM IR
  LLVMContext Ctx;
  SMDiagnostic Err;
  auto M = parseIRFile(InputIR, Err, Ctx);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  // 3. Run the instrumentation pass
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);

  ModulePassManager MPM;
  MPM.addPass(PtaValidatorPass(ptaMap, PtaFile));
  MPM.run(*M, MAM);

  // 4. Verify the resulting IR
  if (verifyModule(*M, &errs())) {
    errs() << "[PtaValidator] ERROR: instrumented IR failed verification.\n";
    return 1;
  }

  // 5. Write output
  std::error_code EC;
  raw_fd_ostream OS(OutputIR, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "[PtaValidator] ERROR: cannot open output file: "
           << EC.message() << "\n";
    return 1;
  }
  M->print(OS, nullptr);
  outs() << "[PtaValidator] Instrumented IR written to " << OutputIR << "\n";

  return 0;
}
