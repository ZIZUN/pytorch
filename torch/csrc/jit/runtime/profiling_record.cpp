#include <torch/csrc/jit/runtime/profiling_record.h>

#include <ATen/core/interned_strings.h>
#include <c10/util/irange.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/clear_profiling.h>
#include <torch/csrc/jit/passes/constant_propagation.h>
#include <torch/csrc/jit/passes/cuda_graph_fuser.h>
#include <torch/csrc/jit/passes/tensorexpr_fuser.h>
#include <torch/csrc/jit/runtime/autodiff.h>
#include <torch/csrc/jit/runtime/graph_executor.h>
#include <torch/csrc/jit/runtime/interpreter.h>
namespace torch {
namespace jit {

namespace {

class ProfileRegistry {
 public:
  static ProfileRegistry* getRegistry() {
    static ProfileRegistry profile_registry_;
    return &profile_registry_;
  }

  void registerProfileNode(const std::function<bool(const Node*)>& func) {
    std::lock_guard<std::mutex> guard(mutex_);
    registry_funcs_.push_back(func);
  }

  bool shouldProfileNode(const Node* node) {
    std::lock_guard<std::mutex> guard(mutex_);
    // to guard differentiable graphs, we want profiling information
    // (in particular requires_grad) for nodes handled by autodiff
    if (isDifferentiable(node)) {
      return true;
    }
    for (const auto& func : registry_funcs_) {
      if (func(node)) {
        return true;
      }
    }
    return false;
  }

