#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/freeze_module.h>

#include <torch/csrc/jit/graph_executor_impl.h>
#include <torch/csrc/jit/passes/inliner.h>

#include <stack>

namespace torch {
namespace jit {

namespace {

class AttributePropagator {
 private:
  // Contains attributes that can't be folded or user directs to keep them.
  std::map<script::Module*, std::set<std::string>> preservedAttrs_;

  void insertModifiedAttr(script::Module& module, std::string name) {
    auto it = preservedAttrs_.find(&module);
    if (it != preservedAttrs_.end()) {
      it->second.insert(name);
    } else {
      std::set<std::string> names;
      names.insert(name);
      preservedAttrs_[&module] = names;
    }
  }

  // We can use a more efficient algorithm to hash each constant GetAttr to its
  // conrresponding Value. Based on initial test on resnet50 and other torch
  // vision tests. GetAttrs are not too frequent so it is ok to chase GetAttr
  // chain to retrieve their values.
  bool findConstantAttr(
      Node* input,
      std::string m_name,
      script::Module& attr_module) {
    std::stack<std::string> names;

    while (!attr_module.hasattr(m_name)) {
      if (input->kind() == prim::GetAttr) {
        m_name = input->s(attr::name);
        names.push(m_name);
        input = input->inputs()[0]->node();
      } else {
        // TODO: handle TupleUnpack.
        return false;
      }
    }

    while (!names.empty()) {
      auto m_name = names.top();
      names.pop();
      attr_module = attr_module.attr(m_name).toModule();
      auto it = preservedAttrs_.find(&attr_module);
      if (it != preservedAttrs_.end()) {
        if (it->second.count(m_name)) {
          // Bail because this attribute is mutable!
          return false;
        }
      }
    }
    auto it = preservedAttrs_.find(&attr_module);
    return it == preservedAttrs_.end() || !it->second.count(m_name);
  }

  void recordMutableAttrs(
      std::shared_ptr<Graph>& graph,
      const script::Module& module_clone) {
    std::stack<Block*> blocks({graph->block()});

    while (!blocks.empty()) {
      Block* block = blocks.top();
      blocks.pop();
      for (auto n : block->nodes()) {
        for (Block* sub_block : n->blocks()) {
          blocks.push(sub_block);
        }
        if (n->kind() == prim::SetAttr) {
          auto name = n->s(attr::name);
          auto attr_module = module_clone;
          auto input = n->inputs()[0]->node();
          if (!findConstantAttr(input, name, attr_module)) {
            continue;
          }
          insertModifiedAttr(attr_module, name);
        }
      }
    }
  }

  std::set<std::string> getReferencedAttrs(
      std::shared_ptr<Graph>& graph,
      script::Module& module_clone) {
    std::stack<Block*> blocks({graph->block()});
    while (!blocks.empty()) {
      Block* block = blocks.top();
      blocks.pop();
      for (auto n : block->nodes()) {
        for (Block* sub_block : n->blocks()) {
          blocks.push(sub_block);
        }
        if (n->kind() == prim::GetAttr) {
          auto name = n->s(attr::name);
          if (module_clone.hasattr(name)) {
            insertModifiedAttr(module_clone, name);
          }
        }
      }
    }
    auto it = preservedAttrs_.find(&module_clone);
    if (it != preservedAttrs_.end())
      return it->second;
    else
      return std::set<std::string>();
  }

 public:
  void propagateAttributes(
      std::shared_ptr<Graph>& graph,
      const script::Module& module_clone) {
    std::stack<Block*> blocks({graph->block()});
    recordMutableAttrs(graph, module_clone);
    while (!blocks.empty()) {
      Block* block = blocks.top();
      blocks.pop();
      for (auto it = block->nodes().begin(); it != block->nodes().end();) {
        Node* n = *it;
        it++; // advance iterator bc the current node may be destroyed

        for (Block* sub_block : n->blocks()) {
          blocks.push(sub_block);
        }
        if (n->kind() == prim::GetAttr) {
          WithInsertPoint guard(n);
          auto name = n->s(attr::name);
          auto attr_module = module_clone;
          auto input = n->inputs()[0]->node();
          if (!findConstantAttr(input, name, attr_module)) {
            continue;
          }
          auto attr = attr_module.attr(name);
          if (attr.type()->cast<ClassType>()) {
            continue;
          }

          if (attr.isTensor()) {
            auto t = attr.toTensor();
            t.set_requires_grad(false);
          }
          // FIXME: support Dict, TUPLE, genericList and DoubleList constants
          // to resolve GetAttr.
          if (attr.isTuple() || attr.isGenericList() || attr.isGenericDict() ||
              attr.isDoubleList()) {
            // TORCH_CHECK(false, "Constant Dict, GenericList, Tuple, and
            // DoubleList  are not yet supported");
            continue;
          }
          Value* param_const = graph->insertConstant(attr);
          n->outputs().at(0)->replaceAllUsesWith(param_const);
          n->removeAllInputs();
          n->destroy();
        }
      }
    }
  }

  // This function cleans up  the Frozen module. it performs the following:
  // 1) Remove unused all attributes.
  // 2) Remove all unreferenced submodules
  // 3) Remove non pulic unreferenced methods.
  // TODO: do #3 because there is no API to 'unsafely' remove methods.

  //  This functions relies on unsafeRemoveAttr.
  void cleanupFrozenModule(
      std::shared_ptr<Graph>& graph,
      script::Module& module_clone) {
    std::vector<std::string> attrsToRemove;
    auto type = module_clone.type();
    size_t N = type->numAttributes();
    auto KeepAttrs = getReferencedAttrs(graph, module_clone);
    for (size_t i = 0; i < N; ++i) {
      auto attrTy = type->getAttribute(i);
      auto name = type->getAttributeName(i);
      if (!KeepAttrs.count(name)) {
        attrsToRemove.push_back(name);
      }
    }
    for (auto name : attrsToRemove) {
      module_clone._ivalue()->unsafeRemoveAttr(name);
      module_clone.type()->unsafeRemoveAttribute(name);
    }
  }
}; // class AttributePropagator
} // namespace

script::Module FreezeModule(const script::Module& module) {
  AttributePropagator attrPropagator;
  auto module_clone = module.clone();
  script::Method method = module_clone.get_method("forward");
  auto graph = method.graph();
  Inline(*graph);
  runOptimization(graph);
  attrPropagator.propagateAttributes(graph, module_clone);
  runOptimization(graph);
  attrPropagator.cleanupFrozenModule(graph, module_clone);

  GRAPH_DUMP(
      module_clone.type()->name()->name() + "::forward() after freezing module",
      method.graph());
  return module_clone;
}

} // namespace jit
} // namespace torch
