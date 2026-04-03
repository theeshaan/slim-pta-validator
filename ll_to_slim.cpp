#include "/home/eshaan/btp_work/SVF/llvm-18.1.0.obj/include/llvm/IRReader/IRReader.h" // For parseIRFile
#include "/home/eshaan/btp_work/SVF/llvm-18.1.0.obj/include/llvm/IR/LLVMContext.h"

#include "/home/eshaan/btp_work/slim/include/IR.h"

using namespace llvm;

LLVMContext context;

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 2)
    {
        llvm::errs() << "We expect exactly one argument - the name of the LLVM IR file!\n";
        exit(1);
    }

    llvm::SMDiagnostic smDiagnostic;

    std::unique_ptr<llvm::Module> module = parseIRFile(argv[1], smDiagnostic, context);

    slim::IR *transformIR = new slim::IR(module);

    transformIR->dumpIR();

    return 0;
}
