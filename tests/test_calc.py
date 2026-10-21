"""Integration tests for the keep-alive calculator (Python standard library only).

The suite starts ./calc on a free port with --idle-timeout 1 and talks to it
over raw sockets, reading every response by its exact Content-Length so that
framing mistakes on either side show up as failures.

Run with:  make test   (or: python3 -I tests/test_calc.py -v)
"""

import socket
import subprocess
import threading
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY = ROOT / "calc"
IDLE_TIMEOUT = 1.0
SOCKET_TIMEOUT = 5.0
INT64_MAX = 2**63 - 1
INT64_MIN = -(2**63)


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def req(method, target, *, host="localhost", version="HTTP/1.1", headers=(), body=b""):
    """Build a raw request. host=None leaves the Host header out."""
    lines = [f"{method} {target} {version}"]
    if host is not None:
        lines.append(f"Host: {host}")
    lines.extend(headers)
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body


def get(target, **kwargs):
    return req("GET", target, **kwargs)


# The handout's six marking requests with expected status and (for 200) body.
MARKER = [
    (get("/add?a=2&b=3"), 200, b"5"),
    (get("/sub?a=10&b=4"), 200, b"6"),
    (get("/mul?a=6&b=7"), 200, b"42"),
    (get("/div?a=1&b=0"), 400, b"division by zero"),
    (get("/pow?a=2&b=8"), 404, None),
    (req("POST", "/add", headers=["Content-Length: 0"]), 405, None),
]


class Response:
    def __init__(self, version, status, reason, headers, body):
        self.version = version
        self.status = status
        self.reason = reason
        self.headers = headers  # lowercased name -> value (last one wins)
        self.body = body

    def header(self, name):
        return self.headers.get(name.lower())

    def __repr__(self):
        return f"<Response {self.status} {self.reason!r} {self.body!r}>"


