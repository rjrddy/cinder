"""Minimal binary-safe RESP2 client; no third-party runtime dependencies."""

from __future__ import annotations

import argparse
import json
import os
import socket
from typing import BinaryIO


class RespError(Exception):
    """Server returned an error response."""


def frame(args: list[str | bytes]) -> bytes:
    parts = [a.encode() if isinstance(a, str) else a for a in args]
    return b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n" % len(p) + p + b"\r\n" for p in parts)


def read(stream: BinaryIO) -> str | bytes | int | None | list:
    line = stream.readline()
    if not line.endswith(b"\r\n"):
        raise ConnectionError("incomplete response")
    kind, value = line[:1], line[1:-2]
    if kind == b"-":
        raise RespError(value.decode())
    if kind == b"+":
        return value.decode()
    if kind == b":":
        return int(value)
    if kind == b"$":
        length = int(value)
        if length == -1:
            return None
        body = stream.read(length)
        if len(body) != length or stream.read(2) != b"\r\n":
            raise ConnectionError("incomplete bulk response")
        return body
    if kind == b"*":
        return [read(stream) for _ in range(int(value))]
    raise ConnectionError("unknown response")


class Client:
    def __init__(self, port: int = 6380, token: str = "") -> None:
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.stream = self.socket.makefile("rb")
        if token:
            self.call("AUTH", token)

    def call(self, *args: str | bytes) -> str | bytes | int | None | list:
        self.socket.sendall(frame(list(args)))
        return read(self.stream)

    def close(self) -> None:
        self.stream.close()
        self.socket.close()

    def __enter__(self) -> Client:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=6380)
    parser.add_argument("command", nargs="+")
    options = parser.parse_args()
    with Client(options.port, os.getenv("CINDER_TOKEN", "")) as client:
        result = client.call(*options.command)
        print(
            json.dumps(
                result.decode(errors="backslashreplace") if isinstance(result, bytes) else result
            )
        )
