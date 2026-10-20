#include "http.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace http {
namespace {

constexpr std::string_view kTcharPunct = "!#$%&'*+-.^_`|~";

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_tchar(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c != '\0' && kTcharPunct.find(c) != std::string_view::npos);
}

bool is_token(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), is_tchar);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_lower(a[i]) != to_lower(b[i])) return false;
    }
    return true;
}

std::string_view trim_ows(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Field values may contain visible characters, SP, HTAB and obs-text; any
// other control character (including a bare CR or LF) is rejected.
bool valid_field_value(std::string_view s) {
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20 && u != '\t') || u == 0x7f) return false;
    }
    return true;
}

// Request targets are visible ASCII only (no spaces, controls or raw UTF-8).
bool valid_target(std::string_view s) {
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7e) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- Request

std::size_t Request::count(std::string_view name) const {
    std::size_t n = 0;
    for (const auto& h : headers) {
        if (h.first == name) ++n;
    }
    return n;
}

const std::string* Request::find(std::string_view name) const {
    for (const auto& h : headers) {
        if (h.first == name) return &h.second;
    }
    return nullptr;
}

bool Request::has_token(std::string_view name, std::string_view token) const {
    for (const auto& h : headers) {
        if (h.first != name) continue;
        std::string_view rest = h.second;
        for (;;) {
            const std::size_t comma = rest.find(',');
            if (iequals(trim_ows(rest.substr(0, comma)), token)) return true;
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
    }
    return false;
}

// ---------------------------------------------------------------- Response

const char* reason_phrase(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

std::string http_date() {
    static const char* const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char out[64];
    std::snprintf(out, sizeof out, "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[tm.tm_wday],
                  tm.tm_mday, kMonths[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min,
                  tm.tm_sec);
    return out;
}

std::string serialize(const Response& resp, bool head_only, bool keep_alive_header) {
    std::string out;
    out.reserve(256 + resp.body.size());
    out += "HTTP/1.1 ";
    out += std::to_string(resp.status);
    out += ' ';
    out += reason_phrase(resp.status);
    out += "\r\nDate: ";
    out += http_date();
    out += "\r\nServer: calc/1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: ";
    out += std::to_string(resp.body.size());
    out += "\r\n";
    for (const auto& h : resp.headers) {
        out += h.first;
        out += ": ";
        out += h.second;
        out += "\r\n";
    }
    if (resp.close) {
        out += "Connection: close\r\n";
    } else if (keep_alive_header) {
        out += "Connection: keep-alive\r\n";
    }
    out += "\r\n";
    if (!head_only) out += resp.body;
    return out;
}

// ---------------------------------------------------------------- Parser

RequestParser::Status RequestParser::fail(int status, std::string reason) {
    stage_ = Stage::Failed;
    error_status_ = status;
    error_reason_ = std::move(reason);
    return Status::Error;
}

std::size_t RequestParser::find_crlf(const std::string& buf) {
    const std::size_t pos = buf.find("\r\n", scan_pos_);
    if (pos == std::string::npos) {
        // The last byte may be a '\r' whose '\n' has not arrived yet.
        scan_pos_ = buf.empty() ? 0 : buf.size() - 1;
    }
    return pos;
}

RequestParser::Status RequestParser::step(std::string& buf) {
    if (stage_ == Stage::Head) {
        const Status s = parse_head(buf);
        if (s != Status::Complete) return s;
        // Head parsed; stage_ now says how the body is framed.
    }
    switch (stage_) {
        case Stage::BodyLength:
            if (buf.size() < remaining_) return Status::NeedMore;
            req_.body.assign(buf, 0, remaining_);
            buf.erase(0, remaining_);  // byte remaining_+1 belongs to the next request
            remaining_ = 0;
            stage_ = Stage::Done;
            return Status::Complete;
        case Stage::BodyChunked:
            return parse_chunked(buf);
        case Stage::Done:
            return Status::Complete;
        case Stage::Failed:
            return Status::Error;
        case Stage::Head:
            break;
    }
    return Status::NeedMore;
}

RequestParser::Status RequestParser::parse_head(std::string& buf) {
    // RFC 9112 2.2: ignore empty lines received before the request line.
    std::size_t skip = 0;
    while (buf.size() - skip >= 2 && buf[skip] == '\r' && buf[skip + 1] == '\n') skip += 2;
    if (skip > 0) {
        buf.erase(0, skip);
        scan_pos_ = 0;
    }
    if (buf.empty()) return Status::NeedMore;

    // Find the blank line that ends the head, resuming where the last search
    // stopped (minus 3 bytes in case the terminator straddles two reads).
    const std::string_view view(buf);
    const std::size_t limit = std::min(view.size(), kMaxHeadBytes);
    const std::size_t end = view.substr(0, limit).find("\r\n\r\n", scan_pos_);
    if (end == std::string_view::npos) {
        if (buf.size() >= kMaxHeadBytes) return fail(431, "request header too large");
        scan_pos_ = limit >= 3 ? limit - 3 : 0;
        return Status::NeedMore;
    }

    const std::string_view head = view.substr(0, end);
    const std::size_t line_end = head.find("\r\n");
    if (parse_request_line(head.substr(0, line_end)) == Status::Error) return Status::Error;

    std::size_t pos = line_end == std::string_view::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
        std::size_t e = head.find("\r\n", pos);
        if (e == std::string_view::npos) e = head.size();
        if (parse_header_line(head.substr(pos, e - pos)) == Status::Error) return Status::Error;
        pos = e + 2;
    }

    buf.erase(0, end + 4);
    scan_pos_ = 0;
    return choose_body_framing();
}

RequestParser::Status RequestParser::parse_request_line(std::string_view line) {
    // method SP request-target SP HTTP-version, exactly two spaces.
    const std::size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return fail(400, "malformed request line");
    const std::size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || line.find(' ', sp2 + 1) != std::string_view::npos) {
        return fail(400, "malformed request line");
    }
    const std::string_view method = line.substr(0, sp1);
    const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = line.substr(sp2 + 1);
    if (!is_token(method) || target.empty() || !valid_target(target)) {
        return fail(400, "malformed request line");
    }

    // HTTP-version = "HTTP/" DIGIT "." DIGIT (case-sensitive).
    if (version.size() != 8 || version.substr(0, 5) != "HTTP/" || !is_digit(version[5]) ||
        version[6] != '.' || !is_digit(version[7])) {
        return fail(400, "malformed http version");
    }
    if (version[5] != '1') return fail(505, "http version not supported");
    // HTTP/1.0 is special; any higher 1.x minor is handled as HTTP/1.1
    // (RFC 9110 2.5).
    req_.http10 = version[7] == '0';
    req_.method.assign(method);
    req_.target.assign(target);
    return Status::Complete;
}

RequestParser::Status RequestParser::parse_header_line(std::string_view line) {
    if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        return fail(400, "obsolete line folding");
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) return fail(400, "malformed header");
    const std::string_view name = line.substr(0, colon);
    if (!is_token(name)) return fail(400, "malformed header name");  // also "Name :"
    const std::string_view value = trim_ows(line.substr(colon + 1));
    if (!valid_field_value(value)) return fail(400, "malformed header value");

    std::string lowered(name);
    for (char& c : lowered) c = to_lower(c);
    req_.headers.emplace_back(std::move(lowered), std::string(value));
    return Status::Complete;
}

