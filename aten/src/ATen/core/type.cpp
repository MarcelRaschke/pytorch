#include <ATen/core/jit_type.h>

#include <iostream>

namespace c10 {

std::ostream& operator<<(std::ostream & out, const Type & t) {
  if(auto value = t.cast<CompleteTensorType>()) {
    out << at::toString(value->scalarType()) << "(";
    auto& sizes = value->sizes();
    auto& strides = value->strides();
    AT_ASSERT(sizes.size() == strides.size());
    for (size_t i = 0; i < sizes.size(); i++) {
      if (i > 0) {
        out << ", ";
      }
      // TODO: figure out a good way to output strides, or
      // add a "debug" printing mode which adds the extra stuff
      out << sizes[i]; // << "%" << strides[i];
      int64_t expected = i + 1 < sizes.size() ? sizes[i+1]*strides[i+1] : 1;
      if (strides[i] != expected) {
        out << "!"; //mark non-contiguous
      }
    }
    out << ")";
  } else if (auto value = t.cast<TensorType>()) {
    out << at::toString(value->scalarType()) << "(";
    for (int i = 0; i < value->dim(); ++i) {
      if (i > 0) {
        out << ", ";
      }
      out << "*";
    }
    out << ")";
  } else if(t.kind() == TypeKind::DynamicType) {
    out << "Dynamic";
  } else if(t.kind() == TypeKind::UndefinedTensorType) {
    out << "Undefined";
  } else if(t.kind() == TypeKind::TupleType) {
    out << "Tuple";
  } else if(t.kind() == TypeKind::NumberType) {
    out << "Number";
  } else if(t.kind() == TypeKind::FloatType) {
    out << "float";
  } else if(t.kind() == TypeKind::IntType) {
    out << "int";
  } else if(t.kind() == TypeKind::BoolType) {
    out << "bool";
  } else if(t.kind() == TypeKind::ListType) {
    auto prim = t.cast<ListType>()->getElementType();
    out << *prim << "[]";
  } else if (t.kind() == TypeKind::OptionalType) {
    auto prim = t.cast<OptionalType>()->getElementType();
    out << *prim << "?";
  } else if(t.kind() == TypeKind::NoneType) {
    out << "None";
  } else if(t.kind() == TypeKind::StringType) {
    out << "string";
  } else if(t.kind() == TypeKind::GeneratorType) {
    out << "Generator";
  } else if(t.kind() == TypeKind::VarType) {
    out << t.expect<VarType>()->name();
  } else if(t.kind() == TypeKind::FutureType) {
    auto elem = t.cast<FutureType>()->getElementType();
    out << "Future[" << *elem << "]";
  } else {
    AT_ERROR("unknown type kind");
  }
  return out;
}

DynamicTypePtr DynamicType::get() {
  static auto value = DynamicType::create();
  return value;
}
UndefinedTensorTypePtr UndefinedTensorType::get() {
  static auto value = UndefinedTensorType::create();
  return value;
}
NumberTypePtr NumberType::get() {
  static auto value = NumberType::create();
  return value;
}
IntTypePtr IntType::get() {
  static auto value = IntType::create();
  return value;
}
FloatTypePtr FloatType::get() {
  static auto value = FloatType::create();
  return value;
}
BoolTypePtr BoolType::get() {
  static auto value = BoolType::create();
  return value;
}
NoneTypePtr NoneType::get() {
  static auto value = NoneType::create();
  return value;
}
GeneratorTypePtr GeneratorType::get() {
  static auto value = GeneratorType::create();
  return value;
}
StringTypePtr StringType::get() {
  static auto value = StringType::create();
  return value;
}
OptionalTypePtr OptionalType::ofTensor() {
  static auto value = OptionalType::create(DynamicType::get());
  return value;
}
ListTypePtr ListType::ofTensors() {
  static auto value = ListType::create(DynamicType::get());
  return value;
}
ListTypePtr ListType::ofInts() {
  static auto value = ListType::create(IntType::get());
  return value;
}
ListTypePtr ListType::ofFloats() {
  static auto value = ListType::create(FloatType::get());
  return value;
}
ListTypePtr ListType::ofBools() {
  static auto value = ListType::create(BoolType::get());
  return value;
}

TypePtr inferTypeFrom(const IValue& value) {
  if (value.isTensor()) {
    return CompleteTensorType::create(value.toTensor());
  } else if (value.isDouble()) {
    return FloatType::get();
  } else if (value.isInt()) {
    return IntType::get();
  } else if (value.isBool()) {
    return BoolType::get();
  } else if (value.isString()) {
    return StringType::get();
  } else if (value.isIntList()) {
    return ListType::ofInts();
  } else if (value.isTensorList()) {
    return ListType::ofTensors();
  } else if (value.isBoolList()) {
    return ListType::ofBools();
  } else if (value.isDoubleList()) {
    return ListType::ofFloats();
  } else if (value.isTuple()) {
    return TupleType::create(fmap(value.toTuple()->elements(), inferTypeFrom));
  }
  AT_ASSERTM(false, "Unhandled IValue kind in inferTypeFrom");
}

c10::optional<TypePtr> unifyTypes(const TypePtr& t1, const TypePtr& t2) {
  //cases that t1 == t2, or t1 is a type refinement of t2 and vice versa
  if (t1->isSubtypeOf(t2)) {
    return t2;
  } else if (t2->isSubtypeOf(t1)) {
    return t1;
  }

  // NB: we do not return NumberType because there is not currently enough
  // operator support for it

  if (t1->isSubtypeOf(DynamicType::get()) && t2->isSubtypeOf(DynamicType::get())) {
    return static_cast<TypePtr>(DynamicType::get());;
  }

  // if t1 is None and t2 is a concrete type, return Optional[t2] and vice versa
  if (t1->isSubtypeOf(NoneType::get()) && !t2->isSubtypeOf(NoneType::get())) {
    return OptionalType::create(t2);
  } else if (t2->isSubtypeOf(NoneType::get()) && !t1->isSubtypeOf(NoneType::get())) {
    return OptionalType::create(t1);
  }

  //types which contain other types
  if (t1->cast<ListType>() && t2->cast<ListType>()) {
    auto unified_type = unifyTypes(t1->cast<ListType>()->getElementType(), t2->cast<ListType>()->getElementType());
    if (unified_type) {
      return static_cast<TypePtr>(ListType::create(*unified_type));
    } else {
      return c10::nullopt;
    }
  } else if(t1->cast<TupleType>() && t2->cast<TupleType>()) {
    auto tuple1 = t1->cast<TupleType>();
    auto tuple2 = t2->cast<TupleType>();
    if (tuple1->elements().size() != tuple2->elements().size()) {
      return c10::nullopt;
    }
    std::vector<TypePtr> elements;
    for (size_t i = 0; i < tuple1->elements().size(); i++) {
      if (auto elem = unifyTypes(tuple1->elements().at(i), tuple2->elements().at(i))) {
        elements.push_back(*elem);
      } else {
        return c10::nullopt;
      }
    }
    return static_cast<TypePtr>(TupleType::create(elements));
  }

  return c10::nullopt;
}

MatchTypeReturn matchTypeVariables(TypePtr formal, TypePtr actual, TypeEnv& type_env) {
  MatchTypeReturn ret;
  if(!formal->hasFreeVariables()) {
    ret.type = formal;
    return ret;
  }

  if(auto vt = formal->cast<VarType>()) {
    auto it = type_env.find(vt->name());
    if(it == type_env.end()) {
      type_env[vt->name()] = actual;
      ret.type = actual;
      return ret;
    } else if(auto unified = unifyTypes(it->second, actual)) {
      type_env[vt->name()] = *unified;
      ret.type = *unified;
      return ret;
    }
    std::stringstream ss;
    ss << "type variable '" << vt->name() <<"' previously matched to type " <<
      it->second->str() << " is matched to type " << actual->str();
    ret.errMsg = ss.str();
    return ret;
  } else if(auto lt_formal = formal->cast<ListType>()) {
    if(auto lt_actual = actual->cast<ListType>()) {
      const auto innerType = matchTypeVariables(
          lt_formal->getElementType(),
          lt_actual->getElementType(),
          type_env);
      if (!innerType.type) {
        // propagate the errMsg onward
        return innerType;
      }
      ret.type = ListType::create(*innerType.type);
      return ret;
    } else {
      std::stringstream ss;
      ss << "cannot match a list to " << actual->str();
      ret.errMsg = ss.str();
      return ret;
    }
  } else if(auto tp_formal = formal->cast<TupleType>()) {
    if(auto tp_actual = actual->cast<TupleType>()) {
      if(tp_formal->elements().size() != tp_actual->elements().size()) {
        ret.errMsg = "cannot match tuples of mismatched size";
        return ret;
      }
      std::vector<TypePtr> elements;
      for(size_t i = 0; i < tp_formal->elements().size(); ++i) {
        const auto result = matchTypeVariables(
            tp_formal->elements()[i],
            tp_actual->elements()[i],
            type_env);
        if (!result.type) {
          return result;
        }
        elements.push_back(*result.type);
      }
      ret.type = TupleType::create(std::move(elements));
      return ret;
    } else {
      std::stringstream ss;
      ss << "cannot match a tuple to " << actual->str();
      ret.errMsg = ss.str();
      return ret;
    }
  } else if (auto lt_formal = formal->cast<FutureType>()) {
    if (auto lt_actual = actual->cast<FutureType>()) {
      const auto innerType = matchTypeVariables(
          lt_formal->getElementType(), lt_actual->getElementType(), type_env);
      if (!innerType.type) {
        return innerType;
      }
      ret.type = FutureType::create(*innerType.type);
      return ret;
    } else {
      std::stringstream ss;
      ss << "cannot match a future to " << actual->str();
      ret.errMsg = ss.str();
      return ret;
    }
  } else if (auto opt_formal = formal->cast<OptionalType>()) {
    if (auto opt_actual = actual->cast<OptionalType>()) {
      const auto optionedType = matchTypeVariables(
          opt_formal->getElementType(), opt_actual->getElementType(), type_env);
      if (!optionedType.type) {
        return optionedType;
      }
      ret.type = OptionalType::create(*optionedType.type);
      return ret;
    } else if (!actual->isSubtypeOf(NoneType::get())) {
      // If the actual type is a non-optional, allow matching to the formal if
      // its element type matches the actual.
      // Don't match None because it is already an optional (but one of
      // unknown type).
      return matchTypeVariables(opt_formal->getElementType(), actual, type_env);
    } else {
      ret.errMsg = "cannot match an Optional[T] to None, because there is no way to determine T from None.";
      return ret;
    }
  }

  AT_ERROR("unhandled free variable container: ", formal->str());
}

// change return types like List[List[t]] into List[List[int]]
CAFFE2_API TypePtr evalTypeVariables(TypePtr type, std::unordered_map<std::string, TypePtr>& type_env) {
  if(!type->hasFreeVariables())
    return type;

  if(auto vt = type->cast<VarType>()) {
    auto it = type_env.find(vt->name());
    AT_ASSERTM(it != type_env.end(), "schema has unbound type variable '", vt->name(), "' in its return type");
    return it->second;
  } else {
    auto new_contained = fmap(type->containedTypes(), [&](TypePtr t) {
      return evalTypeVariables(t, type_env);
    });
    return type->withContained(std::move(new_contained));
  }
}

} // namespace c10
