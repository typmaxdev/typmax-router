// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
#include "json.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace tj {

namespace {
[[noreturn]] void type_error(const char* want) { throw ParseError(std::string("expected ") + want); }
} // namespace

bool Value::as_bool() const
{
    if (type_ != Type::Bool)
        type_error("a boolean");
    return b_;
}

int64_t Value::as_int() const
{
    // Exact integers only: a JSON number with a fraction or an exponent
    // (5000000.0, 1e7) is a Double and is refused (PROTOCOL.md, "integer nanometres").
    if (type_ == Type::Int)
        return i_;
    type_error("an integer");
}

double Value::as_double() const
{
    if (type_ == Type::Int)
        return static_cast<double>(i_);
    if (type_ == Type::Double)
        return d_;
    type_error("a number");
}

const std::string& Value::as_string() const
{
    if (type_ != Type::String)
        type_error("a string");
    return *s_;
}

const Array& Value::as_array() const
{
    if (type_ != Type::Array)
        type_error("an array");
    return *a_;
}

const Object& Value::as_object() const
{
    if (type_ != Type::Object)
        type_error("an object");
    return *o_;
}

Array& Value::mut_array()
{
    if (type_ != Type::Array)
        type_error("an array");
    return *a_;
}

Object& Value::mut_object()
{
    if (type_ != Type::Object)
        type_error("an object");
    return *o_;
}

const Value* Value::find(const std::string& key) const
{
    if (type_ != Type::Object)
        return nullptr;
    for (const auto& kv : *o_)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

Value& Value::set(const std::string& key, Value v)
{
    mut_object().emplace_back(key, std::move(v));
    return o_->back().second;
}

void Value::push(Value v) { mut_array().push_back(std::move(v)); }

Value obj() { return Value(Object{}); }
Value arr() { return Value(Array{}); }

static void dump_string(const std::string& s, std::string& out)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            }
            else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

void Value::dump_to(std::string& out) const
{
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Int: out += std::to_string(i_); break;
    case Type::Double: {
        if (!std::isfinite(d_)) {
            out += "null";
            break;
        }
        char b[40];
        std::snprintf(b, sizeof b, "%.6f", d_);
        out += b;
        break;
    }
    case Type::String: dump_string(*s_, out); break;
    case Type::Array: {
        out += '[';
        bool first = true;
        for (const auto& v : *a_) {
            if (!first)
                out += ',';
            first = false;
            v.dump_to(out);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        bool first = true;
        for (const auto& kv : *o_) {
            if (!first)
                out += ',';
            first = false;
            dump_string(kv.first, out);
            out += ':';
            kv.second.dump_to(out);
        }
        out += '}';
        break;
    }
    }
}

std::string Value::dump() const
{
    std::string out;
    dump_to(out);
    return out;
}

namespace {

class Parser {
public:
    Parser(const std::string& t, size_t max_depth) : t_(t), max_depth_(max_depth) {}

    Value parse_document()
    {
        Value v = parse_value(0);
        skip_ws();
        if (p_ != t_.size())
            fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const
    {
        throw ParseError("JSON parse error at byte " + std::to_string(p_) + ": " + what);
    }

    void skip_ws()
    {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r'))
            p_++;
    }

    bool consume(const char* lit)
    {
        size_t n = std::char_traits<char>::length(lit);
        if (t_.compare(p_, n, lit) == 0) {
            p_ += n;
            return true;
        }
        return false;
    }

    Value parse_value(size_t depth)
    {
        if (depth > max_depth_)
            fail("nesting too deep");
        skip_ws();
        if (p_ >= t_.size())
            fail("unexpected end of input");
        char c = t_[p_];
        if (c == '{')
            return parse_object(depth);
        if (c == '[')
            return parse_array(depth);
        if (c == '"')
            return Value(parse_string());
        if (consume("true"))
            return Value(true);
        if (consume("false"))
            return Value(false);
        if (consume("null"))
            return Value();
        if (c == '-' || (c >= '0' && c <= '9'))
            return parse_number();
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) {
            char b[8]; // never echo a raw byte: the message must stay valid UTF-8
            std::snprintf(b, sizeof b, "0x%02X", static_cast<unsigned char>(c));
            fail(std::string("unexpected byte ") + b);
        }
        fail(std::string("unexpected character '") + c + "'");
    }

    static constexpr size_t SMALL_OBJECT = 16;

    Value parse_object(size_t depth)
    {
        p_++; // {
        Value o = obj();
        std::unordered_set<std::string> seen; // filled once the object has SMALL_OBJECT keys
        skip_ws();
        if (p_ < t_.size() && t_[p_] == '}') {
            p_++;
            return o;
        }
        for (;;) {
            skip_ws();
            if (p_ >= t_.size() || t_[p_] != '"')
                fail("expected a string key");
            std::string k = parse_string();
            skip_ws();
            if (p_ >= t_.size() || t_[p_] != ':')
                fail("expected ':'");
            p_++;
            Value v = parse_value(depth + 1);
            // Duplicate keys are refused. A linear scan per key is quadratic in the
            // key count (review R-1: 80 k keys took 6.6 s), so objects past a small
            // size switch to a hash set of the keys seen so far.
            const Object& so_far = o.as_object();
            if (so_far.size() < SMALL_OBJECT) {
                for (const auto& kv : so_far)
                    if (kv.first == k)
                        fail("duplicate key \"" + k + "\"");
                if (so_far.size() + 1 == SMALL_OBJECT)
                    for (const auto& kv : so_far)
                        seen.insert(kv.first);
            }
            else if (seen.count(k)) {
                fail("duplicate key \"" + k + "\"");
            }
            if (so_far.size() + 1 >= SMALL_OBJECT)
                seen.insert(k);
            o.set(k, std::move(v));
            skip_ws();
            if (p_ < t_.size() && t_[p_] == ',') {
                p_++;
                continue;
            }
            if (p_ < t_.size() && t_[p_] == '}') {
                p_++;
                return o;
            }
            fail("expected ',' or '}'");
        }
    }

