#!/usr/bin/env python3
"""PGXP / graphics A/B verification harness for ctr-native.

Takes the *game's own* framebuffer captures (the port's F12 key -> screenshots/
ctr_*.bmp, pure glReadPixels output, no desktop involvement), drives key toggles
via cua-driver, and produces quantitative diff reports so every rendering change
is verified by numbers instead of eyeballs alone.

Usage:
  python tools/pgxp_ab.py capture <label> [--keys "g" ...] [--pin] [--delay 3]
  python tools/pgxp_ab.py ab <labelA> <labelB> --toggle g [--pin]
  python tools/pgxp_ab.py diff <a.bmp> <b.bmp> [--outdir out]
  python tools/pgxp_ab.py live <label>          # capture from whatever is on screen now

Notes:
  --pin: press F5 (save state) first, then for EVERY capture press F8 (load state)
         before the toggles+capture -> near-deterministic frames for moving scenes.
  Keys are sent to the game window found by title "Crash Team Racing |"; override
  with env CTR_PID / CTR_WID. Game dir: env CTR_GAME_DIR (default E:/Games/CrashCTR-Win).

Outputs land in <game_dir>/ab/<label>.png plus <game_dir>/ab/report_*.txt|png.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

from PIL import Image, ImageChops

GAME_DIR = os.environ.get("CTR_GAME_DIR", r"E:/Games/CrashCTR-Win")
SHOT_DIR = os.path.join(GAME_DIR, "screenshots")
AB_DIR = os.path.join(GAME_DIR, "ab")
CUA = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Programs", "Cua", "cua-driver", "bin", "cua-driver.EXE")
WINDOW_TITLE = "Crash Team Racing |"


def find_window():
    pid, wid = os.environ.get("CTR_PID"), os.environ.get("CTR_WID")
    if pid and wid:
        return int(pid), int(wid)
    out = subprocess.run([CUA, "list_windows"], capture_output=True, text=True, timeout=30).stdout
    data = json.loads(out)
    wins = data.get("windows", data.get("data", [])) if isinstance(data, dict) else data
    for w in wins:
        if WINDOW_TITLE in (w.get("title") or ""):
            return int(w.get("pid")), int(w.get("window_id"))
    raise SystemExit("game window not found (is the game running?)")


def press(pid, wid, key):
    subprocess.run([CUA, "call", "press_key", json.dumps({"key": key, "pid": pid, "window_id": wid})],
                   capture_output=True, timeout=30)


def wait_new_shot(before, timeout=15.0):
    """Wait for a screenshot file that did not exist in `before`; return its path."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        now = set(os.listdir(SHOT_DIR)) if os.path.isdir(SHOT_DIR) else set()
        new = sorted(now - before)
        if new:
            p = os.path.join(SHOT_DIR, new[-1])
            # wait for the file to finish writing (size stable)
            s0 = -1
            while time.time() < deadline:
                s1 = os.path.getsize(p)
                if s1 == s0 and s1 > 0:
                    return p
                s0 = s1
                time.sleep(0.2)
        time.sleep(0.2)
    raise SystemExit("no new screenshot appeared (is the game running and is F12 wired?)")


def capture(label, keys, pin=False, delay=3.0):
    pid, wid = find_window()
    os.makedirs(AB_DIR, exist_ok=True)
    os.makedirs(SHOT_DIR, exist_ok=True)

    if pin:
        press(pid, wid, "f5")  # save a state once; every capture reloads it
        time.sleep(1.5)

    for k in keys:
        press(pid, wid, k)
        time.sleep(0.4)

    if pin:
        press(pid, wid, "f8")  # reload state right before the shot
        time.sleep(1.0)
        for k in keys:
            press(pid, wid, k)
            time.sleep(0.4)

    time.sleep(delay)
    before = set(os.listdir(SHOT_DIR))
    press(pid, wid, "f12")
    shot = wait_new_shot(before)
    dst_png = os.path.join(AB_DIR, label + ".png")
    Image.open(shot).convert("RGB").save(dst_png)
    print(f"captured {label}: {dst_png}")
    return dst_png


