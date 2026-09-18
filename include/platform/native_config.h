#ifndef NATIVE_CONFIG_H
#define NATIVE_CONFIG_H

// Runtime graphics options loaded from ctr-native-config.json, written by
// ctr_config_launcher.py next to the game executable. Keys that are missing
// or malformed keep their default value.

typedef struct
{
	int windowWidth;             // 0 = use the built-in window default
	int windowHeight;            // 0 = use the built-in window default
	int fullscreen;              // 0/1
	int aspectRatio;             // 0 = auto (match window), 1 = 4:3, 2 = 16:9
	int internalResolutionScale; // 1 = native, up to 8
	int internalResolutionAuto;  // 1 = match window/display resolution
	int bilinearFiltering;       // 0/1
	int antialiasing;            // 0/1 - smooth (linear) presentation filter
	int pgxp;                    // 0/1 - EXPERIMENTAL: perspective-correct textures + subpixel geometry; may tear; OFF by default
	int pgxpGeometry;            // 0/1 - EXPERIMENTAL: subpixel vertex positions (clamped to 0.5px); active only with pgxp
	int gamepadDeadzone;         // percent 0..50 applied to stick axes
	int gamepadAnalog;           // 0/1 - new pads start in analog mode
	int gamepadRumble;           // 0/1 - allow pad vibration
	int padMode;                 // 0 = auto, 1 = 4 pads (multitap always), 2 = 2 pads
	int keyboardSlot;            // -1 = auto (keyboard moves aside for pads), 0..3 = fixed player
	int showFps;                 // 0/1
} NativeConfig;

void NativeConfig_SetDefaults(NativeConfig *config);
int NativeConfig_LoadFile(NativeConfig *config, const char *path);
int NativeConfig_SaveFile(const NativeConfig *config, const char *path);
int NativeConfig_SaveDefaultLocation(const NativeConfig *config);

#endif
