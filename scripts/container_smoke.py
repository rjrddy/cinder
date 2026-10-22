"""Linux CI container start, auth, snapshot, restart and live HTTP smoke."""

from __future__ import annotations

import json
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path

from client import Client

TOKEN = "synthetic-container-test-token-32"


def main() -> None:
    with tempfile.TemporaryDirectory() as folder:
        # Bind mount ownership for the nonroot container user.
        Path(folder).chmod(0o777)

        def start() -> None:
            subprocess.run(
                [
                    "docker",
                    "run",
                    "-d",
                    "--name",
                    "cinder-smoke",
                    "--network",
                    "host",
                    "-e",
                    "CINDER_TOKEN=" + TOKEN,
                    "-v",
                    folder + ":/data",
                    "cinder-ci",
                ],
                check=True,
            )
            for _ in range(100):
                try:
                    with Client(6380, TOKEN) as client:
                        assert client.call("PING") == "PONG"
                    return
                except OSError:
                    time.sleep(0.1)
            raise RuntimeError("container readiness")

        def stop() -> None:
            subprocess.run(["docker", "logs", "cinder-smoke"], check=False)
            subprocess.run(["docker", "rm", "-f", "cinder-smoke"], check=True)

        try:
            start()
            with Client(6380, TOKEN) as client:
                client.call("SET", "container", "synthetic")
                assert client.call("SAVE") == "OK"
            stop()
            start()
            with Client(6380, TOKEN) as client:
                assert client.call("GET", "container") == b"synthetic"
            request = urllib.request.Request(
                "http://127.0.0.1:8084/api/state", headers={"Authorization": "Bearer " + TOKEN}
            )
            with urllib.request.urlopen(request) as response:
                assert json.load(response)["resident_keys"] == 1
            print(json.dumps({"container_snapshot_restart": "pass", "live_viewer_state": "pass"}))
        finally:
            stop()


if __name__ == "__main__":
    main()
