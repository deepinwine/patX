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

LOGIN_WAIT_TIMEOUT_SECONDS = float(os.environ.get("PATX_LOGIN_WAIT", str(20 * 60)))
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
        self._browser = None
        self._context = None
        self._page = None
        self._spawned = None

    # ---- lifecycle -----------------------------------------------------
    def _chrome_candidates(self):
        """Real browsers installed on this machine, most preferred first.
        The executable may be overridden with PATX_CHROME / CPQUERY_CHROME."""
        import sys
        override = os.environ.get("PATX_CHROME") or os.environ.get("CPQUERY_CHROME")
        if override:
            return [override]
        if sys.platform.startswith("win"):
            return [
                r"C:\Program Files\Google\Chrome\Application\chrome.exe",
                r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
                r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
            ]
        if sys.platform == "darwin":
            return [
                "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                "/Applications/Chromium.app/Contents/MacOS/Chromium",
                "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
            ]
        return ["google-chrome", "chromium-browser", "chromium", "microsoft-edge"]

    def _attach(self, port, timeout_ms=2000):
        self._browser = self._pw.chromium.connect_over_cdp(
            f"http://127.0.0.1:{port}", timeout=timeout_ms)
        self._context = self._browser.contexts[0] if self._browser.contexts else             self._browser.new_context()
        self._adopt_page()
        return True

    def _adopt_page(self):
        """Prefer the tab actually on the cpquery origin; the SPA and the
        identity platform open several tabs and the spawned browser starts
        with a blank one."""
        pages = [p for p in self._context.pages if not p.is_closed()]
        for p in pages:
            try:
                if "cpquery" in p.url:
                    self._page = p
                    return
            except Exception:
                continue
        for p in pages:
            try:
                if p.url and p.url != "about:blank":
                    self._page = p
                    return
            except Exception:
                continue
        self._page = pages[-1] if pages else self._context.new_page()

    def has_cpquery_token(self) -> bool:
        """True when the adopted tab sits on the cpquery origin with an
        ACCESS_TOKEN - the only state that counts as logged in."""
        try:
            if "cpquery" not in self._page.url:
                return False
            return bool(self._page.evaluate("localStorage.getItem('ACCESS_TOKEN')"))
        except Exception:
            return False

    def _spawn_and_attach(self):
        """Launch a REAL browser ourselves with a debug port, then attach via
        CDP - identical to how the field-tested takeover works. The browser
        carries no automation flags (navigator.webdriver stays false, no
        --enable-automation), which is the only shape the Ruishu-protected
        site serves; a Playwright-spawned browser gets blocked."""
        import subprocess
        import sys
        import time as _time
        import socket as _socket
        exe_found = None
        for exe in self._chrome_candidates():
            if os.path.isfile(exe):
                exe_found = exe
                break
            from shutil import which
            if which(exe):
                exe_found = exe
                break
        if exe_found is None:
            return False
        for port in range(9333, 9341):
            with _socket.socket() as probe:
                if probe.connect_ex(("127.0.0.1", port)) == 0:
                    continue          # occupied by something else
            self._spawned = subprocess.Popen(
                [exe_found, f"--remote-debugging-port={port}",
                 f"--user-data-dir={self.profile_dir}",
                 "--no-first-run", "--no-default-browser-check", "about:blank"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = _time.monotonic() + 20
            while _time.monotonic() < deadline:
                try:
                    return self._attach(port, timeout_ms=3000)
                except Exception:
                    _time.sleep(0.5)
            return False
        return False

    def launch(self):
        if self._page is not None:
            return True
        from playwright.sync_api import sync_playwright
        self.profile_dir.mkdir(parents=True, exist_ok=True)
        self._pw = sync_playwright().start()
        # 1) a browser from a previous run may still be up with our profile -
        #    attach to it (keeps the logged-in session warm)
        for port in range(9333, 9341):
            try:
                return self._attach(port, timeout_ms=1500)
            except Exception:
                continue
        # 2) spawn a real browser and attach (takeover mode)
        if self._spawn_and_attach():
            return True
        # 3) last resort: Playwright's bundled Chromium (works for friendly
        #    sites and fixtures; the Ruishu-protected site will refuse it)
        self._context = self._pw.chromium.launch_persistent_context(
            str(self.profile_dir), headless=self.headless)
        self._adopt_page()
        return True

    def close(self):
        # Detach first; then close a browser we spawned ourselves (the login
        # session lives in the profile dir, so closing keeps it).
        for closer in (self._browser, self._pw):
            try:
                if closer is not None:
                    closer.close()
            except Exception:
                pass
        if self._spawned is not None:
            try:
                self._spawned.terminate()
            except Exception:
                pass
        self._browser = self._context = self._pw = self._page = None
        self._spawned = None

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
    def ensure_login(self, url: str, is_logged_in_fn, cancel,
                     login_entry_selectors=()) -> object:
        """Opens a VISIBLE browser and waits until the user completes their
        own login. The QR scan happens on a SEPARATE identity domain and may
        land the session on any tab, so every tab is polled. We only click
        the login ENTRY (navigation); credentials/captcha stay with the user.
        No password ever passes through us."""
        from ..models import ResultCode
        try:
            self.launch()
        except Exception:
            return ResultCode.NETWORK_ERROR
        if is_logged_in_fn(self._page):
            return ResultCode.OK
        try:
            page = self.open_page(url, cancel)
        except Exception:
            page = None
        if page is None:
            return ResultCode.NETWORK_ERROR

        # navigate to the identity login (user takes it from there)
        for sel in login_entry_selectors:
            try:
                loc = page.locator(sel)
                if loc.count() and loc.first.is_visible():
                    loc.first.click(timeout=5000)
                    page.wait_for_timeout(2000)
                    break
            except Exception:
                continue

        deadline = time.monotonic() + LOGIN_WAIT_TIMEOUT_SECONDS
        debug = os.environ.get("PATX_LOGIN_DEBUG") == "1"
        last_shot = 0.0
        n = 0
        while time.monotonic() < deadline:
            if cancel is not None and cancel.is_set():
                return ResultCode.TEMPORARY_ERROR
            try:
                pages = [p for p in self._context.pages if not p.is_closed()]
            except Exception:
                pages = []
            if debug:
                n += 1
                if n % 2 == 1:   # every other poll (~6s): state line
                    try:
                        print("[login-wait] tabs: " + " | ".join(
                            (p.url or "?")[:60] for p in pages), file=sys.stderr, flush=True)
                        for p in pages:
                            if "cpquery" in (p.url or ""):
                                keys = p.evaluate("Object.keys(localStorage).join(',')")
                                print(f"[login-wait] cpquery localStorage keys: {keys}",
                                      file=sys.stderr, flush=True)
                    except Exception:
                        pass
                if time.monotonic() - last_shot > 20:
                    last_shot = time.monotonic()
                    try:
                        self.capture_debug_artifacts(self._page, "LOGIN_WAIT", "poll")
                    except Exception:
                        pass
            for p in pages:
                try:
                    if is_logged_in_fn(p):
                        self._page = p      # adopt the authenticated tab
                        return ResultCode.OK
                except Exception:
                    continue
            time.sleep(3)
        return ResultCode.AUTH_REQUIRED

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
