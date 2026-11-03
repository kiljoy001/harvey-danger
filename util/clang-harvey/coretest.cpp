/*
 * Link against the libLLVMCore.a that was built inside Harvey, build a
 * function with IRBuilder, and print the module.
 */
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

int main() {
    llvm::LLVMContext ctx;
    auto m = std::make_unique<llvm::Module>("harvey", ctx);
    llvm::IRBuilder<> b(ctx);
    auto *fn = llvm::Function::Create(
        llvm::FunctionType::get(b.getInt32Ty(), false),
        llvm::GlobalValue::ExternalLinkage, "answer", m.get());
    auto *bb = llvm::BasicBlock::Create(ctx, "entry", fn);
    b.SetInsertPoint(bb);
    b.CreateRet(b.getInt32(42));
    m->print(llvm::outs(), nullptr);
    return 0;
}
