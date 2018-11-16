#include "ir.h"


#include "torch/csrc/jit/operator.h"
#include "torch/csrc/autograd/function.h"
#include "torch/csrc/jit/constants.h"
#include "torch/csrc/jit/assertions.h"
#include "torch/csrc/jit/script/compiler.h"
#include "torch/csrc/jit/passes/python_print.h"

#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <stack>
#include <sstream>
#include <algorithm>
#include <string>

namespace torch { namespace jit {
// Constants relating to maintaining the topological index of nodes.
//
// Lower and upper bounds of the index. Inclusive range.
static constexpr topo_position_t kLowerBound = INT64_MIN;
static constexpr topo_position_t kUpperBound = INT64_MAX;
static constexpr topo_position_t kMidPoint = 0;
// How far away to space nodes that are appended to the graph.
// should be 2^n, where:
//   - n is the maximum number of repeated insertions without a re-index
//   - 2^(64-n) is the maximum number of appends to the end without reindex
static constexpr topo_position_t kAppendInterval = 1099511627776ULL /* 2^40 */;

// Sigh, see https://stackoverflow.com/questions/8016780/undefined-reference-to-static-constexpr-char
constexpr Symbol PythonOp::Kind;

void printValueRef(std::ostream & out, const Value * n) {
  out << "%" << n->uniqueName();
}

// NB: This overload will become ambiguous with the one Caffe2 provides in its
// logging, if they ever intersect.
template <typename T>
std::ostream& operator<<(std::ostream & out, const std::vector<T> & nodes) {
  out << at::ArrayRef<T>{nodes};
  return out;
}

template <typename T>
std::ostream& printValueRefs(std::ostream & out, const at::ArrayRef<T> & nodes) {
  size_t i = 0;
  for(auto n : nodes) {
    if(i++ > 0)
      out << ", ";
    printValueRef(out, n);
  }
  return out;
}

// Can't make these two overloads directly a template, it'll be ambiguous with
// the global printer for operator<<.

std::ostream& operator<<(std::ostream & out, const at::ArrayRef<const Value*> & nodes) {
  return printValueRefs(out, nodes);
}

std::ostream& operator<<(std::ostream & out, const at::ArrayRef<Value*> & nodes) {
  return printValueRefs(out, nodes);
}

struct const_value_list_with_types {
  const ArrayRef<const Value*> values;
  bool use_newlines;
  const_value_list_with_types(ArrayRef<const Value*> values, bool use_newlines = false)
    : values(values), use_newlines(use_newlines) {}
};
std::ostream& operator<<(std::ostream & out, const_value_list_with_types l) {
  size_t i = 0;
  for(auto n : l.values) {
    if(i++ > 0) {
      if (l.use_newlines) {
        // TODO: Indent here is hard-coded for "graph(": un-hard-code it
        out << "\n      ";
      } else {
        out << ", ";
      }
    }
    printValueRef(out, n);
    out << " : ";
    out << *n->type();
  }
  return out;
}

void printAttributes(std::ostream & out, const Node * n, bool ignore_subgraph=false) {
  out << "[";
  auto names = n->attributeNames();
  int i = 0;
  for(auto name : names) {
    if (ignore_subgraph && name == attr::Subgraph)
      continue;
    if(i++ > 0)
      out << ", ";
    // TODO: debugging mode to see the qualifier.  We definitely
    // don't want to print the qualifier since it should always
    // be attribute, but you might be able to track down a weird
    // bug by printing it out.
    out << name.toUnqualString() << "=";

    n->printValue(out, name);
  }
  out << "]";
}

static std::ostream & indent(std::ostream & out, size_t level) {
  for(size_t i = 0; i < level; ++i)
    out << "  ";
  return out;
}

std::ostream& printNode(std::ostream & out, size_t level, const Node * n, std::vector<const Node*> * groups) {
  auto outputs = n->outputs();
  indent(out, level) << const_value_list_with_types(outputs);
  out << " = ";
  IR_IFM_CONST(n,PythonOp)
    out << "^" << value->name();
    value->writeScalars(out);
  IR_ELSE()
    if(n->hasAttribute(attr::Subgraph) && groups) {
      out << n->kind().toQualString() << "_" << groups->size();
      if (n->numAttributes() > 1 && n->kind() != prim::DifferentiableGraph) {
        printAttributes(out, n, /*ignore_subgraph=*/true);
      }
      groups->push_back(n);
    } else {
      out << n->kind().toQualString();
      if(n->hasAttributes()) {
        printAttributes(out,n);
      }
    }
  IR_END()
  out << "(" << n->inputs() << ")";
  std::string scopeName = n->scopeName();
  if (scopeName.empty()) {
    out << "\n";
  }
  else {
    out << ", ";
    out << "scope: " << scopeName << "\n";
  }
  for(size_t i = 0; i < n->blocks().size(); ++i) {
    auto b = n->blocks()[i];
    indent(out, level + 1) << "block" << i << "(" << const_value_list_with_types(b->inputs(), false) << ") {\n";
    for(auto n : b->nodes()) {
      printNode(out, level + 2, n, groups);
    }
    indent(out, level + 2) << "-> (" << b->outputs() << ")\n";
    indent(out, level + 1) << "}\n";
  }
  return out;
}

std::ostream& operator<<(std::ostream & out, const Node & n) {
  return printNode(out, 0, &n, nullptr);
}

std::ostream& operator<<(std::ostream & out, const Graph & g) {
  out << "graph(" << const_value_list_with_types(g.inputs(), true) << ") {\n";
  std::vector<const Node*> groups;
  for(auto n : g.nodes()) {
    printNode(out, 1, n, &groups);
  }
  out << "  return (" << g.outputs() << ");\n}\n";
  size_t i = 0;
  for(auto fg : groups) {
    out << "with " << fg->kind().toQualString() << "_" <<i++ << " = " << *fg->g(attr::Subgraph);
  }
  /*
  // Uncomment this to debug all_nodes issues
  {
    out << "\n";
    out << "all_nodes:\n";
    for (auto& n : g.all_nodes) {
      printNode(out, const_cast<Node*>(n), nullptr);
    }
  }
  */
  return out;
}

std::ostream& Graph::prettyPrint(std::ostream & out) {
  PythonPrint(out, *this);
  return out;
}

void Graph::dumpPretty() {
  PythonPrint(std::cout, *this);
}

static void checkSameDevice(const Node* node) {
  bool has_device = false;
  int device;
  auto checkValue = [&](const Value* v) {
    if(CompleteTensorTypePtr type = v->type()->cast<CompleteTensorType>()) {
      if(!has_device) {
        has_device = true;
        device = type->device();
      } else {
        JIT_ASSERT(device == type->device());
      }
    }
  };
  for(auto input : node->inputs()) {
    checkValue(input);
  }
  for(auto output : node->outputs()) {
    checkValue(output);
  }
}

using node_set = std::set<const Node*>;
#define ALL_OF(container) container.begin(), container.end()

// These functions purposely operate on the internal members directly, to force
// you to think about how the invariants change if you change the data
// representation (even if the external API does not change.)

// NB: This assert is written to assume you don't have any unattached
// nodes.  Unattached nodes can occur while manipulations to the
// graph are occurring.
void Node::lint() const {
  // Node invariants
  // - if node should live in list, nodes_iter is consistent
  // - Inputs are all marked as a use by the nodes they refer to
  // - Owning graph is non-null and consistent
  // - The "Select" invariant, when the node is MultiReturn
  //
  // The handle invariant:
  //    If a node takes a handle as an input, it is always the
  //    LAST input of the node.  There is at most one handle input.

  {
    size_t i = 0;
    for (auto input : inputs_) {
      // WARNING: O(n^2)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      JIT_ASSERT(std::find(ALL_OF(input->uses_), Use(const_cast<Node*>(this), i)) != input->uses_.end());
      JIT_ASSERT(graph_->all_nodes.count(this) == 1);
      i++;
    }
  }

  for(auto o : outputs()) {
    size_t i = 0;
    for (auto use : o->uses()) {
      // Use invariants
      // - Use is consistent with inputs
      // - Every user node is live (checked in Graph)
      JIT_ASSERT(use.user->inputs_[use.offset] == o);
      i++;
    }
  }

  // Node subclass invariants
  IR_IF(this,Constant)
    JIT_ASSERT(inputs_.size() == 0);
  IR_ELSEIF(Return)
    // Return uses is zero
    JIT_ASSERT(outputs().size() == 0);
  IR_ELSEIF(Param)
    // Param inputs is zero
    JIT_ASSERT(inputs_.size() == 0);
  IR_ELSEIFM_CONST(PythonOp)
    // Python operator cconv is correct
    size_t n_scalars = 0, n_tensors = 0;
    for (auto c : value->cconv) {
      if (c == 'c') {
        n_scalars++;
      } else if (c == 'd') {
        n_tensors++;
      } else {
        JIT_ASSERT(0);
      }
      JIT_ASSERT(static_cast<bool>(value->pyobj));
    }
    JIT_ASSERT(n_scalars == value->scalar_args.size());
    JIT_ASSERT(n_tensors == inputs_.size());
  IR_ELSEIF(Eval)
    // TODO: add invariants
  // TODO: It's not good for these ops to be top-level, it makes cases longer.
  IR_ELSEIF(FusionGroup)
    checkSameDevice(value);
    // TODO: Typecheck the parameters
    value->g(attr::Subgraph)->lint();
  IR_END()

}

// TODO: When lint fails, give better indication about which
// instruction triggered the failure.
void Graph::lint() const {
  // Graph invariants

  // Uncomment the following to see the graph
  // std::cout << *const_cast<Graph*>(this);

  // nodes
  // - nodes_ is a valid topological ordering for inputs
  // - No repeated nodes
  // - Params and return do NOT occur in nodes
  // - next_unique_ is greater than all uniques in graph
  // - uniques in all_nodes are unique
  // - every use will occur later in the topsort

  struct LintScope {
    LintScope() = default;
    LintScope(std::unique_ptr<LintScope> parent)
    : parent(std::move(parent)) {}
    bool contains(const Value * v) {
      return values.count(v) > 0 || (parent && parent->contains(v));
    }
    bool contains(const Node * n) {
      return nodes.count(n) > 0 || (parent && parent->contains(n));
    }
    void insert(const Value * v) {
      JIT_ASSERT(!contains(v));
      values.insert(v);
    }
    void insert(const Node * n) {
      JIT_ASSERT(!contains(n));
      nodes.insert(n);
    }
    std::unique_ptr<LintScope> parent;
  private:
    std::unordered_set<const Value*> values;
    std::unordered_set<const Node*> nodes;
  };
  // Struct enables mutual recursion in linting methods.
  // Putting it inside Graph::lint enables access to private Graph members
  struct LintImpl {
    LintImpl(const Graph & g)
    : g(g)
    , scope(new LintScope())
    , all_nodes_set(ALL_OF(g.all_nodes)) {} // NB: all_nodes is *unordered*
    const Graph & g;
    std::unique_ptr<LintScope> scope;
    std::unordered_set<size_t> seen_uniques;
    std::unordered_map<const Node*, int64_t> anticipated_uses;
    node_set all_nodes_set;
    node_set sum_set;

    void check_value(const Value* v) {
      scope->insert(v);
      auto b2 = seen_uniques.insert(v->unique());
      JIT_ASSERT(b2.second);  // insertion took place
      JIT_ASSERT(v->unique() < g.next_unique_);

      for (auto use : v->uses()) {
        JIT_ASSERT(!scope->contains(use.user));
        JIT_ASSERT(g.all_nodes.count(use.user) == 1);
        anticipated_uses[use.user]++;  // int default constructs to 0
      }
    }
    void check_node(const Node* n) {
      for (auto input : n->inputs_) {
        if (!scope->contains(input)) {
          JIT_ASSERTM(0, input->unique(), " not in scope");
        }
      }
      JIT_ASSERT(anticipated_uses[n] == static_cast<int64_t>(n->inputs_.size()));
      anticipated_uses[n] = -1;  // we saw the anticipated user!
      scope->insert(n);
      for(auto block : n->blocks()) {
        std::unique_ptr<LintScope> new_scope(new LintScope(std::move(scope)));
        scope = std::move(new_scope);
        check_block(block);
        scope = std::move(scope->parent);
      }
      size_t i = 0;
      for(auto o : n->outputs()) {
        JIT_ASSERT(o->node() == n);
        JIT_ASSERT(i++ == o->offset_);
        check_value(o);
      }
      n->lint();
    }
    void check_block(const Block *b) {
      // Check topological ordering
      JIT_ASSERT(b->param_node()->isBefore(*b->nodes().begin()));
      auto curNode = *b->nodes().begin();
      while (curNode != b->return_node()) {
        JIT_ASSERT(curNode->isBefore(curNode->next()));
        curNode = curNode->next();
      }

      for (auto input : b->inputs()) {
        check_value(input);
        JIT_ASSERT(input->node()->kind_ == prim::Param);
      }

      for (auto n : b->nodes()) {
        JIT_ASSERT(n->kind_ != prim::Param);
        JIT_ASSERT(n->kind_ != prim::Return);
        check_node(n);
      }

      JIT_ASSERT(b->output_->kind() == prim::Return);
      check_node(b->output_);

      // all_nodes
      // - inputs_, output_ and nodes_ are all included in all_nodes
      // - all_nodes does not contain dead nodes??? (likely to be temporarily
      // suspended).  Weaker: all_nodes contains all inputs and returns
      // - only one return node???

      node_set nodes_set(ALL_OF(b->nodes()));
      node_set inputs_set {b->input_};
      node_set output_set {b->output_};
      // TODO: Make a more type safe std::includes wrapper which disallows use on
      // non-ordered containers
      JIT_ASSERT(std::includes(ALL_OF(all_nodes_set), ALL_OF(nodes_set)));
      JIT_ASSERT(std::includes(ALL_OF(all_nodes_set), ALL_OF(inputs_set)));
      JIT_ASSERT(std::includes(ALL_OF(all_nodes_set), ALL_OF(output_set)));

      sum_set.insert(ALL_OF(nodes_set));
      sum_set.insert(ALL_OF(inputs_set));
      sum_set.insert(ALL_OF(output_set));
    }
    void check_graph() {
      node_set all_nodes_set(ALL_OF(g.all_nodes)); // NB: all_nodes is *unordered*

      check_block(g.block_);
      for (auto kv : anticipated_uses) {
        JIT_ASSERT(kv.second == -1);
      }
      JIT_ASSERT(std::includes(ALL_OF(sum_set), ALL_OF(all_nodes_set)));
    }
  };
  LintImpl(*this).check_graph();
}

void Graph::dump() const {
  std::cout << *this << "\n";
}

void LintGraph(std::shared_ptr<Graph>& graph) {
  graph->lint();
}

Block::Block(Graph* graph_, Node* node_)
    : graph_(graph_),
      output_(initOutput(graph_->create(prim::Return, 0))),
      input_(graph_->create(prim::Param, 0)),
      owning_node_(node_) {
  graph_->all_blocks.emplace(this);
  output_->owning_block_ = this;
  output_->topo_position_ = kUpperBound;
  input_->owning_block_ = this;
  input_->topo_position_ = kLowerBound;
}

void Block::reIndexTopology() {
  auto curPos = kLowerBound;
  for (auto node : nodes()) {
    AT_ASSERT(curPos <= (kUpperBound - kAppendInterval));
    curPos += kAppendInterval;
    node->topo_position_ = curPos;
  }
}

void Block::cloneFrom(Block * src, std::function<Value*(Value*)> value_map) {
  std::unordered_map<Value*, Value*> local_map;
  auto env = [&](Value * v) {
    auto it = local_map.find(v);
    if(it != local_map.end())
      return it->second;
    return value_map(v);
  };

  auto graph = owningGraph();
  for(auto input : src->inputs()) {
    local_map[input] = this->addInput()->copyMetadata(input);
  }

  for(auto node : src->nodes()) {
    auto new_node = this->appendNode(graph->createClone(node, env));
    for(size_t i = 0; i < node->outputs().size(); ++i) {
      auto oo = node->outputs()[i];
      auto no = new_node->outputs()[i];
      local_map[oo] = no;
      no->copyMetadata(oo);
    }
  }
  for(auto output : src->outputs()) {
    this->registerOutput(env(output));
  }
}

void Block::destroy() {
  // we cannot destroy the output because it is used as the sentinel
  // for the nodes() list and has to remain valid for the loop
  output_->removeAllInputs();
  for(auto it = this->nodes().reverse().begin(),
      end = this->nodes().reverse().end();
      it != end; ++it) {
    it.destroyCurrent();
  }
  output_->destroy();
  input_->destroy();
  graph_->freeBlock(this);
}

std::shared_ptr<Graph> Graph::copy() {
  auto new_g = std::make_shared<Graph>();
  auto env = [](Value* v) -> Value* {
    AT_ERROR(
        "Graph::copy() encountered a use of a value not in scope. Run lint!");
  };
  new_g->block()->cloneFrom(this->block(), env);
  return new_g;
}

std::string Value::uniqueNameBase() const {
  std::string name = uniqueName();
  std::string name_base = name;
  auto last_dot_pos = name.find_last_of('.');
  if (last_dot_pos != std::string::npos && last_dot_pos + 1 != name.size()) {
    if (name.find_first_not_of("0123456789", last_dot_pos + 1) == std::string::npos) {
      name_base = name.substr(0, last_dot_pos);
    }
  }
  return name_base;
}

Value* Value::setUniqueName(const std::string & name) {
  if (name.size() > 0 && name.find_first_not_of("0123456789") == std::string::npos) {
    throw std::runtime_error("names may not be integers: " + name);
  }

  auto & names = node()->owningGraph()->unique_names_;

  // clear any old name from the map
  if(hasUniqueName()) {
    names.erase(unique_name_);
    unique_name_ = "";
  }

  // allow "" to clear the uniquename
  if(name == "")
    return this;

  // if someone else has this name, then rename the other value
  auto old_owner_of_name = names.find(name);
  if(old_owner_of_name != names.end()) {
    size_t suffix = 1;
    std::string name_base = name;
    auto last_dot_pos = name.find_last_of('.');
    if (last_dot_pos != std::string::npos && last_dot_pos + 1 != name.size()) {
      if (name.find_first_not_of("0123456789", last_dot_pos + 1) == std::string::npos) {
        suffix = std::stoll(name.substr(last_dot_pos + 1));
        name_base = name.substr(0, last_dot_pos);
      }
    }
    std::string replacement_name;
    do {
      std::stringstream ss;
      ss << name_base << "." << suffix++;
      replacement_name = ss.str();
    } while(names.count(replacement_name) > 0);
    old_owner_of_name->second->setUniqueName(replacement_name);
  }

  names[name] = this;
  unique_name_ = name;
  return this;
}

Value* Value::copyMetadata(Value * from) {
  setType(from->type());
  if (from->hasUniqueName())
    setUniqueName(from->uniqueName());
  return this;
}

void Value::replaceFirstUseWith(Value * newValue) {
  JIT_ASSERT(owningGraph() == newValue->owningGraph());
  auto u = uses()[0];
  u.user->inputs_[u.offset] = newValue;
  newValue->uses_.push_back(u);
  uses_.erase(uses_.begin());
}

void Value::replaceAllUsesWith(Value * newValue) {
  while (!uses().empty()) {
    replaceFirstUseWith(newValue);
  }
}

size_t findArgument(const FunctionSchema& the_schema, Symbol name) {
  auto name_str = name.toUnqualString();
  for (size_t i = 0; i < the_schema.arguments().size(); ++i) {
    const Argument* arg = &the_schema.arguments()[i];
    if (arg->name() == name_str) {
      return i;
    }
  }
  throw std::runtime_error(std::string("Couldn't find an argument called ") + name.toQualString());
}

c10::optional<IValue> Node::get(Symbol name) const {
  return toIValue(namedInput(name));
}

Value* Node::namedInput(Symbol name) const {
  return input(findArgument(schema(), name));
}

bool Node::matches(const char *signature_literal, at::ArrayRef<Symbol> const_inputs) const {
  if (!sig(signature_literal).matches(this)) return false;
  for (Symbol s : const_inputs) {
    if (!is_constant(s)) return false;
  }
  return true;
}

void Node::dump() const {
  std::cout << *this << "\n";
}

void Node::findSchema() const {
  schema_ = &getOperatorFor(this).schema();
}

const FunctionSchema* Node::maybeSchema() const {
  if(!schema_) {
    if(auto op = findOperatorFor(this)) {
      schema_ = &op->schema();
    }
  }
  return schema_;
}

bool Node::isNondeterministic() const {
  static const OperatorSet nondeterministic_ops = {
    "aten::dropout(Tensor input, float p, bool train) -> Tensor",
    "aten::_fused_dropout(Tensor self, float p, Generator generator) -> (Tensor, Tensor)",
    "aten::_standard_gamma(Tensor self, Generator generator) -> Tensor",
    "aten::bernoulli(Tensor self, *, Generator generator) -> Tensor",
    "aten::bernoulli(Tensor self, float p, *, Generator generator) -> Tensor",
    "aten::multinomial(Tensor self, int num_samples, bool replacement, *, Generator generator) -> Tensor",
    "aten::normal(Tensor mean, Tensor std, *, Generator generator) -> Tensor",
    "aten::normal(float mean, Tensor std, *, Generator generator) -> Tensor",
    "aten::normal(Tensor mean, float std, *, Generator generator) -> Tensor",
    "aten::poisson(Tensor self, Generator generator) -> Tensor",
    "aten::rrelu(Tensor self, Scalar lower, Scalar upper, bool training, Generator generator) -> Tensor",
    "aten::rrelu_with_noise(Tensor self, Tensor noise, Scalar lower, Scalar upper, bool training, Generator generator) -> Tensor",
    "aten::rand(int[] size, *, int dtype, int layout, int[] device) -> Tensor",
    "aten::rand_like(Tensor self) -> Tensor",
    "aten::rand_like(Tensor self, *, int dtype, int layout, int[] device) -> Tensor",
    "aten::randint(int high, int[] size, *, int dtype, int layout, int[] device) -> Tensor",
    "aten::randint(int low, int high, int[] size, *, int dtype, int layout, int[] device) -> Tensor",
    "aten::randint_like(Tensor self, int high) -> Tensor",
    "aten::randint_like(Tensor self, int low, int high) -> Tensor",
    "aten::randint_like(Tensor self, int high, *, int dtype, int layout, int[] device) -> Tensor",
    "aten::randint_like(Tensor self, int low, int high, *, int dtype, int layout, int[] device) -> Tensor",
    "aten::randn(int[] size, *, int dtype, int layout, int[] device) -> Tensor",
    "aten::randn_like(Tensor self) -> Tensor",
    "aten::randn_like(Tensor self, *, int dtype, int layout, int[] device) -> Tensor",
    "aten::randperm(int n, *, int dtype, int layout, int[] device) -> Tensor"
  };

  if (nondeterministic_ops.find(this) == nullptr) {
    return false;
  }
  // Dropout with train = False is deterministic
  if (matches("aten::dropout(Tensor input, float p, bool train) -> Tensor") && is_constant(attr::train) && !get<bool>(attr::train).value()) {
    return false;
  }
  return true;
}

// Assign this node a topological position, to facilitate fast isBefore() and
// isAfter() queries. Must be called right after a node is inserted into the
// node list.
//
// The basic scheme is: assign every node a position (uint64_t).  The common
// case (appending to the end of the graph) is made more efficient by advancing
// a fixed interval past the previous node and placing `this` there. Otherwise,
// assign `this` a position at the midpoint between its prev() and next()
// nodes.
//
// If we ever run out of space (by, e.g. inserting too much in place), we
// reindex by spreading out all the nodes again.
void Node::assignTopoPosition() {
  auto returnNode = owningBlock()->return_node();
  const auto prevPos = prev()->topo_position_;
  const auto nextPos = next()->topo_position_;

  // Append to the end of the graph
  if (next() == returnNode) {
    if (next() == prev()) {
      // the node list is empty, assign the first position
      topo_position_ = kMidPoint;
      return;
    }

    if (prevPos >= (kUpperBound - kAppendInterval)) {
      // we're running off the edge
      owningBlock()->reIndexTopology();
      return;
    }

    topo_position_ = prevPos + kAppendInterval;

    // Prepend to the graph
  } else if (prev() == returnNode) {
    // next() is the first element in the block list
    if (nextPos <= (kLowerBound + kAppendInterval)) {
      // we're running off the edge
      owningBlock()->reIndexTopology();
      return;
    }

    topo_position_ = nextPos - kAppendInterval;

    // insert between two existing nodes
  } else {
    const auto posBetween = prevPos + (nextPos - prevPos) / 2;
    if (posBetween == prevPos) {
      // There was no room
      owningBlock()->reIndexTopology();
      return;
    }
    topo_position_ = posBetween;
  }
}

Node::Node(Graph * graph_, NodeKind kind_) :
  kind_(kind_),
  graph_(graph_),
  owning_block_(nullptr),
  scope_(graph_->current_scope_),
  schema_(nullptr),
  topo_position_(0) {
  graph_->all_nodes.emplace(this);
}

void Node::eraseOutput(size_t i) {
  JIT_ASSERT(i < outputs_.size());
  JIT_ASSERT(outputs_[i]->uses().empty());
  schema_ = nullptr;
  Value * n = outputs_[i];
  outputs_.erase(outputs_.begin() + i);
  owningGraph()->freeValue(n);
  for(size_t j = i; j < outputs_.size(); j++) {
    outputs_[j]->offset_--;
  }
}

Block * Node::addBlock() {
  schema_ = nullptr;
  blocks_.push_back(new Block(owningGraph(), this));
  return blocks_.back();
}

void Node::eraseBlock(size_t i) {
  JIT_ASSERT(i < blocks_.size());
  schema_ = nullptr;
  Block * n = blocks_[i];
  blocks_.erase(blocks_.begin() + i);
  n->destroy();
}

void Node::destroy() {
  while(!outputs().empty())
    eraseOutput(outputs().size() - 1);
  while(!blocks().empty())
    eraseBlock(blocks().size() - 1);
  removeAllInputs();
  if(inBlockList())
    removeFromList();
  graph_->freeNode(this);
}

void Node::cloneFrom(Node * s) {
  setSourceLocation(s->getSourceLocation());
  if(s->scope_ && !s->scope_->isBlank())
    scope_ = s->scope_;
  copyAttributes(*s);
}

void Node::replaceAllUsesWith(Node * n) {
  JIT_ASSERT(outputs().size() == n->outputs().size());
  size_t nOutputs = outputs().size();
  for(size_t i = 0; i < nOutputs; i++) {
    outputs()[i]->replaceAllUsesWith(n->outputs()[i]);
  }
}

Value* Node::insertInput(size_t i, Value* value) {
  JIT_ASSERT(graph_ == value->owningGraph());
  schema_ = nullptr;
  // First we update the offsets for all existing inputs that will reside
  // after the one we're inserting. Concretely, these are the inputs at
  // indices [i, # input). Since we're inserting one input before all of
  // these inputs, increment their use offsets for this value by 1
  for (size_t use_itr = i; use_itr < inputs_.size(); ++use_itr) {
    // See Note [User node does not uniquely identify use]
    auto use = findUseForInput(use_itr);
    use->offset += 1;
  }
  // Insert the actual input at the specified index
  inputs_.insert(inputs_.begin() + i, value);
  // Register the new use of the value we're inserted as an input.
  value->uses_.emplace_back(this, i);
  return value;
}

Value* Node::addInput(Value * value) {
  JIT_ASSERT(graph_ == value->owningGraph());
  schema_ = nullptr;
  value->uses_.emplace_back(this, inputs_.size());
  inputs_.push_back(value);
  return value;
}

Value* Node::replaceInput(size_t i, Value * newValue) {
  JIT_ASSERT(newValue->owningGraph() == graph_);
  schema_ = nullptr;
  Value * old = dropInput(i);
  inputs_[i] = newValue;
  newValue->uses_.emplace_back(this, i);
  return old;
}

void Node::replaceInputWith(Value * from, Value * to) {
  JIT_ASSERT(from->owningGraph() == graph_);
  JIT_ASSERT(to->owningGraph() == graph_);
  schema_ = nullptr;
  size_t i = 0;
  for(auto input : inputs()) {
    if(input == from)
      replaceInput(i, to);
    i++;
  }
}

Value* Node::addOutput() {
  outputs_.push_back(new Value(this, outputs_.size()));
  schema_ = nullptr;
  return outputs_.back();
}

Value* Node::insertOutput(size_t i) {
  schema_ = nullptr;
  outputs_.insert(outputs_.begin() + i, new Value(this, i));
  for (size_t itr = i + 1; itr < outputs_.size(); ++itr) {
    outputs_[itr]->setOffset(outputs_[itr]->offset() + 1);
  }
  return outputs_.at(i);
}

bool Node::isBefore(const Node * n) const {
  if (this == n) {
    return false;
  }
  return !isAfter(n);
}

bool Node::isAfter(const Node * n) const {
  JIT_ASSERT(this->owningGraph() == n->owningGraph());

  if (this->owningBlock() == n->owningBlock()) {
    return this->topo_position_ > n->topo_position_;
  }

  // These nodes don't share a common block. Traverse the blockchains upward
  // until we find the first common block.
  auto lhs = this;
  while (lhs) {
    JIT_ASSERT(lhs->owningBlock());

    auto rhs = n;
    while (rhs) {
      JIT_ASSERT(rhs->owningBlock());

      if (lhs->owningBlock() == rhs->owningBlock()) {
        return lhs->isAfter(rhs);
      }
      rhs = rhs->owningBlock()->owningNode();
    }

    lhs = lhs->owningBlock()->owningNode();
  }
  // should never reach here, since both nodes are ultimately in the same graph
  JIT_ASSERT(false);
}

Node* Node::insertBefore(Node * n) {
  JIT_ASSERT(n->inBlockList());
  insertAfter(n->prev());
  return this;
}

Node* Node::insertAfter(Node * n) {
  JIT_ASSERT(!inBlockList() && n->inBlockList());
  JIT_ASSERT(n->owningBlock());
  this->owning_block_ = n->owningBlock();
  Node * next = n->next();
  n->next() = this;
  this->prev() = n;
  this->next() = next;
  next->prev() = this;
  assignTopoPosition();
  return this;
}

bool Node::moveAfterTopologicallyValid(Node* n) {
  return tryMove(n, MoveSide::AFTER);
}

bool Node::moveBeforeTopologicallyValid(Node* n) {
  // We have to distinguish the move side (instead of just moving after
  // n->prev()). Consider the following example:
  //   If the dependency graph looks like this -> n -> o then moveBefore(o) will
  //   end up with [this, o, n], but moveAfter(n) will return false.
  return tryMove(n, MoveSide::BEFORE);
}

// Helper for topologically-safe node moves. See `tryMove()` for details.
namespace {
struct WorkingSet {
 public:
  explicit WorkingSet(Node* mover) {
    add(mover);
  }

