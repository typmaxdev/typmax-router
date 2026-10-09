// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Minimal JSON value, parser and serializer for the typmax-router protocol.
// Objects keep insertion order so the serialized bytes are a pure function of
// the order the program builds them in (the determinism contract, PROTOCOL.md).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tj {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Value {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(int i) : type_(Type::Int), i_(i) {}
    Value(int64_t i) : type_(Type::Int), i_(i) {}
    Value(double d) : type_(Type::Double), d_(d) {}
    Value(const char* s) : type_(Type::String), s_(std::make_shared<std::string>(s)) {}
    Value(std::string s) : type_(Type::String), s_(std::make_shared<std::string>(std::move(s))) {}
    Value(Array a) : type_(Type::Array), a_(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : type_(Type::Object), o_(std::make_shared<Object>(std::move(o))) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_int() const { return type_ == Type::Int; }
    bool is_number() const { return type_ == Type::Int || type_ == Type::Double; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const;
    int64_t as_int() const;      // exact integers only
    double as_double() const;    // any number
    const std::string& as_string() const;
    const Array& as_array() const;
    const Object& as_object() const;
    Array& mut_array();
    Object& mut_object();

    // Object lookup: nullptr when absent (or when this is not an object).
    const Value* find(const std::string& key) const;
    // Object append (no duplicate check; callers build fresh objects).
    Value& set(const std::string& key, Value v);
    // Array append.
    void push(Value v);

    std::string dump() const;
    static Value parse(const std::string& text, size_t max_depth = 256);

private:
    void dump_to(std::string& out) const;
    Type type_ = Type::Null;
    bool b_ = false;
    int64_t i_ = 0;
    double d_ = 0;
    std::shared_ptr<std::string> s_;
    std::shared_ptr<Array> a_;
    std::shared_ptr<Object> o_;
};

Value obj();
Value arr();

} // namespace tj
