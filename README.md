# calc: an HTTP/1.1 keep-alive calculator

A small integer calculator served over HTTP/1.1, written in C++17 directly on
POSIX sockets. There is no HTTP library or framework: the server reads bytes
from a TCP socket, works out where each request ends, and answers on the same
connection for as long as the client wants to keep it open.

```
GET /add?a=2&b=3  -> 200 5        GET /div?a=1&b=0  -> 400
GET /sub?a=10&b=4 -> 200 6        GET /add?a=x&b=3  -> 400
GET /mul?a=6&b=7  -> 200 42       GET /pow?a=2&b=8  -> 404
GET /div?a=9&b=3  -> 200 3        POST /add         -> 405
                                  GET /add (no Host) -> 400
```

All the stretch goals are implemented: `Connection: close`, an idle timeout,
chunked request bodies and pipelining.

## Build and run

Requirements: a C++17 compiler (Apple clang on macOS is the default) and
Python 3 (standard library only) for the tests.

```sh
make                 # builds ./calc with -Wall -Wextra -Wpedantic
./calc               # listens on port 8080, idle timeout 10 s
./calc 9000          # another port
./calc --port 9000 --idle-timeout 2.5
```

On start the server prints one line to stderr, for example
`listening on [::]:8080 (idle-timeout 10s)`. It listens on a dual-stack IPv6
socket, so `localhost`, `127.0.0.1` and `::1` all work. If IPv6 is not
available it falls back to `0.0.0.0`.

| Option | Meaning |
|---|---|
| `[port]` or `--port P` | TCP port, 1 to 65535 (default 8080) |
| `--idle-timeout S` | seconds a connection may sit idle; any positive number, fractions allowed (default 10) |

## Tests

```sh
make test            # builds, starts its own server on a free port, runs 27 tests
make mark            # runs the handout's marking script against a running server
PORT=9000 make mark  # same, on another port
```

`make test` runs `tests/test_calc.py` (unittest). It starts `./calc` on a free
port with `--idle-timeout 1` and reads every response by its exact
`Content-Length`, so a framing bug on either side makes a test fail rather than
hang. It covers the marking sequence, arithmetic edge cases, every error status,
pipelining, byte-at-a-time delivery, chunked bodies, the close policy, the idle
timeout, IPv4 and IPv6, and 20 concurrent keep-alive clients.

`make mark` needs a server that is already running (start `./calc` in another
terminal first). It opens one TCP connection, sends the six marking requests on
it, and ends with:

```
GET /add?a=2&b=3 -> 200 5
GET /sub?a=10&b=4 -> 200 6
GET /mul?a=6&b=7 -> 200 42
GET /div?a=1&b=0 -> 400 division by zero
GET /pow?a=2&b=8 -> 404 not found
POST /add -> 405 method not allowed
socket still open: True, 1 TCP handshake, 6 responses
```

## Trying it by hand

curl reuses one connection for several URLs given in a single command; the
verbose output shows `Re-using existing connection` and `left intact`:

```sh
curl -s -v "http://localhost:8080/add?a=2&b=3" "http://localhost:8080/mul?a=6&b=7"
```

With netcat (on macOS, `nc -c` sends CRLF line endings; type a blank line to
finish each request):

```sh
nc -c localhost 8080
GET /add?a=2&b=3 HTTP/1.1
Host: localhost

```

Pipelining, six requests in one write, answered in order on one connection:

```sh
printf 'GET /add?a=2&b=3 HTTP/1.1\r\nHost: x\r\n\r\nGET /sub?a=10&b=4 HTTP/1.1\r\nHost: x\r\n\r\nGET /mul?a=6&b=7 HTTP/1.1\r\nHost: x\r\n\r\nGET /div?a=1&b=0 HTTP/1.1\r\nHost: x\r\n\r\nGET /pow?a=2&b=8 HTTP/1.1\r\nHost: x\r\n\r\nPOST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n' | nc localhost 8080
```

A chunked body (the POST gets 405, then the GET behind it still gets 200):

```sh
printf 'POST /add HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\nGET /add?a=2&b=3 HTTP/1.1\r\nHost: x\r\n\r\n' | nc localhost 8080
```

## API

| Request | Result |
|---|---|
| `GET /add?a=A&b=B` | `A + B` |
| `GET /sub?a=A&b=B` | `A - B` |
| `GET /mul?a=A&b=B` | `A * B` |
| `GET /div?a=A&b=B` | `A / B`, truncated toward zero (`7/2 = 3`, `-7/2 = -3`) |
| `HEAD` on any of the above | same status and headers, no body |