 private:
  std::vector<std::function<bool(const Node*)>> registry_funcs_;
  std::mutex mutex_;
};

} // namespace

void RegisterProfilingNode(const std::function<bool(const Node*)>& func) {
  ProfileRegistry::getRegistry()->registerProfileNode(func);
}

bool ShapeSymbolTable::bindSymbolicShapes(
    at::IntArrayRef new_sizes,
    const c10::SymbolicShape& sym_shapes) {
  if (!sym_shapes.rank().has_value()) {
    return true;
  }
  if (*sym_shapes.rank() != new_sizes.size()) {
    return false;
  }
  for (const auto i : c10::irange(new_sizes.size())) {
    auto symbol = (*sym_shapes.sizes())[i];
    if (!symbol.is_static()) {
      continue;
    }

    if (!isBound(symbol)) {
      assign(symbol, new_sizes[i]);
      continue;
    }

    if (getValue(symbol) != new_sizes[i]) {
      return false;
    }
  }
  return true;
}

ProfilingRecord::ProfilingRecord(std::shared_ptr<Graph> g)
    : profiled_graph_(std::move(g)), profiling_count_(getNumProfiledRuns()) {}

ProfileOp* ProfilingRecord::createProfileNode(
    const std::function<void(Stack&)>& fp,
    at::ArrayRef<Value*> inputs) {
  auto pn = new ProfileOp(profiled_graph_.get(), fp);

  for (auto in : inputs) {
    pn->addInput(in);
  }
  return pn;
}

ProfileIValueOp* ProfilingRecord::createProfileIValueNode(Value* in_val) {
  auto pn = new ProfileIValueOp(this->profiled_graph_.get(), nullptr);
  pn->addInput(in_val);
  auto pno = pn->addOutput();
  pno->setType(in_val->type());
  return pn;
}

ProfileIValueOp* ProfilingRecord::createProfileIValueNode(
    ArrayRef<Value*> inputs) {
  auto pn = new ProfileIValueOp(this->profiled_graph_.get(), nullptr);
  for (auto inp : inputs) {
    pn->addInput(inp);
    auto pno = pn->addOutput();
    pno->setType(inp->type());
  }
  return pn;
}

void ProfilingRecord::insertShapeProfile(Node* n, size_t offset) {
  Value* i = n->input(offset);
  auto pn = createProfileNode(nullptr, {i});
  auto pno = pn->addOutput();
  pn->ty_(attr::profiled_type, TensorType::get());
  pno->setType(TensorType::get());
  std::function<void(Stack&)> shape_profiler = [this, pn, pno](Stack& stack) {
    int64_t frame_id = 0;
    pop(stack, frame_id);
    IValue v;
    pop(stack, v);

    if (v.isTensor()) {
      auto& t = v.toTensor();
      auto new_tensor_type = tensorTypeInCurrentExecutionContext(t);

      std::lock_guard<std::mutex> lock(this->mutex_);
      if (profiling_count_ > 0) {
        GRAPH_DEBUG(
            "In run ",
            frame_id,
            " annotating %",
            pno->debugName(),
            " with ",
            *new_tensor_type);

        if (pn->hasRun()) {
          const auto& existing_tensor_type =
              pn->ty(attr::profiled_type)->expectRef<TensorType>();
          GRAPH_DEBUG(
              "Existing type for %",
              pno->debugName(),
              ": ",
              existing_tensor_type);
          auto merged_type = new_tensor_type->merge(existing_tensor_type);
          GRAPH_DEBUG(
              "Merged type for %", pno->debugName(), ": ", *merged_type);
          pn->ty_(attr::profiled_type, std::move(merged_type));
        } else {
          pn->setHasRun(true);
          pn->ty_(attr::profiled_type, std::move(new_tensor_type));
        }
      }
    }
    // passing t through
    push(stack, v);
  };

  pn->setCallback(shape_profiler);
  pn->insertBefore(n);
  n->replaceInput(offset, pn->output());
}

bool needsProfiledInputs(Node* n) {
  if (tensorexpr::isSupported(n) ||
#ifndef C10_MOBILE
      (RegisterCudaFuseGraph::isRegistered() && fuser::cuda::profileNode(n))
#else
      false
#endif
  ) {
    return true;
  }

  switch (n->kind()) {
    // specialize_autogradzero
    case prim::AutogradAdd:
    case prim::AutogradAnyNonZero:
    case prim::AutogradAllNonZero:
    case prim::AutogradAllZero:
    case prim::AutogradZero:
    // peephole
    case aten::dim:
    case aten::size:
    case aten::expand:
    case prim::dtype:
    case prim::device:
    case prim::is_cuda:
    case aten::is_floating_point:
    case aten::type_as:
    // TODO: hack to make `test_lstm_gates_permutations_cuda`
    // pass.
    case aten::t:
    case aten::mm:
      return true;
    default:
      return ProfileRegistry::getRegistry()->shouldProfileNode(n);
  }
}

bool needsProfiledOutput(Node* n) {
  if (tensorexpr::isSupported(n) ||
#ifndef C10_MOBILE
      (RegisterCudaFuseGraph::isRegistered() && fuser::cuda::profileNode(n))
#else
      false
#endif
  ) {
    return true;
  }

  switch (n->kind()) {
    case prim::AutogradAdd:
    case prim::AutogradZero:
      return true;
    default:
      return ProfileRegistry::getRegistry()->shouldProfileNode(n);
  }
}

void ProfilingRecord::removeProfileCounter(Block* b) {
  for (auto it = b->nodes().rbegin(); it != b->nodes().rend();) {
    auto n = *it;
    if (n->kind() == prim::profile && n->inputs().size() == 0) {
      it.destroyCurrent();
      // there is only one counter node
      return;
    } else {
      it++;
    }
  }
}

void ProfilingRecord::instrumentBlock(Block* block) {
  for (auto it = block->nodes().begin(); it != block->nodes().end(); ++it) {
    auto n = *it;
    for (const auto offset : c10::irange(n->inputs().size())) {
      auto i = n->input(offset);
      if (i->type()->kind() == c10::TypeKind::TensorType &&
          (needsProfiledInputs(n) || needsProfiledOutput(i->node()))) {
        insertShapeProfile(n, offset);
      }
    }

    for (auto b : n->blocks()) {
      instrumentBlock(b);
    }
  }

  // inserting profile nodes on block outputs
  // allows us to eliminate more guards as
  // the use of a guard is now in the same
  // block as opposed to being separated from
  // the definition by block boundaries
  for (size_t offset = 0; offset < block->return_node()->inputs().size();
       offset++) {
    auto i = block->return_node()->input(offset);
    if (i->type()->isSubtypeOf(*TensorType::get())) {
      insertShapeProfile(block->return_node(), offset);
    }
  }
}

void ProfilingRecord::removeProfilingNodes(Block* b) {
  for (auto it = b->nodes().begin(); it != b->nodes().end(); it++) {
    if (it->kind() == prim::profile || it->kind() == prim::profile_ivalue) {
      it->output()->replaceAllUsesWith(it->input());
      it.destroyCurrent();
    } else {
      for (Block* ib : it->blocks()) {
        removeProfilingNodes(ib);
      }
    }
  }
}

bool ProfilingRecord::ready() const {
  std::lock_guard<std::mutex> lock(this->mutex_);
  return profiling_count_ == 0;
}

std::unique_ptr<ProfilingRecord> ProfilingRecord::instrumentGraph(
    const std::shared_ptr<Graph>& graph) {
  auto new_g = graph->copy();

  auto pr = std::unique_ptr<ProfilingRecord>(new ProfilingRecord(new_g));
  auto raw_pr = pr.get();
  unprofileGraphInputs(new_g);
  unprofileBlock(new_g->block());
  pr->instrumentBlock(new_g->block());

  std::function<void(Stack&)> counter = [raw_pr](Stack& stack) {
    int64_t frame_id = 0;
    pop(stack, frame_id);

    std::lock_guard<std::mutex> lock(raw_pr->mutex_);

    if (raw_pr->profiling_count_ > 0) {
      raw_pr->profiling_count_--;
    }
  };

  auto pop = pr->createProfileNode(counter, {});
  new_g->appendNode(pop);
  GRAPH_DUMP("Instrumented Graph: ", new_g);
  return pr;
}

} // namespace jit
} // namespace torch
