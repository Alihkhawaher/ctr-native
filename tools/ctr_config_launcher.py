#!/usr/bin/env python3
"""
CTR Native — configuration launcher (GUI)

Shows a settings window BEFORE the game runs, saves the chosen options to
a JSON config file next to the game executable, and then launches the game.

NOTE: The engine does not consume this config file yet. The settings are
stored for future wiring; the only thing that changes today is the JSON
that gets written to disk. See "Engine hookup" below.

Run:   python ctr_config_launcher.py
"""

import json
import os
import subprocess
import sys
import tkinter as tk
from tkinter import messagebox, ttk

APP_NAME = "CTR Native Config"
CONFIG_FILENAME = "ctr-native-config.json"
DEFAULT_GAME_EXE = "ctr_native.exe"

# --- Resolution presets (label -> width x height) --------------------------
RESOLUTIONS = {
    "640 x 480  (4:3)":  (640, 480),
    "800 x 600  (4:3)":  (800, 600),
    "1024 x 768 (4:3)":  (1024, 768),
    "1280 x 720 (16:9)": (1280, 720),
    "1280 x 960 (4:3)":  (1280, 960),
    "1920 x 1080 (16:9)": (1920, 1080),
    "2560 x 1440 (16:9)": (2560, 1440),
    "3840 x 2160 (16:9)": (3840, 2160),
}

INTERNAL_RES_SCALES = {
    "Auto (screen)": 0,
    "1x  (native PSX)": 1,
    "2x": 2,
    "3x": 3,
    "4x": 4,
    "8x": 8,
}

ASPECT_RATIOS = ["Auto", "4:3", "16:9"]

PAD_MODES = {
    "4 pads (always on, even if disconnected)": 1,
    "Auto (detect at boot)": 0,
    "2 pads (single tap)": 2,
}

KEYBOARD_SLOTS = {
    "Pads only": -2,
    "Player 1": 0,
    "Player 2": 1,
    "Player 3": 2,
    "Player 4": 3,
    "Auto (moves aside for pads)": -1,
}

REGION_LABELS = {
    "SCUS": "NTSC-U (supported)",
    "SLUS": "NTSC-U (supported)",
    "SCES": "PAL - not supported by the NTSC-U build",
    "SLES": "PAL - not supported by the NTSC-U build",
    "SCPS": "NTSC-J - not supported by the NTSC-U build",
    "SLPS": "NTSC-J - not supported by the NTSC-U build",
}