- Numbers are signed 64-bit integers: an optional `+` or `-`, then one or more
  digits, nothing else. `1.5`, `1e3`, `0x10`, ` 1` and an empty value are 400.
- Query values are percent-decoded (`%2B5` is `+5`). A `+` is not turned into a
  space; a malformed escape such as `%zz` is 400.
- `a` and `b` must each appear exactly once (missing or repeated is 400). Other
  parameters and empty pairs (`&&`) are ignored.
- Results that do not fit in 64 bits are 400 `overflow`, including
  `INT64_MIN / -1`. Division by zero is 400 `division by zero`.
- A 200 body is the decimal result with no trailing newline (`5` has
  `Content-Length: 1`). Error bodies are a short lowercase reason.
- Every response carries `Date`, `Server: calc/1.0`,
  `Content-Type: text/plain; charset=utf-8` and `Content-Length`. 405 adds
  `Allow: GET, HEAD`.
- Paths are exact and case-sensitive: `/ADD` and `/add/` are 404. An unknown
  path is 404 whatever the method, because 405 would claim the resource exists.

## How request boundaries are found

This is the hard part of keep-alive: one TCP connection is a single byte
stream, and a `recv()` can return half a request, exactly one, or several
glued together. The server never assumes one read equals one request.

Each connection owns a byte buffer and an incremental parser
(`RequestParser` in `src/http.cpp`). New bytes are appended to the buffer, and
the parser consumes complete pieces from its front. Whatever it does not
consume stays in the buffer as the start of the next request.

1. **Head.** Empty lines before a request line are skipped (RFC 9112 2.2). The
   parser then looks for the blank line (`\r\n\r\n`) that ends the head. Each
   search resumes where the last one stopped (minus 3 bytes, in case the
   terminator was split across two reads), so each byte is examined only a
   constant number of times, however the request is split up. If the
   head is not complete within 8 KiB the answer is 431. Only CRLF line endings
   are accepted; a bare CR, bare LF or other control character in the head is
   400.
2. **Body framing**, decided from the headers (RFC 9112 6.3):
   - `Transfer-Encoding` and `Content-Length` together: 400 and close (the
     classic request-smuggling ambiguity).
   - `Transfer-Encoding: chunked` (exactly that): read chunks until the
     zero-size chunk and the trailer section. Any other transfer coding is 501.
   - `Content-Length: N`: take exactly N bytes. Byte N+1 belongs to the next
     request.
   - Neither header: the request has no body.
3. **Chunked bodies** use a small state machine: size line (1 to 16 hex digits,
   optional `;extension` ignored), data, the CRLF after the data, then trailer
   lines up to an empty line. Chunk data is appended as it arrives.
4. When the request is complete the parser hands it over and resets. The server
   answers it, then calls the parser again *before* reading from the socket, so
   pipelined requests already in the buffer are answered in order without
   waiting for more input.

The server only reads more from the socket when the parser says it needs more,
so the buffer stays bounded (8 KiB head + 1 MiB body + one 64 KiB read).

A `Content-Length` that is smaller than the bytes actually sent shows why this
matters: the surplus is treated as the start of the next request, which is then
malformed, so the client gets 400 and the connection is closed. The tests check
exactly that.

## Close policy

The connection stays open unless the table says otherwise. Errors about the
*content* of a well-framed request keep it open; errors that make the framing
untrustworthy close it, because the server can no longer tell where the next
request starts.

| Situation | Status | Closes? |
|---|---|---|
| bad number, division by zero, overflow, missing or duplicate `a`/`b`, bad percent-encoding | 400 | no |
| missing `Host` (HTTP/1.1), or more than one `Host` | 400 | no |
| request target not starting with `/` | 400 | no |
| unknown path | 404 | no |
| known path, method not GET or HEAD (body consumed first) | 405 + `Allow` | no |
| malformed request line, header line or version token | 400 | yes |
| HTTP major version other than 1 | 505 | yes |
| `Content-Length` not all digits, conflicting duplicates, or both `Transfer-Encoding` and `Content-Length` | 400 | yes |
| `Content-Length` or chunked body over 1 MiB | 413 | yes |
| `Transfer-Encoding` other than exactly `chunked` | 501 | yes |
| head or trailer section over 8 KiB | 431 | yes |
| bad chunk size line, or missing CRLF after chunk data | 400 | yes |
| idle timeout with a partial request buffered | 408 | yes |
| idle timeout with nothing buffered | none | yes, silently |
| `Connection: close`, or HTTP/1.0 without `Connection: keep-alive` | normal response | yes, after the response |

