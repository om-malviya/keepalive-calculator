// Entry point: argument parsing, listening socket and accept loop.
// One detached thread serves each accepted connection.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

#include "server.hpp"

namespace {

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [port] [--port P] [--idle-timeout SECONDS]\n"
              << "  port defaults to 8080, idle timeout to 10 seconds (must be > 0)\n";
}

bool parse_port(const char* text, int& port) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) return false;
    port = static_cast<int>(value);
    return true;
}

bool parse_timeout(const char* text, double& seconds) {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !std::isfinite(value) || value <= 0) {
        return false;
    }
    seconds = value;
    return true;
}

// Dual-stack IPv6 socket on [::]:port (also accepts IPv4); falls back to
// IPv4-only 0.0.0.0:port if IPv6 is unavailable. Returns -1 on failure.
int open_listener(int port, std::string& label) {
    const int on = 1;
    const int off = 0;

    int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd >= 0) {
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in6 addr{};
#ifdef SIN6_LEN
        addr.sin6_len = sizeof addr;
#endif
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(static_cast<std::uint16_t>(port));
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
            label = "[::]:" + std::to_string(port);
            return fd;
        }
        ::close(fd);
    }

    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::perror("bind");
        ::close(fd);
        return -1;
    }
    label = "0.0.0.0:" + std::to_string(port);
    return fd;
}

void configure_client_socket(int fd, double idle_timeout) {
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    // A client that never reads cannot block a thread in send() forever.
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(idle_timeout);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((idle_timeout - static_cast<double>(tv.tv_sec)) * 1e6);
    if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_usec = 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8080;
    double idle_timeout = 10.0;
    bool have_positional = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port") {
            if (++i >= argc || !parse_port(argv[i], port)) {
                usage(argv[0]);
                return 2;
            }
        } else if (arg == "--idle-timeout") {
            if (++i >= argc || !parse_timeout(argv[i], idle_timeout)) {
                usage(argv[0]);
                return 2;
            }
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (!have_positional && !arg.empty() && arg[0] != '-' &&
                   parse_port(arg.c_str(), port)) {
            have_positional = true;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    // Writing to a socket the peer has closed must return EPIPE, not kill us.
    std::signal(SIGPIPE, SIG_IGN);

    std::string label;
    const int listen_fd = open_listener(port, label);
    if (listen_fd < 0) return 1;
    if (::listen(listen_fd, 128) != 0) {
        std::perror("listen");
        return 1;
    }
    std::cerr << "listening on " << label << " (idle-timeout " << idle_timeout << "s)"
              << std::endl;

    const server::Config cfg{idle_timeout};
    for (;;) {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            const int err = errno;
            if (err == EINTR || err == ECONNABORTED) continue;
            std::cerr << "accept: " << std::strerror(err) << std::endl;
            if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        configure_client_socket(fd, idle_timeout);
        try {
            std::thread(server::handle_connection, fd, cfg).detach();
        } catch (const std::system_error& e) {
            std::cerr << "thread: " << e.what() << std::endl;
            ::close(fd);
        }
    }
}
