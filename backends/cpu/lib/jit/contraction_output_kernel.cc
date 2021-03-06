// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//===- contraction_output_kernel.cc -----------------------------*- C++ -*-===//
//
// Jit compiled contraction output kernel implementation.
//
//===----------------------------------------------------------------------===//

#include "tfrt/cpu/jit/contraction_output_kernel.h"

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/Mangling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "mlir/ExecutionEngine/CRunnerUtils.h"
#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/FileUtilities.h"
#include "tfrt/host_context/shared_context.h"
#include "tfrt/support/error_util.h"
#include "tfrt/support/forward_decls.h"
#include "tfrt/support/mutex.h"

namespace tfrt {
namespace cpu {
namespace jit {

//----------------------------------------------------------------------------//
// CompiledContractionOutputKernel owns the mlir ExecutionEngine that holds jit
// code generation result.
//----------------------------------------------------------------------------//

class CompiledContractionOutputKernel {
 public:
  explicit CompiledContractionOutputKernel(
      string_view function_name, SmallVector<int, 4> additional_args_ranks,
      std::unique_ptr<mlir::ExecutionEngine> engine)
      : function_name_(function_name.str()),
        additional_args_ranks_(std::move(additional_args_ranks)),
        engine_(std::move(engine)) {}

  Error Verify(ArrayRef<const DenseHostTensor*> additional_args) {
    if (additional_args.size() != additional_args_ranks_.size()) {
      return MakeStringError(
          "Wrong number of additional argumets. Expected %d got %d",
          additional_args_ranks_.size(), additional_args.size());
    }

    for (int i = 0; i < additional_args.size(); ++i) {
      const auto& additional_arg = additional_args[i];
      if (additional_arg->shape().GetRank() != additional_args_ranks_[i]) {
        return MakeStringError(
            "Wrong additional argument #%d rank. Expected %d got %d", i,
            additional_args_ranks_[i], additional_arg->shape().GetRank());
      }
    }

    return Error::success();
  }

  void Call(void* data, int64_t stride, int64_t row_offset, int64_t col_offset,
            int64_t rows, int64_t cols,
            ArrayRef<const DenseHostTensor*> additional_args) {
    auto fptr = engine_->lookup(function_name_);
    assert(fptr);

    // Jit compiled MLIR function takes arguments by `void**` type erased
    // pointers.
    llvm::SmallVector<void*, 32> fn_args;

    // Add a memref argument to `fn_args`, it is compatible with compiled
    // MLIR function memref ABI.
    auto add_memref_arg = [&](MemrefArg& memref) -> void {
      const int rank = memref.sizes.size();
      assert(memref.sizes.size() == memref.strides.size());
      fn_args.push_back(&memref.data);  // memref.basePtr
      fn_args.push_back(&memref.data);  // memref.data
      fn_args.push_back(&memref.offset);
      for (int i = 0; i < rank; ++i) fn_args.push_back(&memref.sizes[i]);
      for (int i = 0; i < rank; ++i) fn_args.push_back(&memref.strides[i]);
    };

    // ---------------------------------------------------------------------- //
    // Pack default output keren function arguments.
    // ---------------------------------------------------------------------- //

    // NOTE: The jitted output kernel expects a memref in row-major layout, so
    // we swap rows with columns when we pass output block to the output kernel.
    MemrefArg output_block(2);
    output_block.data = data;
    output_block.offset = 0;
    output_block.sizes[0] = cols;
    output_block.sizes[1] = rows;
    output_block.strides[0] = stride;
    output_block.strides[1] = 1;

    add_memref_arg(output_block);

    // Offsets are also swapped.
    fn_args.push_back(&col_offset /*row_offset*/);
    fn_args.push_back(&row_offset /*col_offset*/);

    // ---------------------------------------------------------------------- //
    // Pack additional output kernel arguments.
    // ---------------------------------------------------------------------- //
    assert(additional_args.size() == additional_args_ranks_.size());

    // We need a pointer-stable storage to keep additional function arguments.
    llvm::SmallVector<MemrefArg, 4> additional_memref_args;
    additional_memref_args.reserve(additional_args.size());

    // TODO(ezhulenev): Support additional args of all ranks.
    for (int i = 0; i < additional_args.size(); ++i) {
      const auto& additional_arg = additional_args[i];
      const auto& shape = additional_arg->shape();

      additional_memref_args.emplace_back(shape.GetRank());
      MemrefArg& memref = additional_memref_args.back();

      assert(shape.GetRank() == 1);
      memref.data = const_cast<void*>(additional_arg->data());
      memref.offset = 0;
      memref.sizes[0] = shape.GetDimensionSize(0);
      memref.strides[0] = 1;

      add_memref_arg(memref);
    }

    (*fptr)(fn_args.data());
  }

