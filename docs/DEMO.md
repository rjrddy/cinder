# Record a ninety-second Cinder demo

Use only synthetic values. Prerequisites: C++20 compiler, CMake, Python, Node for lint. From a fresh clone, `make demo` installs locked developer tools, builds the native executable, seeds synthetic keys and prints the local viewer URL. This starts the real C++ process, not a mock service. Ctrl-C stops the owned child. The optional environment token is kept in the viewer tab only.

Suggested recording timeline (these are instructions, not measured performance):

- 0–15 seconds: open the viewer; point out resident payload versus RSS, select a shard, inspect keys, bucket count and collision chain.
- 15–30 seconds: SET `ember:01` to `synthetic payload`, GET it, then INCR it. Explain the actual integer-validation error and show the old value was retained.
- 30–45 seconds: SET `counter` to `0`, INCR it, show its changing value and shard.
- 45–60 seconds: write a short-lived key with the preset; inspect its TTL, then observe disappearance from the actual engine.
- 60–75 seconds: SAVE, then show `python scripts/client.py GET counter` against the same native RESP listener.
- 75–90 seconds: show docs/results/benchmark/summary.json and the Redis comparison, then the corruption rejection and bounded checker negative control. State the finite scope and snapshot durability gap.

Optional automated recorder: install requirements-browser.txt into the local virtualenv, run `.venv/bin/python -m playwright install chromium ffmpeg`, then `.venv/bin/python scripts/review.py --record`. It starts and stops its own server and exercises repeated actual TTL lifecycles. CHROMIUM_PATH and PLAYWRIGHT_FFMPEG_PATH can point to already installed compatible tools. Video is written under docs/results/browser/recording/. Verify the actual duration using ffprobe; do not pad a short recording.

Docker alternative: set CINDER_TOKEN to your own random token with at least 24 characters, then `docker compose up --build`; enter the token in the viewer. The snapshot volume survives container replacement on the same Docker host. See docs/DEPLOY.md before considering hosting.
