#!/usr/bin/env python3
import os
import socket
import threading

SOCK = os.environ.get("FAKE_MEMCACHED_SOCK", "/sock/fake-memcached.sock")


def response_for(line: bytes) -> bytes:
    parts = line.rstrip(b"\r\n").split(b" ")
    if len(parts) < 2:
        return b"ERROR\r\n"

    cmd, key = parts[0], parts[1]

    if cmd == b"get":
        suffix = key.split(b":", 1)[-1]
        cases = {
            b"bad-empty": b"",
            b"bad-error": b"ERROR\r\n",
            b"bad-client-error": b"CLIENT_ERROR bad command line format\r\n",
            b"bad-no-crlf": b"VALUE app:bad-no-crlf 0 1\n",
            b"bad-short-value-line": b"VALUE\r\n",
            b"bad-missing-length": b"VALUE app:bad-missing-length 0\r\n",
            b"bad-nonnumeric-length": b"VALUE app:bad-nonnumeric-length 0 nope\r\n",
            b"bad-negative-length": b"VALUE app:bad-negative-length 0 -1\r\n",
            b"bad-wrong-key": b"VALUE app:other 0 1\r\nx\r\nEND\r\n",
            b"bad-garbage": b"\x00\x01not memcached\r\n",
        }
        return cases.get(suffix, b"END\r\n")

    if cmd == b"delete":
        return b"NOT_FOUND\r\n"

    if cmd == b"set":
        return b"STORED\r\n"

    return b"ERROR\r\n"


def handle(conn: socket.socket) -> None:
    with conn:
        data = b""
        while b"\n" not in data and len(data) < 4096:
            chunk = conn.recv(4096)
            if not chunk:
                break
            data += chunk
        if not data:
            return
        conn.sendall(response_for(data.split(b"\n", 1)[0] + b"\n"))


def main() -> None:
    os.makedirs(os.path.dirname(SOCK), exist_ok=True)
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass

    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(SOCK)
    os.chmod(SOCK, 0o777)
    srv.listen(128)

    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
