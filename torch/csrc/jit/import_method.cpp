#include "torch/csrc/jit/import_method.h"
#include "torch/csrc/jit/script/parser.h"

namespace torch { namespace jit {


// this is a much simpler accessor that only handles modules, parameters, and
// and methods. It does not depend on python to work.
struct ModuleAccessorValue : public script::SugaredValue {
  ModuleAccessorValue(std::shared_ptr<script::Module> module)
  : module(std::move(module)) {}
  std::string kind() const override {
    return "module";
  }
  // select an attribute on it, e.g. `this.field`
  std::shared_ptr<SugaredValue> attr(SourceRange loc, script::Method & m, const std::string& field) override {
    if(script::NamedModule* v = module->find_module(field)) {
      return std::make_shared<ModuleAccessorValue>(v->module);
    } else if(script::NamedParameter* v = module->find_parameter(field)) {
      return std::make_shared<script::SimpleValue>(m.get_or_add_parameter(v->slot()));
    } else if(script::Method* m = module->find_method(field)) {
      return std::make_shared<script::MethodValue>(module, *m);
    }
    return script::SugaredValue::attr(loc, m, field);
  }
private:
  std::shared_ptr<script::Module> module;
};

// This value maps attributes CONSTANTS.c0 CONSTANTS.c1 to entries
// in the 'constants' vector. This table is will be stored in a container format
// and given to the import_method when restoring the code.
struct ConstantTableValue : public script::SugaredValue {
  ConstantTableValue(ArrayRef<at::Tensor> constants)
  : constants_(constants) {}
  std::string kind() const override {
    return "CONSTANTS";
  }
  // select an attribute on it, e.g. `this.field`
  std::shared_ptr<SugaredValue> attr(SourceRange loc, script::Method & m, const std::string& field) override {
    const char* field_s = field.c_str();
    char* end;
    int64_t offset = std::strtoll(field_s + 1, &end, 10);
    if(field.size() < 2 || *end != 0)
      throw script::ErrorReport(loc) << "invalid constant specifier: " << field;
    if (offset < 0 || size_t(offset) >= constants_.size()) {
      throw script::ErrorReport(loc) << "constant index " << offset
                                     << " is out of bounds (constant table has "
                                     << constants_.size() << " entries).";
    }
    Value* value = m.graph()->insertConstant(constants_[offset], loc);
    return std::make_shared<script::SimpleValue>(value);
  }

 private:
   ArrayRef<at::Tensor> constants_;
};

static size_t parseVersionNumber(script::Lexer& L) {
  auto range = L.cur().range;
  auto name = L.expect(script::TK_IDENT).text();
  L.expect('=');
  std::string version_text = L.expect(script::TK_NUMBER).text();
  L.expect(script::TK_NEWLINE);
  auto version = script::Const::create(L.cur().range, version_text);
  if (name != "op_version_set")
    throw script::ErrorReport(range) << "expected an assignment to op_version_set";
  if (!version.isIntegral())
    throw script::ErrorReport(range) << "expected an integral version but found " << version.text();
   return size_t(version.asIntegral());
}

void import_method(const std::shared_ptr<script::Module>& mod, const std::string& src, const std::vector<at::Tensor>& constant_table) {
  script::Parser p(src);

  size_t version = parseVersionNumber(p.lexer());
  auto aten = std::make_shared<script::BuiltinModule>("aten", version);
  auto prim = std::make_shared<script::BuiltinModule>("prim", version);
  auto constants = std::make_shared<ConstantTableValue>(constant_table);
  auto fork = std::make_shared<script::ForkValue>();
  auto annotate = std::make_shared<script::AnnotateValue>();

  auto resolver = [&](const std::string& name, script::Method& m, const SourceRange& loc)
  -> std::shared_ptr<script::SugaredValue> {
    if(name == "aten")
      return aten;
    if (name == "prim")
      return prim;
    if (name == "CONSTANTS")
      return constants;
    if (name == "fork")
      return fork;
    if (name == "annotate")
      return annotate;
    if (name == "inf") {
      return std::make_shared<script::SimpleValue>(
          m.graph()->insertConstant(std::numeric_limits<float>::infinity()));
    }
    return nullptr;
  };

  std::vector<script::Def> definitions;
  std::vector<script::Resolver> resolvers;

  while (p.lexer().cur().kind != script::TK_EOF) {
    auto def = script::Def(p.parseFunction(/*is_method=*/true));
    definitions.emplace_back(def);
    resolvers.emplace_back(resolver);
  }
  auto self = std::make_shared<ModuleAccessorValue>(mod);
  script::defineMethodsInModule(mod, definitions, resolvers, self);
}

}}
