// Reads the graphics options written by ctr_config_launcher.py
// (ctr-native-config.json). The engine intentionally avoids a JSON
// dependency: the document is small and flat, so a scanner for the handful of
// known quoted keys is enough. Unknown keys and formatting are ignored, and
// malformed values leave the default in place.

#include "platform/native_config.h"

#include <macros.h>

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_CONFIG_MIN_WINDOW_WIDTH  320
#define NATIVE_CONFIG_MIN_WINDOW_HEIGHT 200
#define NATIVE_CONFIG_MAX_WINDOW_WIDTH  7680
#define NATIVE_CONFIG_MAX_WINDOW_HEIGHT 4320
#define NATIVE_CONFIG_MAX_SCALE         8

internal void NativeConfig_SkipWhitespace(const char **cursor)
{
	while ((**cursor == ' ') || (**cursor == '\t') || (**cursor == '\r') || (**cursor == '\n'))
	{
		(*cursor)++;
	}
}

internal const char *NativeConfig_FindValue(const char *text, const char *key)
{
	char needle[64];
	const char *cursor;

	snprintf(needle, sizeof(needle), "\"%s\"", key);

	cursor = strstr(text, needle);
	if (cursor == NULL)
	{
		return NULL;
	}

	cursor += strlen(needle);
	NativeConfig_SkipWhitespace(&cursor);

	if (*cursor != ':')
	{
		return NULL;
	}

	cursor++;
	NativeConfig_SkipWhitespace(&cursor);
	return cursor;
}

internal int NativeConfig_ReadInt(const char *text, const char *key, int *value)
{
	const char *cursor = NativeConfig_FindValue(text, key);
	char *end = NULL;
	long parsed;

	if (cursor == NULL)
	{
		return 0;
	}

	parsed = strtol(cursor, &end, 10);
	if (end == cursor)
	{
		return 0;
	}

	*value = (int)parsed;
	return 1;
}

internal int NativeConfig_ReadBool(const char *text, const char *key, int *value)
{
	const char *cursor = NativeConfig_FindValue(text, key);

	if (cursor == NULL)
	{
		return 0;
	}

	if (strncmp(cursor, "true", 4) == 0)
	{
		*value = 1;
		return 1;
	}

	if (strncmp(cursor, "false", 5) == 0)
	{
		*value = 0;
		return 1;
	}

	return 0;
}

internal int NativeConfig_ReadString(const char *text, const char *key, char *out, size_t outSize)
{
	const char *cursor = NativeConfig_FindValue(text, key);
	size_t length = 0;

	if ((cursor == NULL) || (*cursor != '"'))
	{
		return 0;
	}

	cursor++;
	while ((cursor[length] != '\0') && (cursor[length] != '"') && ((length + 1) < outSize))
	{
		out[length] = cursor[length];
		length++;
	}

	if ((cursor[length] != '"') || (length == 0))
	{
		out[0] = '\0';
		return 0;
	}

	out[length] = '\0';
	return 1;
}

void NativeConfig_SetDefaults(NativeConfig *config)
{
	config->windowWidth = 0;
	config->windowHeight = 0;
	config->fullscreen = 1;
	config->aspectRatio = 1; // 4:3
	config->internalResolutionScale = 1;
	config->bilinearFiltering = 0;
	config->showFps = 0;
	config->antialiasing = 0;
	config->pgxp = 0;         // experimental: OFF by default (tearing)
	config->pgxpGeometry = 1; // only active when pgxp is on
	config->internalResolutionAuto = 1; // Auto (match screen height at 240 lines)
	config->gamepadDeadzone = 5; // percent; wider than the old 1.5% so Xbox sticks at rest stay neutral
	config->gamepadAnalog = 1;
	config->gamepadRumble = 1;
	config->padMode = 1; // 4-pad bus from startup (pads attach into slots as they connect)
	config->keyboardSlot = -2; // default: "Pads only" — the keyboard drives no player unless assigned
	config->discImage[0] = '\0'; // default: assets/ctr-u.bin next to the executable
}

