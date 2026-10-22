"""One-command local synthetic demonstration. Ctrl-C stops only the owned child."""

from __future__ import annotations

import os
import signal
import subprocess
import time
from pathlib import Path

from client import Client

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    (ROOT / ".cache").mkdir(exist_ok=True)
    port = int(os.getenv("CINDER_PORT", "6380"))
    http = int(os.getenv("CINDER_HTTP_PORT", "8084"))
    process = subprocess.Popen(
        [
            str(ROOT / "build/cinder"),
            "--port",
            str(port),
            "--http-port",
            str(http),
            "--snapshot",
            str(ROOT / ".cache/demo.snap"),
            "--web",
            str(ROOT / "web"),
        ]
    )

    def stop(signum: int, _: object) -> None:
        process.send_signal(signum)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        for _ in range(100):
            if process.poll() is not None:
                raise RuntimeError("Cinder exited before readiness")
            try:
                with Client(port, os.getenv("CINDER_TOKEN", "")) as client:
                    for i in range(64):
                        client.call("SET", f"synthetic:{i:02}", f"payload-{i}")
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError("readiness timed out")
        print(f"Viewer: http://127.0.0.1:{http} · RESP: {port} · synthetic keys seeded", flush=True)
        process.wait()
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=8)


if __name__ == "__main__":
    main()