RequestParser::Status RequestParser::choose_body_framing() {
    // RFC 9112 6.3.
    const std::size_t te_count = req_.count("transfer-encoding");
    const std::size_t cl_count = req_.count("content-length");

    if (te_count > 0 && cl_count > 0) {
        return fail(400, "both transfer-encoding and content-length");
    }

    if (te_count > 0) {
        std::string combined;
        for (const auto& h : req_.headers) {
            if (h.first != "transfer-encoding") continue;
            if (!combined.empty()) combined += ", ";
            combined += h.second;
        }
        if (!iequals(trim_ows(combined), "chunked")) {
            return fail(501, "unsupported transfer-encoding");
        }
        stage_ = Stage::BodyChunked;
        chunk_stage_ = ChunkStage::Size;
    } else if (cl_count > 0) {
        const std::string* first = nullptr;
        for (const auto& h : req_.headers) {
            if (h.first != "content-length") continue;
            if (first == nullptr) first = &h.second;
            else if (h.second != *first) return fail(400, "conflicting content-length");
        }
        const std::string& text = *first;
        if (text.empty() || !std::all_of(text.begin(), text.end(), is_digit)) {
            return fail(400, "invalid content-length");
        }
        // Stop accumulating once past the limit, so overflow cannot happen.
        std::uint64_t n = 0;
        bool too_large = false;
        for (char c : text) {
            n = n * 10 + static_cast<std::uint64_t>(c - '0');
            if (n > kMaxBodyBytes) {
                too_large = true;
                break;
            }
        }
        if (too_large) return fail(413, "content too large");
        remaining_ = static_cast<std::size_t>(n);
        stage_ = remaining_ > 0 ? Stage::BodyLength : Stage::Done;
    } else {
        stage_ = Stage::Done;  // a request without either header has no body
    }

    // RFC 9110 10.1.1: a server must ignore 100-continue from HTTP/1.0 clients.
    if (stage_ != Stage::Done && !req_.http10) {
        const std::string* expect = req_.find("expect");
        expect_continue_ = expect != nullptr && iequals(*expect, "100-continue");
    }
    return Status::Complete;
}