int NativeConfig_LoadFile(NativeConfig *config, const char *path)
{
	size_t size = 0;
	char *text;
	int value;

	text = (char *)SDL_LoadFile(path, &size);
	if (text == NULL)
	{
		return 0;
	}

	if ((NativeConfig_ReadInt(text, "window_width", &value) != 0) && (value >= NATIVE_CONFIG_MIN_WINDOW_WIDTH) && (value <= NATIVE_CONFIG_MAX_WINDOW_WIDTH))
	{
		config->windowWidth = value;
	}

	if ((NativeConfig_ReadInt(text, "window_height", &value) != 0) && (value >= NATIVE_CONFIG_MIN_WINDOW_HEIGHT) && (value <= NATIVE_CONFIG_MAX_WINDOW_HEIGHT))
	{
		config->windowHeight = value;
	}

	if (NativeConfig_ReadBool(text, "fullscreen", &value) != 0)
	{
		config->fullscreen = value;
	}

	{
		char aspect[32];

		if (NativeConfig_ReadString(text, "aspect_ratio", aspect, sizeof(aspect)) != 0)
		{
			if (strcmp(aspect, "4:3") == 0)
			{
				config->aspectRatio = 1;
			}
			else if (strcmp(aspect, "16:9") == 0)
			{
				config->aspectRatio = 2;
			}
			else
			{
				config->aspectRatio = 0;
			}
		}
	}

	if ((NativeConfig_ReadInt(text, "internal_resolution_scale", &value) != 0) && (value >= 0) && (value <= NATIVE_CONFIG_MAX_SCALE))
	{
		config->internalResolutionAuto = (value == 0) ? 1 : 0;
		config->internalResolutionScale = (value == 0) ? 1 : value;
	}

	if (NativeConfig_ReadBool(text, "bilinear_filtering", &value) != 0)
	{
		config->bilinearFiltering = value;
	}

	if (NativeConfig_ReadBool(text, "show_fps", &value) != 0)
	{
		config->showFps = value;
	}

	if (NativeConfig_ReadBool(text, "antialiasing", &value) != 0)
	{
		config->antialiasing = value;
	}

	if (NativeConfig_ReadBool(text, "pgxp", &value) != 0)
	{
		config->pgxp = value;
	}

	if (NativeConfig_ReadBool(text, "pgxp_geometry", &value) != 0)
	{
		config->pgxpGeometry = value;
	}

	if ((NativeConfig_ReadInt(text, "gamepad_deadzone", &value) != 0) && (value >= 0) && (value <= 50))
	{
		config->gamepadDeadzone = value;
	}

	if (NativeConfig_ReadBool(text, "gamepad_analog", &value) != 0)
	{
		config->gamepadAnalog = value;
	}

	if (NativeConfig_ReadBool(text, "gamepad_rumble", &value) != 0)
	{
		config->gamepadRumble = value;
	}

	if ((NativeConfig_ReadInt(text, "pad_mode", &value) != 0) && (value >= 0) && (value <= 2))
	{
		config->padMode = value;
	}

	if ((NativeConfig_ReadInt(text, "keyboard_slot", &value) != 0) && (value >= -2) && (value <= 3))
	{
		config->keyboardSlot = value;
	}

	// Optional: explicit disc image path chosen in the launcher (absolute, or
	// relative to the game folder). Missing/malformed keeps the default empty.
	NativeConfig_ReadString(text, "disc_image", config->discImage, sizeof(config->discImage));

	SDL_free(text);
	return 1;
}

// Copies a raw JSON string value (escapes preserved) so saving engine-owned
// options never clobbers launcher-managed keys.
internal void NativeConfig_CopyStringValue(const char *text, const char *key, const char *fallback, char *out, size_t outSize)
{
	const char *cursor = NativeConfig_FindValue(text, key);
	size_t length = 0;

	if ((cursor == NULL) || (*cursor != '"'))
	{
		snprintf(out, outSize, "%s", fallback);
		return;
	}

	cursor++;
	while ((cursor[length] != '\0') && (cursor[length] != '"') && ((length + 1) < outSize))
	{
		out[length] = cursor[length];
		length++;
	}

	if (cursor[length] != '"')
	{
		snprintf(out, outSize, "%s", fallback);
		return;
	}

	out[length] = '\0';
}

