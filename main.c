#define _CRT_SECURE_NO_WARNINGS
#define SDL_MAIN_HANDLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include "platform/native_win32.h"
#else
#include <unistd.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#define _EnterCriticalSection(x)
#define EnterCriticalSection(x)
#define ExitCriticalSection()

#include "platform/native_assets.h"
#include "platform/native_config.h"
#include "platform/native_log.h"
#include "platform/native_memory.h"
#include "platform/native_perf.h"
#include "platform/native_replay_scheduler.h"
#include "platform/native_savestate.h"

#include <platform.h>

#include "game/game_unity.h"

#include "game/zGlobal_RDATA.c"
#include "game/zGlobal_DATA.c"
#include "game/zGlobal_SDATA.c"

#undef RECT

#include "platform/native_disc_image.c"
#include "platform/native_assets.c"
#include "platform/native_audio.c"
#include "platform/native_memory.c"
#include "platform/native_checkpoint.c"
#include "platform/native_checkpoint_file.c"
#include "platform/native_cd.c"
#include "platform/native_gpu_links.c"
#include "platform/native_gpu.c"
#include "platform/native_gte_core.c"
#include "platform/native_glad.c"
#include "platform/native_input.c"
#include "platform/native_inline_c.c"
#include "platform/native_libapi.c"
#include "platform/native_libetc.c"
#include "platform/native_libgte.c"
#include "platform/native_libgpu.c"
#include "platform/native_libpad.c"
#include "platform/native_libspu.c"
#include "platform/native_log.c"
#include "platform/native_memcard.c"
#include "platform/native_memcard_adapter.c"
#include "platform/native_perf.c"
#include "platform/native_platform.c"
#include "platform/native_replay_scheduler.c"
#include "platform/native_renderer.c"
#include "platform/native_savestate.c"
#include "platform/native_state.c"
#include "platform/native_str.c"
#include "platform/native_config.c"
#include "platform/native_overlay.c"

#ifndef CC
#if defined(__GNUC__)
#if _WIN32
#ifndef __clang__
#define CC "MINGW-GCC"
#else
#define CC "MINGW-CLANG"
#endif
#else
#ifndef __clang__
#define CC "GCC"
#else
#define CC "CLANG"
#endif
#endif
#elif defined(_MSC_VER)
#define CC "MSVC"
#else
#define CC "Unknown"
#endif
#endif

#ifndef CTR_NATIVE_VERSION
#define CTR_NATIVE_VERSION "0.0.0-dev"
#endif

#ifndef CTR_NATIVE_BUILD_ID
#define CTR_NATIVE_BUILD_ID "unknown"
#endif

static int NativeConsole_ShouldPauseOnError(void)
{
#if defined(_WIN32)
	DWORD consoleProcesses[2];
	DWORD consoleProcessCount;

	if (GetConsoleWindow() == NULL)
		return 0;

	consoleProcessCount = GetConsoleProcessList(consoleProcesses, (DWORD)(sizeof(consoleProcesses) / sizeof(consoleProcesses[0])));
	return (consoleProcessCount == 1) && (consoleProcesses[0] == GetCurrentProcessId());
#else
	return 0;
#endif
}

static s32 NativeConsole_Return(const u32 result)
{
	if ((result != 0) && NativeConsole_ShouldPauseOnError())
	{
		fflush(stdout);
		fflush(stderr);
		fprintf(stderr, "\n[CTR Native] Press Enter to close this window...");
		fflush(stderr);

		while (getchar() != '\n' && !feof(stdin))
		{
		}
	}

	return (s32)result;
}

// TODO(aalhendi): just make an argparser?
static int NativeArg_IsVersion(const char *arg)
{
	return (arg != NULL) && ((strcmp(arg, "--version") == 0) || (strcmp(arg, "-v") == 0));
}

// NOTE: "-v" is taken by --version, so verbose is long-form only.
static int NativeArg_IsDumpBoot(const char *arg)
{
	return (arg != NULL) && (strcmp(arg, "--dump-boot") == 0);
}

static int NativeArg_IsHelp(const char *arg)
{
	return (arg != NULL) && ((strcmp(arg, "--help") == 0) || (strcmp(arg, "-h") == 0));
}

static int NativeArg_IsQuiet(const char *arg)
{
	return (arg != NULL) && ((strcmp(arg, "--quiet") == 0) || (strcmp(arg, "-q") == 0));
}

static int NativeArg_IsVerbose(const char *arg)
{
	return (arg != NULL) && (strcmp(arg, "--verbose") == 0);
}

