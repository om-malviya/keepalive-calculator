#include "calc.hpp"

#include <charconv>
#include <limits>
#include <system_error>
#include <utility>

namespace calc {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Result failure(std::string reason) {
    Result r;
    r.error = std::move(reason);
    return r;
}

Result success(std::int64_t value) {
    Result r;
    r.ok = true;
    r.value = value;
    return r;
}

}  // namespace

std::optional<Op> op_from_path(std::string_view path) {
    if (path == "/add") return Op::Add;
    if (path == "/sub") return Op::Sub;
    if (path == "/mul") return Op::Mul;
    if (path == "/div") return Op::Div;
    return std::nullopt;
}

bool percent_decode(std::string_view in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (i + 2 >= in.size()) return false;
        const int hi = hex_value(in[i + 1]);
        const int lo = hex_value(in[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return true;
}

ParseStatus parse_int64(std::string_view text, std::int64_t& out) {
    std::string_view digits = text;
    if (!digits.empty() && (digits.front() == '+' || digits.front() == '-')) {
        digits.remove_prefix(1);
    }
    if (digits.empty()) return ParseStatus::Invalid;
    for (char c : digits) {
        if (c < '0' || c > '9') return ParseStatus::Invalid;
    }
    // from_chars accepts a leading '-' but not a leading '+'.
    const std::string_view number = text.front() == '+' ? digits : text;
    const char* first = number.data();
    const char* last = first + number.size();
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec == std::errc::result_out_of_range) return ParseStatus::OutOfRange;
    if (ec != std::errc() || ptr != last) return ParseStatus::Invalid;
    out = value;
    return ParseStatus::Ok;
}

Result apply(Op op, std::int64_t a, std::int64_t b) {
    std::int64_t r = 0;
    switch (op) {
        case Op::Add:
            if (__builtin_add_overflow(a, b, &r)) return failure("overflow");
            return success(r);
        case Op::Sub:
            if (__builtin_sub_overflow(a, b, &r)) return failure("overflow");
            return success(r);
        case Op::Mul:
            if (__builtin_mul_overflow(a, b, &r)) return failure("overflow");
            return success(r);
        case Op::Div:
            if (b == 0) return failure("division by zero");
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                return failure("overflow");
            }
            return success(a / b);
    }
    return failure("unknown operation");
}

Result evaluate(Op op, std::string_view query) {
    std::optional<std::string> a_text;
    std::optional<std::string> b_text;

    std::size_t pos = 0;
    while (pos <= query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string_view::npos) amp = query.size();
        const std::string_view pair = query.substr(pos, amp - pos);
        pos = amp + 1;
        if (pair.empty()) continue;

        const std::size_t eq = pair.find('=');
        const std::string_view raw_key = pair.substr(0, eq);
        const std::string_view raw_value =
            eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1);

        std::string key;
        std::string value;
        if (!percent_decode(raw_key, key) || !percent_decode(raw_value, value)) {
            return failure("bad percent-encoding");
        }

        std::optional<std::string>* slot = nullptr;
        if (key == "a") slot = &a_text;
        else if (key == "b") slot = &b_text;
        if (slot == nullptr) continue;  // unknown parameters are ignored
        if (slot->has_value()) return failure("duplicate parameter " + key);
        *slot = std::move(value);
    }

    if (!a_text) return failure("missing parameter a");
    if (!b_text) return failure("missing parameter b");

    // Returns an empty string on success, otherwise the error reason.
    auto parse_param = [](const char* name, const std::string& text, std::int64_t& value) {
        switch (parse_int64(text, value)) {
            case ParseStatus::Ok:
                return std::string();
            case ParseStatus::Invalid:
                return std::string("invalid number for ") + name;
            case ParseStatus::OutOfRange:
                return std::string("number out of range for ") + name;
        }
        return std::string("invalid number for ") + name;
    };

    std::int64_t a = 0;
    std::int64_t b = 0;
    if (std::string err = parse_param("a", *a_text, a); !err.empty()) return failure(err);
    if (std::string err = parse_param("b", *b_text, b); !err.empty()) return failure(err);
    return apply(op, a, b);
}

}  // namespace calc