// Copies the launcher's configured game executable (raw JSON string content,
// escapes preserved) so saving graphics options never clobbers that setting.
internal void NativeConfig_CopyLauncherExecutable(const char *text, char *out, size_t outSize)
{
	NativeConfig_CopyStringValue(text, "game_executable", "ctr_native.exe", out, outSize);
}

int NativeConfig_SaveFile(const NativeConfig *config, const char *path)
{
	static const char *s_aspectNames[3] = {"Auto", "4:3", "16:9"};
	char tempPath[512];
	char launcherExecutable[512];
	char discImage[512];
	char *existing;
	size_t existingSize = 0;
	FILE *file;
	int written;

	existing = (char *)SDL_LoadFile(path, &existingSize);
	if (existing != NULL)
	{
		NativeConfig_CopyLauncherExecutable(existing, launcherExecutable, sizeof(launcherExecutable));
		NativeConfig_CopyStringValue(existing, "disc_image", "", discImage, sizeof(discImage));
		SDL_free(existing);
	}
	else
	{
		snprintf(launcherExecutable, sizeof(launcherExecutable), "ctr_native.exe");
		discImage[0] = '\0';
	}

	if (snprintf(tempPath, sizeof(tempPath), "%s.tmp", path) <= 0)
	{
		return 0;
	}

	file = fopen(tempPath, "wb");
	if (file == NULL)
	{
		return 0;
	}

	written = fprintf(file,
	                  "{\n"
	                  "  \"graphics\": {\n"
	                  "    \"window_width\": %d,\n"
	                  "    \"window_height\": %d,\n"
	                  "    \"fullscreen\": %s,\n"
	                  "    \"aspect_ratio\": \"%s\",\n"
	                  "    \"internal_resolution_scale\": %d,\n"
	                  "    \"bilinear_filtering\": %s,\n"
	                  "    \"antialiasing\": %s,\n"
	                  "    \"pgxp\": %s,\n"
	                  "    \"pgxp_geometry\": %s,\n"
	                  "    \"show_fps\": %s\n"
	                  "  },\n"
	                  "  \"input\": {\n"
	                  "    \"pad_mode\": %d,\n"
	                  "    \"keyboard_slot\": %d,\n"
	                  "    \"gamepad_deadzone\": %d,\n"
	                  "    \"gamepad_analog\": %s,\n"
	                  "    \"gamepad_rumble\": %s\n"
	                  "  },\n"
	                  "  \"launcher\": {\n"
	                  "    \"game_executable\": \"%s\"\n"
	                  "  },\n"
	                  "  \"game_data\": {\n"
	                  "    \"disc_image\": \"%s\"\n"
	                  "  }\n"
	                  "}\n",
	                  config->windowWidth,
	                  config->windowHeight,
	                  (config->fullscreen != 0) ? "true" : "false",
	                  s_aspectNames[(config->aspectRatio >= 0 && config->aspectRatio < 3) ? config->aspectRatio : 0],
	                  ((config->internalResolutionAuto != 0) ? 0 : config->internalResolutionScale),
	                  (config->bilinearFiltering != 0) ? "true" : "false",
	                  (config->antialiasing != 0) ? "true" : "false",
	                  (config->pgxp != 0) ? "true" : "false",
	                  (config->pgxpGeometry != 0) ? "true" : "false",
	                  (config->showFps != 0) ? "true" : "false",
	                  config->padMode,
	                  config->keyboardSlot,
	                  config->gamepadDeadzone,
	                  (config->gamepadAnalog != 0) ? "true" : "false",
	                  (config->gamepadRumble != 0) ? "true" : "false",
	                  launcherExecutable,
	                  discImage);

	fclose(file);

	if (written <= 0)
	{
		remove(tempPath);
		return 0;
	}

	remove(path);
	return rename(tempPath, path) == 0;
}

int NativeConfig_SaveDefaultLocation(const NativeConfig *config)
{
	const char *basePath = SDL_GetBasePath();
	char configPath[512];

	if ((basePath == NULL) || (snprintf(configPath, sizeof(configPath), "%sctr-native-config.json", basePath) <= 0))
	{
		return 0;
	}

	return NativeConfig_SaveFile(config, configPath);
}
