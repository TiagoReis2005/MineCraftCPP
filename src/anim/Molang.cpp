#include "anim/Molang.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace mc {

struct MolangExpr::Node {
    enum class Kind { Const, Var, This, Unary, Binary, Ternary, Call };
    Kind kind = Kind::Const;
    float value = 0.0f;      // Const
    std::string name;        // Var (normalized), Call
    // Binary ops: '+','-','*','/','<','>','L'(<=),'G'(>=),'E'(==),'N'(!=),'&','|','?'(??)
    char op = 0;
    std::vector<Node> children;
};

namespace {

using Node = MolangExpr::Node;
using Kind = Node::Kind;

constexpr float kDegToRad = 0.01745329252f;

// Truthiness with an epsilon: vanilla expressions rely on math.sin(180) == 0, which
// float sin() misses by ~1e-7; without this the attack ternary leaks a constant 30
// degree arm twist at idle.
bool truthy(float v) { return std::fabs(v) > 1e-5f; }

class Parser {
public:
    explicit Parser(const std::string& src) : s_(src) {}

    Node parse() {
        Node n = parseTernary();
        skipWs();
        if (pos_ != s_.size()) fail("trailing characters");
        return n;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const char* why) {
        throw std::runtime_error("Molang parse error in \"" + s_ + "\" at " +
                                 std::to_string(pos_) + ": " + why);
    }

    void skipWs() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    bool eat(const char* tok) { // multi-char operator
        skipWs();
        size_t len = 0;
        while (tok[len]) ++len;
        if (s_.compare(pos_, len, tok) == 0) {
            pos_ += len;
            return true;
        }
        return false;
    }

    char peek() {
        skipWs();
        return pos_ < s_.size() ? s_[pos_] : '\0';
    }

    Node binary(char op, Node a, Node b) {
        Node n;
        n.kind = Kind::Binary;
        n.op = op;
        n.children.push_back(std::move(a));
        n.children.push_back(std::move(b));
        return n;
    }

    Node parseTernary() {
        Node cond = parseNullCoalesce();
        if (peek() == '?' && s_.compare(pos_, 2, "??") != 0) {
            ++pos_;
            Node a = parseTernary();
            skipWs();
            if (peek() != ':') fail("expected ':'");
            ++pos_;
            Node b = parseTernary();
            Node n;
            n.kind = Kind::Ternary;
            n.children.push_back(std::move(cond));
            n.children.push_back(std::move(a));
            n.children.push_back(std::move(b));
            return n;
        }
        return cond;
    }

    Node parseNullCoalesce() {
        Node a = parseOr();
        while (eat("??")) a = binary('?', std::move(a), parseOr());
        return a;
    }

    Node parseOr() {
        Node a = parseAnd();
        while (eat("||")) a = binary('|', std::move(a), parseAnd());
        return a;
    }

    Node parseAnd() {
        Node a = parseEquality();
        while (eat("&&")) a = binary('&', std::move(a), parseEquality());
        return a;
    }

    Node parseEquality() {
        Node a = parseRelational();
        while (true) {
            if (eat("==")) a = binary('E', std::move(a), parseRelational());
            else if (eat("!=")) a = binary('N', std::move(a), parseRelational());
            else return a;
        }
    }

    Node parseRelational() {
        Node a = parseAdditive();
        while (true) {
            if (eat("<=")) a = binary('L', std::move(a), parseAdditive());
            else if (eat(">=")) a = binary('G', std::move(a), parseAdditive());
            else if (peek() == '<') { ++pos_; a = binary('<', std::move(a), parseAdditive()); }
            else if (peek() == '>') { ++pos_; a = binary('>', std::move(a), parseAdditive()); }
            else return a;
        }
    }

    Node parseAdditive() {
        Node a = parseMultiplicative();
        while (true) {
            char c = peek();
            if (c == '+') { ++pos_; a = binary('+', std::move(a), parseMultiplicative()); }
            else if (c == '-') { ++pos_; a = binary('-', std::move(a), parseMultiplicative()); }
            else return a;
        }
    }

    Node parseMultiplicative() {
        Node a = parseUnary();
        while (true) {
            char c = peek();
            if (c == '*') { ++pos_; a = binary('*', std::move(a), parseUnary()); }
            else if (c == '/') { ++pos_; a = binary('/', std::move(a), parseUnary()); }
            else return a;
        }
    }

    Node parseUnary() {
        if (peek() == '-') {
            ++pos_;
            Node n;
            n.kind = Kind::Unary;
            n.op = '-';
            n.children.push_back(parseUnary());
            return n;
        }
        if (peek() == '!') {
            ++pos_;
            Node n;
            n.kind = Kind::Unary;
            n.op = '!';
            n.children.push_back(parseUnary());
            return n;
        }
        return parsePrimary();
    }

