// Copyright Doug Moen 2016-2017.
// Distributed under The MIT License.
// See accompanying file LICENCE.md or https://opensource.org/licenses/MIT

#include <curv/module.h>
#include <curv/function.h>

namespace curv {

const char Module_Base::name[] = "module";

void
Module_Base::print(std::ostream& out) const
{
    out << "{";
    bool first = true;
    for (auto i : *this) {
        if (!first) out << ",";
        first = false;
        out << i.first << "=";
        i.second.print(out);
    }
    out << "}";
}

void
Module_Base::putfields(Atom_Map<Value>& out) const
{
    for (auto i : *this)
        out[i.first] = i.second;
}

Value
Module_Base::get(slot_t i) const
{
    Value val = array_[i];
    if (val.is_ref()) {
        auto& ref = val.get_ref_unsafe();
        if (ref.type_ == Ref_Value::ty_lambda)
            return {make<Closure>((Lambda&)ref, *(Module*)this)};
    }
    return val;
}

Value
Module_Base::getfield(Atom name, const Context& cx) const
{
    auto b = dictionary_->find(name);
    if (b != dictionary_->end())
        return get(b->second);
    return Structure::getfield(name, cx);
}

bool
Module_Base::hasfield(Atom name) const
{
    auto b = dictionary_->find(name);
    return (b != dictionary_->end());
}

Shared<List>
Module_Base::fields() const
{
    auto list = List::make(dictionary_->size());
    int i = 0;
    for (auto f : *dictionary_) {
        list->at(i) = f.first.to_value();
        ++i;
    }
    return {std::move(list)};
}

} // namespace curv
