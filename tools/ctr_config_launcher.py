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
            "pgxp_geometry": True,
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
            for section in ("graphics", "launcher"):
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

        # Graphics group
        gfx = ttk.LabelFrame(main, text="Graphics")
        gfx.grid(row=0, column=0, sticky="ew", pady=(0, 8))

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
        ttk.Checkbutton(gfx, text="PGXP geometry — EXPERIMENTAL (subpixel positions, G)", variable=self.pgxp_geo_var).grid(
            row=7, column=0, columnspan=2, sticky="w", **pad
        )

        # Launcher group
        launch = ttk.LabelFrame(main, text="Game executable")
        launch.grid(row=1, column=0, sticky="ew", pady=(0, 8))
        self.game_path_var = tk.StringVar()
        ttk.Entry(launch, textvariable=self.game_path_var, width=44).grid(
            row=0, column=0, sticky="ew", padx=10, pady=6
        )
        ttk.Button(launch, text="Browse...", command=self._browse_game).grid(
            row=0, column=1, padx=(0, 10), pady=6
        )

        # Buttons
        btns = ttk.Frame(main)
        btns.grid(row=2, column=0, sticky="ew")
        ttk.Button(btns, text="Save", command=self._save_only).pack(side="left", padx=4)
        ttk.Button(btns, text="Save & Play", command=self._save_and_play).pack(side="left", padx=4)
        ttk.Button(btns, text="Quit", command=self.root.destroy).pack(side="left", padx=4)

        # Status note
        note = (
            "Settings are written to ctr-native-config.json next to the game\n"
            "and applied when the game starts."
        )
        ttk.Label(main, text=note, foreground="#666666", justify="left").grid(
            row=3, column=0, sticky="w", pady=(10, 0)
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
        self.pgxp_geo_var.set(bool(g.get("pgxp_geometry", True)))

        game = self.config.get("launcher", {}).get("game_executable", DEFAULT_GAME_EXE)
        if not os.path.isabs(game):
            game = os.path.join(self.launcher_dir, game)
        self.game_path_var.set(game)

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
        self.config["launcher"]["game_executable"] = self.game_path_var.get()

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