 private:
  struct MemrefArg {
    explicit MemrefArg(int rank) : sizes(rank), strides(rank) {}
    void* data;
    int64_t offset;
    SmallVector<int64_t, 4> sizes;
    SmallVector<int64_t, 4> strides;
  };

  std::string function_name_;
  SmallVector<int, 4> additional_args_ranks_;
  std::unique_ptr<mlir::ExecutionEngine> engine_;
};

namespace {

//----------------------------------------------------------------------------//
// Setup MLIR pass pipeline to lower to LLVM dialect, and use ORC JIT to codegen
// functions at runtime.
//----------------------------------------------------------------------------//

void InitializeCompiler() {
  static const bool initialized = ([]() -> bool {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    mlir::registerAllDialects();
    mlir::registerAllPasses();

    return true;
  })();
  (void)initialized;
}

struct CompilationOptions {
  Optional<llvm::CodeGenOpt::Level> jit_code_opt_level;
};

// Verifies output kernel function signature and returns a vector with expected
// ranks of the additional output kernel arguments.
Expected<SmallVector<int, 4>> VerifyOutputKerneSignature(
    mlir::ModuleOp module, string_view function_name) {
  mlir::FuncOp fn = module.lookupSymbol<mlir::FuncOp>(function_name);
  if (!fn) {
    return MakeStringError("Function not found");
  }

  if (fn.getNumArguments() < 3) {
    return MakeStringError(
        "Output kernel function must have at least 4 arguments, got %d",
        fn.getNumArguments());
  }

  auto output_block = fn.getArgument(0).getType().dyn_cast<mlir::MemRefType>();
  if (!output_block || !output_block.hasRank() || output_block.getRank() != 2) {
    return MakeStringError(
        "First output kernel argument must be a rank 2 memref");
  }

  auto row_offset = fn.getArgument(1).getType().dyn_cast<mlir::IntegerType>();
  if (!row_offset || row_offset.getWidth() != 64) {
    return MakeStringError(
        "Second output kernel argument must be a row offset of i64 type");
  }

  auto col_offset = fn.getArgument(2).getType().dyn_cast<mlir::IntegerType>();
  if (!col_offset || col_offset.getWidth() != 64) {
    return MakeStringError(
        "Third output kernel argument must be a col offset of i64 type");
  }

  // Verify additional output kernel arguments and collect their ranks.
  SmallVector<int, 4> args_ranks;
  for (int i = 3; i < fn.getNumArguments(); ++i) {
    auto arg = fn.getArgument(i).getType().dyn_cast<mlir::MemRefType>();
    if (!arg || !arg.hasRank())
      return MakeStringError(
          "Additional output kernel arguments must be ranked memrefs");

    // TODO(ezhulenev): Support other ranks for additional arguments.
    if (arg.getRank() != 1) {
      return MakeStringError(
          "Additional output kernel argument must be rank 1 memref");
    }

    args_ranks.push_back(arg.getRank());
  }

  return args_ranks;
}

Expected<CompiledContractionOutputKernel> Compile(string_view function_name,
                                                  string_view source,
                                                  CompilationOptions opts) {
  auto str = source.str();
  auto src = llvm::MemoryBuffer::getMemBuffer(str, "<unknown>");

  llvm::SourceMgr source_mgr;
  source_mgr.AddNewSourceBuffer(std::move(src), llvm::SMLoc());

  mlir::MLIRContext context;
  mlir::OwningModuleRef module(mlir::parseSourceFile(source_mgr, &context));
  if (!module) {
    return MakeStringError("Failed to parse kernel source");
  }

  // Verify output kernel function signature and collect additional arguments
  // ranks.
  auto additional_args = VerifyOutputKerneSignature(*module, function_name);
  if (auto err = additional_args.takeError()) return std::move(err);

  mlir::LowerToLLVMOptions lower_to_llvm_opts;
  mlir::PassManager pm(&context);
  pm.addPass(mlir::createLowerToCFGPass());
  pm.addPass(mlir::createLowerToLLVMPass(lower_to_llvm_opts));

  if (failed(pm.run(*module))) {
    return MakeStringError("Failed to lower module to LLVM");
  }

  // Prepare JIT target machine for code generation.
  auto builder = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!builder) return builder.takeError();