Every response sent just before a close carries `Connection: close`. Closing is
"lingering": the server half-closes its side (`shutdown(SHUT_WR)`), then reads
and discards anything the client is still sending for up to 1 second before
closing the socket. Closing a socket with unread input makes the kernel send a
TCP RST, which can destroy the error response before the client reads it.

HTTP/1.0 requests are answered with an `HTTP/1.1` status line, as RFC 9110
allows. They close by default; with `Connection: keep-alive` the response says
`Connection: keep-alive` and the connection stays open.

## Idle timeout: why this design

There is one timer per connection. It starts when the connection is accepted
and restarts each time a response is sent. Bytes arriving in the middle of a
request do not restart it.

- **Timer expires with nothing buffered:** the connection is closed without a
  response. That is normal keep-alive behaviour (RFC 9112 9.6); clients must
  expect an idle connection to be closed and simply open a new one.
- **Timer expires with a partial request buffered:** the client gets
  `408 Request Timeout` with `Connection: close`, so it knows why.
- **Why 10 seconds by default:** long enough that a client sending a burst of
  requests, or a person typing into `nc`, keeps its connection; short enough
  that idle or deliberately slow connections cannot tie up threads for long.
  With one thread per connection, a "slowloris" client that trickles one byte
  at a time would otherwise hold a thread forever. Because bytes inside a
  request do not restart the timer, each request has to arrive within the
  window.
- `--idle-timeout` changes the value (the tests use 1 second). It must be
  greater than zero.

The same value is also used as the socket send timeout, so a client that sends
requests but never reads the responses cannot block a thread in `send()`
forever.

## Limits

| Limit | Value | Over the limit |
|---|---|---|
| request line + headers | 8 KiB | 431, close |
| request body (Content-Length or chunked total) | 1 MiB | 413, close |
| one chunk-size line | 8 KiB | 400, close |
| trailer section | 8 KiB | 431, close |
| idle time between requests | 10 s (configurable) | close (408 if a request was half sent) |
| integers | signed 64-bit | 400 |

## Design decisions

- **No HTTP library.** Everything above the socket API is in `src/`:
  `main.cpp` (arguments, listening socket, accept loop), `server.cpp`
  (connection loop, dispatch, close policy), `http.cpp` (parser and
  serializer) and `calc.cpp` (query parsing and checked arithmetic).
- **Strict parsing.** CRLF only, no obsolete line folding, no whitespace
  before a header colon, exact version syntax. Being lenient is how
  request-smuggling bugs start; the cost of strictness here is nothing.
- **404 before 405.** 405 means "this resource exists but not with that
  method", so it is only used for the four real paths.
- **HEAD** is supported because RFC 9110 requires it wherever GET is.
- **`Expect: 100-continue`** gets an interim `100 Continue` when the body has
  not arrived yet (HTTP/1.1 only, as RFC 9110 requires).
- **HTTP/1.x minor versions above 1** (for example `HTTP/1.2`) are handled as
  HTTP/1.1 (RFC 9110 2.5). 505 is only for a different major version.
- **More than one `Host` header** is 400 in any version; a missing `Host` is
  only an error for HTTP/1.1 (RFC 9112 3.2).
- **Checked arithmetic** uses the compiler's `__builtin_*_overflow` helpers,
  so overflow is reported rather than being undefined behaviour.
- **`SIGPIPE` is ignored** and `SO_NOSIGPIPE` is set on every accepted socket, so a
  client that disconnects mid-response cannot kill the server.
- **`TCP_NODELAY`** is set on accepted sockets, so small responses are not
  held back by Nagle's algorithm while the client waits for them.

## Concurrency

Each accepted connection gets its own detached `std::thread` running the
connection loop. Threads share nothing mutable (the configuration is copied),
so no locks are needed. A blocked or idle client only ever holds its own
thread, and the idle timeout bounds how long that lasts. The tests run 20
keep-alive clients in parallel next to an idle connection.

## Known limitations

- Only origin-form request targets (`/path?query`). Absolute-form
  (`http://host/path`) and `*` get 400.
- No cap on the number of simultaneous connections; each one costs a thread.
  A flood of connections is limited only by the idle timeout and OS limits.
- No TLS; plain HTTP only.
- No HTTP/2 and no `Upgrade`.
- Request bodies are read and discarded (no endpoint takes a body), but they
  still count against the 1 MiB limit.
- Chunk-size lines are limited to 16 hex digits, so a size with extra leading
  zeros beyond that is rejected.
- The server runs until it is killed (Ctrl+C or `kill`); there is no graceful
  shutdown that waits for open connections.
