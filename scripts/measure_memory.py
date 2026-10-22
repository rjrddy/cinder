"""Observe payload accounting and process RSS independently; no RSS-limit claim."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from client import Client  # noqa: E402
from integration import Server  # noqa: E402


def main() -> None:
    out = Path("docs/results")
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as folder:
        server = Server(Path(folder) / "state.snap", memory=64 * 1024 * 1024)
        server.start()
        try:

            def observe() -> dict:
                with urllib.request.urlopen(
                    f"http://127.0.0.1:{server.http}/api/state"
                ) as response:
                    state = json.load(response)
                return {
                    "payload_bytes": state["payload_bytes"],
                    "resident_keys": state["resident_keys"],
                    "rss_kib": int(
                        subprocess.check_output(
                            ["ps", "-o", "rss=", "-p", str(server.process.pid)], text=True
                        )
                    ),
                    "hash_buckets": sum(s["buckets"] for s in state["shards"]),
                }

            empty = observe()
            with Client(server.port) as client:
                for i in range(10000):
                    assert client.call("SET", f"mem:{i:06}", "x" * 128) == "OK"
                filled = observe()
                assert filled["resident_keys"] == 10000
                assert filled["payload_bytes"] == sum(
                    len(f"mem:{i:06}") + 128 for i in range(10000)
                )
                for i in range(10000):
                    assert client.call("DEL", f"mem:{i:06}") == 1
            deleted = observe()
            assert deleted["payload_bytes"] == 0 and deleted["resident_keys"] == 0
            result = {
                "empty": empty,
                "filled": filled,
                "deleted": deleted,
                "notes": "Point-in-time ps RSS in KiB; includes native workers, allocator, buckets and buffers. Payload cap is not an RSS cap. Erase need not return allocator memory or shrink buckets.",
            }
            (out / "memory.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result))
        finally:
            server.close()


if __name__ == "__main__":
    main()
