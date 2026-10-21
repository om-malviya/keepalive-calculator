// Per-connection loop (buffering, idle timeout, pipelining, close policy) and
// request dispatch.
#pragma once

#include "http.hpp"

namespace server {

struct Config {
    double idle_timeout_seconds = 10.0;
};

// Maps a complete request to a response. Never asks for the connection to be
// closed: 400/404/405 produced here keep the connection open.
http::Response handle(const http::Request& req);

// "Connection: close", or HTTP/1.0 without "Connection: keep-alive".
bool client_wants_close(const http::Request& req);

// Serves one accepted socket until it is closed; takes ownership of fd.
void handle_connection(int fd, Config cfg);

}  // namespace server