  auto target_machine = builder->createTargetMachine();
  if (!target_machine) return target_machine.takeError();

  // Link with shared libraries for symbol resolution.
  llvm::SmallVector<llvm::StringRef, 4> libs;

  // Additional LLVM passes to run.
  llvm::SmallVector<const llvm::PassInfo*, 4> passes;
  auto transformer = mlir::makeLLVMPassesTransformer(passes, /*mbOptLevel=*/0,
                                                     target_machine->get());

  // Build MLIR exection engine.
  auto engine = mlir::ExecutionEngine::create(*module, transformer,
                                              opts.jit_code_opt_level, libs);
  if (!engine) return engine.takeError();

  return CompiledContractionOutputKernel(
      function_name, std::move(*additional_args), std::move(*engine));
}

//----------------------------------------------------------------------------//
// CompiledContractionOutputKernelsContext owns all compiled contraction output
// kernels in a SharedContext that is owned by a HostContext.
//----------------------------------------------------------------------------//

class CompiledKernelsContext : public SharedContext {
 public:
  explicit CompiledKernelsContext(HostContext* host_context) {}

  CompiledKernelsContext(const CompiledKernelsContext&) = delete;
  void operator=(const CompiledKernelsContext&) = delete;

  Expected<CompiledContractionOutputKernel*> GetCompiledKernel(
      string_view key, string_view function_name, string_view mlir_module) {
    {  // Fast path. Check if the kernel is already compiled.
      mutex_lock lock(mu_);
      auto it = kernels_.find(key);
      if (it != kernels_.end()) return {&it->getValue()};
    }

    // Compile the kernel without holding a lock.
    auto compiled = Compile(function_name, mlir_module, opts_);
    if (!compiled) return compiled.takeError();

    // Double check that concurrent execution did not already compile the kernel
    // before the current thread of execution.
    mutex_lock lock(mu_);
    auto it = kernels_.find(key);
    if (it != kernels_.end()) return {&it->getValue()};

    auto inserted = kernels_.try_emplace(key, std::move(*compiled));
    return &inserted.first->getValue();
  }

 private:
  mutex mu_;
  CompilationOptions opts_;
  llvm::StringMap<CompiledContractionOutputKernel> kernels_
      TFRT_GUARDED_BY(mu_);
};

}  // namespace

// Returns contraction output kernel compiled from the MLIR module.
Expected<CompiledContractionOutputKernel*> GetCompiledContractionOutputKernel(
    HostContext* host, string_view function_name, string_view mlir_module) {
  InitializeCompiler();

  auto& ctx = host->GetOrCreateSharedContext<CompiledKernelsContext>();
  // TODO(ezhulenev): Create a shorter fingerprint from the mlir module to use
  // as a lookup key for the cache.
  const string_view key = mlir_module;
  return ctx.GetCompiledKernel(key, function_name, mlir_module);
}

void CallCompiledContractionOutputKernel(
    CompiledContractionOutputKernel* kernel, DType dtype, void* data,
    int64_t stride, int64_t row_offset, int64_t col_offset, int64_t rows,
    int64_t cols, ArrayRef<const DenseHostTensor*> additional_args) {
  kernel->Call(data, stride, row_offset, col_offset, rows, cols,
               additional_args);
}

Error VerifyCompiledContractionOutoutKernelArgs(
    CompiledContractionOutputKernel* kernel,
    ArrayRef<const DenseHostTensor*> additional_args) {
  return kernel->Verify(additional_args);
}

}  // namespace jit
}  // namespace cpu
}  // namespace tfrt