class Client:
    """One TCP connection plus a receive buffer, framing responses by Content-Length."""

    def __init__(self, port, host="127.0.0.1", family=None):
        if family is None:
            self.sock = socket.create_connection((host, port), timeout=SOCKET_TIMEOUT)
        else:
            self.sock = socket.socket(family, socket.SOCK_STREAM)
            self.sock.settimeout(SOCKET_TIMEOUT)
            self.sock.connect((host, port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def close(self):
        self.sock.close()

    def send(self, data):
        self.sock.sendall(data)

    def send_bytewise(self, data, delay=0.001):
        for i in range(len(data)):
            self.sock.sendall(data[i : i + 1])
            time.sleep(delay)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError("server closed the connection")
        self.buf += chunk

    def read_response(self, head_only=False):
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        version, status, reason = lines[0].split(" ", 2)
        headers = {}
        for line in lines[1:]:
            name, _, value = line.partition(":")
            headers[name.strip().lower()] = value.strip()
        length = 0 if head_only else int(headers.get("content-length", "0"))
        while len(self.buf) < length:
            self._fill()
        body, self.buf = self.buf[:length], self.buf[length:]
        return Response(version, int(status), reason, headers, body)

    def request(self, data, head_only=False):
        self.send(data)
        return self.read_response(head_only)

    def at_eof(self, timeout=3.0):
        """True if the server closes the connection (with no further bytes) within timeout."""
        if self.buf:
            return False
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(1)
        except TimeoutError:
            return False
        except ConnectionResetError:
            return True
        finally:
            self.sock.settimeout(SOCKET_TIMEOUT)
        if data:
            self.buf += data
            return False
        return True

    def still_open(self, wait=0.2):
        """True if nothing arrives (no data, no EOF) within `wait` seconds."""
        if self.buf:
            return False
        self.sock.settimeout(wait)
        try:
            data = self.sock.recv(1)
        except TimeoutError:
            return True
        except OSError:
            return False
        finally:
            self.sock.settimeout(SOCKET_TIMEOUT)
        self.buf += data
        return False


class CalculatorTest(unittest.TestCase):
    proc = None
    port = None

    @classmethod
    def setUpClass(cls):
        if not BINARY.exists():
            raise RuntimeError(f"{BINARY} not found; run 'make' first")
        cls.port = free_port()
        cls.proc = subprocess.Popen(
            [str(BINARY), str(cls.port), "--idle-timeout", str(IDLE_TIMEOUT)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 5.0
        while True:
            if cls.proc.poll() is not None:
                raise RuntimeError(f"calc exited early with status {cls.proc.returncode}")
            try:
                socket.create_connection(("127.0.0.1", cls.port), timeout=0.2).close()
                break
            except OSError:
                if time.monotonic() > deadline:
                    cls.proc.kill()
                    cls.proc.wait()
                    raise
                time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        if cls.proc is not None:
            cls.proc.terminate()
            try:
                cls.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.proc.kill()
                cls.proc.wait()

    # ------------------------------------------------------------ helpers

    def client(self, host="127.0.0.1", family=None):
        c = Client(self.port, host, family)
        self.addCleanup(c.close)
        return c

    def assert_status(self, resp, status, body=None):
        self.assertEqual(resp.status, status, resp)
        if body is not None:
            self.assertEqual(resp.body, body, resp)
        self.assertEqual(resp.header("content-length"), str(len(resp.body)))

    def assert_open_and_working(self, c):
        """The connection still serves requests afterwards."""
        self.assert_status(c.request(get("/add?a=1&b=1")), 200, b"2")

    def assert_error_then_closed(self, c, data, status):
        """Send data, expect `status` with Connection: close, then EOF."""
        c.send(data)
        resp = c.read_response()
        self.assert_status(resp, status)
        self.assertEqual(resp.header("connection"), "close", resp)
        self.assertTrue(c.at_eof(), "server should close after a framing error")

    # ------------------------------------------------------------ tests

    def test_01_marker_sequence_on_one_socket(self):
        c = self.client()
        for data, status, body in MARKER:
            self.assert_status(c.request(data), status, body)
        self.assert_status(c.request(get("/div?a=4&b=2")), 200, b"2")
        self.assertTrue(c.still_open())

    def test_02_arithmetic(self):
        cases = [
            ("/add?a=2&b=3", b"5"),
            ("/sub?a=10&b=4", b"6"),
            ("/mul?a=6&b=7", b"42"),
            ("/div?a=9&b=3", b"3"),
            ("/add?a=-5&b=3", b"-2"),
            ("/sub?a=3&b=10", b"-7"),
            ("/mul?a=-4&b=-5", b"20"),
            ("/mul?a=-4&b=5", b"-20"),
            ("/div?a=7&b=2", b"3"),
            ("/div?a=-7&b=2", b"-3"),
            ("/div?a=7&b=-2", b"-3"),
            ("/div?a=0&b=5", b"0"),
            ("/add?a=%2B5&b=1", b"6"),
            ("/add?a=+5&b=1", b"6"),
            ("/add?a=-0&b=0", b"0"),
            ("/add?a=007&b=1", b"8"),
            ("/add?b=3&a=2", b"5"),
        ]
        c = self.client()
        for target, body in cases:
            with self.subTest(target=target):
                resp = c.request(get(target))
                self.assert_status(resp, 200, body)
                self.assertEqual(resp.header("content-type"), "text/plain; charset=utf-8")
                self.assertEqual(resp.version, "HTTP/1.1")
                self.assertIsNotNone(resp.header("date"))
        resp = c.request(get("/add?a=2&b=3"))
        self.assertEqual(resp.header("content-length"), "1")  # no trailing newline

    def test_03_bad_numbers_are_400_and_keep_connection(self):
        bad = [
            "a=x&b=3", "a=1.5&b=3", "a=&b=3", "a&b=3", "a=%zz&b=3", "a=1%20&b=3",
            "a=%20&b=3", "a=1e3&b=3", "a=0x10&b=3", "a=--1&b=3", "a=+-1&b=3",
            "a=%2&b=3", "a=1+&b=3", "a=%2B&b=3", "a=-&b=3", "a=3&b=four",
        ]
        c = self.client()
        for query in bad:
            with self.subTest(query=query):
                resp = c.request(get("/add?" + query))
                self.assert_status(resp, 400)
                self.assertNotEqual(resp.header("connection"), "close")
        self.assert_open_and_working(c)

    def test_04_parameters(self):
        c = self.client()
        self.assert_status(c.request(get("/add?a=1")), 400, b"missing parameter b")
        self.assert_status(c.request(get("/add?b=1")), 400, b"missing parameter a")
        self.assert_status(c.request(get("/add")), 400)
        self.assert_status(c.request(get("/add?a=1&a=2&b=3")), 400, b"duplicate parameter a")
        self.assert_status(c.request(get("/add?a=1&b=2&c=1")), 200, b"3")
        self.assert_status(c.request(get("/add?&a=1&&b=2&")), 200, b"3")
        self.assert_status(c.request(get("/add?%61=1&b=2")), 200, b"3")
        self.assert_open_and_working(c)

    def test_05_overflow_and_range(self):
        bad = [
            f"/add?a={INT64_MAX}&b=1",
            f"/add?a={INT64_MIN}&b=-1",
            f"/sub?a={INT64_MIN}&b=1",
            f"/sub?a=0&b={INT64_MIN}",
            f"/mul?a={2**62}&b=2",
            f"/mul?a={INT64_MIN}&b=-1",
            f"/div?a={INT64_MIN}&b=-1",
            f"/add?a={INT64_MAX + 1}&b=0",
            f"/add?a={INT64_MIN - 1}&b=0",
            "/add?a=99999999999999999999999&b=0",
        ]
        good = [
            (f"/add?a={INT64_MAX}&b=0", str(INT64_MAX).encode()),
            (f"/add?a={INT64_MIN}&b=0", str(INT64_MIN).encode()),
            (f"/mul?a={2**62}&b=-2", str(INT64_MIN).encode()),
            (f"/div?a={INT64_MIN}&b=1", str(INT64_MIN).encode()),
            (f"/sub?a=-1&b={INT64_MAX}", str(INT64_MIN).encode()),
        ]
        c = self.client()
        for target in bad:
            with self.subTest(target=target):
                self.assert_status(c.request(get(target)), 400)
        for target, body in good:
            with self.subTest(target=target):
                self.assert_status(c.request(get(target)), 200, body)

    def test_06_host_header(self):
        c = self.client()
        resp = c.request(get("/add?a=2&b=3", host=None))
        self.assert_status(resp, 400, b"missing Host header")
        self.assertNotEqual(resp.header("connection"), "close")
        self.assert_open_and_working(c)
        resp = c.request(get("/add?a=2&b=3", headers=["Host: other"]))
        self.assert_status(resp, 400, b"duplicate Host header")
        self.assert_open_and_working(c)

    def test_07_404_versus_405(self):
        c = self.client()
        self.assert_status(c.request(get("/pow?a=2&b=8")), 404)
        self.assert_status(c.request(req("POST", "/pow", headers=["Content-Length: 0"])), 404)
        resp = c.request(req("PUT", "/add?a=1&b=2", headers=["Content-Length: 0"]))
        self.assert_status(resp, 405)
        self.assertEqual(resp.header("allow"), "GET, HEAD")
        self.assert_status(c.request(get("/ADD?a=1&b=2")), 404)
        self.assert_status(c.request(get("/add/?a=1&b=2")), 404)
        self.assert_status(c.request(req("DELETE", "/add")), 405)
        self.assert_open_and_working(c)

    def test_08_head(self):
        c = self.client()
        c.send(req("HEAD", "/add?a=2&b=3"))
        resp = c.read_response(head_only=True)
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.header("content-length"), "1")
        self.assertTrue(c.still_open(), "HEAD response must not carry a body")
        self.assert_status(c.request(get("/mul?a=6&b=7")), 200, b"42")

    def test_09_request_split_one_byte_at_a_time(self):
        c = self.client()
        c.send_bytewise(get("/add?a=2&b=3"))
        self.assert_status(c.read_response(), 200, b"5")

    def test_10_six_pipelined_in_one_send(self):
        c = self.client()
        c.send(b"".join(data for data, _, _ in MARKER))
        for _, status, body in MARKER:
            self.assert_status(c.read_response(), status, body)
        self.assertTrue(c.still_open())

    def test_11_post_body_consumed_before_next_request(self):
        c = self.client()
        c.send(req("POST", "/add", headers=["Content-Length: 5"], body=b"hello") + get("/add?a=2&b=3"))
        self.assert_status(c.read_response(), 405)
        self.assert_status(c.read_response(), 200, b"5")

    def test_12_get_with_body(self):
        c = self.client()
        c.send(
            get("/add?a=2&b=3", headers=["Content-Length: 4"]) + b"abcd" + get("/mul?a=6&b=7")
        )
        self.assert_status(c.read_response(), 200, b"5")
        self.assert_status(c.read_response(), 200, b"42")

    def test_13_chunked_body(self):
        body = b"5;name=value\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: yes\r\n\r\n"
        data = req("POST", "/add", headers=["Transfer-Encoding: chunked"]) + body
        data += get("/add?a=2&b=3")
        with self.subTest("one send"):
            c = self.client()
            c.send(data)
            self.assert_status(c.read_response(), 405)
            self.assert_status(c.read_response(), 200, b"5")
        with self.subTest("byte by byte"):
            c = self.client()
            c.send_bytewise(data)
            self.assert_status(c.read_response(), 405)
            self.assert_status(c.read_response(), 200, b"5")

    def test_14_bad_chunked_framing_closes(self):
        head = req("POST", "/add", headers=["Transfer-Encoding: chunked"])
        cases = {
            "bad size": b"zz\r\nhello\r\n0\r\n\r\n",
            "missing CRLF after data": b"5\r\nhelloXX0\r\n\r\n",
            "junk after size": b"5 x\r\nhello\r\n0\r\n\r\n",
        }
        for name, body in cases.items():
            with self.subTest(name):
                self.assert_error_then_closed(self.client(), head + body, 400)

    def test_15_transfer_encoding_rules(self):
        both = req("POST", "/add", headers=["Transfer-Encoding: chunked", "Content-Length: 5"])
        self.assert_error_then_closed(self.client(), both + b"5\r\nhello\r\n0\r\n\r\n", 400)
        gzip = req("POST", "/add", headers=["Transfer-Encoding: gzip"])
        self.assert_error_then_closed(self.client(), gzip, 501)

    def test_16_bad_content_length_closes(self):
        cases = {
            "abc": ["Content-Length: abc"],
            "-1": ["Content-Length: -1"],
            "1 2": ["Content-Length: 1 2"],
            "differing duplicates": ["Content-Length: 5", "Content-Length: 6"],
        }
        for name, headers in cases.items():
            with self.subTest(name):
                data = req("POST", "/add", headers=headers, body=b"hello")
                self.assert_error_then_closed(self.client(), data, 400)
        with self.subTest("too large"):
            data = req("POST", "/add", headers=["Content-Length: 2097152"])
            self.assert_error_then_closed(self.client(), data, 413)
        with self.subTest("identical duplicates are fine"):
            c = self.client()
            headers = ["Content-Length: 5", "Content-Length: 5"]
            c.send(req("POST", "/add", headers=headers, body=b"hello"))
            self.assert_status(c.read_response(), 405)
            self.assert_open_and_working(c)

    def test_17_content_length_too_small_desyncs(self):
        # CL says 5, but "hello world" is sent: " world" becomes the start of
        # the next request, whose request line is then malformed.
        c = self.client()
        c.send(
            req("POST", "/add", headers=["Content-Length: 5"], body=b"hello world")
            + get("/add?a=2&b=3")
        )
        self.assert_status(c.read_response(), 405)
        resp = c.read_response()
        self.assert_status(resp, 400)
        self.assertEqual(resp.header("connection"), "close")
        self.assertTrue(c.at_eof())

    def test_18_header_too_large(self):
        data = get("/add?a=2&b=3", headers=["X-Big: " + "a" * 10240])
        self.assert_error_then_closed(self.client(), data, 431)

    def test_19_connection_close(self):
        c = self.client()
        resp = c.request(get("/add?a=2&b=3", headers=["Connection: close"]))
        self.assert_status(resp, 200, b"5")
        self.assertEqual(resp.header("connection"), "close")
        self.assertTrue(c.at_eof())

    def test_20_http10(self):
        with self.subTest("default close, no Host needed"):
            c = self.client()
            resp = c.request(b"GET /add?a=2&b=3 HTTP/1.0\r\n\r\n")
            self.assert_status(resp, 200, b"5")
            self.assertEqual(resp.version, "HTTP/1.1")
            self.assertEqual(resp.header("connection"), "close")
            self.assertTrue(c.at_eof())
        with self.subTest("keep-alive"):
            c = self.client()
            data = b"GET /add?a=2&b=3 HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"
            for _ in range(2):
                resp = c.request(data)
                self.assert_status(resp, 200, b"5")
                self.assertEqual(resp.header("connection"), "keep-alive")
            self.assertTrue(c.still_open())

    def test_21_bad_versions(self):
        self.assert_error_then_closed(self.client(), get("/add?a=2&b=3", version="HTTP/2.0"), 505)
        self.assert_error_then_closed(self.client(), get("/add?a=2&b=3", version="HTTP/1.1x"), 400)

    def test_22_leading_crlf_tolerated(self):
        c = self.client()
        self.assert_status(c.request(b"\r\n\r\n" + get("/add?a=2&b=3")), 200, b"5")
        self.assert_status(c.request(b"\r\n" + get("/mul?a=6&b=7")), 200, b"42")

    def test_23_idle_timeout(self):
        with self.subTest("silent close when idle"):
            c = self.client()
            time.sleep(0.5)
            self.assertTrue(c.still_open(0.1), "closed too early")
            time.sleep(1.0)
            self.assertTrue(c.at_eof(1.0), "idle connection should be closed without a response")
        with self.subTest("408 for a partial request"):
            c = self.client()
            start = time.monotonic()
            c.send(b"GET /add?a=2")
            resp = c.read_response()
            self.assertGreaterEqual(time.monotonic() - start, 0.8)
            self.assert_status(resp, 408)
            self.assertEqual(resp.header("connection"), "close")
            self.assertTrue(c.at_eof())
        with self.subTest("timer restarts after each response"):
            c = self.client()
            for _ in range(3):
                self.assert_status(c.request(get("/add?a=2&b=3")), 200, b"5")
                time.sleep(0.7)
            self.assert_status(c.request(get("/add?a=2&b=3")), 200, b"5")

    def test_24_ipv4_loopback(self):
        c = self.client("127.0.0.1", socket.AF_INET)
        self.assert_status(c.request(get("/add?a=2&b=3")), 200, b"5")

    def test_24_ipv6_loopback(self):
        if not socket.has_ipv6:
            self.skipTest("Python built without IPv6")
        try:
            c = self.client("::1", socket.AF_INET6)
        except OSError as e:
            self.skipTest(f"IPv6 loopback unavailable: {e}")
        self.assert_status(c.request(get("/add?a=2&b=3", host="[::1]")), 200, b"5")

    def test_25_concurrent_connections(self):
        idle = self.client()  # an idle socket must not block anyone else
        errors = []

        def worker(tid):
            try:
                c = Client(self.port)
                try:
                    for i in range(20):
                        resp = c.request(get(f"/add?a={i}&b={tid}"))
                        if resp.status != 200 or resp.body != str(i + tid).encode():
                            errors.append(f"thread {tid} request {i}: {resp!r}")
                finally:
                    c.close()
            except Exception as e:  # noqa: BLE001 - report everything
                errors.append(f"thread {tid}: {e!r}")

        threads = [threading.Thread(target=worker, args=(t,)) for t in range(20)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=30)
        self.assertEqual(errors, [])
        self.assertTrue(all(not t.is_alive() for t in threads))
        idle.close()

    def test_26_expect_100_continue(self):
        c = self.client()
        c.send(req("POST", "/add", headers=["Content-Length: 5", "Expect: 100-continue"]))
        interim = c.read_response()
        self.assertEqual(interim.status, 100)
        c.send(b"hello")
        self.assert_status(c.read_response(), 405)
        self.assert_open_and_working(c)


if __name__ == "__main__":
    unittest.main()
