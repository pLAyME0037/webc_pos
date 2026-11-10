#!/usr/bin/env python3
"""ImageUpload browser suite - drives the photo upload flow in a real browser.

Why this exists: js/ImageUpload.js (img-icon button, live preview, crop
modal, client-side compression) is invisible to every HTML-scraping suite -
the same gap that hid the disabled GeoCascade selects. This suite picks an
actual file over the DevTools protocol and walks the whole flow:

  big noise PNG -> compressed under the server's 2 MB cap -> preview shown
  -> crop modal: zoom + drag -> Apply -> 512x512 file in the input ->
  submit -> row in the DB with a real webp upload -> edit page wrapper ->
  GIF left untouched.

SKIPs loudly (exit 0) when chromium or the `websocket` module is missing.
"""
import os
import shutil
import sqlite3
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testlib import Client, Checker  # noqa: E402
# Browser/find_chromium/websocket_mod are import-safe (geo test has a
# __main__ guard); its module level only reads the same HOST/PORT/TMP argv.
from geo_cascade_test import Browser, find_chromium, websocket_mod  # noqa: E402

HOST = sys.argv[1]
PORT = int(sys.argv[2])
TMP = sys.argv[3]
DB = os.path.join(TMP, "nested", "deep", "db")
PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPLOADS_STATE = os.path.join(TMP, "uploads.txt")

# Server cap + multipart margin - must match IU_MAX_BYTES in ImageUpload.js.
SERVER_CAP = 2 * 1024 * 1024

t = Checker()


# ------------------------------------------------------------- test files

def make_noise_png(path, w=1600, h=1200):
    """RGB noise PNG (~5.7 MB): incompressible, so the canvas JPEG pass has
    something real to shrink and the no-gain shortcut cannot mask a bug."""
    raw = b"".join(b"\x00" + os.urandom(w * 3) for _ in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", ihdr)
                + chunk(b"IDAT", zlib.compress(raw, 1))
                + chunk(b"IEND", b""))


def make_gif(path):
    """1x1 GIF, bytes from the GIF89a spec example."""
    data = bytes.fromhex(
        "47494638396101000100800000000000ffffff21f9040100000000"
        "2c00000000010001000002024401003b")
    with open(path, "wb") as f:
        f.write(data)


# ---------------------------------------------------------------- helpers

