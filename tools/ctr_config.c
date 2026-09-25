// CTR Native — configuration launcher (native Windows build, no Python)
//
// Replacement for tools/ctr_config_launcher.py: a single self-contained
// ctr_config.exe (static CRT, no runtime dependencies) that edits
// ctr-native-config.json next to the game executable and can start the game.
//
// The JSON schema matches platform/native_config.c exactly (that file is the
// authority for keys, defaults and bounds). The scanner below is the same
// "find the quoted key, then parse the value" approach the engine uses so the
// two stay interchangeable:
//
//   graphics: window_width, window_height, fullscreen, aspect_ratio,
//             internal_resolution_scale (0 = Auto), bilinear_filtering,
//             antialiasing, pgxp, pgxp_geometry, show_fps
//   input:    pad_mode (0..2), keyboard_slot (-2..3), gamepad_deadzone (0..50),
//             gamepad_analog, gamepad_rumble
//   launcher: game_executable
//   game_data: disc_image
//
// CLI (useful for scripted deployment and tests; the GUI is the default):
//   ctr_config.exe                     open the settings window
//   ctr_config.exe --dump              print the parsed configuration
//   ctr_config.exe --resave            load + save (round-trip check)
//   ctr_config.exe --write-defaults    write a fresh default config
//   ctr_config.exe --config PATH       operate on PATH instead of the exe folder
//   ctr_config.exe --out PATH          also copy --dump/--verbose text to PATH
//   ctr_config.exe --verbose           log every step (console + ctr_config.log)
//   ctr_config.exe --help
//
// Debug logging is permanent: --verbose (or any failed save/launch) appends to
// ctr_config.log next to the exe; it is never removed, only gated by the flag.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define APP_TITLE L"CTR Native Config"
#define CONFIG_FILENAME "ctr-native-config.json"
#define DEFAULT_GAME_EXE "ctr_native.exe"
#define LOG_FILENAME "ctr_config.log"

#define CONFIG_MAX_BYTES (1024 * 1024)
#define STR_MAX 512
#define WSTR_MAX 512

// Engine bounds (platform/native_config.c)
#define MIN_WINDOW_WIDTH 320
#define MAX_WINDOW_WIDTH 7680
#define MIN_WINDOW_HEIGHT 200
#define MAX_WINDOW_HEIGHT 4320
#define MAX_SCALE 8

// Control IDs
enum
{
	IDC_RES_COMBO = 100,
	IDC_RES_W,
	IDC_RES_H,
	IDC_ASPECT,
	IDC_SCALE,
	IDC_FULLSCREEN,
	IDC_BILINEAR,
	IDC_AA,
	IDC_PGXP,
	IDC_PGXP_GEO,
	IDC_SHOWFPS,
	IDC_HINT_PGXP,
	IDC_PADMODE = 120,
	IDC_KBSLOT,
	IDC_DEADZONE,
	IDC_ANALOG,
	IDC_RUMBLE,
	IDC_PADNOTE,
	IDC_GAMEEXE = 130,
	IDC_BROWSE_EXE,
	IDC_DISC = 140,
	IDC_BROWSE_DISC,
	IDC_DISCHINT,
	IDC_REGION,
	IDC_SAVE = 150,
	IDC_SAVEPLAY,
	IDC_QUIT,
	IDC_NOTE
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static HINSTANCE g_hInst;
static HWND g_hWnd;
static HFONT g_hFont;
static int g_dpi = 96;
static int g_verbose = 0;
static FILE *g_outCopy = NULL;      // optional --out capture file
static char g_launcherDirUtf8[STR_MAX];
static wchar_t g_launcherDirW[WSTR_MAX];
static char g_configPathUtf8[STR_MAX];
static wchar_t g_configPathW[WSTR_MAX];
static char g_logPathUtf8[STR_MAX];
static COLORREF g_regionColor = RGB(110, 110, 110);

typedef struct
{
	int windowWidth;
	int windowHeight;
	int fullscreen;
	int aspect;         // 0 Auto, 1 4:3, 2 16:9
	int scale;          // 0 Auto, 1..8
	int bilinear;
	int antialiasing;
	int pgxp;
	int pgxpGeometry;
	int showFps;
	int padMode;
	int keyboardSlot;
	int deadzone;
	int analog;
	int rumble;
	wchar_t gameExeW[WSTR_MAX];   // display (unescaped) form
	wchar_t discImageW[WSTR_MAX]; // display (unescaped) form
} LauncherConfig;

static LauncherConfig g_cfg;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static int SX(int v) { return MulDiv(v, g_dpi, 96); }

static void Print(const char *fmt, ...)
{
	char line[1024];
	va_list args;

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	if (GetConsoleWindow() != NULL)
	{
		fputs(line, stdout);
		fflush(stdout);
	}

	if (g_outCopy != NULL)
	{
		fputs(line, g_outCopy);
		fflush(g_outCopy);
	}
}

// Debug logging is permanent (never removed); LogLine writes to the log file
// only under --verbose, LogError always records failures.
static void LogWrite(const char *line)
{
	FILE *file;

	file = fopen(g_logPathUtf8, "a");
	if (file != NULL)
	{
		fputs(line, file);
		fclose(file);
	}
}

static void LogLine(const char *fmt, ...)
{
	char line[1024];
	va_list args;

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	Print("%s", line);

	if (g_verbose)
	{
		LogWrite(line);
	}
}

static void LogError(const char *fmt, ...)
{
	char line[1024];
	va_list args;

	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	Print("%s", line);
	LogWrite(line);
}

static void Utf8ToWide(const char *utf8, wchar_t *out, size_t outCount)
{
	if ((utf8 == NULL) || (outCount == 0))
	{
		if (outCount > 0)
		{
			out[0] = 0;
		}
		return;
	}

	if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)outCount) == 0)
	{
		out[0] = 0;
	}
}

static void WideToUtf8(const wchar_t *wide, char *out, size_t outCount)
{
	if ((wide == NULL) || (outCount == 0))
	{
		if (outCount > 0)
		{
			out[0] = 0;
		}
		return;
	}

	if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)outCount, NULL, NULL) == 0)
	{
		out[0] = 0;
	}
}

static int FileExistsW(const wchar_t *path)
{
	DWORD attrs = GetFileAttributesW(path);
	return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static void GetDirPartW(const wchar_t *path, wchar_t *out, size_t outCount)
{
	const wchar_t *slash = wcsrchr(path, L'\\');
	size_t length = (slash != NULL) ? (size_t)(slash - path + 1) : 0;

	if (length >= outCount)
	{
		length = outCount - 1;
	}

	wmemcpy(out, path, length);
	out[length] = 0;
}

static void JoinPathW(const wchar_t *dir, const wchar_t *name, wchar_t *out, size_t outCount)
{
	size_t dirLen = wcslen(dir);
	int needSlash = (dirLen > 0) && (dir[dirLen - 1] != L'\\');

	_snwprintf(out, outCount, L"%s%s%s", dir, needSlash ? L"\\" : L"", name);
	out[outCount - 1] = 0;
}

static int IsAbsolutePathW(const wchar_t *path)
{
	if (path[0] == L'\\' || path[0] == L'/')
	{
		return 1;
	}
	return (iswalpha(path[0]) && (path[1] == L':'));
}

// ---------------------------------------------------------------------------
// JSON value helpers (mirrors platform/native_config.c scanning rules)
// ---------------------------------------------------------------------------

static const char *JsonFindValue(const char *text, const char *key)
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
	while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == '\r') || (*cursor == '\n'))
	{
		cursor++;
	}

	if (*cursor != ':')
	{
		return NULL;
	}

	cursor++;
	while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == '\r') || (*cursor == '\n'))
	{
		cursor++;
	}

	return cursor;
}