#if defined(_WIN32)
// NOTE: Windows Error Reporting is disabled on this machine, so crashes would
// otherwise leave no trace. Log the exception code, faulting address and a raw
// stack trace to the game log before letting the process die.
static void NativeConsole_LogAddressModule(const char *label, void *address)
{
	HMODULE module = NULL;
	char modulePath[MAX_PATH];
	const char *moduleName = "unknown";

	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module) && (module != NULL))
	{
		if (GetModuleFileNameA(module, modulePath, (DWORD)sizeof(modulePath)) > 0)
		{
			const char *lastSlash = strrchr(modulePath, '\\');
			moduleName = (lastSlash != NULL) ? (lastSlash + 1) : modulePath;
		}

		Platform_LogError("[CTR Native]   %s=%p (%s+0x%lX)\n", label, address, moduleName, (unsigned long)((uintptr_t)address - (uintptr_t)module));
		return;
	}

	Platform_LogError("[CTR Native]   %s=%p (no module)\n", label, address);
}

static LONG WINAPI NativeConsole_UnhandledExceptionFilter(EXCEPTION_POINTERS *exceptionInfo)
{
	void *frames[16];
	USHORT frameCount;
	int frameIndex;
	char label[16];

	Platform_LogError("[CTR Native] UNHANDLED EXCEPTION code=0x%08lX address=%p\n",
		                  (unsigned long)exceptionInfo->ExceptionRecord->ExceptionCode,
		                  exceptionInfo->ExceptionRecord->ExceptionAddress);

		NativeConsole_LogAddressModule("exception", exceptionInfo->ExceptionRecord->ExceptionAddress);

		{
			const CONTEXT *context = exceptionInfo->ContextRecord;

			Platform_LogError("[CTR Native]   eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX\n",
			                  (unsigned long)context->Eax, (unsigned long)context->Ebx, (unsigned long)context->Ecx, (unsigned long)context->Edx,
			                  (unsigned long)context->Esi, (unsigned long)context->Edi, (unsigned long)context->Ebp, (unsigned long)context->Esp,
			                  (unsigned long)context->Eip);
		}

	frameCount = CaptureStackBackTrace(1, (DWORD)(sizeof(frames) / sizeof(frames[0])), frames, NULL);
	for (frameIndex = 0; frameIndex < (int)frameCount; frameIndex++)
	{
		snprintf(label, sizeof(label), "trace[%d]", frameIndex);
		NativeConsole_LogAddressModule(label, frames[frameIndex]);
	}

	Platform_LogFlush();
	return EXCEPTION_CONTINUE_SEARCH;
}

// NOTE: This is a console-subsystem app; when its console window is closed
// (taskbar close, Windows Terminal tab close) Windows would terminate the
// process. Detach from the console instead so the game keeps running.
static BOOL WINAPI NativeConsole_ControlHandler(DWORD controlType)
{
	if (controlType == CTRL_CLOSE_EVENT)
	{
		Platform_LogWarn("[CTR Native] console closed; detached and still running\n");
		Platform_LogFlush();
		FreeConsole();
		return TRUE;
	}

	return FALSE;
}
#endif


// Config loaded before asset init so `disc_image` can choose the game data
// source before validation; the graphics setup below reuses the same struct.
static NativeConfig g_nativeConfig;

