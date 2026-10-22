"""Exercise the real viewer, save responsive screenshots, optionally record a demo."""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

from playwright.sync_api import expect, sync_playwright

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from integration import Server  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args()
    out = Path("docs/results/browser").resolve()
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as folder:
        server = Server(Path(folder) / "demo.snap")
        server.start()
        errors: list[str] = []
        try:
            with sync_playwright() as p:
                browser = p.chromium.launch(
                    executable_path=os.getenv("CHROMIUM_PATH", p.chromium.executable_path)
                )
                context = browser.new_context(
                    viewport={"width": 1440, "height": 1050},
                    **(
                        {
                            "record_video_dir": str(out / "recording"),
                            "record_video_size": {"width": 1440, "height": 1050},
                        }
                        if args.record
                        else {}
                    ),
                )
                page = context.new_page()
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.goto(f"http://127.0.0.1:{server.http}")
                page.get_by_text("● LIVE / 1s POLL").wait_for()
                page.get_by_role("button", name="Seed synthetic keys").click()
                expect(page.locator("#keys")).to_have_text("64")
                page.get_by_label("Shard 0,", exact=False).click()
                page.screenshot(path=str(out / "desktop.png"), full_page=True)

                def execute(
                    op: str, key: str = "ember:01", value: str = "synthetic", ttl: str = ""
                ) -> None:
                    page.get_by_label("Operation", exact=True).select_option(op)
                    if op != "SAVE":
                        page.get_by_label("Key", exact=True).fill(key)
                    if op == "SET":
                        page.get_by_label("Value", exact=True).fill(value)
                        page.get_by_label("TTL in milliseconds", exact=False).fill(ttl)
                    page.get_by_role("button", name="Execute command", exact=False).click()
                    page.wait_for_timeout(400)

                execute("SET", value="first visible write")
                expect(page.locator("#rows")).to_contain_text("first visible write")
                execute("GET")
                assert "first visible write" in page.locator("#response").inner_text()
                execute("INCR")
                assert "not an integer" in page.locator("#response").inner_text()
                execute("SET", key="atomic:counter", value="0")
                for _ in range(3):
                    execute("INCR", key="atomic:counter")
                page.get_by_role("button", name="Write a short-lived key").click()
                expect(page.locator("#rows")).to_contain_text("ephemeral:ember")
                expect(page.locator("#rows")).not_to_contain_text("ephemeral:ember", timeout=10000)
                # Repeated real TTL lifecycles: inspect the selected shard while expiry changes actual state.
                if args.record:
                    for cycle in range(15):
                        execute(
                            "SET",
                            key=f"observe:{cycle}",
                            value=f"synthetic TTL lifecycle {cycle}",
                            ttl="4000",
                        )
                        execute("PTTL", key=f"observe:{cycle}")
                        page.get_by_label("Shard " + str(cycle) + ",", exact=False).click()
                        page.wait_for_timeout(4200)
                        execute("GET", key=f"observe:{cycle}")
                        assert "$-1" in page.locator("#response").inner_text()
                execute("SAVE")
                assert "+OK" in page.locator("#response").inner_text()
                sizes = []
                for width in [390, 768, 1024, 1440]:
                    page.set_viewport_size({"width": width, "height": 1000})
                    page.wait_for_timeout(100)
                    assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")
                    page.screenshot(path=str(out / f"width-{width}.png"), full_page=True)
                    sizes.append(width)
                page.set_viewport_size({"width": 390, "height": 900})
                page.get_by_label("Operation", exact=True).select_option("GET")
                page.get_by_label("Key", exact=True).fill("atomic:counter")
                page.get_by_role("button", name="Execute command", exact=False).click()
                page.wait_for_timeout(300)
                assert "$1" in page.locator("#response").inner_text()
                # Data stays text even if it looks like HTML.
                execute(
                    "SET", key="<script>alert(1)</script>", value="<img src=x onerror=alert(1)>"
                )
                assert page.locator("#rows img").count() == 0
                page.reload()
                page.get_by_text("● LIVE / 1s POLL").wait_for()
                assert not errors, errors
                video = page.video.path() if args.record else None
                context.close()
                browser.close()
                (out / "verification.json").write_text(
                    json.dumps(
                        {
                            "real_server": True,
                            "seeded_keys": 64,
                            "viewports_no_overflow": sizes,
                            "write_read": "pass",
                            "integer_error": "caught",
                            "ttl_disappearance": "pass",
                            "snapshot_save": "pass",
                            "html_rendered_as_text": True,
                            "page_errors": errors,
                            "video": str(video) if video else None,
                        },
                        indent=2,
                    )
                    + "\n"
                )
        finally:
            server.close()


if __name__ == "__main__":
    main()