static int JsonReadInt(const char *text, const char *key, int *value)
{
	const char *cursor = JsonFindValue(text, key);
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

static int JsonReadBool(const char *text, const char *key, int *value)
{
	const char *cursor = JsonFindValue(text, key);

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

// Copies a quoted JSON string, UNESCAPING \\ and \" (unlike the engine's raw
// copy, the UI needs the human-readable form). Other escape sequences are
// passed through literally.
static int JsonReadStringUnescaped(const char *text, const char *key, char *out, size_t outCount)
{
	const char *cursor = JsonFindValue(text, key);
	size_t length = 0;

	if ((cursor == NULL) || (*cursor != '"'))
	{
		return 0;
	}

	cursor++;

	while ((cursor[0] != 0) && (cursor[0] != '"') && ((length + 1) < outCount))
	{
		if ((cursor[0] == '\\') && ((cursor[1] == '\\') || (cursor[1] == '"')))
		{
			out[length++] = cursor[1];
			cursor += 2;
			continue;
		}

		out[length++] = cursor[0];
		cursor++;
	}

	if (cursor[0] != '"')
	{
		if (outCount > 0)
		{
			out[0] = 0;
		}
		return 0;
	}

	out[length] = 0;
	return 1;
}

// JSON-escapes a UTF-8 string (backslash and quote only; both are all a
// Windows path can contain).
static void JsonEscape(const char *in, char *out, size_t outCount)
{
	size_t w = 0;

	for (size_t r = 0; (in[r] != 0) && ((w + 2) < outCount); r++)
	{
		if ((in[r] == '\\') || (in[r] == '"'))
		{
			out[w++] = '\\';
		}
		out[w++] = in[r];
	}

	out[w] = 0;
}

// ---------------------------------------------------------------------------
// Config load / save
// ---------------------------------------------------------------------------

static void ConfigSetDefaults(LauncherConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	cfg->windowWidth = 1920;
	cfg->windowHeight = 1080;
	cfg->fullscreen = 1;
	cfg->aspect = 1;          // 4:3
	cfg->scale = 0;           // Auto
	cfg->bilinear = 0;
	cfg->antialiasing = 0;
	cfg->pgxp = 0;
	cfg->pgxpGeometry = 0;
	cfg->showFps = 0;
	cfg->padMode = 1;         // 4 pads (always on)
	cfg->keyboardSlot = -2;   // Pads only
	cfg->deadzone = 5;
	cfg->analog = 1;
	cfg->rumble = 1;
	wcsncpy(cfg->gameExeW, L"ctr_native.exe", WSTR_MAX - 1);
	cfg->discImageW[0] = 0;
}

static int ConfigLoad(LauncherConfig *cfg, const char *path)
{
	FILE *file;
	char *text;
	long size;
	int value;
	char buffer[STR_MAX];
	wchar_t wide[WSTR_MAX];

	ConfigSetDefaults(cfg);

	file = fopen(path, "rb");
	if (file == NULL)
	{
		LogLine("[ctr_config] config not found: %s (defaults in use)\n", path);
		return 0;
	}

	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if ((size <= 0) || (size > CONFIG_MAX_BYTES))
	{
		fclose(file);
		LogLine("[ctr_config] config unreadable (size %ld): %s\n", size, path);
		return 0;
	}

	text = (char *)malloc((size_t)size + 1);
	if (text == NULL)
	{
		fclose(file);
		return 0;
	}

	if (fread(text, 1, (size_t)size, file) != (size_t)size)
	{
		fclose(file);
		free(text);
		return 0;
	}
	text[size] = 0;
	fclose(file);

	if ((JsonReadInt(text, "window_width", &value) != 0) && (value >= MIN_WINDOW_WIDTH) && (value <= MAX_WINDOW_WIDTH))
	{
		cfg->windowWidth = value;
	}
	if ((JsonReadInt(text, "window_height", &value) != 0) && (value >= MIN_WINDOW_HEIGHT) && (value <= MAX_WINDOW_HEIGHT))
	{
		cfg->windowHeight = value;
	}
	if (JsonReadBool(text, "fullscreen", &value) != 0)
	{
		cfg->fullscreen = value;
	}
	if (JsonReadStringUnescaped(text, "aspect_ratio", buffer, sizeof(buffer)) != 0)
	{
		if (strcmp(buffer, "4:3") == 0)
		{
			cfg->aspect = 1;
		}
		else if (strcmp(buffer, "16:9") == 0)
		{
			cfg->aspect = 2;
		}
		else
		{
			cfg->aspect = 0;
		}
	}
	if ((JsonReadInt(text, "internal_resolution_scale", &value) != 0) && (value >= 0) && (value <= MAX_SCALE))
	{
		cfg->scale = value;
	}
	if (JsonReadBool(text, "bilinear_filtering", &value) != 0)
	{
		cfg->bilinear = value;
	}
	if (JsonReadBool(text, "antialiasing", &value) != 0)
	{
		cfg->antialiasing = value;
	}
	if (JsonReadBool(text, "pgxp", &value) != 0)
	{
		cfg->pgxp = value;
	}
	if (JsonReadBool(text, "pgxp_geometry", &value) != 0)
	{
		cfg->pgxpGeometry = value;
	}
	if (JsonReadBool(text, "show_fps", &value) != 0)
	{
		cfg->showFps = value;
	}
	if ((JsonReadInt(text, "pad_mode", &value) != 0) && (value >= 0) && (value <= 2))
	{
		cfg->padMode = value;
	}
	if ((JsonReadInt(text, "keyboard_slot", &value) != 0) && (value >= -2) && (value <= 3))
	{
		cfg->keyboardSlot = value;
	}
	if ((JsonReadInt(text, "gamepad_deadzone", &value) != 0) && (value >= 0) && (value <= 50))
	{
		cfg->deadzone = value;
	}
	if (JsonReadBool(text, "gamepad_analog", &value) != 0)
	{
		cfg->analog = value;
	}
	if (JsonReadBool(text, "gamepad_rumble", &value) != 0)
	{
		cfg->rumble = value;
	}
	if ((JsonReadStringUnescaped(text, "game_executable", buffer, sizeof(buffer)) != 0) && (buffer[0] != 0))
	{
		Utf8ToWide(buffer, wide, WSTR_MAX);
		wcsncpy(cfg->gameExeW, wide, WSTR_MAX - 1);
	}
	if (JsonReadStringUnescaped(text, "disc_image", buffer, sizeof(buffer)) != 0)
	{
		Utf8ToWide(buffer, wide, WSTR_MAX);
		wcsncpy(cfg->discImageW, wide, WSTR_MAX - 1);
	}

	free(text);
	return 1;
}

static int ConfigSave(const LauncherConfig *cfg, const char *path)
{
	static const char *aspectNames[3] = { "Auto", "4:3", "16:9" };
	char tempPath[STR_MAX];
	char gameExeUtf8[STR_MAX];
	char gameExeEscaped[STR_MAX * 2];
	char discUtf8[STR_MAX];
	char discEscaped[STR_MAX * 2];
	FILE *file;
	int written;

	WideToUtf8(cfg->gameExeW, gameExeUtf8, sizeof(gameExeUtf8));
	WideToUtf8(cfg->discImageW, discUtf8, sizeof(discUtf8));
	JsonEscape(gameExeUtf8, gameExeEscaped, sizeof(gameExeEscaped));
	JsonEscape(discUtf8, discEscaped, sizeof(discEscaped));

	snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);

	file = fopen(tempPath, "wb");
	if (file == NULL)
	{
		LogError("[ctr_config] ERROR: cannot write %s\n", tempPath);
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
	                  cfg->windowWidth,
	                  cfg->windowHeight,
	                  (cfg->fullscreen != 0) ? "true" : "false",
	                  aspectNames[(cfg->aspect >= 0 && cfg->aspect < 3) ? cfg->aspect : 0],
	                  cfg->scale,
	                  (cfg->bilinear != 0) ? "true" : "false",
	                  (cfg->antialiasing != 0) ? "true" : "false",
	                  (cfg->pgxp != 0) ? "true" : "false",
	                  (cfg->pgxpGeometry != 0) ? "true" : "false",
	                  (cfg->showFps != 0) ? "true" : "false",
	                  cfg->padMode,
	                  cfg->keyboardSlot,
	                  cfg->deadzone,
	                  (cfg->analog != 0) ? "true" : "false",
	                  (cfg->rumble != 0) ? "true" : "false",
	                  gameExeEscaped,
	                  discEscaped);

	fclose(file);

	if (written <= 0)
	{
		remove(tempPath);
		LogError("[ctr_config] ERROR: short write to %s\n", tempPath);
		return 0;
	}

	remove(path);
	if (rename(tempPath, path) != 0)
	{
		LogError("[ctr_config] ERROR: rename %s -> %s failed\n", tempPath, path);
		return 0;
	}

	return 1;
}

// ---------------------------------------------------------------------------
// Disc region detection (port of the Python launcher's reader)
// ---------------------------------------------------------------------------

static const char *RegionLabelFromPrefix(const char *prefix4)
{
	if ((strcmp(prefix4, "SCUS") == 0) || (strcmp(prefix4, "SLUS") == 0))
	{
		return "NTSC-U (supported)";
	}
	if ((strcmp(prefix4, "SCES") == 0) || (strcmp(prefix4, "SLES") == 0))
	{
		return "PAL - not supported by the NTSC-U build";
	}
	if ((strcmp(prefix4, "SCPS") == 0) || (strcmp(prefix4, "SLPS") == 0))
	{
		return "NTSC-J - not supported by the NTSC-U build";
	}
	return "unrecognized";
}

static unsigned int ReadLe32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

// Returns: 0 ok (bootId/label filled), 1 could not identify, 2 file missing.
static int DiscDetectRegion(const wchar_t *pathW, char *bootIdOut, size_t bootIdCount, char *labelOut, size_t labelCount)
{
	static const int sectors[3] = { 2352, 2352, 2048 };
	static const int offsets[3] = { 24, 16, 0 };
	FILE *file;
	__int64 fileSize = 0;
	int found = -1;
	int sector = 0;
	int off = 0;
	unsigned char raw[2352];
	unsigned char pvd[2048];
	unsigned char data[65536];
	size_t dataLen = 0;
	unsigned int rootLba;
	unsigned int rootSize;
	unsigned int pos;

	bootIdOut[0] = 0;
	labelOut[0] = 0;

	file = _wfopen(pathW, L"rb");
	if (file == NULL)
	{
		return 2;
	}

	_fseeki64(file, 0, SEEK_END);
	fileSize = _ftelli64(file);
	_fseeki64(file, 0, SEEK_SET);

	for (int i = 0; i < 3; i++)
	{
		if ((fileSize % sectors[i]) != 0)
		{
			continue;
		}

		_fseeki64(file, (__int64)16 * sectors[i], SEEK_SET);
		if (fread(raw, 1, (size_t)sectors[i], file) == (size_t)sectors[i])
		{
			if (memcmp(raw + offsets[i] + 1, "CD001", 5) == 0)
			{
				found = i;
				break;
			}
		}
	}

	if (found < 0)
	{
		fclose(file);
		return 1;
	}

	sector = sectors[found];
	off = offsets[found];

// Reads one sector and returns its 2048-byte user area in `out`.
	#define READ_SECTOR(lba) _fseeki64(file, (__int64)(lba) * sector, SEEK_SET), \
		(fread(raw, 1, (size_t)sector, file) == (size_t)sector ? memcpy(pvd, raw + off, 2048), 0 : 1)

	if (READ_SECTOR(16) != 0)
	{
		fclose(file);
		return 1;
	}

	rootLba = ReadLe32(pvd + 158);
	rootSize = ReadLe32(pvd + 166);
	if (rootSize > sizeof(data))
	{
		rootSize = sizeof(data);
	}

	{
		unsigned int lba = rootLba;
		unsigned int remaining = rootSize;

		while (remaining > 0)
		{
			size_t chunk = (remaining > 2048) ? 2048 : remaining;

			if (READ_SECTOR(lba) != 0)
			{
				break;
			}
			memcpy(data + dataLen, pvd, chunk);
			dataLen += chunk;
			remaining -= (unsigned int)chunk;
			lba++;
		}
	}

	pos = 0;
	while (pos < dataLen)
	{
		unsigned int recLen = data[pos];
		unsigned int nameLen;
		char name[64];

		if (recLen == 0)
		{
			pos = ((pos / 2048) + 1) * 2048;
			continue;
		}

		if ((pos + 33 + 1) > dataLen)
		{
			break;
		}

		nameLen = data[pos + 32];
		if (nameLen >= sizeof(name))
		{
			nameLen = sizeof(name) - 1;
		}
		memcpy(name, data + pos + 33, nameLen);
		name[nameLen] = 0;

		{
			char *semi = strchr(name, ';');
			if (semi != NULL)
			{
				*semi = 0;
			}
		}

		if (_stricmp(name, "SYSTEM.CNF") == 0)
		{
			unsigned int fileLba = ReadLe32(data + pos + 2);
			unsigned int entrySize = ReadLe32(data + pos + 10);
			char text[16384];
			size_t textLen = 0;

			if (entrySize > sizeof(text) - 1)
			{
				entrySize = sizeof(text) - 1;
			}

			while (textLen < entrySize)
			{
				size_t chunk = entrySize - textLen;

				if (chunk > 2048)
				{
					chunk = 2048;
				}
				if (READ_SECTOR(fileLba) != 0)
				{
					break;
				}
				memcpy(text + textLen, pvd, chunk);
				textLen += chunk;
				fileLba++;
			}
			text[textLen] = 0;

			{
				char *line = text;
				while ((line != NULL) && (*line != 0))
				{
					char *next = strchr(line, '\n');
					char upper[512];

					if (next != NULL)
					{
						*next = 0;
						next++;
					}

					if (strlen(line) < sizeof(upper))
					{
						size_t i;
						for (i = 0; line[i] != 0; i++)
						{
							upper[i] = (char)toupper((unsigned char)line[i]);
						}
						upper[i] = 0;

						if ((strstr(upper, "BOOT") != NULL) && (strstr(upper, "CDROM:") != NULL))
						{
							char *value = strstr(upper, "CDROM:") + 6;

							while ((*value == '\\') || (*value == '/') || (*value == ' '))
							{
								value++;
							}
							{
								char *semi = strchr(value, ';');
								if (semi != NULL)
								{
									*semi = 0;
								}
							}
							{
								size_t len = strlen(value);
								while ((len > 0) && ((value[len - 1] == ' ') || (value[len - 1] == '\r')))
								{
									value[--len] = 0;
								}
							}

							snprintf(bootIdOut, bootIdCount, "%s", value);
							{
								char prefix4[5] = { 0 };
								for (int p = 0; (p < 4) && (value[p] != 0); p++)
								{
									prefix4[p] = (char)toupper((unsigned char)value[p]);
								}
								snprintf(labelOut, labelCount, "%s", RegionLabelFromPrefix(prefix4));
							}
							fclose(file);
							return 0;
						}
					}

					line = next;
				}
			}

			fclose(file);
			return 1;
		}

		pos += recLen;
	}

	fclose(file);
	return 1;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

static const wchar_t *g_resLabels[] = {
	L"640 x 480  (4:3)",
	L"800 x 600  (4:3)",
	L"1024 x 768 (4:3)",
	L"1280 x 720 (16:9)",
	L"1280 x 960 (4:3)",
	L"1920 x 1080 (16:9)",
	L"2560 x 1440 (16:9)",
	L"3840 x 2160 (16:9)",
};
static const int g_resSizes[][2] = {
	{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 },
	{ 1280, 960 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
};
#define RES_COUNT (int)(sizeof(g_resLabels) / sizeof(g_resLabels[0]))

static const wchar_t *g_scaleLabels[] = { L"Auto (screen)", L"1x  (native PSX)", L"2x", L"3x", L"4x", L"8x" };
static const int g_scaleValues[] = { 0, 1, 2, 3, 4, 8 };

static const wchar_t *g_padLabels[] = {
	L"4 pads (always on, even if disconnected)",
	L"Auto (detect at boot)",
	L"2 pads (single tap)",
};
static const int g_padValues[] = { 1, 0, 2 };

static const wchar_t *g_kbLabels[] = {
	L"Pads only",
	L"Player 1",
	L"Player 2",
	L"Player 3",
	L"Player 4",
	L"Auto (moves aside for pads)",
};
static const int g_kbValues[] = { -2, 0, 1, 2, 3, -1 };

static HWND MkCtl(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
	HWND ctl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
	                           SX(x), SX(y), SX(w), SX(h),
	                           g_hWnd, (HMENU)(INT_PTR)id, g_hInst, NULL);
	SendMessageW(ctl, WM_SETFONT, (WPARAM)g_hFont, TRUE);
	return ctl;
}

static void ComboFill(HWND hCombo, const wchar_t *const *labels, const int *values, int count, int current)
{
	int select = 0;

	for (int i = 0; i < count; i++)
	{
		int index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)labels[i]);
		SendMessageW(hCombo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)values[i]);
		if (values[i] == current)
		{
			select = index;
		}
	}
	SendMessageW(hCombo, CB_SETCURSEL, (WPARAM)select, 0);
}