def diff(a_path, b_path, outdir=None, tag="diff"):
    a = Image.open(a_path).convert("RGB")
    b = Image.open(b_path).convert("RGB")
    if a.size != b.size:
        b = b.resize(a.size)
    outdir = outdir or AB_DIR
    os.makedirs(outdir, exist_ok=True)

    d = ImageChops.difference(a, b)
    gray = d.convert("L")
    hist = gray.histogram()
    total = a.size[0] * a.size[1]
    ge8 = 100.0 * sum(hist[8:]) / total
    ge24 = 100.0 * sum(hist[24:]) / total
    ge48 = 100.0 * sum(hist[48:]) / total

    # worst tiles (8x6 grid) by mean diff
    cols, rows = 8, 6
    tw, th = a.size[0] // cols, a.size[1] // rows
    tiles = []
    for r in range(rows):
        for c in range(cols):
            box = (c * tw, r * th, (c + 1) * tw, (r + 1) * th)
            t = gray.crop(box)
            tiles.append((sum(i * v for i, v in enumerate(t.histogram())) / (t.size[0] * t.size[1]), box))
    tiles.sort(reverse=True)

    # amplify heatmap and save artifacts
    heat = d.point(lambda v: min(255, v * 6))
    heat.save(os.path.join(outdir, f"heat_{tag}.png"))
    side = Image.new("RGB", (a.size[0], a.size[1] * 2))
    side.paste(a, (0, 0))
    side.paste(b, (0, a.size[1]))
    side.save(os.path.join(outdir, f"side_{tag}.png"))

    worst = tiles[:3]
    for i, (score, box) in enumerate(worst):
        for name, img in (("a", a), ("b", b)):
            crop = img.crop(box)
            crop = crop.resize((crop.size[0] * 3, crop.size[1] * 3), Image.NEAREST)
            crop.save(os.path.join(outdir, f"{tag}_worst{i}_{name}.png"))

    report = [
        f"A = {a_path}",
        f"B = {b_path}",
        f"pixels differing >=8:  {ge8:.2f}%",
        f"pixels differing >=24: {ge24:.2f}%",
        f"pixels differing >=48: {ge48:.2f}%",
        "worst tiles (mean-abs-diff, box):",
    ] + [f"  {s:6.1f}  {box}" for s, box in tiles[:8]]
    text = "\n".join(report)
    rp = os.path.join(outdir, f"report_{tag}.txt")
    with open(rp, "w") as f:
        f.write(text + "\n")
    print(text)
    print(f"artifacts in {outdir}")
    return {"ge8": ge8, "ge24": ge24, "ge48": ge48, "report": rp}


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("capture")
    c.add_argument("label")
    c.add_argument("--keys", nargs="*", default=[])
    c.add_argument("--pin", action="store_true")
    c.add_argument("--delay", type=float, default=3.0)

    l = sub.add_parser("live")  # alias with a clearer name
    l.add_argument("label")

    ab = sub.add_parser("ab")
    ab.add_argument("label_a")
    ab.add_argument("label_b")
    ab.add_argument("--toggle", required=True, help="key that flips the setting between captures")
    ab.add_argument("--pin", action="store_true")
    ab.add_argument("--keys", nargs="*", default=[], help="extra keys pressed in BOTH captures (e.g. o for status view)")

    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--outdir", default=None)

    args = ap.parse_args()
    if args.cmd in ("capture", "live"):
        capture(args.label, getattr(args, "keys", []) or [], getattr(args, "pin", False), getattr(args, "delay", 3.0))
    elif args.cmd == "ab":
        # NOTE: extra keys are pressed only for capture A — stateful toggles (o, p, g)
        # would otherwise flip twice and the pair would compare different states.
        pa = capture(args.label_a, args.keys, args.pin)
        pid, wid = find_window()
        press(pid, wid, args.toggle)
        time.sleep(0.8)
        pb = capture(args.label_b, [], args.pin)
        diff(pa, pb, tag=f"{args.label_a}_vs_{args.label_b}")
    elif args.cmd == "diff":
        diff(args.a, args.b, args.outdir)


if __name__ == "__main__":
    main()
