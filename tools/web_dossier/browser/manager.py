"""Playwright browser manager - persistent profile, human pace, no tricks.

Guardrails baked in on purpose:
- headful by default (the user may need to log in / solve captchas THEMSELVES)
- one page, one query at a time, minimum interval between queries
- persistent profile under the user's app-data dir; we never read or store
  passwords, never automate captchas, never rotate fingerprints/proxies
- on failure: screenshot + HTML + error.json under debug/web_dossier/ for
  selector fixes; cookies and auth headers are never written
"""
from __future__ import annotations

import datetime as _dt
import json
import os
import sys
import time
from pathlib import Path
from typing import Optional, Tuple

LOGIN_WAIT_TIMEOUT_SECONDS = 20 * 60   # the user gets 20 minutes, no loop
STEP_TIMEOUT_MS = 30_000


def default_profile_dir(provider: str) -> Path:
    home = Path.home()
    if sys.platform.startswith("win"):
        base = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming")) / "patX"
    elif sys.platform == "darwin":
        base = home / "Library" / "Application Support" / "patX"
    else:
        base = home / ".patx"
    return base / "browser-profiles" / provider


class BrowserManager:
    """Owns one persistent Chromium context for one provider."""

    def __init__(self, provider: str, headless: bool = False,
                 profile_dir: Optional[str] = None, debug_root: str = "debug/web_dossier"):
        self.provider = provider
        self.headless = headless
        self.profile_dir = Path(profile_dir) if profile_dir else default_profile_dir(provider)
        self.debug_root = Path(debug_root)
        self._pw = None
        self._context = None
        self._page = None

    # ---- lifecycle -----------------------------------------------------
    def _launch_channel(self):
        """Prefer browsers already installed on the machine (Chrome, then
        Edge - preinstalled on Windows) over Playwright's own Chromium build.
        No downloads, no version-pinned 300 MB cache. Falls back to the
        bundled build for portable/offline packages that ship one."""
        import sys
        last_error = None
        channels = ["chrome", "msedge"]
        if sys.platform == "darwin":
            channels = ["chrome"]          # no Edge on locked-down macOS
        for channel in channels:
            try:
                self._context = self._pw.chromium.launch_persistent_context(
                    str(self.profile_dir), headless=self.headless, channel=channel)
                return True
            except Exception as exc:       # channel not installed
                last_error = exc
        try:
            self._context = self._pw.chromium.launch_persistent_context(
                str(self.profile_dir), headless=self.headless)
            return True
        except Exception:
            raise last_error

    def launch(self):
        if self._context is not None:
            return True
        from playwright.sync_api import sync_playwright
        self.profile_dir.mkdir(parents=True, exist_ok=True)
        self._pw = sync_playwright().start()
        self._launch_channel()
        self._page = self._context.pages[0] if self._context.pages else self._context.new_page()
        return True

    def close(self):
        for closer in (self._context, self._pw):
            try:
                if closer is not None:
                    closer.close()
            except Exception:
                pass
        self._context = self._pw = self._page = None

    # ---- pages ---------------------------------------------------------
    def open_page(self, url: str, cancel) -> Optional[object]:
        """Navigates the single page; None on network failure or cancel."""
        try:
            self.launch()
            if cancel is not None and cancel.is_set():
                return None
            self._page.goto(url, timeout=STEP_TIMEOUT_MS, wait_until="domcontentloaded")
            return self._page
        except Exception:
            return None

    def click_first_text(self, page, texts: Tuple[str, ...], cancel) -> bool:
        """Clicks the first visible element matching any text, with role and
        text fallbacks. Returns False when nothing matched (structure drift)."""
        for text in texts:
            if cancel is not None and cancel.is_set():
                return False
            for locator in (page.get_by_role("tab", name=text),
                            page.get_by_role("link", name=text),
                            page.get_by_role("button", name=text),
                            page.get_by_text(text, exact=True)):
                try:
                    if locator.count() > 0 and locator.first.is_visible():
                        locator.first.click(timeout=STEP_TIMEOUT_MS)
                        page.wait_for_load_state("domcontentloaded",
                                                 timeout=STEP_TIMEOUT_MS)
                        return True
                except Exception:
                    continue
        return False

    # ---- login -----------------------------------------------------------
    def ensure_login(self, url: str, is_logged_in_fn, cancel) -> object:
        """Opens a VISIBLE browser at the login page and waits until the page
        looks authenticated. The user types credentials/captchas themselves;
        we only poll the page state - no password ever passes through us."""
        from ..models import ResultCode
        was_headless = self.headless
        self.headless = False   # authentication always happens headful
        try:
            page = self.open_page(url, cancel)
            if page is None:
                return ResultCode.NETWORK_ERROR
            deadline = time.monotonic() + LOGIN_WAIT_TIMEOUT_SECONDS
            while time.monotonic() < deadline:
                if cancel is not None and cancel.is_set():
                    return ResultCode.TEMPORARY_ERROR if not is_logged_in_fn(
                        page.content()) else ResultCode.OK
                try:
                    if is_logged_in_fn(page.content()):
                        return ResultCode.OK
                except Exception:
                    pass
                time.sleep(3)
            return ResultCode.AUTH_REQUIRED
        finally:
            # keep headful for this session; next provider start may go headless
            self.headless = was_headless

    # ---- diagnostics -------------------------------------------------------
    def capture_debug_artifacts(self, page, error_code: str, case_no: str):
        """screenshot + page.html + error.json for selector fixes. Never
        stores cookies, storage state, or auth headers."""
        try:
            stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
            safe_case = "".join(c for c in case_no if c.isalnum() or c in "._-")[:40] or "case"
            folder = self.debug_root / f"{stamp}_{self.provider}_{safe_case}"
            folder.mkdir(parents=True, exist_ok=True)
            try:
                page.screenshot(path=str(folder / "screenshot.png"), full_page=True)
            except Exception:
                pass
            html = ""
            try:
                html = page.content()
            except Exception:
                pass
            (folder / "page.html").write_text(html, encoding="utf-8")
            meta = {
                "timestamp": stamp,
                "provider": self.provider,
                "case": case_no,
                "error_code": error_code,
                "page_title": (page.title() if page else "")[:200],
            }
            (folder / "error.json").write_text(
                json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
        except Exception:
            pass   # diagnostics must never break the sync flow