static int ComboSelectedValue(HWND hCombo)
{
	int index = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);

	if (index == CB_ERR)
	{
		return -999;
	}
	return (int)SendMessageW(hCombo, CB_GETITEMDATA, (WPARAM)index, 0);
}

static void SetEditText(int id, const wchar_t *text)
{
	SetDlgItemTextW(g_hWnd, id, text);
}

static void GetEditText(int id, wchar_t *out, size_t outCount)
{
	GetDlgItemTextW(g_hWnd, id, out, (int)outCount);
}

static int GetEditInt(int id, int fallback)
{
	wchar_t text[32];
	wchar_t *end = NULL;
	long value;

	GetEditText(id, text, 32);
	value = wcstol(text, &end, 10);
	if (end == text)
	{
		return fallback;
	}
	return (int)value;
}

static void UpdateRegionLabel(void);

static void UpdateRegionLabel(void)
{
	wchar_t disc[WSTR_MAX];
	wchar_t full[WSTR_MAX];
	char bootId[64];
	char label[128];
	int result;

	GetEditText(IDC_DISC, disc, WSTR_MAX);

	if (disc[0] == 0)
	{
		g_regionColor = RGB(110, 110, 110);
		SetDlgItemTextW(g_hWnd, IDC_REGION, L"No disc image set - the game will use assets/ctr-u.bin.");
		return;
	}

	if (IsAbsolutePathW(disc))
	{
		wcsncpy(full, disc, WSTR_MAX - 1);
	}
	else
	{
		JoinPathW(g_launcherDirW, disc, full, WSTR_MAX);
	}

	if (!FileExistsW(full))
	{
		wchar_t message[WSTR_MAX + 64];
		g_regionColor = RGB(176, 0, 32);
		_snwprintf(message, WSTR_MAX + 64, L"File not found: %s", full);
		message[WSTR_MAX + 63] = 0;
		SetDlgItemTextW(g_hWnd, IDC_REGION, message);
		return;
	}

	result = DiscDetectRegion(full, bootId, sizeof(bootId), label, sizeof(label));
	if (result == 0)
	{
		wchar_t message[256];
		g_regionColor = (strncmp(label, "NTSC-U", 6) == 0) ? RGB(26, 127, 55) : RGB(176, 0, 32);
		_snwprintf(message, 256, L"Detected: %hs - %hs", label, bootId);
		message[255] = 0;
		SetDlgItemTextW(g_hWnd, IDC_REGION, message);
	}
	else
	{
		g_regionColor = RGB(176, 0, 32);
		SetDlgItemTextW(g_hWnd, IDC_REGION, L"Could not identify this image (CHD/ECM files must be converted first).");
	}
}

