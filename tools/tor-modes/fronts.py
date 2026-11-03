#!/usr/bin/env python3
"""Fronts for the "Use Tor for" test: which node did the wallet ask, and for what.

  fronts.py http  PORT UPSTREAM LOG [HOLD]   the direct node: an HTTP front for UPSTREAM
  fronts.py socks PORT UPSTREAM LOG          Tor: a SOCKS 4, 4a and 5 front; every target is UPSTREAM

Both log one line per request (`<path>`, and `connect <host:port>` for SOCKS) and refuse a
TLS ClientHello, so wallet2 falls back to plain HTTP and its paths can be read. While the
file HOLD exists, the http front holds each `.bin` request. Prints the port it listens on.
"""
import os
import socket
import struct
import sys
import threading
import time


def log_to(path):
    lock = threading.Lock()

    def log(line):
        with lock, open(path, "a") as f:
            f.write(line + "\n")
    return log


def read_head(sock, buf):
    while b"\r\n\r\n" not in buf:
        data = sock.recv(65536)
        if not data:
            return None, buf
        buf += data
    end = buf.index(b"\r\n\r\n") + 4
    return buf[:end], buf[end:]


def read_exact(sock, buf, n):
    while len(buf) < n:
        data = sock.recv(65536)
        if not data:
            raise EOFError
        buf += data
    return buf[:n], buf[n:]


def content_length(head):
    for line in head.split(b"\r\n")[1:]:
        key, _, value = line.partition(b":")
        if key.strip().lower() == b"content-length":
            return int(value.strip())
    return 0


def relay_http(client, upstream, log, hold=None, cbuf=b""):
    peek = cbuf[:1] or client.recv(1, socket.MSG_PEEK)
    if not peek:
        return
    if peek[0] == 0x16:
        log("tls refused")
        return
    host, port = upstream.rsplit(":", 1)
    up = socket.create_connection((host, int(port)), timeout=120)
    ubuf = b""
    try:
        while True:
            head, cbuf = read_head(client, cbuf)
            if head is None:
                return
            body, cbuf = read_exact(client, cbuf, content_length(head))
            path = head.split(b" ", 2)[1].decode("latin-1")
            log(path)
            while hold and path.endswith(".bin") and os.path.exists(hold):
                time.sleep(0.2)
            up.sendall(head + body)
            rhead, ubuf = read_head(up, ubuf)
            if rhead is None:
                return
            rbody, ubuf = read_exact(up, ubuf, content_length(rhead))
            client.sendall(rhead + rbody)
    finally:
        up.close()


def socks_target(c):
    """The SOCKS handshake; answers success and returns (`host:port` asked for, bytes read past it)."""
    buf = b""

    def take(n):
        nonlocal buf
        out, buf = read_exact(c, buf, n)
        return out

    def until_nul():
        nonlocal buf
        while b"\x00" not in buf:
            data = c.recv(65536)
            if not data:
                raise EOFError
            buf += data
        out, _, buf = buf.partition(b"\x00")
        return out

    ver = take(1)
    if ver == b"\x04":
        cmd, port, ip = struct.unpack("!BH4s", take(7))
        until_nul()  # user id
        host = until_nul().decode() if ip[:3] == b"\x00\x00\x00" and ip[3] != 0 else socket.inet_ntoa(ip)
        c.sendall(b"\x00\x5a" + b"\x00" * 6)
        return f"{host}:{port}", buf
    if ver == b"\x05":
        take(take(1)[0])
        c.sendall(b"\x05\x00")
        _, _, _, atyp = take(4)
        if atyp == 1:
            host = socket.inet_ntoa(take(4))
        elif atyp == 3:
            host = take(take(1)[0]).decode()
        else:
            host = socket.inet_ntop(socket.AF_INET6, take(16))
        port = struct.unpack("!H", take(2))[0]
        c.sendall(b"\x05\x00\x00\x01" + b"\x00" * 6)
        return f"{host}:{port}", buf
    raise ValueError("not SOCKS")


def serve(kind, port, upstream, log_path, hold):
    log = log_to(log_path)
    open(log_path, "a").close()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(64)
    print(srv.getsockname()[1], flush=True)

    def one(c):
        try:
            rest = b""
            if kind == "socks":
                target, rest = socks_target(c)
                log("connect " + target)
            relay_http(c, upstream, log, hold if kind == "http" else None, rest)
        except Exception as e:  # a test front: report, never crash
            log(f"error {type(e).__name__}")
        finally:
            c.close()

    while True:
        c, _ = srv.accept()
        threading.Thread(target=one, args=(c,), daemon=True).start()


if __name__ == "__main__":
    kind, port, upstream, log_path = sys.argv[1:5]
    serve(kind, int(port), upstream, log_path, sys.argv[5] if len(sys.argv) > 5 else None)