int main(int argc, char *argv[])
{
#if defined(_WIN32)
	SetUnhandledExceptionFilter(NativeConsole_UnhandledExceptionFilter);
	SetConsoleCtrlHandler(NativeConsole_ControlHandler, TRUE);
#endif

	extern int g_cli_dumpBoot;

	for (int argIndex = 1; argIndex < argc; argIndex++)
	{
		if (NativeArg_IsVersion(argv[argIndex]))
		{
			printf("CTR Native %s (%s)\n", CTR_NATIVE_VERSION, CTR_NATIVE_BUILD_ID);
			return 0;
		}
		else if (NativeArg_IsHelp(argv[argIndex]))
		{
			printf("CTR Native %s (%s)\n", CTR_NATIVE_VERSION, CTR_NATIVE_BUILD_ID);
			printf("Usage: ctr_native.exe [options]\n");
			printf("  --verbose     enable [CTR Debug] logging (default)\n");
			printf("  -q, --quiet   silence [CTR Debug] logging\n");
			printf("  --dump-boot   capture the first 150 frames to boot_dump/ (debug)\n");
			printf("  -h, --help    show this help\n");
			return 0;
		}
		else if (NativeArg_IsQuiet(argv[argIndex]))
		{
			Platform_LogSetDebugEnabled(0);
		}
		else if (NativeArg_IsVerbose(argv[argIndex]))
		{
			Platform_LogSetDebugEnabled(1);
		}
		else if (NativeArg_IsDumpBoot(argv[argIndex]))
		{
			g_cli_dumpBoot = 1;
		}
	}

	printf("[CTR Native] Starting...\n");
	printf("[CTR Native] Debug logging: %s\n", Platform_LogDebugEnabled() ? "enabled (--quiet to silence)" : "disabled (--verbose to enable)");
	fflush(stdout);

	const char *sdlBasePath = SDL_GetBasePath();
	printf("[CTR Native] SDL base path: %s\n", sdlBasePath ? sdlBasePath : "(null)");
	fflush(stdout);

	// Load the config before asset init: `disc_image` chooses the game data
	// source, and asset validation must see the same file the launcher picked.
	NativeConfig_SetDefaults(&g_nativeConfig);
	{
		char configPath[512];

		if ((sdlBasePath != NULL) && (snprintf(configPath, sizeof(configPath), "%sctr-native-config.json", sdlBasePath) > 0))
		{
			if (NativeConfig_LoadFile(&g_nativeConfig, configPath) != 0)
			{
				printf("[CTR Native] Config: %s\n", configPath);
			}
			else
			{
				printf("[CTR Native] Config: %s (not found; using defaults)\n", configPath);
			}
		}
	}

	snprintf(g_cfg_discImage, sizeof(g_cfg_discImage), "%s", g_nativeConfig.discImage);

	if (!NativeAssets_Init(sdlBasePath))
	{
		fprintf(stderr, "[CTR Native] Failed to initialize asset paths.\n");
		return NativeConsole_Return(1);
	}

	printf("[CTR Native] Version: %s (%s)\n", CTR_NATIVE_VERSION, CTR_NATIVE_BUILD_ID);
	printf("[CTR Native] Built with: " CC "\n");
	printf("[CTR Native] Base: %s\n", NativeAssets_GetBaseDir());
	printf("[CTR Native] Assets: %s\n", NativeAssets_GetAssetDir());
	fflush(stdout);

	if (chdir(NativeAssets_GetBaseDir()) != 0)
	{
		fprintf(stderr, "[CTR Native] Failed to enter base directory: %s\n", NativeAssets_GetBaseDir());
		return NativeConsole_Return(1);
	}

	if (!NativeAssets_Validate())
	{
		return NativeConsole_Return(1);
	}

#if defined(CTR_INTERNAL)
	if (NativeReplayScheduler_PrepareReportFromArgs(argc, argv) != 0)
	{
		return NativeConsole_Return(1);
	}
#endif

	{
		NativeConfig nativeConfig = g_nativeConfig;
		int windowWidth;
		int windowHeight;

		g_cfg_aspectRatio = nativeConfig.aspectRatio;
		g_cfg_internalResolutionScale = nativeConfig.internalResolutionScale;
		g_cfg_internalResolutionAuto = nativeConfig.internalResolutionAuto;
		g_cfg_bilinearFiltering = nativeConfig.bilinearFiltering;
		g_cfg_antialiasing = nativeConfig.antialiasing;
		g_cfg_pgxp = nativeConfig.pgxp;
		g_cfg_pgxpGeometry = nativeConfig.pgxpGeometry;
		g_cfg_showFps = nativeConfig.showFps;

		g_cfg_gamepadDeadzone = nativeConfig.gamepadDeadzone * 32768 / 100;
		g_cfg_gamepadAnalog = nativeConfig.gamepadAnalog;
		g_cfg_gamepadRumble = nativeConfig.gamepadRumble;
		g_cfg_padMode = nativeConfig.padMode;
		g_cfg_keyboardSlot = nativeConfig.keyboardSlot;

#ifdef USE_16BY9
		printf("[CTR Native] Widescreen\n");
		windowWidth = (nativeConfig.windowWidth > 0) ? nativeConfig.windowWidth : 1280;
		windowHeight = (nativeConfig.windowHeight > 0) ? nativeConfig.windowHeight : 720;
#else
		printf("[CTR Native] 4:3\n");
		windowWidth = (nativeConfig.windowWidth > 0) ? nativeConfig.windowWidth : 800;
		windowHeight = (nativeConfig.windowHeight > 0) ? nativeConfig.windowHeight : 600;
#endif

		printf("[CTR Native] Window %dx%d fullscreen=%d scale=%dx bilinear=%d aspect=%d\n",
		       windowWidth, windowHeight, nativeConfig.fullscreen,
		       nativeConfig.internalResolutionScale, nativeConfig.bilinearFiltering,
		       nativeConfig.aspectRatio);

		Platform_Init("Crash Team Racing", windowWidth, windowHeight, nativeConfig.fullscreen);
	}

#if defined(CTR_INTERNAL)
	if (NativePerf_ConfigureFromArgs(argc, argv) != 0)
	{
		Platform_LogFlush();
		Platform_Shutdown();
		return NativeConsole_Return(1);
	}
#endif

	Platform_InitScratchpad();
	Platform_RepairResidentPointers(0);

#if defined(CTR_INTERNAL)
	if (NativeReplayScheduler_ConfigureFromArgs(argc, argv) != 0)
	{
		Platform_LogFlush();
		Platform_Shutdown();
		return NativeConsole_Return(1);
	}
#else
	(void)argc;
	(void)argv;
#endif

	const int result = CTR_Main();

	Platform_Shutdown();
	return NativeConsole_Return(result);
}