def wait_js(b, expr, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if b.eval_js(expr):
                return True
        except Exception:
            pass
        time.sleep(0.2)
    return False


def set_file(b, path):
    """Hand the file input a real file, like the native picker would."""
    doc = b.cmd("DOM.getDocument", {"depth": -1})
    node = b.cmd("DOM.querySelector", {
        "nodeId": doc["root"]["nodeId"],
        "selector": "[data-img-upload] input[type=file]"})
    if not node.get("nodeId"):
        raise RuntimeError("file input not found in the DOM")
    b.cmd("DOM.setFileInputFiles", {"files": [path],
                                    "nodeId": node["nodeId"]})
    # Deterministic: fire change ourselves regardless of what CDP did.
    b.eval_js("""(() => {
        const i = document.querySelector('[data-img-upload] input[type=file]');
        i.dispatchEvent(new Event('change', { bubbles: true }));
        return true;
    })()""")


INPUT_JS = "document.querySelector('[data-img-upload] input[type=file]')"
SPAN_JS = "document.querySelector('[data-img-upload] [data-img-name]')"
IMG_JS = "document.querySelector('[data-img-upload] [data-img-preview]')"
TRANSFORM_JS = ("document.querySelector('[data-iu-img]').style.transform")


# ------------------------------------------------------------------ main

def main():
    exe = find_chromium()
    ws_mod = websocket_mod()
    if not exe or not ws_mod:
        why = []
        if not exe:
            why.append("no chromium/chrome on PATH")
        if not ws_mod:
            why.append("python websocket module not installed")
        print("SKIP: img_upload_test needs a real browser (%s)"
              % "; ".join(why))
        return

    base = f"http://{HOST}:{PORT}"
    c = Client(HOST, PORT)
    st, _ = c.login_as("sd.staff1", "posadmin1")
    t.chk("login", st in (200, 302, 303), True)

    # The browser must execute the ImageUpload.js on disk: /js/* is served
    # from build/bundle.h (compiled in), so a stale binary silently serves
    # old JS and no scraping suite would notice.
    import hashlib
    import urllib.request
    served = urllib.request.urlopen(f"{base}/js/ImageUpload.js",
                                    timeout=15).read()
    with open(os.path.join(PROJECT, "js", "ImageUpload.js"), "rb") as fh:
        on_disk = fh.read()
    t.chk("browser will execute the ImageUpload.js on disk",
          hashlib.md5(served).hexdigest(), hashlib.md5(on_disk).hexdigest())

    src = os.path.join(TMP, "imgup_src.png")
    gif = os.path.join(TMP, "imgup_anim.gif")
    make_noise_png(src)
    make_gif(gif)
    src_size = os.path.getsize(src)
    t.chk("noise PNG bigger than the server cap", src_size > SERVER_CAP, True)

    marker = "IMGUP%d" % int(time.time())
    with Browser(exe) as b:
        b.set_cookies(c.cookies, base)
        b.goto(f"{base}/users")

        # ---- wrapper is wired by ImageUpload.js ------------------------
        wired = b.eval_js("""(() => {
            const w = document.querySelector('[data-img-upload]');
            if (!w) return null;
            return { ready: w.dataset.iuReady === '1',
                     accept: (w.querySelector('input[type=file]').accept || ''),
                     script: !!document.querySelector('script[src="/js/ImageUpload.js"]') };
        })()""")
        t.chk("wrapper present", wired is not None, True)
        if not wired:
            return
        t.chk("wrapper wired on DOMContentLoaded", wired["ready"], True)
        t.chk("script tag in page", wired["script"], True)
        t.chk("accept stays image-only", "image/png" in wired["accept"], True)

        # ---- pick the file: compression must kick in -------------------
        set_file(b, src)
        t.chk("compression replaced the file under the cap",
              wait_js(b, f"""(() => {{
                  const i = {INPUT_JS};
                  return i.files.length === 1 && i.files[0].size < {SERVER_CAP}
                      && i.files[0].size < {src_size};
              }})()"""), True)
        t.chk("name line shows the size trade",
              wait_js(b, f"{SPAN_JS}.textContent.includes('\\u2192')"), True)
        t.chk("preview visible",
              b.eval_js(f"""(() => {{
                  const i = {IMG_JS};
                  return !i.classList.contains('hidden')
                      && i.src.startsWith('blob:')
                      && i.complete && i.naturalWidth > 0;
              }})()"""), True)
        t.chk("placeholder icon hidden",
              b.eval_js("""document.querySelector(
                  '[data-img-upload] [data-img-placeholder]'
              ).classList.contains('hidden')"""), True)
        t.chk("crop button revealed",
              b.eval_js("""(() => {
                  const c = document.querySelector('[data-img-upload] [data-img-crop]');
                  return !c.classList.contains('hidden');
              })()"""), True)

        # ---- crop modal: zoom then drag -------------------------------
        b.eval_js("""(() => {
            document.querySelector('[data-img-upload] [data-img-crop]').click();
            return true;
        })()""")
        t.chk("crop modal opens with the image loaded",
              wait_js(b, """(() => {
                  const m = document.querySelector('[data-iu-viewport]');
                  if (!m) return false;
                  const img = document.querySelector('[data-iu-img]');
                  return !m.closest('.fixed').classList.contains('hidden')
                      && img.complete && img.naturalWidth > 0;
              })()"""), True)

        before = b.eval_js(TRANSFORM_JS)
        # Zoom first: at zoom=1 a square image exactly fills the square
        # viewport and has zero pan freedom - zooming creates it.
        b.eval_js("""(() => {
            const z = document.querySelector('[data-iu-zoom]');
            z.value = 200;
            z.dispatchEvent(new Event('input', { bubbles: true }));
            return true;
        })()""")
        zoomed = b.eval_js(TRANSFORM_JS)
        t.chk("zoom slider rescales the image", zoomed != before, True)

        rect = b.eval_js("""(() => {
            const r = document.querySelector('[data-iu-viewport]').getBoundingClientRect();
            return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
        })()""")
        x, y = int(rect["x"]), int(rect["y"])
        for kind, dx, dy in (("mousePressed", 0, 0), ("mouseMoved", -60, -40),
                             ("mouseReleased", -60, -40)):
            params = {"type": kind, "x": x + dx, "y": y + dy,
                      "button": "left", "clickCount": 1}
            if kind != "mousePressed":
                params["buttons"] = 1 if kind == "mouseMoved" else 0
            b.cmd("Input.dispatchMouseEvent", params)
        dragged = b.eval_js(TRANSFORM_JS)
        t.chk("drag pans the image", dragged != zoomed, True)

        # ---- Apply: the input now holds a 512x512 crop -----------------
        b.eval_js("""(() => {
            document.querySelector('[data-iu-apply]').click();
            return true;
        })()""")
        t.chk("apply writes a 512x512 file into the input",
              wait_js(b, """(async () => {
                  const i = %s;
                  if (i.files.length !== 1) return false;
                  const bmp = await createImageBitmap(i.files[0]);
                  return bmp.width === 512 && bmp.height === 512;
              })()""" % INPUT_JS), True)
        t.chk("name line says cropped",
              b.eval_js(f"{SPAN_JS}.textContent.includes('cropped')"), True)
        t.chk("crop modal closed",
              b.eval_js("""(() => {
                  const m = document.querySelector('[data-iu-viewport]');
                  return !!m && m.closest('.fixed').classList.contains('hidden');
              })()"""), True)

        # ---- submit the real form -------------------------------------
        submitted = b.eval_js(r"""(async (marker) => {
            const form = document.querySelector('[data-img-upload]')
                .closest('form');
            if (!form) return { fatal: 'no form' };
            const vals = { first_name: 'Img Upload ' + marker,
                           last_name: '',
                           username: marker.toLowerCase(),
                           email: marker.toLowerCase() + '@test.local',
                           phone: '09' + marker.slice(5) };
            for (const [n, v] of Object.entries(vals)) {
                const el = form.querySelector('[name="' + n + '"]');
                if (!el) return { fatal: 'missing field ' + n };
                el.value = v;
            }
            const fd = new FormData(form);
            const r = await fetch(form.getAttribute('action'),
                                  { method: 'POST', body: fd,
                                    credentials: 'same-origin' });
            return { status: r.status, redirected: r.redirected };
        })(%s)""" % ('"' + marker + '"'))
        # fetch follows the PRG, so the final response is the landing page
        # (200) - `redirected` is what proves the 302 happened.
        t.chk("create redirected (PRG)", submitted.get("redirected"), True)
        t.chk("create response is sane",
              submitted.get("status") in (200, 302), True)

        # ---- the upload really landed on disk -------------------------
        conn = sqlite3.connect(DB, timeout=10)
        try:
            row = conn.execute(
                "SELECT id, profile_pic FROM users WHERE username = ?",
                (marker.lower(),)).fetchone()
        finally:
            conn.close()
        t.chk("row created", row is not None, True)
        if not row:
            return
        uid, pic = row
        t.chk("profile_pic is a server-made upload",
              bool(pic and pic.startswith("/resource/image/upload/")
                   and pic.endswith(".webp")), True)
        if pic:
            disk = os.path.join(PROJECT, pic.lstrip("/"))
            t.chk("uploaded webp exists on disk", os.path.isfile(disk), True)
            with open(UPLOADS_STATE, "a") as f:  # run.sh cleanup
                f.write(pic + "\n")

        # ---- edit page carries the same wrapper ------------------------
        b.goto(f"{base}/users/{uid}/edit")
        t.chk("edit page wrapper wired",
              b.eval_js("""(() => {
                  const w = document.querySelector('[data-img-upload]');
                  return !!w && w.dataset.iuReady === '1'
                      && !!w.querySelector('[data-img-crop]');
              })()"""), True)

        # ---- GIF: preview yes, compression/crop no ---------------------
        set_file(b, gif)
        t.chk("GIF kept as-is (no compression, crop hidden)",
              wait_js(b, f"""(() => {{
                  const i = {INPUT_JS};
                  const c = document.querySelector(
                      '[data-img-upload] [data-img-crop]');
                  return i.files.length === 1
                      && i.files[0].name.endsWith('.gif')
                      && c.classList.contains('hidden')
                      && {SPAN_JS}.textContent.includes('animated');
              }})()"""), True)


if __name__ == "__main__":
    main()
    ok = t.summary()
    sys.exit(0 if ok else 1)
