"""Reproduces the handout's marking script.

Opens ONE TCP connection to localhost, sends the six marking requests on it
one after another, reads each response by its Content-Length, then checks
that the server has not closed the socket.

Usage:  python3 tests/marker.py [PORT]      (or PORT=8081 make mark)
The server must already be running. Exit status 0 means everything passed.
"""

import os
import socket
import sys

# method, target, expected status, expected body (None = do not check)
REQUESTS = [
    ("GET", "/add?a=2&b=3", 200, b"5"),
    ("GET", "/sub?a=10&b=4", 200, b"6"),
    ("GET", "/mul?a=6&b=7", 200, b"42"),
    ("GET", "/div?a=1&b=0", 400, None),
    ("GET", "/pow?a=2&b=8", 404, None),
    ("POST", "/add", 405, None),
]


def read_response(sock, buf):
    """Returns (status, body, leftover bytes)."""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            raise ConnectionError("server closed the connection")
        buf += chunk
    head, buf = buf.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split(" ", 2)[1])
    length = 0
    for line in lines[1:]:
        name, _, value = line.partition(":")
        if name.strip().lower() == "content-length":
            length = int(value.strip())
    while len(buf) < length:
        chunk = sock.recv(65536)
        if not chunk:
            raise ConnectionError("server closed the connection mid-body")
        buf += chunk
    return status, buf[:length], buf[length:]


def socket_still_open(sock):
    """Nothing arrives within 0.5 s (no EOF, no stray bytes): the socket is open."""
    sock.settimeout(0.5)
    try:
        sock.recv(1)
    except TimeoutError:
        return True
    except OSError:
        return False
    return False  # EOF or unexpected bytes


def main():
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    else:
        port = int(os.environ.get("PORT") or 8080)

    handshakes = 0
    responses = 0
    all_ok = True
    still_open = False

    try:
        s = socket.create_connection(("localhost", port), timeout=5)
    except OSError as e:
        print(f"cannot connect to localhost:{port}: {e}")
        print("socket still open: False, 0 TCP handshakes, 0 responses")
        return 1
    handshakes += 1

    with s:
        buf = b""
        try:
            for method, target, expected_status, expected_body in REQUESTS:
                extra = "Content-Length: 0\r\n" if method == "POST" else ""
                s.sendall(f"{method} {target} HTTP/1.1\r\nHost: localhost\r\n{extra}\r\n".encode())
                status, body, buf = read_response(s, buf)
                responses += 1
                print(f"{method} {target} -> {status} {body.decode('utf-8', 'replace')}")
                if status != expected_status or (
                    expected_body is not None and body != expected_body
                ):
                    all_ok = False
            still_open = not buf and socket_still_open(s)
        except (OSError, ValueError) as e:
            print(f"connection failed: {e}")
            all_ok = False

    plural = "" if handshakes == 1 else "s"
    print(f"socket still open: {still_open}, {handshakes} TCP handshake{plural}, {responses} responses")
    return 0 if all_ok and still_open and responses == len(REQUESTS) else 1


if __name__ == "__main__":
    sys.exit(main())
