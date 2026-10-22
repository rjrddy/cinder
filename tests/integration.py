"""Real sockets, bounded history checking, crash recovery and hostile input gates."""

from __future__ import annotations

import concurrent.futures
import copy
import json
import os
import random
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from client import Client, RespError, frame, read

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.getenv("CINDER_BINARY", str(ROOT / "build/cinder"))).resolve()
RESULTS = Path(os.getenv("CINDER_RESULTS", str(ROOT / "docs/results")))


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Server:
    def __init__(self, path: Path, token: str = "", memory: int = 1048576) -> None:
        self.path, self.token, self.memory = path, token, memory
        self.port, self.http = free_port(), free_port()
        while self.port == self.http:
            self.http = free_port()
        self.process: subprocess.Popen | None = None
        self.log = (path.parent / "server.log").open("a")

    def start(self) -> None:
        self.process = subprocess.Popen(
            [
                str(BINARY),
                "--port",
                str(self.port),
                "--http-port",
                str(self.http),
                "--snapshot",
                str(self.path),
                "--memory",
                str(self.memory),
                "--web",
                str(ROOT / "web"),
            ],
            stdout=self.log,
            stderr=self.log,
            env={**os.environ, "CINDER_TOKEN": self.token},
        )
        for _ in range(200):
            if self.process.poll() is not None:
                raise RuntimeError(
                    "server exited; " + (self.path.parent / "server.log").read_text()
                )
            try:
                with Client(self.port, self.token) as client:
                    assert client.call("PING") == "PONG"
                return
            except OSError:
                time.sleep(0.025)
        raise RuntimeError("server readiness timeout")

    def stop(self, kill: bool = False) -> None:
        if self.process and self.process.poll() is None:
            self.process.kill() if kill else self.process.terminate()
            code = self.process.wait(timeout=8)
            if not kill and code != 0:
                raise RuntimeError(
                    "server failed at shutdown: " + (self.path.parent / "server.log").read_text()
                )

    def close(self) -> None:
        self.stop()
        self.log.close()

    def request(
        self,
        path: str,
        body: bytes | None = None,
        token: str | None = None,
        origin: str | None = None,
    ) -> bytes:
        headers = {"Content-Type": "application/octet-stream"}
        if token is not None:
            headers["Authorization"] = "Bearer " + token
        if origin:
            headers["Origin"] = origin
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.http}{path}", data=body, headers=headers
        )
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.read()


def linearizable(history: list[dict]) -> bool:
    """Bounded DFS over enabled operations; honor real-time precedence and results."""
    if len(history) > 10:
        raise ValueError("bounded checker: at most ten completed operations")
    predecessors = [
        {j for j, before in enumerate(history) if before["end"] < op["start"]} for op in history
    ]

    def search(done: frozenset[int], state: dict[str, str]) -> bool:
        if len(done) == len(history):
            return True
        for i, op in enumerate(history):
            if i in done or not predecessors[i].issubset(done):
                continue
            updated = state.copy()
            key, kind, actual = op["key"], op["kind"], op["result"]
            if kind == "SET":
                expected = "OK"
                updated[key] = op["value"]
            elif kind == "NX":
                expected = None if key in state else "OK"
                if expected:
                    updated[key] = op["value"]
            elif kind == "DEL":
                expected = int(key in state)
                updated.pop(key, None)
            elif kind == "INCR":
                value = state.get(key, "0")
                try:
                    expected = int(value) + 1
                    updated[key] = str(expected)
                except ValueError:
                    expected = "ERROR"
            else:
                expected = state.get(key)
            if actual == expected and search(done | {i}, updated):
                return True
        return False

    return search(frozenset(), {})