static void LoadConfigIntoUi(void)
{
	HWND hResCombo = GetDlgItem(g_hWnd, IDC_RES_COMBO);
	wchar_t buffer[WSTR_MAX];

	ComboFill(hResCombo, g_resLabels, (const int *)g_resSizes, RES_COUNT, -1); // fill only

	// Window size: pick matching preset, still allow arbitrary edits.
	{
		int select = -1;
		for (int i = 0; i < RES_COUNT; i++)
		{
			if ((g_resSizes[i][0] == g_cfg.windowWidth) && (g_resSizes[i][1] == g_cfg.windowHeight))
			{
				select = i;
				break;
			}
		}
		SendMessageW(hResCombo, CB_SETCURSEL, (WPARAM)select, 0); // CB_ERR (-1) = custom
	}

	_snwprintf(buffer, WSTR_MAX, L"%d", g_cfg.windowWidth);
	buffer[WSTR_MAX - 1] = 0;
	SetEditText(IDC_RES_W, buffer);
	_snwprintf(buffer, WSTR_MAX, L"%d", g_cfg.windowHeight);
	buffer[WSTR_MAX - 1] = 0;
	SetEditText(IDC_RES_H, buffer);

	{
		static const wchar_t *aspectLabels[] = { L"Auto", L"4:3", L"16:9" };
		static const int aspectValues[] = { 0, 1, 2 };

		ComboFill(GetDlgItem(g_hWnd, IDC_ASPECT), aspectLabels, aspectValues, 3, g_cfg.aspect);
		ComboFill(GetDlgItem(g_hWnd, IDC_SCALE), g_scaleLabels, g_scaleValues, 6, g_cfg.scale);
	}

	CheckDlgButton(g_hWnd, IDC_FULLSCREEN, g_cfg.fullscreen ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(g_hWnd, IDC_BILINEAR, g_cfg.bilinear ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(g_hWnd, IDC_AA, g_cfg.antialiasing ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(g_hWnd, IDC_PGXP, g_cfg.pgxp ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(g_hWnd, IDC_PGXP_GEO, g_cfg.pgxpGeometry ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(g_hWnd, IDC_SHOWFPS, g_cfg.showFps ? BST_CHECKED : BST_UNCHECKED);

	ComboFill(GetDlgItem(g_hWnd, IDC_PADMODE), g_padLabels, g_padValues, 3, g_cfg.padMode);
	ComboFill(GetDlgItem(g_hWnd, IDC_KBSLOT), g_kbLabels, g_kbValues, 6, g_cfg.keyboardSlot);

	_snwprintf(buffer, WSTR_MAX, L"%d", g_cfg.deadzone);
	buffer[WSTR_MAX - 1] = 0;
	SetEditText(IDC_DEADZONE, buffer);

	CheckDlgButton(g_hWnd, IDC_ANALOG, g_cfg.analog ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(g_hWnd, IDC_RUMBLE, g_cfg.rumble ? BST_CHECKED : BST_UNCHECKED);

	SetEditText(IDC_GAMEEXE, g_cfg.gameExeW[0] ? g_cfg.gameExeW : L"ctr_native.exe");

	// Disc image: show the configured value, else the bundled default if present.
	if (g_cfg.discImageW[0] == 0)
	{
		wchar_t candidate[WSTR_MAX];
		JoinPathW(g_launcherDirW, L"assets\\ctr-u.bin", candidate, WSTR_MAX);
		if (FileExistsW(candidate))
		{
			wcsncpy(g_cfg.discImageW, L"assets\\ctr-u.bin", WSTR_MAX - 1);
		}
	}
	SetEditText(IDC_DISC, g_cfg.discImageW);
	UpdateRegionLabel();

	// Verbose read-back: shows exactly what the controls hold after loading
	// (permanent debug aid; active with --verbose).
	if (g_verbose)
	{
		wchar_t dz[64];
		wchar_t wr[64];
		wchar_t hr[64];

		GetEditText(IDC_DEADZONE, dz, 64);
		GetEditText(IDC_RES_W, wr, 64);
		GetEditText(IDC_RES_H, hr, 64);
		LogLine("[ctr_config] ui-loaded: w='%ls' h='%ls' deadzone='%ls' pgxp=%d\n",
		        wr, hr, dz, IsDlgButtonChecked(g_hWnd, IDC_PGXP));
	}
}

// Makes a path relative to the launcher folder when it lives inside it (keeps
// the install movable), otherwise returns it unchanged (mirrors the Python
// launcher's _portable_path).
static void PortablePathW(const wchar_t *input, wchar_t *out, size_t outCount)
{
	wchar_t candidate[WSTR_MAX];
	wchar_t full[WSTR_MAX];
	DWORD length;
	size_t dirLen;

	if ((input == NULL) || (input[0] == 0))
	{
		out[0] = 0;
		return;
	}

	// Relative inputs are relative to the launcher folder (how they are
	// stored), not to the process working directory.
	if (IsAbsolutePathW(input))
	{
		wcsncpy(candidate, input, WSTR_MAX - 1);
		candidate[WSTR_MAX - 1] = 0;
	}
	else
	{
		JoinPathW(g_launcherDirW, input, candidate, WSTR_MAX);
	}

	length = GetFullPathNameW(candidate, WSTR_MAX, full, NULL);
	if ((length == 0) || (length >= WSTR_MAX))
	{
		wcsncpy(out, input, outCount - 1);
		out[outCount - 1] = 0;
		return;
	}

	dirLen = wcslen(g_launcherDirW);
	if ((_wcsnicmp(full, g_launcherDirW, dirLen) == 0) && (full[dirLen] != 0))
	{
		const wchar_t *rel = full + dirLen;
		if ((rel[0] != L'.') || (rel[1] != L'.'))
		{
			wcsncpy(out, rel, outCount - 1);
			out[outCount - 1] = 0;
			return;
		}
	}

	wcsncpy(out, input, outCount - 1);
	out[outCount - 1] = 0;
}

static int SaveConfigFromUi(int quiet)
{
	wchar_t buffer[WSTR_MAX];
	wchar_t portable[WSTR_MAX];
	int value;
	int ok;

	g_cfg.windowWidth = GetEditInt(IDC_RES_W, g_cfg.windowWidth);
	g_cfg.windowHeight = GetEditInt(IDC_RES_H, g_cfg.windowHeight);
	if (g_cfg.windowWidth < MIN_WINDOW_WIDTH)
	{
		g_cfg.windowWidth = MIN_WINDOW_WIDTH;
	}
	if (g_cfg.windowWidth > MAX_WINDOW_WIDTH)
	{
		g_cfg.windowWidth = MAX_WINDOW_WIDTH;
	}
	if (g_cfg.windowHeight < MIN_WINDOW_HEIGHT)
	{
		g_cfg.windowHeight = MIN_WINDOW_HEIGHT;
	}
	if (g_cfg.windowHeight > MAX_WINDOW_HEIGHT)
	{
		g_cfg.windowHeight = MAX_WINDOW_HEIGHT;
	}

	value = ComboSelectedValue(GetDlgItem(g_hWnd, IDC_ASPECT));
	g_cfg.aspect = (value >= 0) ? value : 0;

	value = ComboSelectedValue(GetDlgItem(g_hWnd, IDC_SCALE));
	g_cfg.scale = (value >= 0) ? value : 0;

	g_cfg.fullscreen = (IsDlgButtonChecked(g_hWnd, IDC_FULLSCREEN) == BST_CHECKED) ? 1 : 0;
	g_cfg.bilinear = (IsDlgButtonChecked(g_hWnd, IDC_BILINEAR) == BST_CHECKED) ? 1 : 0;
	g_cfg.antialiasing = (IsDlgButtonChecked(g_hWnd, IDC_AA) == BST_CHECKED) ? 1 : 0;
	g_cfg.pgxp = (IsDlgButtonChecked(g_hWnd, IDC_PGXP) == BST_CHECKED) ? 1 : 0;
	g_cfg.pgxpGeometry = (IsDlgButtonChecked(g_hWnd, IDC_PGXP_GEO) == BST_CHECKED) ? 1 : 0;
	g_cfg.showFps = (IsDlgButtonChecked(g_hWnd, IDC_SHOWFPS) == BST_CHECKED) ? 1 : 0;

	value = ComboSelectedValue(GetDlgItem(g_hWnd, IDC_PADMODE));
	g_cfg.padMode = (value >= -1) ? value : 1;

	value = ComboSelectedValue(GetDlgItem(g_hWnd, IDC_KBSLOT));
	g_cfg.keyboardSlot = (value >= -2) ? value : -2;

	g_cfg.deadzone = GetEditInt(IDC_DEADZONE, g_cfg.deadzone);
	if (g_cfg.deadzone < 0)
	{
		g_cfg.deadzone = 0;
	}
	if (g_cfg.deadzone > 50)
	{
		g_cfg.deadzone = 50;
	}

	g_cfg.analog = (IsDlgButtonChecked(g_hWnd, IDC_ANALOG) == BST_CHECKED) ? 1 : 0;
	g_cfg.rumble = (IsDlgButtonChecked(g_hWnd, IDC_RUMBLE) == BST_CHECKED) ? 1 : 0;

	GetEditText(IDC_GAMEEXE, buffer, WSTR_MAX);
	PortablePathW(buffer, portable, WSTR_MAX);
	if (portable[0] == 0)
	{
		wcsncpy(portable, L"ctr_native.exe", WSTR_MAX - 1);
	}
	wcsncpy(g_cfg.gameExeW, portable, WSTR_MAX - 1);

	GetEditText(IDC_DISC, buffer, WSTR_MAX);
	PortablePathW(buffer, portable, WSTR_MAX);
	wcsncpy(g_cfg.discImageW, portable, WSTR_MAX - 1);

	ok = ConfigSave(&g_cfg, g_configPathUtf8);

	if (g_verbose)
	{
		char exeUtf8[STR_MAX];
		char discUtf8[STR_MAX];
		wchar_t dzRaw[64];

		GetEditText(IDC_DEADZONE, dzRaw, 64);
		WideToUtf8(g_cfg.gameExeW, exeUtf8, sizeof(exeUtf8));
		WideToUtf8(g_cfg.discImageW, discUtf8, sizeof(discUtf8));
		LogLine("[ctr_config] save: window=%dx%d fullscreen=%d aspect=%d scale=%d bilinear=%d aa=%d pgxp=%d pgxpGeo=%d fps=%d pad=%d kb=%d dz=%d dzEdit='%ls' analog=%d rumble=%d exe='%s' disc='%s'\n",
		        g_cfg.windowWidth, g_cfg.windowHeight, g_cfg.fullscreen, g_cfg.aspect, g_cfg.scale,
		        g_cfg.bilinear, g_cfg.antialiasing, g_cfg.pgxp, g_cfg.pgxpGeometry, g_cfg.showFps,
		        g_cfg.padMode, g_cfg.keyboardSlot, g_cfg.deadzone, dzRaw, g_cfg.analog, g_cfg.rumble,
		        exeUtf8, discUtf8);
	}

	if (!ok)
	{
		LogError("[ctr_config] ERROR: could not write %s\n", g_configPathUtf8);
		MessageBoxW(g_hWnd, L"Could not write the configuration file.\nSee ctr_config.log for details.",
		            APP_TITLE, MB_ICONERROR | MB_OK);
		return 0;
	}

	if (!quiet)
	{
		wchar_t message[WSTR_MAX + 64];
		_snwprintf(message, WSTR_MAX + 64, L"Configuration saved to:\n%hs", g_configPathUtf8);
		message[WSTR_MAX + 63] = 0;
		MessageBoxW(g_hWnd, message, APP_TITLE, MB_ICONINFORMATION | MB_OK);
	}

	return 1;
}

static void BrowseForFile(HWND hWnd, int editId, const wchar_t *title, const wchar_t *filter)
{
	wchar_t file[WSTR_MAX] = { 0 };
	OPENFILENAMEW ofn;

	GetEditText(editId, file, WSTR_MAX);

	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hWnd;
	ofn.lpstrFile = file;
	ofn.nMaxFile = WSTR_MAX;
	ofn.lpstrFilter = filter;
	ofn.lpstrTitle = title;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

	if (GetOpenFileNameW(&ofn))
	{
		wchar_t portable[WSTR_MAX];
		PortablePathW(file, portable, WSTR_MAX);
		SetEditText(editId, portable);
		if (editId == IDC_DISC)
		{
			UpdateRegionLabel();
		}
	}
}

static int ResolveGamePathW(wchar_t *out, size_t outCount)
{
	wchar_t exe[WSTR_MAX];

	GetEditText(IDC_GAMEEXE, exe, WSTR_MAX);
	if (exe[0] == 0)
	{
		wcsncpy(exe, L"ctr_native.exe", WSTR_MAX - 1);
	}

	if (IsAbsolutePathW(exe))
	{
		wcsncpy(out, exe, outCount - 1);
	}
	else
	{
		JoinPathW(g_launcherDirW, exe, out, outCount);
	}
	out[outCount - 1] = 0;
	return FileExistsW(out);
}

static void SaveAndPlay(void)
{
	wchar_t game[WSTR_MAX];
	wchar_t disc[WSTR_MAX];
	char bootId[64];
	char label[128];
	int result;

	if (!SaveConfigFromUi(1))
	{
		return;
	}

	if (!ResolveGamePathW(game, WSTR_MAX))
	{
		wchar_t message[WSTR_MAX + 128];
		_snwprintf(message, WSTR_MAX + 128,
		           L"Game executable not found:\n%s\n\nUse Browse... to select ctr_native.exe.", game);
		message[WSTR_MAX + 127] = 0;
		MessageBoxW(g_hWnd, message, APP_TITLE, MB_ICONERROR | MB_OK);
		return;
	}

	GetEditText(IDC_DISC, disc, WSTR_MAX);
	if (disc[0] != 0)
	{
		wchar_t full[WSTR_MAX];

		if (IsAbsolutePathW(disc))
		{
			wcsncpy(full, disc, WSTR_MAX - 1);
		}
		else
		{
			JoinPathW(g_launcherDirW, disc, full, WSTR_MAX);
		}

		if (!FileExistsW(full))
		{
			wchar_t message[WSTR_MAX + 256];
			_snwprintf(message, WSTR_MAX + 256,
			           L"Disc image not found:\n%s\n\nThe game will fall back to assets/ctr-u.bin if present.\nLaunch anyway?", full);
			message[WSTR_MAX + 255] = 0;
			if (MessageBoxW(g_hWnd, message, APP_TITLE, MB_ICONWARNING | MB_YESNO) != IDYES)
			{
				return;
			}
		}
		else
		{
			result = DiscDetectRegion(full, bootId, sizeof(bootId), label, sizeof(label));
			if ((result == 0) && (strncmp(label, "NTSC-U", 6) != 0))
			{
				wchar_t message[512];
				_snwprintf(message, 512,
				           L"This disc is %hs.\n\nThe game will refuse to run it (clean exit, no crash).\nUse an NTSC-U (SCUS-94426) image instead.\nLaunch anyway?", label);
				message[511] = 0;
				if (MessageBoxW(g_hWnd, message, APP_TITLE, MB_ICONWARNING | MB_YESNO) != IDYES)
				{
					return;
				}
			}
		}
	}

	if (MessageBoxW(g_hWnd, L"Launch the game now?", APP_TITLE, MB_ICONQUESTION | MB_YESNO) != IDYES)
	{
		return;
	}

	{
		STARTUPINFOW si;
		PROCESS_INFORMATION pi;
		wchar_t workDir[WSTR_MAX];
		BOOL created;

		memset(&si, 0, sizeof(si));
		si.cb = sizeof(si);
		memset(&pi, 0, sizeof(pi));

		GetDirPartW(game, workDir, WSTR_MAX);

		created = CreateProcessW(game, NULL, NULL, NULL, FALSE, 0, NULL,
		                         workDir[0] ? workDir : NULL, &si, &pi);
		if (!created)
		{
			wchar_t message[256];
			_snwprintf(message, 256, L"Failed to launch game (error %lu):\n%s", (unsigned long)GetLastError(), game);
			message[255] = 0;
			LogError("[ctr_config] ERROR: CreateProcess failed for %ls (err %lu)\n", game, (unsigned long)GetLastError());
			MessageBoxW(g_hWnd, message, APP_TITLE, MB_ICONERROR | MB_OK);
			return;
		}

		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		LogLine("[ctr_config] launched: %ls\n", game);
		DestroyWindow(g_hWnd);
	}
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
		case WM_CREATE:
		{
			// Children are created during CreateWindowExW, before RunGui gets
			// the HWND back; publish it first so MkCtl/GetDlgItem work.
			g_hWnd = hWnd;

			// Graphics group
			MkCtl(L"BUTTON", L"Graphics", BS_GROUPBOX, 12, 12, 436, 286, -1);
			MkCtl(L"STATIC", L"Window resolution:", SS_LEFT, 24, 38, 110, 20, -1);
			MkCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 138, 34, 166, 200, IDC_RES_COMBO);
			MkCtl(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, 310, 34, 48, 22, IDC_RES_W);
			MkCtl(L"STATIC", L"x", SS_LEFT, 362, 38, 12, 20, -1);
			MkCtl(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, 376, 34, 48, 22, IDC_RES_H);

			MkCtl(L"STATIC", L"Aspect ratio:", SS_LEFT, 24, 68, 110, 20, -1);
			MkCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 138, 64, 166, 200, IDC_ASPECT);

			MkCtl(L"STATIC", L"Internal resolution:", SS_LEFT, 24, 98, 110, 20, -1);
			MkCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 138, 94, 166, 200, IDC_SCALE);

			MkCtl(L"BUTTON", L"Fullscreen", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 124, 420, 20, IDC_FULLSCREEN);
			MkCtl(L"BUTTON", L"Bilinear filtering (smoothing)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 146, 420, 20, IDC_BILINEAR);
			MkCtl(L"BUTTON", L"Anti-aliasing (smooths 3D edges only)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 168, 420, 20, IDC_AA);
			MkCtl(L"BUTTON", L"PGXP textures (P) - perspective-correct interpolation (exp.)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 190, 420, 20, IDC_PGXP);
			MkCtl(L"BUTTON", L"PGXP geometry (G) - subpixel vertex positions (exp.; may seam)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 212, 420, 20, IDC_PGXP_GEO);
			MkCtl(L"STATIC", L"PGXP: run one mode at a time - textures (P) or geometry (G),\nnot both (both on can tear).", SS_LEFT, 24, 234, 410, 34, IDC_HINT_PGXP);
			MkCtl(L"BUTTON", L"Show FPS counter (Insert)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 270, 420, 20, IDC_SHOWFPS);

			// Gamepad group
			MkCtl(L"BUTTON", L"Gamepad", BS_GROUPBOX, 460, 12, 432, 286, -1);
			MkCtl(L"STATIC", L"Pad layout:", SS_LEFT, 472, 38, 100, 20, -1);
			MkCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 586, 34, 260, 200, IDC_PADMODE);
			MkCtl(L"STATIC", L"Keyboard plays as:", SS_LEFT, 472, 68, 110, 20, -1);
			MkCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 586, 64, 260, 200, IDC_KBSLOT);
			MkCtl(L"STATIC", L"Stick deadzone (%):", SS_LEFT, 472, 98, 110, 20, -1);
			MkCtl(L"EDIT", L"", ES_NUMBER | WS_BORDER | WS_TABSTOP, 586, 94, 50, 22, IDC_DEADZONE);
			MkCtl(L"BUTTON", L"Analog sticks enabled by default", BS_AUTOCHECKBOX | WS_TABSTOP, 472, 124, 420, 20, IDC_ANALOG);
			MkCtl(L"BUTTON", L"Rumble", BS_AUTOCHECKBOX | WS_TABSTOP, 472, 146, 420, 20, IDC_RUMBLE);
			MkCtl(L"STATIC", L"Pads auto-attach to players 1-4; drops reconnect to the same\nslot. In-game: F6 swaps pads, F4 assigns the keyboard.", SS_LEFT, 472, 172, 410, 60, IDC_PADNOTE);

			// Game executable group
			MkCtl(L"BUTTON", L"Game executable", BS_GROUPBOX, 12, 310, 436, 66, -1);
			MkCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 24, 336, 316, 22, IDC_GAMEEXE);
			MkCtl(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 346, 335, 90, 24, IDC_BROWSE_EXE);

			// Game data group
			MkCtl(L"BUTTON", L"Game data", BS_GROUPBOX, 460, 310, 432, 122, -1);
			MkCtl(L"STATIC", L"Disc image (ctr-u.bin / ISO):", SS_LEFT, 472, 324, 150, 20, -1);
			MkCtl(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 472, 346, 300, 22, IDC_DISC);
			MkCtl(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 780, 345, 100, 24, IDC_BROWSE_DISC);
			MkCtl(L"STATIC", L"Pick any BIN/ISO, or leave empty for assets/ctr-u.bin.", SS_LEFT, 472, 374, 410, 20, IDC_DISCHINT);
			MkCtl(L"STATIC", L"", SS_LEFT, 472, 396, 410, 32, IDC_REGION);

			// Buttons
			MkCtl(L"BUTTON", L"Save", BS_PUSHBUTTON | WS_TABSTOP, 12, 448, 90, 30, IDC_SAVE);
			MkCtl(L"BUTTON", L"Save & Play", BS_PUSHBUTTON | WS_TABSTOP, 112, 448, 110, 30, IDC_SAVEPLAY);
			MkCtl(L"BUTTON", L"Quit", BS_PUSHBUTTON | WS_TABSTOP, 232, 448, 90, 30, IDC_QUIT);

			MkCtl(L"STATIC", L"Settings are written to ctr-native-config.json next to the game and\napplied when the game starts; paths inside the folder are kept relative.", SS_LEFT, 12, 490, 500, 36, IDC_NOTE);

			LoadConfigIntoUi();
			return 0;
		}

		case WM_COMMAND:
		{
			int id = LOWORD(wParam);
			int notify = HIWORD(wParam);

			switch (id)
			{
				case IDC_SAVE:
					SaveConfigFromUi(0);
					return 0;

				case IDC_SAVEPLAY:
					SaveAndPlay();
					return 0;

				case IDC_QUIT:
					DestroyWindow(hWnd);
					return 0;

				case IDC_BROWSE_EXE:
					BrowseForFile(hWnd, IDC_GAMEEXE, L"Select ctr_native.exe",
					              L"Executable\0*.exe\0All files\0*.*\0");
					return 0;

				case IDC_BROWSE_DISC:
					BrowseForFile(hWnd, IDC_DISC, L"Select the game disc image (ctr-u.bin / ISO)",
					              L"Disc image\0*.bin;*.iso;*.img\0All files\0*.*\0");
					return 0;

				case IDC_RES_COMBO:
					if (notify == CBN_SELCHANGE)
					{
						int index = (int)SendMessageW((HWND)lParam, CB_GETCURSEL, 0, 0);
						wchar_t buffer[32];

						if ((index != CB_ERR) && (index < RES_COUNT))
						{
							_snwprintf(buffer, 32, L"%d", g_resSizes[index][0]);
							buffer[31] = 0;
							SetEditText(IDC_RES_W, buffer);
							_snwprintf(buffer, 32, L"%d", g_resSizes[index][1]);
							buffer[31] = 0;
							SetEditText(IDC_RES_H, buffer);
						}
					}
					return 0;

				case IDC_DISC:
					if (notify == EN_KILLFOCUS)
					{
						UpdateRegionLabel();
					}
					return 0;

				default:
					return 0;
			}
		}

		case WM_CTLCOLORSTATIC:
		{
			HWND ctl = (HWND)lParam;
			int id = GetDlgCtrlID(ctl);

			if (id == IDC_REGION)
			{
				SetTextColor((HDC)wParam, g_regionColor);
				return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
			}
			if ((id == IDC_HINT_PGXP) || (id == IDC_PADNOTE) || (id == IDC_DISCHINT) || (id == IDC_NOTE))
			{
				SetTextColor((HDC)wParam, RGB(110, 110, 110));
				return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
			}
			break;
		}

		case WM_CLOSE:
			DestroyWindow(hWnd);
			return 0;

		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;

		default:
			break;
	}

	return DefWindowProcW(hWnd, message, wParam, lParam);
}

static int RunGui(void)
{
	WNDCLASSEXW wc;
	MSG msg;
	NONCLIENTMETRICSW ncm;
	RECT rect;

	g_dpi = GetDeviceCaps(GetDC(NULL), LOGPIXELSX);
	if (g_dpi <= 0)
	{
		g_dpi = 96;
	}

	memset(&ncm, 0, sizeof(ncm));
	ncm.cbSize = sizeof(ncm);
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
	{
		g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
	}
	if (g_hFont == NULL)
	{
		g_hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	}

	memset(&wc, 0, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = g_hInst;
	wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"CtrConfigWnd";
	wc.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
	wc.hIconSm = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);

	if (RegisterClassExW(&wc) == 0)
	{
		return 1;
	}

	rect.left = 0;
	rect.top = 0;
	rect.right = SX(906);
	rect.bottom = SX(536);
	AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

	g_hWnd = CreateWindowExW(0, L"CtrConfigWnd", APP_TITLE,
	                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
	                         CW_USEDEFAULT, CW_USEDEFAULT,
	                         rect.right - rect.left, rect.bottom - rect.top,
	                         NULL, NULL, g_hInst, NULL);
	if (g_hWnd == NULL)
	{
		return 1;
	}

	// Center on the primary monitor.
	{
		RECT win;
		int sw = GetSystemMetrics(SM_CXSCREEN);
		int sh = GetSystemMetrics(SM_CYSCREEN);

		GetWindowRect(g_hWnd, &win);
		SetWindowPos(g_hWnd, NULL,
		             (sw - (win.right - win.left)) / 2,
		             (sh - (win.bottom - win.top)) / 2,
		             0, 0, SWP_NOSIZE | SWP_NOZORDER);
	}

	ShowWindow(g_hWnd, SW_SHOW);
	UpdateWindow(g_hWnd);

	LogLine("[ctr_config] ui: dpi=%d launcherDir=%s config=%s\n", g_dpi, g_launcherDirUtf8, g_configPathUtf8);
	{
		wchar_t dzProbe[64];

		GetDlgItemTextW(g_hWnd, IDC_DEADZONE, dzProbe, 64);
		LogLine("[ctr_config] ui: hwnd=%p deadzoneCtl=%p deadzoneTxt='%ls'\n",
		        (void *)g_hWnd, (void *)GetDlgItem(g_hWnd, IDC_DEADZONE), dzProbe);
	}

	while (GetMessageW(&msg, NULL, 0, 0) > 0)
	{
		if ((IsDialogMessageW(g_hWnd, &msg) == FALSE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	if (g_hFont != NULL)
	{
		DeleteObject(g_hFont);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void PrintConfig(void)
{
	LauncherConfig *cfg = &g_cfg;
	static const char *aspectNames[3] = { "Auto", "4:3", "16:9" };
	char exeUtf8[STR_MAX];
	char discUtf8[STR_MAX];

	WideToUtf8(cfg->gameExeW, exeUtf8, sizeof(exeUtf8));
	WideToUtf8(cfg->discImageW, discUtf8, sizeof(discUtf8));

	Print("config=%s\n", g_configPathUtf8);
	Print("window_width=%d\n", cfg->windowWidth);
	Print("window_height=%d\n", cfg->windowHeight);
	Print("fullscreen=%s\n", cfg->fullscreen ? "true" : "false");
	Print("aspect_ratio=%s\n", aspectNames[(cfg->aspect >= 0 && cfg->aspect < 3) ? cfg->aspect : 0]);
	Print("internal_resolution_scale=%d\n", cfg->scale);
	Print("bilinear_filtering=%s\n", cfg->bilinear ? "true" : "false");
	Print("antialiasing=%s\n", cfg->antialiasing ? "true" : "false");
	Print("pgxp=%s\n", cfg->pgxp ? "true" : "false");
	Print("pgxp_geometry=%s\n", cfg->pgxpGeometry ? "true" : "false");
	Print("show_fps=%s\n", cfg->showFps ? "true" : "false");
	Print("pad_mode=%d\n", cfg->padMode);
	Print("keyboard_slot=%d\n", cfg->keyboardSlot);
	Print("gamepad_deadzone=%d\n", cfg->deadzone);
	Print("gamepad_analog=%s\n", cfg->analog ? "true" : "false");
	Print("gamepad_rumble=%s\n", cfg->rumble ? "true" : "false");
	Print("game_executable=%s\n", exeUtf8);
	Print("disc_image=%s\n", discUtf8);
}

static void PrintHelp(void)
{
	Print("CTR Native Config (ctr_config.exe)\n");
	Print("\n");
	Print("Usage: ctr_config.exe [options]\n");
	Print("\n");
	Print("  (no options)       open the settings window\n");
	Print("  --dump             print the parsed configuration and exit\n");
	Print("  --resave           load + save the configuration (round-trip check)\n");
	Print("  --write-defaults   write a fresh default configuration\n");
	Print("  --config PATH      operate on PATH instead of <exe folder>/ctr-native-config.json\n");
	Print("  --out PATH         also copy dump/verbose text into PATH\n");
	Print("  --verbose          log every step to the console and ctr_config.log\n");
	Print("  --help             this text\n");
	Print("\n");
	Print("Exit codes: 0 ok, 1 error, 2 config file missing, 3 write failed\n");
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	int argc = 0;
	wchar_t **argv;
	int cliAction = 0; // 1 dump, 2 resave, 3 write-defaults, 4 help
	const wchar_t *configOverride = NULL;
	const wchar_t *outPath = NULL;
	int loaded;
	int exitCode = 0;

	(void)hPrevInstance;
	(void)lpCmdLine;
	(void)nCmdShow;

	g_hInst = hInstance;

	argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv == NULL)
	{
		return 1;
	}

	for (int i = 1; i < argc; i++)
	{
		if ((wcscmp(argv[i], L"--help") == 0) || (wcscmp(argv[i], L"-h") == 0))
		{
			cliAction = 4;
		}
		else if (wcscmp(argv[i], L"--dump") == 0)
		{
			cliAction = 1;
		}
		else if (wcscmp(argv[i], L"--resave") == 0)
		{
			cliAction = 2;
		}
		else if (wcscmp(argv[i], L"--write-defaults") == 0)
		{
			cliAction = 3;
		}
		else if ((wcscmp(argv[i], L"--config") == 0) && ((i + 1) < argc))
		{
			configOverride = argv[++i];
		}
		else if ((wcscmp(argv[i], L"--out") == 0) && ((i + 1) < argc))
		{
			outPath = argv[++i];
		}
		else if (wcscmp(argv[i], L"--verbose") == 0)
		{
			g_verbose = 1;
		}
	}

	// Launcher dir: the folder containing this exe.
	GetModuleFileNameW(NULL, g_launcherDirW, WSTR_MAX);
	GetDirPartW(g_launcherDirW, g_launcherDirW, WSTR_MAX); // in-place is fine (shortens)
	WideToUtf8(g_launcherDirW, g_launcherDirUtf8, sizeof(g_launcherDirUtf8));

	snprintf(g_logPathUtf8, sizeof(g_logPathUtf8), "%s%s", g_launcherDirUtf8, LOG_FILENAME);

	if (configOverride != NULL)
	{
		wchar_t full[WSTR_MAX];
		DWORD length = GetFullPathNameW(configOverride, WSTR_MAX, full, NULL);
		if ((length == 0) || (length >= WSTR_MAX))
		{
			wcsncpy(full, configOverride, WSTR_MAX - 1);
		}
		WideToUtf8(full, g_configPathUtf8, sizeof(g_configPathUtf8));
	}
	else
	{
		JoinPathW(g_launcherDirW, L"ctr-native-config.json", g_configPathW, WSTR_MAX);
		WideToUtf8(g_configPathW, g_configPathUtf8, sizeof(g_configPathUtf8));
	}

	if ((cliAction != 0) || g_verbose)
	{
		// Attach to the invoking console (if any) so CLI text is visible, and
		// open the optional --out capture file.
		if (AttachConsole(ATTACH_PARENT_PROCESS))
		{
			freopen("CONOUT$", "w", stdout);
			freopen("CONOUT$", "w", stderr);
		}
	}
	if (outPath != NULL)
	{
		char outUtf8[STR_MAX];
		WideToUtf8(outPath, outUtf8, sizeof(outUtf8));
		g_outCopy = fopen(outUtf8, "wb");
	}

	if (cliAction != 0)
	{
		if (cliAction == 4)
		{
			PrintHelp();
			exitCode = 0;
		}
		else
		{
			loaded = ConfigLoad(&g_cfg, g_configPathUtf8);

			switch (cliAction)
			{
				case 1:
					PrintConfig();
					exitCode = loaded ? 0 : 2;
					break;

				case 2:
					if (!ConfigSave(&g_cfg, g_configPathUtf8))
					{
						Print("resave: FAILED (%s)\n", g_configPathUtf8);
						exitCode = 3;
					}
					else
					{
						Print("resave: ok (%s)\n", g_configPathUtf8);
						exitCode = 0;
					}
					break;

				case 3:
					ConfigSetDefaults(&g_cfg);
					if (!ConfigSave(&g_cfg, g_configPathUtf8))
					{
						Print("write-defaults: FAILED (%s)\n", g_configPathUtf8);
						exitCode = 3;
					}
					else
					{
						Print("write-defaults: ok (%s)\n", g_configPathUtf8);
						exitCode = 0;
					}
					break;

				default:
					break;
			}
		}

		if (g_outCopy != NULL)
		{
			fclose(g_outCopy);
		}
		LocalFree(argv);
		return exitCode;
	}

	(void)ConfigLoad(&g_cfg, g_configPathUtf8);
	exitCode = RunGui();

	if (g_outCopy != NULL)
	{
		fclose(g_outCopy);
	}
	LocalFree(argv);
	return exitCode;
}