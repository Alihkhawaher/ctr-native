#ifndef PLATFORM_NATIVE_INPUT_H
#define PLATFORM_NATIVE_INPUT_H

#include <macros.h>

#define PLATFORM_INPUT_PAD_COUNT 4

// Gamepad configuration (loaded from ctr-native-config.json; applied by main.c).
extern int g_cfg_gamepadDeadzone; // raw SDL axis units (percent * 32768 / 100)
extern int g_cfg_gamepadAnalog;   // 1 = new pads start in analog mode
extern int g_cfg_gamepadRumble;   // 1 = allow pad vibration
extern int g_cfg_padMode;         // 0 = auto (detect at boot), 1 = 4 pads (always multitap), 2 = 2 pads
extern int g_cfg_keyboardSlot;    // -1 = auto, 0..3 = the keyboard plays as that player

struct PlatformInputPadSnapshot
{
	u8 status;
	u8 id;
	u8 buttons[2];
	u8 analog[4];
	u8 connected;
	u8 reserved[3];
};

int Platform_InputInit(void);
void Platform_InputShutdown(void);
void Platform_InputUpdate(void);
void Platform_InputControllerAdded(int deviceIndex);
void Platform_InputControllerRemoved(int instanceId);
int Platform_InputCycleKeyboardController(void);
int Platform_InputCycleGamepadController(void);

void Platform_InputPadInit(int slot, unsigned char *padData);
int Platform_InputPadGetState(int port);
void Platform_InputPadVibrate(int port, unsigned char *table, int len);
void Platform_InputRumbleTest(void);
int Platform_InputCapturePadSnapshots(struct PlatformInputPadSnapshot *dst, int count);
int Platform_InputInstallPadSnapshots(const struct PlatformInputPadSnapshot *src, int count);
void Platform_InputClearInstalledPadSnapshots(void);
int Platform_InputGetStateSize(void);
int Platform_InputCaptureState(void *dst, int dstSize);
int Platform_InputRestoreState(const void *src, int srcSize);

#endif