class Integration(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        RESULTS.mkdir(parents=True, exist_ok=True)

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.server = Server(Path(self.temp.name) / "state.snap")
        self.server.start()

    def tearDown(self) -> None:
        self.server.close()
        self.temp.cleanup()

    def test_binary_fragmentation_pipeline_and_expiry(self) -> None:
        with Client(self.server.port) as client:
            value = bytes(range(256))
            wire = frame(["SET", b"\x00key\r\n", value])
            for byte in wire:
                client.socket.sendall(bytes([byte]))
            self.assertEqual(read(client.stream), "OK")
            client.socket.sendall(frame(["GET", b"\x00key\r\n"]) + frame(["PING"]))
            self.assertEqual(read(client.stream), value)
            self.assertEqual(read(client.stream), "PONG")
            client.call("SET", "ttl", "v", "PX", "40")
            self.assertGreater(client.call("PTTL", "ttl"), 0)
            time.sleep(0.06)
            self.assertIsNone(client.call("GET", "ttl"))
            self.assertEqual(client.call("DEL", "ttl"), 0)

    def test_invalid_input_and_oom_do_not_mutate(self) -> None:
        failures = []
        with Client(self.server.port) as client:
            client.call("SET", "safe", "original")
            for args in [
                ("SET", "safe", "bad", "PX", "-1"),
                ("SET", "safe", "bad", "XX"),
                ("INCR", "safe"),
                ("SET", "safe", "x" * 40000),
                ("DEL", "safe", "other"),
            ]:
                with self.assertRaises(RespError) as caught:
                    client.call(*args)
                failures.append({"command": list(args[:2]), "caught": str(caught.exception)})
                self.assertEqual(client.call("GET", "safe"), b"original")
            for malformed in [
                b"*99\r\n",
                b"*1\r\n$-2\r\n",
                b"*1\r\n$9999999\r\n",
                b"*1\r\n$1\r\nx!!",
            ]:
                with socket.create_connection(("127.0.0.1", self.server.port)) as sock:
                    sock.sendall(malformed)
                    self.assertTrue(sock.recv(100).startswith(b"-ERR"))
            self.assertEqual(client.call("PING"), "PONG")
        (RESULTS / "caught-failures.json").write_text(json.dumps(failures, indent=2) + "\n")

    def test_snapshot_sigkill_recovery_and_corruption_gate(self) -> None:
        with Client(self.server.port) as client:
            client.call("SET", "durable", "synthetic")
            client.call("SET", "expiring", "v", "PX", "80")
            self.assertEqual(client.call("SAVE"), "OK")
            client.call("SET", "after-save", "not-durable")
        self.server.stop(kill=True)
        time.sleep(0.1)
        self.server.start()
        with Client(self.server.port) as client:
            self.assertEqual(client.call("GET", "durable"), b"synthetic")
            self.assertIsNone(client.call("GET", "expiring"))
            self.assertIsNone(client.call("GET", "after-save"))
        self.server.stop()
        data = bytearray(self.server.path.read_bytes())
        data[20] ^= 0x40
        self.server.path.write_bytes(data)
        with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
            self.server.start()
        (RESULTS / "corruption-rejected.log").write_text(
            (self.server.path.parent / "server.log").read_text()
        )

    def test_auth_origin_and_viewer_live_state(self) -> None:
        self.server.stop()
        self.server.token = "synthetic-test-token-32-characters"
        self.server.start()
        with Client(self.server.port) as anonymous:
            with self.assertRaises(RespError):
                anonymous.call("GET", "x")
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.server.request("/api/state")
        self.assertEqual(caught.exception.code, 401)
        with self.assertRaises(urllib.error.HTTPError) as caught:
            self.server.request(
                "/api/command",
                frame(["SET", "x", "v"]),
                self.server.token,
                "https://evil.invalid",
            )
        self.assertEqual(caught.exception.code, 403)
        response = self.server.request(
            "/api/command", frame(["SET", "viewer", "real"]), self.server.token
        )
        self.assertEqual(json.loads(response)["response"], "+OK\r\n")
        state = json.loads(self.server.request("/api/state", token=self.server.token))
        self.assertEqual(state["payload_bytes"], len("viewerreal"))
        self.assertTrue(any(row["key"] == "viewer" for row in state["sample"]))
        self.assertEqual(len(state["shards"]), 32)
        self.assertIn(b"Cinder", self.server.request("/"))

    def test_linearizable_histories_and_checker_negative_control(self) -> None:
        rng = random.Random(84017)
        histories = []
        for case in range(60):
            key = f"history:{case}"
            barrier = threading.Barrier(3)
            operations = [
                [rng.choice(["SET", "NX", "GET", "INCR", "DEL"]) for _ in range(3)]
                for _ in range(3)
            ]

            def worker(
                inputs: tuple[int, threading.Barrier, list[list[str]], list[int], str],
            ) -> list[dict]:
                index, barrier, operations, rng_value, key = inputs
                history = []
                with Client(self.server.port) as client:
                    barrier.wait(timeout=5)
                    for kind in operations[index]:
                        value = str(rng_value[index])
                        args = ["SET", key, value] if kind in {"SET", "NX"} else [kind, key]
                        if kind == "NX":
                            args.append("NX")
                        start = time.monotonic_ns()
                        try:
                            result = client.call(*args)
                            if isinstance(result, bytes):
                                result = result.decode()
                        except RespError:
                            result = "ERROR"
                        end = time.monotonic_ns()
                        history.append(
                            {
                                "kind": kind,
                                "key": key,
                                "value": value,
                                "result": result,
                                "start": start,
                                "end": end,
                            }
                        )
                return history

            rng_value = [rng.randrange(100) for _ in range(3)]
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
                history = [
                    op
                    for batch in executor.map(
                        worker, [(i, barrier, operations, rng_value, key) for i in range(3)]
                    )
                    for op in batch
                ]
            self.assertTrue(linearizable(history), history)
            histories.append(history)
        # Impossible completed read after completed write: a real-time stale-read bug.
        negative = [
            {
                "kind": "SET",
                "key": "x",
                "value": "7",
                "result": "OK",
                "start": 1,
                "end": 2,
            },
            {"kind": "GET", "key": "x", "result": "old", "start": 3, "end": 4},
        ]
        self.assertFalse(linearizable(negative))
        overlap = copy.deepcopy(negative)
        overlap[1].update(result=None, start=1, end=3)
        self.assertTrue(linearizable(overlap))
        (RESULTS / "histories.json").write_text(
            json.dumps(
                {
                    "seed": 84017,
                    "histories": histories,
                    "passed": len(histories),
                    "operations_each": 9,
                    "negative_control": negative,
                    "negative_control_rejected": True,
                    "overlapping_read_accepted": True,
                },
                indent=2,
            )
            + "\n"
        )


if __name__ == "__main__":
    RESULTS.mkdir(parents=True, exist_ok=True)
    unittest.main(verbosity=2)