    Value parse_array(size_t depth)
    {
        p_++; // [
        Value a = arr();
        skip_ws();
        if (p_ < t_.size() && t_[p_] == ']') {
            p_++;
            return a;
        }
        for (;;) {
            a.push(parse_value(depth + 1));
            skip_ws();
            if (p_ < t_.size() && t_[p_] == ',') {
                p_++;
                continue;
            }
            if (p_ < t_.size() && t_[p_] == ']') {
                p_++;
                return a;
            }
            fail("expected ',' or ']'");
        }
    }

    static void put_utf8(uint32_t cp, std::string& out)
    {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    uint32_t parse_hex4()
    {
        if (p_ + 4 > t_.size())
            fail("short \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= c - '0';
            else if (c >= 'a' && c <= 'f')
                v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v |= c - 'A' + 10;
            else
                fail("bad \\u escape");
        }
        return v;
    }

    // One UTF-8 sequence starting at p_, validated (RFC 3629: no overlong forms,
    // no surrogates, nothing past U+10FFFF) and appended. Invalid bytes are a
    // parse error, so every string the service echoes back is valid UTF-8 (R-5).
    void take_utf8(std::string& out)
    {
        const auto byte = [this](size_t i) -> unsigned {
            return p_ + i < t_.size() ? static_cast<unsigned char>(t_[p_ + i]) : 0x100u;
        };
        const unsigned b0 = byte(0);
        size_t n = 0;
        unsigned lo = 0x80, hi = 0xBF; // the valid range of the second byte
        if (b0 >= 0xC2 && b0 <= 0xDF)
            n = 2;
        else if (b0 >= 0xE0 && b0 <= 0xEF) {
            n = 3;
            if (b0 == 0xE0)
                lo = 0xA0;
            else if (b0 == 0xED)
                hi = 0x9F;
        }
        else if (b0 >= 0xF0 && b0 <= 0xF4) {
            n = 4;
            if (b0 == 0xF0)
                lo = 0x90;
            else if (b0 == 0xF4)
                hi = 0x8F;
        }
        else
            fail("invalid UTF-8 in string");
        const unsigned b1 = byte(1);
        if (b1 < lo || b1 > hi)
            fail("invalid UTF-8 in string");
        for (size_t i = 2; i < n; i++) {
            const unsigned b = byte(i);
            if (b < 0x80 || b > 0xBF)
                fail("invalid UTF-8 in string");
        }
        out.append(t_, p_, n);
        p_ += n;
    }

    std::string parse_string()
    {
        p_++; // "
        std::string out;
        for (;;) {
            if (p_ >= t_.size())
                fail("unterminated string");
            char c = t_[p_++];
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 0x20)
                fail("control character in string");
            if (c != '\\') {
                if (static_cast<unsigned char>(c) >= 0x80) {
                    p_--;
                    take_utf8(out);
                    continue;
                }
                out += c;
                continue;
            }
            if (p_ >= t_.size())
                fail("unterminated escape");
            char e = t_[p_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = parse_hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (!consume("\\u"))
                        fail("lone surrogate");
                    uint32_t lo = parse_hex4();
                    if (lo < 0xDC00 || lo > 0xDFFF)
                        fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    fail("lone surrogate");
                }
                put_utf8(cp, out);
                break;
            }
            default: fail("bad escape");
            }
        }
    }

    Value parse_number()
    {
        size_t start = p_;
        bool is_int = true;
        if (t_[p_] == '-')
            p_++;
        while (p_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[p_])))
            p_++;
        if (p_ < t_.size() && t_[p_] == '.') {
            is_int = false;
            p_++;
            while (p_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[p_])))
                p_++;
        }
        if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
            is_int = false;
            p_++;
            if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-'))
                p_++;
            while (p_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[p_])))
                p_++;
        }
        std::string s = t_.substr(start, p_ - start);
        if (s == "-" || s.empty())
            fail("bad number");
        if (is_int) {
            errno = 0;
            char* end = nullptr;
            long long v = std::strtoll(s.c_str(), &end, 10);
            if (errno == ERANGE)
                fail("integer out of range");
            return Value(static_cast<int64_t>(v));
        }
        return Value(std::strtod(s.c_str(), nullptr));
    }

    const std::string& t_;
    size_t p_ = 0;
    size_t max_depth_;
};

} // namespace

Value Value::parse(const std::string& text, size_t max_depth)
{
    Parser p(text, max_depth);
    return p.parse_document();
}

} // namespace tj