  // Add `n` to the working set
  void add(Node* n) {
    nodes_.push_back(n);
    for (const auto user : getUsersSameBlock(n)) {
      users_[user]++;
    }
  }

  void eraseMover() {
    auto mover = nodes_.front();
    for (const auto user : getUsersSameBlock(mover)) {
      // If this user node only uses the mover, we can remove it
      if (users_[user] == 1) {
        users_.erase(user);
      }
    }
    nodes_.pop_front();
  }

  const std::list<Node*>& nodes() {
    return nodes_;
  }

  // Does the working set depend on `n`?
  bool dependsOn(Node* n) const {
    if (nodes_.empty()) {
      return false;
    }

    if (n->isAfter(nodes_.front())) {
      return producesFor(n);
    } else {
      return consumesFrom(n);
    }
  }

  // Does the working set produce any values consumed by `n`?
  bool producesFor(Node* n) const {
    // This equivalent to asking: does the total use-set of all the nodes in the
    // working set include `n`?
    return users_.count(n) != 0;
  }

  // Does the working set consume any values produced by `n`?
  bool consumesFrom(Node* n) const {
    const auto users = getUsersSameBlock(n);
    return std::any_of(nodes_.begin(), nodes_.end(), [&](Node* node) {
      return users.count(node) != 0;
    });
  }