def detect_disc_region(path):
    """Return (boot_id, region_label) for a raw 2352/2048 disc image, or (None, None).

    Reads the PVD + SYSTEM.CNF directly; CHD/ECM files must be converted first.
    """
    try:
        size = os.path.getsize(path)
    except OSError:
        return None, None

    sector = off = None
    for s, o in ((2352, 24), (2352, 16), (2048, 0)):
        if size % s:
            continue
        try:
            with open(path, "rb") as f:
                f.seek(16 * s)
                raw = f.read(s)
        except OSError:
            return None, None
        if len(raw) == s and raw[o + 1:o + 6] == b"CD001":
            sector, off = s, o
            break
    if sector is None:
        return None, None

    try:
        with open(path, "rb") as f:
            def read_sector(lba):
                f.seek(lba * sector)
                raw = f.read(sector)
                return raw[off:off + 2048] if len(raw) == sector else b""

            pvd = read_sector(16)
            root_lba = int.from_bytes(pvd[158:162], "little")
            root_size = int.from_bytes(pvd[166:170], "little")
            data = b""
            lba, remaining = root_lba, root_size
            while remaining > 0:
                chunk = read_sector(lba)
                if not chunk:
                    break
                data += chunk
                remaining -= len(chunk)
                lba += 1
            pos = 0
            while pos < len(data):
                rec_len = data[pos]
                if rec_len == 0:
                    pos = ((pos // 2048) + 1) * 2048
                    continue
                rec = data[pos:pos + rec_len]
                name = rec[33:33 + rec[32]].decode("ascii", "replace").split(";")[0]
                if name.upper() == "SYSTEM.CNF":
                    flba = int.from_bytes(rec[2:6], "little")
                    fsize = int.from_bytes(rec[10:14], "little")
                    text = b""
                    while len(text) < fsize:
                        chunk = read_sector(flba)
                        if not chunk:
                            break
                        text += chunk
                        flba += 1
                    for line in text.decode("ascii", "replace").splitlines():
                        if "BOOT" in line.upper() and "cdrom:" in line:
                            value = line.split("cdrom:")[1].strip().lstrip("\\/ ")
                            boot_id = value.split(";")[0].strip()
                            prefix = boot_id[:4].upper()
                            label = REGION_LABELS.get(prefix, f"unrecognized ({boot_id})")
                            return boot_id, label
                    return None, None
                pos += rec_len
    except (OSError, ValueError, IndexError):
        return None, None
    return None, None


def default_config():
    """Return the default configuration dict."""
    return {
        "graphics": {
            "window_width": 1920,
            "window_height": 1080,
            "fullscreen": True,
            "aspect_ratio": "4:3",
            "internal_resolution_scale": 0,
            "bilinear_filtering": False,
            "antialiasing": False,
            "pgxp": False,
            "pgxp_geometry": False,
        },
        "input": {
            "pad_mode": 1,
            "keyboard_slot": -2,
            "gamepad_deadzone": 5,
            "gamepad_analog": True,
            "gamepad_rumble": True,
        },
        "game_data": {
            "disc_image": "",
        },
        "launcher": {
            "game_executable": DEFAULT_GAME_EXE,
        },
    }


def resolve_game_path(config, launcher_dir):
    """Return the absolute path to the game executable to launch."""
    game = config.get("launcher", {}).get("game_executable", DEFAULT_GAME_EXE)
    if os.path.isabs(game):
        return game
    return os.path.join(launcher_dir, game)


def load_config(config_path):
    """Load config from disk, falling back to defaults with per-key merge."""
    cfg = default_config()
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            disk = json.load(f)
        # Shallow merge of known sections/keys so new fields don't clobber.
        if isinstance(disk, dict):
            for section in ("graphics", "input", "game_data", "launcher"):
                if isinstance(disk.get(section), dict):
                    cfg.setdefault(section, {}).update(disk[section])
    except (OSError, ValueError):
        pass
    return cfg


class ConfigApp:
    def __init__(self, root):
        self.root = root
        root.title(APP_NAME)
        root.resizable(False, False)

        self.launcher_dir = os.path.dirname(os.path.abspath(__file__))
        self.config_path = os.path.join(self.launcher_dir, CONFIG_FILENAME)
        self.config = load_config(self.config_path)

        self._build_ui()
        self._load_config_into_ui()

    # --- UI construction ---------------------------------------------------
    def _build_ui(self):
        pad = {"padx": 10, "pady": 4}

        main = ttk.Frame(self.root)
        main.grid(row=0, column=0, sticky="nsew", padx=12, pady=12)
        main.grid_columnconfigure(0, weight=1, uniform="cols")
        main.grid_columnconfigure(1, weight=1, uniform="cols")

        # Two columns: Graphics + executable | Gamepad + data, buttons below,
        # so the window fits on 1200p screens without clipping.
        gfx = ttk.LabelFrame(main, text="Graphics")
        gfx.grid(row=0, column=0, sticky="new", padx=(0, 10), pady=(0, 8))

        ttk.Label(gfx, text="Window resolution:").grid(row=0, column=0, sticky="w", **pad)
        self.res_var = tk.StringVar()
        self.res_combo = ttk.Combobox(gfx, textvariable=self.res_var, width=22, state="readonly")
        self.res_combo["values"] = list(RESOLUTIONS.keys())
        self.res_combo.grid(row=0, column=1, sticky="w", **pad)

        ttk.Label(gfx, text="Aspect ratio:").grid(row=1, column=0, sticky="w", **pad)
        self.aspect_var = tk.StringVar()
        self.aspect_combo = ttk.Combobox(gfx, textvariable=self.aspect_var, width=22, state="readonly")
        self.aspect_combo["values"] = ASPECT_RATIOS
        self.aspect_combo.grid(row=1, column=1, sticky="w", **pad)

        ttk.Label(gfx, text="Internal resolution:").grid(row=2, column=0, sticky="w", **pad)
        self.scale_var = tk.StringVar()
        self.scale_combo = ttk.Combobox(gfx, textvariable=self.scale_var, width=22, state="readonly")
        self.scale_combo["values"] = list(INTERNAL_RES_SCALES.keys())
        self.scale_combo.grid(row=2, column=1, sticky="w", **pad)

        self.fullscreen_var = tk.BooleanVar()
        ttk.Checkbutton(gfx, text="Fullscreen", variable=self.fullscreen_var).grid(
            row=3, column=0, columnspan=2, sticky="w", **pad
        )

        self.bilinear_var = tk.BooleanVar()
        ttk.Checkbutton(gfx, text="Bilinear filtering (smoothing)", variable=self.bilinear_var).grid(
            row=4, column=0, columnspan=2, sticky="w", **pad
        )

        self.aa_var = tk.BooleanVar()
        ttk.Checkbutton(gfx, text="Anti-aliasing (smooth presentation)", variable=self.aa_var).grid(
            row=5, column=0, columnspan=2, sticky="w", **pad
        )

        self.pgxp_var = tk.BooleanVar()
        ttk.Checkbutton(gfx, text="PGXP — EXPERIMENTAL, may cause tearing (perspective-correct textures, P)", variable=self.pgxp_var).grid(
            row=6, column=0, columnspan=2, sticky="w", **pad
        )

        self.pgxp_geo_var = tk.BooleanVar()
        ttk.Checkbutton(gfx, text="PGXP geometry — EXPERIMENTAL (subpixel positions; may seam at high internal res, G)", variable=self.pgxp_geo_var).grid(
            row=7, column=0, columnspan=2, sticky="w", **pad
        )

        # Gamepad group
        gamepad = ttk.LabelFrame(main, text="Gamepad")
        gamepad.grid(row=0, column=1, sticky="new", pady=(0, 8))

        ttk.Label(gamepad, text="Pad layout:").grid(row=0, column=0, sticky="w", **pad)
        self.pad_mode_var = tk.StringVar()
        self.pad_mode_combo = ttk.Combobox(gamepad, textvariable=self.pad_mode_var, width=30, state="readonly")
        self.pad_mode_combo["values"] = list(PAD_MODES.keys())
        self.pad_mode_combo.grid(row=0, column=1, sticky="w", **pad)

        ttk.Label(gamepad, text="Stick deadzone (%):").grid(row=2, column=0, sticky="w", **pad)
        self.gp_deadzone_var = tk.IntVar()
        ttk.Spinbox(gamepad, from_=0, to=50, textvariable=self.gp_deadzone_var, width=6).grid(
            row=2, column=1, sticky="w", **pad
        )

        ttk.Label(gamepad, text="Keyboard plays as:").grid(row=1, column=0, sticky="w", **pad)
        self.kb_slot_var = tk.StringVar()
        self.kb_slot_combo = ttk.Combobox(gamepad, textvariable=self.kb_slot_var, width=30, state="readonly")
        self.kb_slot_combo["values"] = list(KEYBOARD_SLOTS.keys())
        self.kb_slot_combo.grid(row=1, column=1, sticky="w", **pad)

        self.gp_analog_var = tk.BooleanVar()
        ttk.Checkbutton(gamepad, text="Analog sticks enabled by default", variable=self.gp_analog_var).grid(
            row=3, column=0, columnspan=2, sticky="w", **pad
        )

        self.gp_rumble_var = tk.BooleanVar()
        ttk.Checkbutton(gamepad, text="Rumble", variable=self.gp_rumble_var).grid(
            row=4, column=0, columnspan=2, sticky="w", **pad
        )

        ttk.Label(
            gamepad,
            text="Pads auto-attach to players 1-4; drops reconnect to the same slot.\nIn-game: F6 swaps pads between players, F4 assigns the keyboard.",
            justify="left",
        ).grid(row=5, column=0, columnspan=2, sticky="w", **pad)

        # Game data group
        data = ttk.LabelFrame(main, text="Game data")
        data.grid(row=1, column=1, sticky="ew", pady=(0, 8))
        ttk.Label(data, text="Disc image (ctr-u.bin / ISO):").grid(row=0, column=0, sticky="w", **pad)
        self.disc_var = tk.StringVar()
        ttk.Entry(data, textvariable=self.disc_var, width=36).grid(row=0, column=1, sticky="ew", padx=10, pady=6)
        ttk.Button(data, text="Browse...", command=self._browse_disc).grid(row=0, column=2, padx=(0, 10), pady=6)
        ttk.Label(
            data,
            text="Pick any BIN/ISO, or leave empty for assets/ctr-u.bin.",
            justify="left",
        ).grid(row=1, column=0, columnspan=3, sticky="w", **pad)

        self.disc_region_var = tk.StringVar(value="")
        self.disc_region_label = ttk.Label(data, textvariable=self.disc_region_var, foreground="#666666")
        self.disc_region_label.grid(row=2, column=0, columnspan=3, sticky="w", **pad)

        # Launcher group
        launch = ttk.LabelFrame(main, text="Game executable")
        launch.grid(row=1, column=0, sticky="ew", padx=(0, 10), pady=(0, 8))
        self.game_path_var = tk.StringVar()
        ttk.Entry(launch, textvariable=self.game_path_var, width=44).grid(
            row=0, column=0, sticky="ew", padx=10, pady=6
        )
        ttk.Button(launch, text="Browse...", command=self._browse_game).grid(
            row=0, column=1, padx=(0, 10), pady=6
        )

        # Buttons
        btns = ttk.Frame(main)
        btns.grid(row=2, column=0, columnspan=2, sticky="ew")
        ttk.Button(btns, text="Save", command=self._save_only).pack(side="left", padx=4)
        ttk.Button(btns, text="Save & Play", command=self._save_and_play).pack(side="left", padx=4)
        ttk.Button(btns, text="Quit", command=self.root.destroy).pack(side="left", padx=4)

        # Status note
        note = (
            "Settings are written to ctr-native-config.json next to the game and\n"
            "applied when the game starts; paths inside the folder are kept relative."
        )
        ttk.Label(main, text=note, foreground="#666666", justify="left").grid(
            row=3, column=0, columnspan=2, sticky="w", pady=(10, 0)
        )

    # --- Config <-> UI -----------------------------------------------------
    def _load_config_into_ui(self):
        g = self.config["graphics"]
        w, h = g.get("window_width", 1280), g.get("window_height", 720)

        label = None
        for key, (rw, rh) in RESOLUTIONS.items():
            if rw == w and rh == h:
                label = key
                break
        if label is None:
            # Build a custom label so the current value is visible.
            label = f"{w} x {h} (custom)"
            self.res_combo["values"] = [label] + list(RESOLUTIONS.keys())
        self.res_var.set(label)

        aspect = g.get("aspect_ratio", "Auto")
        if aspect not in ASPECT_RATIOS:
            aspect = "Auto"
        self.aspect_var.set(aspect)

        scale = g.get("internal_resolution_scale", 1)
        scale_label = next((k for k, v in INTERNAL_RES_SCALES.items() if v == scale), "1x  (native PSX)")
        self.scale_var.set(scale_label)

        self.fullscreen_var.set(bool(g.get("fullscreen", False)))
        self.bilinear_var.set(bool(g.get("bilinear_filtering", False)))
        self.aa_var.set(bool(g.get("antialiasing", False)))
        self.pgxp_var.set(bool(g.get("pgxp", False)))
        self.pgxp_geo_var.set(bool(g.get("pgxp_geometry", False)))

        inp = self.config.get("input", {})
        self.pad_mode_var.set(next((k for k, v in PAD_MODES.items() if v == int(inp.get("pad_mode", 1))), "4 pads (always on, even if disconnected)"))
        self.kb_slot_var.set(next((k for k, v in KEYBOARD_SLOTS.items() if v == int(inp.get("keyboard_slot", -2))), "Pads only"))
        self.gp_deadzone_var.set(int(inp.get("gamepad_deadzone", 5)))
        self.gp_analog_var.set(bool(inp.get("gamepad_analog", True)))
        self.gp_rumble_var.set(bool(inp.get("gamepad_rumble", True)))

        game = self.config.get("launcher", {}).get("game_executable", DEFAULT_GAME_EXE)
        self.game_path_var.set(self._portable_path(game))

        disc = self.config.get("game_data", {}).get("disc_image", "")
        if not disc and os.path.isfile(os.path.join(self.launcher_dir, "assets", "ctr-u.bin")):
            disc = os.path.join("assets", "ctr-u.bin")
        self.disc_var.set(self._portable_path(disc))
        self._update_disc_region_label()

    def _update_disc_region_label(self):
        disc = self._portable_path(self.disc_var.get())
        if not disc:
            self.disc_region_var.set("No disc image set - the game will use assets/ctr-u.bin.")
            self.disc_region_label.configure(foreground="#666666")
            return
        full = disc if os.path.isabs(disc) else os.path.join(self.launcher_dir, disc)
        if not os.path.isfile(full):
            self.disc_region_var.set(f"File not found: {full}")
            self.disc_region_label.configure(foreground="#b00020")
            return
        boot_id, label = detect_disc_region(full)
        if boot_id is None:
            self.disc_region_var.set("Could not identify this image (CHD/ECM files must be converted first).")
            self.disc_region_label.configure(foreground="#b00020")
        else:
            self.disc_region_var.set(f"Detected: {label} - {boot_id}")
            self.disc_region_label.configure(
                foreground="#1a7f37" if label.startswith("NTSC-U") else "#b00020"
            )

    # --- Path helpers ------------------------------------------------------
    def _portable_path(self, path):
        """Relative when the file lives inside the launcher folder (keeps the
        install movable), otherwise the path unchanged."""
        path = (path or "").strip()
        if not path:
            return ""
        full = path if os.path.isabs(path) else os.path.join(self.launcher_dir, path)
        try:
            rel = os.path.relpath(full, self.launcher_dir)
        except ValueError:
            return path  # different drive: keep as-is
        if rel.startswith(".."):
            return path
        return rel

    def _save_config_from_ui(self):
        res_label = self.res_var.get()
        w, h = RESOLUTIONS.get(res_label, (1280, 720))

        scale_label = self.scale_var.get()
        scale = INTERNAL_RES_SCALES.get(scale_label, 1)

        self.config["graphics"].update({
            "window_width": w,
            "window_height": h,
            "fullscreen": self.fullscreen_var.get(),
            "aspect_ratio": self.aspect_var.get(),
            "internal_resolution_scale": scale,
            "bilinear_filtering": self.bilinear_var.get(),
            "antialiasing": self.aa_var.get(),
            "pgxp": self.pgxp_var.get(),
            "pgxp_geometry": self.pgxp_geo_var.get(),
        })
        self.config["input"] = {
            "pad_mode": PAD_MODES.get(self.pad_mode_var.get(), 1),
            "keyboard_slot": KEYBOARD_SLOTS.get(self.kb_slot_var.get(), -2),
            "gamepad_deadzone": int(self.gp_deadzone_var.get()),
            "gamepad_analog": self.gp_analog_var.get(),
            "gamepad_rumble": self.gp_rumble_var.get(),
        }
        self.config["launcher"]["game_executable"] = self._portable_path(self.game_path_var.get()) or DEFAULT_GAME_EXE
        self.config["game_data"] = {"disc_image": self._portable_path(self.disc_var.get())}

    def _write_config(self):
        self._save_config_from_ui()
        with open(self.config_path, "w", encoding="utf-8") as f:
            json.dump(self.config, f, indent=2)
        return self.config_path

    # --- Actions -----------------------------------------------------------
    def _browse_game(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(
            title="Select ctr_native.exe",
            filetypes=[("Executable", "*.exe"), ("All files", "*.*")],
        )
        if path:
            self.game_path_var.set(path)

    def _browse_disc(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(
            title="Select the game disc image (ctr-u.bin / ISO)",
            filetypes=[("Disc image", "*.bin *.iso *.img"), ("All files", "*.*")],
        )
        if path:
            self.disc_var.set(path)
            self._update_disc_region_label()

    def _save_only(self):
        path = self._write_config()
        messagebox.showinfo(APP_NAME, f"Configuration saved to:\n{path}")

    def _save_and_play(self):
        self._write_config()
        game = resolve_game_path(self.config, self.launcher_dir)

        if not os.path.isfile(game):
            messagebox.showerror(
                APP_NAME,
                f"Game executable not found:\n{game}\n\n"
                "Use Browse... to select ctr_native.exe.",
            )
            return

        disc = self.config.get("game_data", {}).get("disc_image", "")
        if disc:
            disc_full = disc if os.path.isabs(disc) else os.path.join(self.launcher_dir, disc)
            if not os.path.isfile(disc_full):
                if not messagebox.askyesno(
                    APP_NAME,
                    f"Disc image not found:\n{disc_full}\n\n"
                    "The game will fall back to assets/ctr-u.bin if present.\nLaunch anyway?",
                ):
                    return
            else:
                boot_id, label = detect_disc_region(disc_full)
                if boot_id and not label.startswith("NTSC-U"):
                    if not messagebox.askyesno(
                        APP_NAME,
                        f"This disc is {label}.\n\nThe game will refuse to run it (clean exit, no crash).\n"
                        "Use an NTSC-U (SCUS-94426) image instead.\nLaunch anyway?",
                    ):
                        return

        if messagebox.askyesno(APP_NAME, "Launch the game now?"):
            try:
                subprocess.Popen([game], cwd=os.path.dirname(game))
            except OSError as exc:
                messagebox.showerror(APP_NAME, f"Failed to launch game:\n{exc}")
                return
            self.root.destroy()


def main():
    root = tk.Tk()
    try:
        root.iconbitmap(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ctr_native.ico"))
    except Exception:
        pass
    ConfigApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()