RequestParser::Status RequestParser::parse_chunked(std::string& buf) {
    for (;;) {
        switch (chunk_stage_) {
            case ChunkStage::Size: {
                const std::size_t eol = find_crlf(buf);
                if (eol == std::string::npos) {
                    if (buf.size() > kMaxChunkLineBytes) return fail(400, "chunk size line too long");
                    return Status::NeedMore;
                }
                if (eol > kMaxChunkLineBytes) return fail(400, "chunk size line too long");
                const std::string_view line(buf.data(), eol);

                // chunk-size = 1*HEXDIG, then optional BWS ";" chunk-ext (ignored).
                std::size_t i = 0;
                std::uint64_t size = 0;
                while (i < line.size() && hex_digit(line[i]) >= 0) {
                    size = size * 16 + static_cast<std::uint64_t>(hex_digit(line[i]));
                    ++i;
                    if (i > 16) return fail(400, "invalid chunk size");
                }
                if (i == 0) return fail(400, "invalid chunk size");
                std::string_view rest = line.substr(i);
                if (!rest.empty()) {
                    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
                        rest.remove_prefix(1);
                    }
                    if (rest.empty() || rest.front() != ';' || !valid_field_value(rest)) {
                        return fail(400, "invalid chunk size");
                    }
                }

                buf.erase(0, eol + 2);
                scan_pos_ = 0;
                if (size == 0) {
                    chunk_stage_ = ChunkStage::Trailers;
                    trailer_bytes_ = 0;
                    break;
                }
                if (size > kMaxBodyBytes - req_.body.size()) return fail(413, "content too large");
                remaining_ = static_cast<std::size_t>(size);
                chunk_stage_ = ChunkStage::Data;
                break;
            }
            case ChunkStage::Data: {
                const std::size_t take = std::min(buf.size(), remaining_);
                req_.body.append(buf, 0, take);
                buf.erase(0, take);
                remaining_ -= take;
                if (remaining_ > 0) return Status::NeedMore;
                chunk_stage_ = ChunkStage::DataEnd;
                break;
            }
            case ChunkStage::DataEnd: {
                if (buf.empty()) return Status::NeedMore;
                if (buf[0] != '\r') return fail(400, "missing chunk terminator");
                if (buf.size() < 2) return Status::NeedMore;
                if (buf[1] != '\n') return fail(400, "missing chunk terminator");
                buf.erase(0, 2);
                chunk_stage_ = ChunkStage::Size;
                break;
            }
            case ChunkStage::Trailers: {
                const std::size_t eol = find_crlf(buf);
                if (eol == std::string::npos) {
                    if (trailer_bytes_ + buf.size() > kMaxTrailerBytes) {
                        return fail(431, "trailer section too large");
                    }
                    return Status::NeedMore;
                }
                trailer_bytes_ += eol + 2;
                if (trailer_bytes_ > kMaxTrailerBytes) return fail(431, "trailer section too large");
                const bool last = eol == 0;  // the empty line ends the message
                buf.erase(0, eol + 2);
                scan_pos_ = 0;
                if (last) {
                    stage_ = Stage::Done;
                    return Status::Complete;
                }
                break;  // trailer fields are ignored
            }
        }
    }
}

Request RequestParser::take() {
    Request out = std::move(req_);
    req_ = Request{};
    stage_ = Stage::Head;
    chunk_stage_ = ChunkStage::Size;
    scan_pos_ = 0;
    remaining_ = 0;
    trailer_bytes_ = 0;
    expect_continue_ = false;
    continue_sent_ = false;
    return out;
}

bool RequestParser::take_continue() {
    if (!expect_continue_ || continue_sent_) return false;
    if (stage_ != Stage::BodyLength && stage_ != Stage::BodyChunked) return false;
    continue_sent_ = true;
    return true;
}

bool RequestParser::in_progress(const std::string& buf) const {
    return stage_ != Stage::Head || !buf.empty();
}

}  // namespace http