 private:
  // Get all users of outputs of `n`, in the same block as `n`.
  // This means if there is an `if` node that uses an output of `n` in some
  // inner sub-block, we will consider the whole `if` node a user of `n`.
  std::unordered_set<Node*> getUsersSameBlock(Node* n) const {
    std::unordered_set<Node*> users;
    for (const auto output : n->outputs()) {
      for (const auto& use : output->uses()) {
        if (use.user->owningBlock() == n->owningBlock()) {
          users.insert(use.user);
        } else {
          // This user is in a sub-block. Traverse the blockchain upward until
          // we arrive at a node that shares a block with `this`
          auto curNode = use.user;
          while (curNode->owningBlock() != n->owningBlock()) {
            curNode = curNode->owningBlock()->owningNode();
            JIT_ASSERT(curNode);
          }
          users.insert(curNode);
        }
      }
    }

    return users;
  }

  std::list<Node*> nodes_;
  // users => # of working set nodes it uses
  std::unordered_map<Node*, size_t> users_;
};
} // namespace

// Try to move `this` before/after `movePoint` while preserving value
// dependencies. Returns false iff such a move could not be made
//
// The basic approach is: have a "working set" that we are moving forward, one
// node at a time. When we can't move past a node (because it depends on the
// working set), then add it to the working set and keep moving until we hit
// `moveAfter`.
bool Node::tryMove(Node* movePoint, MoveSide moveSide) {
  JIT_ASSERT(this->inBlockList() && movePoint->inBlockList());
  JIT_ASSERT(this->owningBlock() == movePoint->owningBlock());
  if (this == movePoint) {
    return true;
  }

  // 1. Move from `this` toward movePoint, building up the working set of
  // dependencies
  WorkingSet workingSet(this);

  int direction;
  if (this->isAfter(movePoint)) {
    direction = kPrevDirection;
  } else {
    direction = kNextDirection;
  }

  auto curNode = this->next_in_graph[direction];
  // Move forward one node at a time
  while (curNode != movePoint) {
    if (workingSet.dependsOn(curNode)) {
      // If we can't move past this node, add it to the working set
      workingSet.add(curNode);
    }
    curNode = curNode->next_in_graph[direction];
  }

  // 2. Decide whether we can move it all to `movePoint`.

  // Say we are moving directly before movePoint and `this` starts before
  // movePoint in the graph. The move looks like
  //
  //  `this`              `this`           |
  //  <dependencies>  ->  `movePoint`      | `this` and deps are split
  //  `movePoint`         <dependencies>   |
  //
  // Contrast with the case where `this` starts AFTER movePoint:
  //
  //  `movePoint`         <dependencies>   |
  //  <dependencies>  ->  `this`           | `this` and deps are together
  //  `this`              `movePoint`      |
  //
  // In the first case, we need to split `this` off from its dependencies, so we
  // can move the dependencies below `movePoint` and keep `this` above.
  const bool splitThisAndDeps =
      (moveSide == MoveSide::BEFORE && this->isBefore(movePoint)) ||
      (moveSide == MoveSide::AFTER && this->isAfter(movePoint));

  if (splitThisAndDeps) {
    // remove `this` from dependencies to be moved past `movePoint`
    workingSet.eraseMover();
  }

  // Check if we can move the working set past the move point
  if (workingSet.dependsOn(movePoint)) {
    // if we can't, then there are intermediate dependencies between the
    // `this` and `movePoint`, so we can't do the move
    return false;
  }

  // 3. Execute the move
  JIT_ASSERT(curNode == movePoint);
  if (splitThisAndDeps) {
    // Move `this`
    this->move(movePoint, moveSide);

    // Then move all of its dependencies on the other side of `movePoint`
    const auto reversed =
        moveSide == MoveSide::BEFORE ? MoveSide::AFTER : MoveSide::BEFORE;
    for (auto toMove : workingSet.nodes()) {
      toMove->move(curNode, reversed);
      curNode = toMove;
    }
  } else {
    // Just append/prepend everything to `movePoint`
    for (auto toMove : workingSet.nodes()) {
      toMove->move(curNode, moveSide);
      curNode = toMove;
    }
  }
  return true;
}

// Helper function so we can generalize `tryMove`
void Node::move(Node* movePoint, MoveSide moveSide) {
  switch (moveSide) {
    case MoveSide::BEFORE:
      this->moveBefore(movePoint);
      break;
    case MoveSide::AFTER:
      this->moveAfter(movePoint);
      break;
  }
}

void Node::moveAfter(Node * n) {
  removeFromList();
  insertAfter(n);
}

void Node::moveBefore(Node * n) {
  removeFromList();
  insertBefore(n);
}

void Node::removeInput(size_t i) {
  schema_ = nullptr;
  dropInput(i);
  // everything after this input shifts left,
  // so we need to update their use offsets to match
  for(size_t j = i+1; j < inputs_.size(); j++) {
    auto it = findUseForInput(j);
    it->offset--;
  }
  inputs_.erase(inputs_.begin() + i);
}

void Node::removeAllInputs() {
  schema_ = nullptr;
  for(size_t i = 0; i < inputs().size(); ++i)
    dropInput(i);
  inputs_.clear();
}

use_list::iterator Node::findUseForInput(size_t i) {
  auto & input_uses = inputs_[i]->uses_;
  // O(N) on the use list, but unless we get nodes with +100 uses
  // vector traversal still is probably faster than linked list
  auto use_it = std::find(input_uses.begin(), input_uses.end(), Use(this, i));
  JIT_ASSERT(use_it != input_uses.end());
  return use_it;
}

Value* Node::dropInput(size_t i) {
  JIT_ASSERT(i < inputs_.size());
  auto input_node = inputs_[i];
  auto use_it = findUseForInput(i);
  input_node->uses_.erase(use_it);
  inputs_[i] = nullptr;
  return input_node;
}

void Node::removeFromList() {
  JIT_ASSERT(inBlockList());
  this->owning_block_ = nullptr;
  Node * next = this->next();
  Node * prev = this->prev();
  prev->next() = next;
  next->prev() = prev;
  this->next() = nullptr;
  this->prev() = nullptr;
}

inline const SourceRange& fakeRange() {
  static SourceRange range(std::make_shared<std::string>("<internally-created-node>"), 0, 1);
  return range;
}

Value* Graph::insert(
    Symbol opname,
    at::ArrayRef<NamedValue> args,
    at::ArrayRef<NamedValue> kwargs,
    c10::optional<SourceRange> range) {
  return script::emitBuiltinCall(
      range.value_or(fakeRange()),
      *this,
      opname,
      c10::nullopt,
      args,
      kwargs,
      /*required=*/true);
}

Node* Graph::create(NodeKind kind, size_t num_outputs) {
  // NB: Node constructor adds node to all_nodes
  auto n = new Node(this, kind);
  for(size_t i = 0; i < num_outputs; i++)
    n->addOutput();
  return n;
}

Node* Graph::create(NodeKind kind, ArrayRef<Value*> inputs, size_t num_outputs) {
  auto n = create(kind, num_outputs);
  for(auto i : inputs)
    n->addInput(i);
  return n;
}

Node* Graph::createUndefined() {
  return create(prim::Undefined);
}

Node* Graph::createNone(TypePtr typ) {
  Node * n = create(prim::None);
  n->output()->setType(OptionalType::create(typ));
  return n;
}

Node * Graph::createNoneGenerator() {
  auto n = create(prim::NoneGenerator);
  n->output()->setType(GeneratorType::get());
  return n;
}

Node * Graph::createFusionGroup() {
  auto n = create(prim::FusionGroup, 0);
  n->g_(attr::Subgraph,std::make_shared<Graph>(current_scope()));
  return n;
}

Node* Graph::createTuple(at::ArrayRef<Value*> values) {
  auto types = fmap(values, [](Value* v) { return v->type(); });
  auto tt = TupleType::create(std::move(types));
  auto n = create(prim::TupleConstruct, values);
  n->output()->setType(tt);
  return n;
}

Node* Graph::createTupleUnpack(Value * v) {
  TupleTypePtr tt = v->type()->expect<TupleType>();
  auto n = create(prim::TupleUnpack, {v}, 0);
  for(auto & element : tt->elements()) {
    n->addOutput()->setType(element);
  }
  return n;
}

Node* Graph::createTupleIndex(Value * tup, int64_t index) {
  auto n = create(prim::TupleIndex, {tup});
  n->i_(attr::index, index);
  auto tuple_type = tup->type()->expect<TupleType>();
  n->output()->setType(tuple_type->elements().at(index));
  return n;
}

Node* Graph::createTupleSlice(Value * tup, int64_t beg, int64_t end) {
  auto n = create(prim::TupleSlice, {tup});
  auto tuple_type = tup->type()->expect<TupleType>();
  n->i_(attr::beg, beg);
  n->i_(attr::end, end);
  std::vector<TypePtr> output_types;
  for (auto i = beg; i < end; ++i) {
    output_types.push_back(tuple_type->elements().at(i));
  }
  auto tt = TupleType::create(std::move(output_types));
  n->output()->setType(tt);
  return n;
}

Node* Graph::createList(const TypePtr& elem_type, at::ArrayRef<Value*> values) {
  auto n = create(prim::ListConstruct, values);
  for(const auto & v : values) {
    JIT_ASSERT(v->type()->isSubtypeOf(elem_type));
  }
  n->output()->setType(ListType::create(elem_type));
  return n;
}
Node* Graph::createListUnpack(Value *v, size_t size) {
  ListTypePtr list_type = v->type()->expect<ListType>();
  TypePtr elem_type = list_type->getElementType();
  auto n = create(prim::ListUnpack, {v}, 0);
  for (size_t i = 0; i < size; ++i) {
    n->addOutput()->setType(elem_type);
  }
  return n;
}

Node* Graph::createNumToTensor(Value* value) {
  auto typ = value->type();
  Node * result = create(prim::NumToTensor, {value});
  result->output()->setType(CompleteTensorType::fromNumberType(typ));
  return result;
}

Node* Graph::createBoolToTensor(Value* value) {
  auto typ = value->type();
  Node * result = create(prim::BoolToTensor, {value});
  if (!typ->isSubtypeOf(BoolType::get())) {
    AT_ERROR("Cannot create bool type from ", typ->str());
  }
  result->output()->setType(CompleteTensorType::fromBoolType());
  return result;
}
Node* Graph::createTensorToNum(const TypePtr& type, Value* value) {
  auto* result = create(prim::TensorToNum, {value});
  result->output()->setType(type);
  return result;
}

Node* Graph::createImplicitTensorToNum(const TypePtr& type, Value* value) {
  auto* result = create(prim::ImplicitTensorToNum, {value});
  result->output()->setType(type);
  return result;
}

Node* Graph::createTensorToBool(Value* value) {
  auto* result = create(prim::TensorToBool, {value});
  result->output()->setType(BoolType::get());
  return result;
}

Node* Graph::createIntToFloat(Value* value) {
  JIT_ASSERT(*value->type() == *IntType::get());
  auto* result = create(prim::IntToFloat, {value});
  result->output()->setType(FloatType::get());
  return result;
}

Node* Graph::createFloatToInt(Value* value) {
  JIT_ASSERT(*value->type() == *FloatType::get());
  auto* result = create(prim::FloatToInt, {value});
  result->output()->setType(IntType::get());
  return result;
}

Node* Graph::createStringToFloat(Value* value) {
  JIT_ASSERT(*value->type() == *StringType::get());
  auto* result = create(prim::StringToFloat, {value});
  result->output()->setType(FloatType::get());
  return result;
}

Node* Graph::createClone(Node * n, std::function<Value*(Value*)> value_map, bool copy_blocks) {
  //n can be from a different graph
  Node * r = n->allocNewInstance(this);
  for(auto o : n->outputs()) {
    r->addOutput()->copyMetadata(o);
  }
  r->cloneFrom(n);
  for(auto i : n->inputs()) {
    r->addInput(value_map(i));
  }
  if(copy_blocks) {
    for(auto b : n->blocks()) {
      r->addBlock()->cloneFrom(b, value_map);
    }
  }
  return r;
}

Value* Graph::insertConstant(
    IValue val,
    c10::optional<SourceRange> loc,
    c10::optional<ScopePtr> scope) {
  return jit::insertConstant(*this, std::move(val), loc, scope);
}

std::string Graph::toString() const {
  std::ostringstream oss;
  oss << *this;
  return oss.str();
}

Graph::~Graph() {
  for (const Node * n : all_nodes)
    delete n;
  for (const Value * v : all_values)
    delete v;
  for (const Block * b : all_blocks)
    delete b;
}

void Graph::freeNode(Node * n) {
  auto it = all_nodes.find(n);
  JIT_ASSERT(it != all_nodes.end());
  delete *it;
  all_nodes.erase(it);
}
void Graph::freeValue(Value * v) {
  v->setUniqueName("");
  auto it = all_values.find(v);
  JIT_ASSERT(it != all_values.end());
  delete *it;
  all_values.erase(it);
}
void Graph::freeBlock(Block * b) {
  auto it = all_blocks.find(b);
  JIT_ASSERT(it != all_blocks.end());
  delete *it;
  all_blocks.erase(it);
}

PythonOp* defaultAllocPythonOp(Graph*g) {
  throw std::runtime_error("Trying to allocate a Python object without python bindings loaded");
}
std::atomic<decltype(&defaultAllocPythonOp)> alloc_python_op;

// patched in when python bindings are loaded
PythonOp* allocPythonOp(Graph* g) {
  return alloc_python_op.load()(g);
}
void setAllocPythonOp(PythonOp* (*v)(Graph* g)) {
  alloc_python_op.store(v);
}

}} // namespace torch::jit