    Node parsePrimary() {
        char c = peek();
        if (c == '(') {
            ++pos_;
            Node n = parseTernary();
            if (peek() != ')') fail("expected ')'");
            ++pos_;
            return n;
        }
        if (c == '\'') { // string literal (locator names etc.) -> constant 0
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] != '\'') ++pos_;
            if (pos_ >= s_.size()) fail("unterminated string");
            ++pos_;
            Node n;
            n.kind = Kind::Const;
            return n;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            size_t start = pos_;
            while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
                                        s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')) {
                ++pos_;
            }
            Node n;
            n.kind = Kind::Const;
            n.value = static_cast<float>(std::strtod(s_.c_str() + start, nullptr));
            return n;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = pos_;
            while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) ||
                                        s_[pos_] == '_' || s_[pos_] == '.')) {
                ++pos_;
            }
            std::string name = s_.substr(start, pos_ - start);
            for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (name == "math.pi") { // Molang is case-insensitive; vanilla writes "Math.Pi"
                Node n;
                n.kind = Kind::Const;
                n.value = 3.14159265f;
                return n;
            }
            if (peek() == '(') { // function call
                ++pos_;
                Node n;
                n.kind = Kind::Call;
                n.name = name;
                if (peek() != ')') {
                    while (true) {
                        n.children.push_back(parseTernary());
                        if (peek() == ',') { ++pos_; continue; }
                        break;
                    }
                }
                if (peek() != ')') fail("expected ')'");
                ++pos_;
                return n;
            }
            if (name == "this") {
                Node n;
                n.kind = Kind::This;
                return n;
            }
            Node n;
            n.kind = Kind::Var;
            n.name = normalize(name);
            return n;
        }
        fail("unexpected character");
    }

    static std::string normalize(const std::string& name) {
        if (name.rfind("q.", 0) == 0) return "query." + name.substr(2);
        if (name.rfind("v.", 0) == 0) return "variable." + name.substr(2);
        if (name.rfind("t.", 0) == 0) return "temp." + name.substr(2);
        return name;
    }
};

// `defined` supports `??`: false only when the value came from an unknown variable.
float evalNode(const Node& n, const MolangContext& ctx, bool& defined) {
    defined = true;
    switch (n.kind) {
        case Kind::Const: return n.value;
        case Kind::This: return ctx.thisValue;
        case Kind::Var: {
            auto it = ctx.vars.find(n.name);
            if (it == ctx.vars.end()) {
                defined = false;
                return 0.0f;
            }
            return it->second;
        }
        case Kind::Unary: {
            bool d;
            float v = evalNode(n.children[0], ctx, d);
            return n.op == '-' ? -v : (truthy(v) ? 0.0f : 1.0f);
        }
        case Kind::Ternary: {
            bool d;
            float c = evalNode(n.children[0], ctx, d);
            return evalNode(n.children[truthy(c) ? 1 : 2], ctx, d);
        }
        case Kind::Binary: {
            bool dl, dr;
            if (n.op == '?') { // null coalesce
                float l = evalNode(n.children[0], ctx, dl);
                if (dl) return l;
                return evalNode(n.children[1], ctx, dr);
            }
            float a = evalNode(n.children[0], ctx, dl);
            float b = evalNode(n.children[1], ctx, dr);
            switch (n.op) {
                case '+': return a + b;
                case '-': return a - b;
                case '*': return a * b;
                case '/': return b != 0.0f ? a / b : 0.0f;
                case '<': return a < b ? 1.0f : 0.0f;
                case '>': return a > b ? 1.0f : 0.0f;
                case 'L': return a <= b ? 1.0f : 0.0f;
                case 'G': return a >= b ? 1.0f : 0.0f;
                case 'E': return a == b ? 1.0f : 0.0f;
                case 'N': return a != b ? 1.0f : 0.0f;
                case '&': return (truthy(a) && truthy(b)) ? 1.0f : 0.0f;
                case '|': return (truthy(a) || truthy(b)) ? 1.0f : 0.0f;
                default: return 0.0f;
            }
        }
        case Kind::Call: {
            auto arg = [&](size_t i) {
                bool d;
                return i < n.children.size() ? evalNode(n.children[i], ctx, d) : 0.0f;
            };
            const std::string& f = n.name;
            if (f == "math.sin") return std::sin(arg(0) * kDegToRad);
            if (f == "math.cos") return std::cos(arg(0) * kDegToRad);
            if (f == "math.pow") return std::pow(arg(0), arg(1));
            if (f == "math.sqrt") return std::sqrt(std::fabs(arg(0)));
            if (f == "math.abs") return std::fabs(arg(0));
            if (f == "math.mod") { float b = arg(1); return b != 0.0f ? std::fmod(arg(0), b) : 0.0f; }
            if (f == "math.floor") return std::floor(arg(0));
            if (f == "math.ceil") return std::ceil(arg(0));
            if (f == "math.min") return std::min(arg(0), arg(1));
            if (f == "math.max") return std::max(arg(0), arg(1));
            if (f == "math.clamp") return std::min(std::max(arg(0), arg(1)), arg(2));
            if (f == "math.lerp") { float a = arg(0); return a + (arg(1) - a) * arg(2); }
            // Unknown function (e.g. query.get_root_locator_offset): evaluate to 0.
            return 0.0f;
        }
    }
    return 0.0f;
}

} // namespace

MolangExpr MolangExpr::constant(float v) {
    MolangExpr e;
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::Const;
    node->value = v;
    e.root_ = std::move(node);
    return e;
}

MolangExpr MolangExpr::parse(const std::string& src) {
    MolangExpr e;
    e.root_ = std::make_shared<const Node>(Parser(src).parse());
    return e;
}

float MolangExpr::eval(const MolangContext& ctx) const {
    if (!root_) return 0.0f;
    bool defined;
    return evalNode(*root_, ctx, defined);
}

} // namespace mc
