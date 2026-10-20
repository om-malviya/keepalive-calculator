// Arithmetic core: query-string parsing, percent-decoding, int64 parsing and
// overflow-checked arithmetic. No networking in here.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace calc {

enum class Op { Add, Sub, Mul, Div };

// Maps "/add", "/sub", "/mul", "/div" (exact, case-sensitive) to an Op.
std::optional<Op> op_from_path(std::string_view path);

struct Result {
    bool ok = false;
    std::int64_t value = 0;
    std::string error;  // short lowercase reason when !ok
};

// Decodes %XX escapes. '+' is NOT turned into a space. Returns false on a
// malformed escape (a '%' not followed by two hex digits).
bool percent_decode(std::string_view in, std::string& out);

enum class ParseStatus { Ok, Invalid, OutOfRange };

// Accepts an optional '+' or '-' followed by one or more ASCII digits and
// nothing else. The value must fit in a signed 64-bit integer.
ParseStatus parse_int64(std::string_view text, std::int64_t& out);

// Applies op with overflow checking. Division truncates toward zero.
Result apply(Op op, std::int64_t a, std::int64_t b);

// Parses the raw query string (the part after '?'), extracts a and b and
// applies op.
Result evaluate(Op op, std::string_view query);

}  // namespace calc
