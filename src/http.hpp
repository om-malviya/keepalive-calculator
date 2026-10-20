// HTTP/1.1 message framing: request structs, an incremental request parser
// that finds where one request ends and the next begins, and the response
// serializer.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace http {

constexpr std::size_t kMaxHeadBytes = 8192;           // request line + headers + CRLFCRLF
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;    // 1 MiB, Content-Length or chunked
constexpr std::size_t kMaxChunkLineBytes = 8192;      // one chunk-size line
constexpr std::size_t kMaxTrailerBytes = 8192;        // whole trailer section

struct Request {
    std::string method;
    std::string target;
    bool http10 = false;  // HTTP/1.0 (otherwise treated as HTTP/1.1)
    std::vector<std::pair<std::string, std::string>> headers;  // names lowercased
    std::string body;

    // Header lookups take a lowercase name.
    std::size_t count(std::string_view name) const;
    const std::string* find(std::string_view name) const;
    // True if any header line called `name` has `token` in its comma list
    // (case-insensitive), e.g. has_token("connection", "close").
    bool has_token(std::string_view name, std::string_view token) const;
};

struct Response {
    int status = 200;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;  // extra headers (Allow)
    bool close = false;  // adds "Connection: close"
};

const char* reason_phrase(int status);

// Current time as an IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
std::string http_date();

// Status line, Date, Server, Content-Type, Content-Length, extra headers,
// Connection, blank line, then the body unless head_only.
// keep_alive_header adds "Connection: keep-alive" (persistent HTTP/1.0).
std::string serialize(const Response& resp, bool head_only, bool keep_alive_header);

// Incremental request parser. The caller owns the connection buffer and calls
// step() whenever new bytes have been appended. The parser consumes bytes from
// the front of the buffer; whatever it does not consume belongs to the next
// request. No byte is scanned more than a constant number of times.
class RequestParser {
public:
    enum class Status {
        NeedMore,  // request incomplete, read more bytes
        Complete,  // a whole request is ready, call take()
        Error,     // framing error; error_status()/error_reason(); connection must close
    };

    Status step(std::string& buf);

    // Returns the completed request and resets the parser for the next one.
    Request take();

    int error_status() const { return error_status_; }
    const std::string& error_reason() const { return error_reason_; }

    // True exactly once when an interim "100 Continue" should be sent: the
    // request carried "Expect: 100-continue" and its body is not buffered yet.
    bool take_continue();

    // True if a request has been partly received (used for 408 vs silent close).
    bool in_progress(const std::string& buf) const;

private:
    enum class Stage { Head, BodyLength, BodyChunked, Done, Failed };
    enum class ChunkStage { Size, Data, DataEnd, Trailers };

    Status fail(int status, std::string reason);
    Status parse_head(std::string& buf);
    Status parse_request_line(std::string_view line);
    Status parse_header_line(std::string_view line);
    Status choose_body_framing();
    Status parse_chunked(std::string& buf);
    std::size_t find_crlf(const std::string& buf);

    Stage stage_ = Stage::Head;
    ChunkStage chunk_stage_ = ChunkStage::Size;
    std::size_t scan_pos_ = 0;       // where the next terminator search resumes
    std::size_t remaining_ = 0;      // body or chunk bytes still expected
    std::size_t trailer_bytes_ = 0;
    bool expect_continue_ = false;
    bool continue_sent_ = false;
    Request req_;
    int error_status_ = 0;
    std::string error_reason_;
};

}  // namespace http
