#include "server.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <string>
#include <string_view>
#include <vector>

#include "calc.hpp"

namespace server {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kRecvChunk = 65536;
constexpr auto kLingerTime = std::chrono::seconds(1);

http::Response make_error(int status, std::string reason) {
    http::Response r;
    r.status = status;
    r.body = std::move(reason);
    return r;
}

// Milliseconds until deadline, rounded up, clamped to [0, INT_MAX].
int ms_until(Clock::time_point deadline) {
    const auto left = deadline - Clock::now();
    if (left <= Clock::duration::zero()) return 0;
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return ms > INT_MAX ? INT_MAX : static_cast<int>(ms);
}

// Writes everything, coping with partial writes and EINTR. Returns false if
// the peer is gone or the send timeout (SO_SNDTIMEO) expired.
bool send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::send(fd, data.data(), data.size(), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

// Half-close, then read and discard whatever the client is still sending
// (for at most kLingerTime) before closing. Closing a socket with unread
// input makes the kernel send RST, which can destroy the response we just
// wrote before the client reads it.
void lingering_close(int fd) {
    ::shutdown(fd, SHUT_WR);
    const auto deadline = Clock::now() + kLingerTime;
    char sink[4096];
    for (;;) {
        const int timeout = ms_until(deadline);
        if (timeout == 0) break;
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, timeout);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        const ssize_t n = ::recv(fd, sink, sizeof sink, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
    }
    ::close(fd);
}

}  // namespace

bool client_wants_close(const http::Request& req) {
    if (req.has_token("connection", "close")) return true;
    return req.http10 && !req.has_token("connection", "keep-alive");
}

http::Response handle(const http::Request& req) {
    // RFC 9112 3.2: HTTP/1.1 requests need exactly one Host; more than one is
    // an error in any version.
    const std::size_t hosts = req.count("host");
    if (hosts > 1) return make_error(400, "duplicate Host header");
    if (hosts == 0 && !req.http10) return make_error(400, "missing Host header");

    // Only origin-form targets ("/path?query") are supported.
    if (req.target.empty() || req.target.front() != '/') {
        return make_error(400, "request target must start with /");
    }
    const std::string_view target(req.target);
    const std::size_t q = target.find('?');
    const std::string_view path = target.substr(0, q);
    const std::string_view query =
        q == std::string_view::npos ? std::string_view() : target.substr(q + 1);

    // 404 before 405: 405 claims the resource exists, so unknown paths are
    // 404 whatever the method.
    const auto op = calc::op_from_path(path);
    if (!op) return make_error(404, "not found");

    if (req.method != "GET" && req.method != "HEAD") {
        http::Response r = make_error(405, "method not allowed");
        r.headers.emplace_back("Allow", "GET, HEAD");
        return r;
    }

    const calc::Result result = calc::evaluate(*op, query);
    if (!result.ok) return make_error(400, result.error);

    http::Response r;
    r.status = 200;
    r.body = std::to_string(result.value);
    return r;
}

void handle_connection(int fd, Config cfg) {
    const auto idle = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(cfg.idle_timeout_seconds));

    std::string buf;  // bytes received but not yet consumed by the parser
    http::RequestParser parser;
    std::vector<char> chunk(kRecvChunk);
    auto deadline = Clock::now() + idle;

    for (;;) {
        // Answer every complete request already buffered, in order
        // (pipelining). Only read from the socket once the parser needs more.
        http::RequestParser::Status status;
        while ((status = parser.step(buf)) == http::RequestParser::Status::Complete) {
            const http::Request req = parser.take();
            http::Response resp = handle(req);
            const bool close_after = resp.close || client_wants_close(req);
            resp.close = close_after;
            const bool keep_alive_header = req.http10 && !close_after;
            if (!send_all(fd, http::serialize(resp, req.method == "HEAD", keep_alive_header))) {
                ::close(fd);
                return;
            }
            if (close_after) {
                lingering_close(fd);
                return;
            }
            deadline = Clock::now() + idle;  // one timer, restarted per answered request
        }

        if (status == http::RequestParser::Status::Error) {
            // Framing is lost: we cannot know where the next request starts.
            http::Response resp = make_error(parser.error_status(), parser.error_reason());
            resp.close = true;
            send_all(fd, http::serialize(resp, false, false));
            lingering_close(fd);
            return;
        }

        if (parser.take_continue()) {
            if (!send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n")) {
                ::close(fd);
                return;
            }
        }

        const int timeout = ms_until(deadline);
        int r = 0;
        if (timeout > 0) {
            pollfd p{fd, POLLIN, 0};
            r = ::poll(&p, 1, timeout);
            if (r < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                return;
            }
        }
        if (r == 0) {
            if (Clock::now() < deadline) continue;  // woke early; wait again
            if (parser.in_progress(buf)) {
                http::Response resp = make_error(408, "request timeout");
                resp.close = true;
                send_all(fd, http::serialize(resp, false, false));
                lingering_close(fd);
            } else {
                ::close(fd);  // idle between requests: close silently (RFC 9112 9.6)
            }
            return;
        }

        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n == 0) {  // client closed its side
            ::close(fd);
            return;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return;
        }
        buf.append(chunk.data(), static_cast<std::size_t>(n));
    }
}

}  // namespace server
