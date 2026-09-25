#include <platform/native_input.h>

#include <macros.h>
#include "psx/libpad.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NATIVE_INPUT_MAX_CONTROLLERS       PLATFORM_INPUT_PAD_COUNT
#define NATIVE_INPUT_PHYSICAL_SLOT_COUNT   2
#define NATIVE_INPUT_PAD_PACKET_BYTES      8
#define NATIVE_INPUT_MULTITAP_HEADER       2
#define NATIVE_INPUT_PAD_DIGITAL           0x41
#define NATIVE_INPUT_PAD_ANALOG            0x73
#define NATIVE_INPUT_PAD_MULTITAP          0x80
#define NATIVE_INPUT_PAD_DISCONNECT        0xff
#define NATIVE_INPUT_AXIS_DEADZONE         500
#define NATIVE_INPUT_MAP_FLAG_AXIS         0x4000
#define NATIVE_INPUT_MAP_FLAG_INVERSE      0x8000
#define NATIVE_INPUT_DEFAULT_KEYBOARD_SLOT 0
// NOTE(aalhendi): Little-endian tag `CTRI` = CTR native Input snapshot.
#define NATIVE_INPUT_STATE_MAGIC           0x49525443
#define NATIVE_INPUT_STATE_VERSION         1

// NOTE(aalhendi): Native input preserves behavior from PsyCross's
// MIT-licensed pad implementation while moving host ownership into ctr-native.
// See THIRD_PARTY_NOTICES.md.

struct NativeInputKeyboardMapping
{
	s32 id;

	s32 kc_square, kc_circle, kc_triangle, kc_cross;

	s32 kc_l1, kc_l2, kc_l3;
	s32 kc_r1, kc_r2, kc_r3;

	s32 kc_start, kc_select;

	s32 kc_dpad_left, kc_dpad_right, kc_dpad_up, kc_dpad_down;
};

struct NativeInputControllerMapping
{
	s32 id;

	s32 gc_square, gc_circle, gc_triangle, gc_cross;

	s32 gc_l1, gc_l2, gc_l3;
	s32 gc_r1, gc_r2, gc_r3;

	s32 gc_start, gc_select;

	s32 gc_dpad_left, gc_dpad_right, gc_dpad_up, gc_dpad_down;

	s32 gc_axis_left_x, gc_axis_left_y;
	s32 gc_axis_right_x, gc_axis_right_y;
};

struct NativeInputController
{
	SDL_JoystickID instanceId;
	SDL_Gamepad *controller;
	s32 analogEnabled;
	s32 switchingAnalog;
	struct PlatformInputPadSnapshot snapshot;
};

struct NativeInputControllerStateSnapshot
{
	struct PlatformInputPadSnapshot snapshot;
	s32 analogEnabled;
	s32 switchingAnalog;
	s32 controllerToSlotMapping;
};

struct NativeInputStateSnapshot
{
	u32 magic;
	u32 version;
	u32 size;
	s32 keyboardControllerSlot;
	s32 lastActiveControllerSlot;
	s32 installedSnapshotsActive;
	struct PlatformInputPadSnapshot installedSnapshots[NATIVE_INPUT_MAX_CONTROLLERS];
	struct NativeInputControllerStateSnapshot controllers[NATIVE_INPUT_MAX_CONTROLLERS];
};

global_variable struct NativeInputControllerMapping s_controllerMapping;
global_variable struct NativeInputKeyboardMapping s_keyboardMapping;
global_variable s32 s_controllerToSlotMapping[NATIVE_INPUT_MAX_CONTROLLERS] = {-1, -1, -1, -1};

global_variable struct NativeInputController s_controllers[NATIVE_INPUT_MAX_CONTROLLERS];
global_variable struct PlatformInputPadSnapshot s_installedSnapshots[NATIVE_INPUT_MAX_CONTROLLERS];
global_variable u8 *s_padSlotData[NATIVE_INPUT_PHYSICAL_SLOT_COUNT];
global_variable const bool *s_keyboardState;
global_variable s32 s_inputInitialized;
global_variable s32 s_installedSnapshotsActive;

// Gamepad config (set from ctr-native-config.json by main.c; the initializers
// are only pre-config fallbacks).
int g_cfg_gamepadDeadzone = 500;
int g_cfg_gamepadAnalog = 1;
int g_cfg_gamepadRumble = 1;
int g_cfg_padMode = 1; // 0 = auto, 1 = 4 pads (multitap always), 2 = 2 pads
int g_cfg_keyboardSlot = -1; // -1 = auto; 0..3 = keyboard plays as that player

// Per-slot remembered device path: pads return to the SAME player slot after
// a battery/cable drop (matched via SDL_GetGamepadPathForID; the path is
// unique per physical device, including the Bluetooth address).
global_variable char s_controllerRememberedPath[NATIVE_INPUT_MAX_CONTROLLERS][256];

// Pad-bus layout fixed for the whole session (see Platform_InputInit).
global_variable s32 s_multitapLatched = 0;
global_variable s32 s_keyboardControllerSlot = NATIVE_INPUT_DEFAULT_KEYBOARD_SLOT;
global_variable s32 s_lastActiveControllerSlot = -1;

extern s32 g_padCommEnable;

internal u16 NativeInput_GetSnapshotButtons(const struct PlatformInputPadSnapshot *snapshot)
{
	return (u16)(snapshot->buttons[0] | (snapshot->buttons[1] << 8));
}

internal void NativeInput_SetSnapshotButtons(struct PlatformInputPadSnapshot *snapshot, u16 buttons)
{
	snapshot->buttons[0] = (u8)(buttons & 0xff);
	snapshot->buttons[1] = (u8)(buttons >> 8);
}

internal void NativeInput_ResetSnapshot(s32 slot)
{
	struct PlatformInputPadSnapshot *snapshot = &s_controllers[slot].snapshot;

	snapshot->connected = slot == 0;
	snapshot->status = snapshot->connected ? 0 : NATIVE_INPUT_PAD_DISCONNECT;
	snapshot->id = snapshot->connected ? NATIVE_INPUT_PAD_DIGITAL : NATIVE_INPUT_PAD_DISCONNECT;
	NativeInput_SetSnapshotButtons(snapshot, 0xffff);
	snapshot->analog[0] = 0x80;
	snapshot->analog[1] = 0x80;
	snapshot->analog[2] = 0x80;
	snapshot->analog[3] = 0x80;
	memset(snapshot->reserved, 0, sizeof(snapshot->reserved));
}

internal s32 NativeInput_IsValidControllerSlot(s32 slot)
{
	return (slot >= 0) && (slot < NATIVE_INPUT_MAX_CONTROLLERS);
}

internal s32 NativeInput_NextControllerSlot(s32 slot)
{
	slot++;
	if (slot >= NATIVE_INPUT_MAX_CONTROLLERS)
	{
		slot = 0;
	}

	return slot;
}

internal void NativeInput_MoveKeyboardOffControllerSlot(s32 slot)
{
	// Only the Auto mode (-1) lets pads displace the keyboard. Fixed (0..3)
	// keeps it on its player; "Pads only" (-2) has no keyboard player at all.
	if (g_cfg_keyboardSlot != -1)
	{
		return;
	}

	if (s_keyboardControllerSlot == slot)
	{
		s_keyboardControllerSlot = NativeInput_NextControllerSlot(s_keyboardControllerSlot);
	}
}

// "4 pads always on even if they do not exist" (pad_mode 1): present an empty
// slot as a connected, idle pad. A flaky pad dropping or reconnecting then
// never changes what the game sees on the controller bus.
internal void NativeInput_MakeIdleConnectedSnapshot(struct PlatformInputPadSnapshot *snapshot)
{
	if (snapshot == NULL)
	{
		return;
	}

	snapshot->connected = 1;
	snapshot->status = 0;
	snapshot->id = NATIVE_INPUT_PAD_DIGITAL;
	NativeInput_SetSnapshotButtons(snapshot, 0xffff);
	snapshot->analog[0] = 0x80;
	snapshot->analog[1] = 0x80;
	snapshot->analog[2] = 0x80;
	snapshot->analog[3] = 0x80;
	memset(snapshot->reserved, 0, sizeof(snapshot->reserved));
}

internal void NativeInput_MakeDisconnectedSnapshot(struct PlatformInputPadSnapshot *snapshot)
{
	if (snapshot == NULL)
	{
		return;
	}

	snapshot->connected = 0;
	snapshot->status = NATIVE_INPUT_PAD_DISCONNECT;
	snapshot->id = NATIVE_INPUT_PAD_DISCONNECT;
	NativeInput_SetSnapshotButtons(snapshot, 0xffff);
	snapshot->analog[0] = 0x80;
	snapshot->analog[1] = 0x80;
	snapshot->analog[2] = 0x80;
	snapshot->analog[3] = 0x80;
	memset(snapshot->reserved, 0, sizeof(snapshot->reserved));
}

internal void NativeInput_WritePadPacket(u8 *dst, const struct PlatformInputPadSnapshot *snapshot)
{
	if ((dst == NULL) || (snapshot == NULL))
	{
		return;
	}

	dst[0] = snapshot->status;
	dst[1] = snapshot->id;
	dst[2] = snapshot->buttons[0];
	dst[3] = snapshot->buttons[1];
	dst[4] = snapshot->analog[0];
	dst[5] = snapshot->analog[1];
	dst[6] = snapshot->analog[2];
	dst[7] = snapshot->analog[3];
}

internal s32 NativeInput_UseMultitapBus(void)
{
	// Session-fixed bus layout (see Platform_InputInit): 1 = always 4 pads
	// (multitap bus from startup so flaky pads attach into any slot), 2 =
	// always 2 pads, 0 = auto (latched at boot from what was connected).
	if (g_cfg_padMode == 1)
	{
		return 1;
	}

	if (g_cfg_padMode == 2)
	{
		return 0;
	}

	return s_multitapLatched;
}

internal void NativeInput_WritePadBus(void)
{
	u8 *slot0 = s_padSlotData[0];
	u8 *slot1 = s_padSlotData[1];
	s32 useMultitap = NativeInput_UseMultitapBus();
	s32 slot;

	if (slot0 != NULL)
	{
		if (useMultitap != 0)
		{
			slot0[0] = 0;
			slot0[1] = NATIVE_INPUT_PAD_MULTITAP;
			for (slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
			{
				NativeInput_WritePadPacket(&slot0[NATIVE_INPUT_MULTITAP_HEADER + (slot * NATIVE_INPUT_PAD_PACKET_BYTES)], &s_controllers[slot].snapshot);
			}
		}
		else
		{
			NativeInput_WritePadPacket(slot0, &s_controllers[0].snapshot);
		}
	}

	if (slot1 != NULL)
	{
		if (useMultitap != 0)
		{
			struct PlatformInputPadSnapshot disconnected;

			NativeInput_MakeDisconnectedSnapshot(&disconnected);
			NativeInput_WritePadPacket(slot1, &disconnected);
		}
		else
		{
			NativeInput_WritePadPacket(slot1, &s_controllers[1].snapshot);
		}
	}
}

internal void NativeInput_WriteInstalledSnapshots(void)
{
	for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		s_controllers[slot].snapshot = s_installedSnapshots[slot];
	}

	NativeInput_WritePadBus();
}

internal void NativeInput_DefaultMappings(void)
{
	s_keyboardMapping.kc_square = SDL_SCANCODE_X;
	s_keyboardMapping.kc_circle = SDL_SCANCODE_V;
	s_keyboardMapping.kc_triangle = SDL_SCANCODE_Z;
	s_keyboardMapping.kc_cross = SDL_SCANCODE_C;

	s_keyboardMapping.kc_l1 = SDL_SCANCODE_LSHIFT;
	s_keyboardMapping.kc_l2 = SDL_SCANCODE_LCTRL;
	s_keyboardMapping.kc_l3 = SDL_SCANCODE_LEFTBRACKET;

	s_keyboardMapping.kc_r1 = SDL_SCANCODE_RSHIFT;
	s_keyboardMapping.kc_r2 = SDL_SCANCODE_RCTRL;
	s_keyboardMapping.kc_r3 = SDL_SCANCODE_RIGHTBRACKET;

	s_keyboardMapping.kc_dpad_up = SDL_SCANCODE_UP;
	s_keyboardMapping.kc_dpad_down = SDL_SCANCODE_DOWN;
	s_keyboardMapping.kc_dpad_left = SDL_SCANCODE_LEFT;
	s_keyboardMapping.kc_dpad_right = SDL_SCANCODE_RIGHT;

	s_keyboardMapping.kc_select = SDL_SCANCODE_SPACE;
	s_keyboardMapping.kc_start = SDL_SCANCODE_RETURN;

	s_controllerMapping.gc_square = SDL_GAMEPAD_BUTTON_WEST;
	s_controllerMapping.gc_circle = SDL_GAMEPAD_BUTTON_EAST;
	s_controllerMapping.gc_triangle = SDL_GAMEPAD_BUTTON_NORTH;
	s_controllerMapping.gc_cross = SDL_GAMEPAD_BUTTON_SOUTH;

	s_controllerMapping.gc_l1 = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
	s_controllerMapping.gc_l2 = SDL_GAMEPAD_AXIS_LEFT_TRIGGER | NATIVE_INPUT_MAP_FLAG_AXIS;
	s_controllerMapping.gc_l3 = SDL_GAMEPAD_BUTTON_LEFT_STICK;

	s_controllerMapping.gc_r1 = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
	s_controllerMapping.gc_r2 = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER | NATIVE_INPUT_MAP_FLAG_AXIS;
	s_controllerMapping.gc_r3 = SDL_GAMEPAD_BUTTON_RIGHT_STICK;

	s_controllerMapping.gc_dpad_up = SDL_GAMEPAD_BUTTON_DPAD_UP;
	s_controllerMapping.gc_dpad_down = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
	s_controllerMapping.gc_dpad_left = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
	s_controllerMapping.gc_dpad_right = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;

	s_controllerMapping.gc_select = SDL_GAMEPAD_BUTTON_BACK;
	s_controllerMapping.gc_start = SDL_GAMEPAD_BUTTON_START;

	s_controllerMapping.gc_axis_left_x = SDL_GAMEPAD_AXIS_LEFTX | NATIVE_INPUT_MAP_FLAG_AXIS;
	s_controllerMapping.gc_axis_left_y = SDL_GAMEPAD_AXIS_LEFTY | NATIVE_INPUT_MAP_FLAG_AXIS;
	s_controllerMapping.gc_axis_right_x = SDL_GAMEPAD_AXIS_RIGHTX | NATIVE_INPUT_MAP_FLAG_AXIS;
	s_controllerMapping.gc_axis_right_y = SDL_GAMEPAD_AXIS_RIGHTY | NATIVE_INPUT_MAP_FLAG_AXIS;
}

internal s32 NativeInput_ControllerButtonState(SDL_Gamepad *controller, s32 buttonOrAxis)
{
	if (controller == NULL)
	{
		return 0;
	}

	if ((buttonOrAxis & NATIVE_INPUT_MAP_FLAG_AXIS) != 0)
	{
		s32 axis = buttonOrAxis & ~(NATIVE_INPUT_MAP_FLAG_AXIS | NATIVE_INPUT_MAP_FLAG_INVERSE);
		s32 value = SDL_GetGamepadAxis(controller, (SDL_GamepadAxis)axis);

		if ((abs(value) > g_cfg_gamepadDeadzone) && ((buttonOrAxis & NATIVE_INPUT_MAP_FLAG_INVERSE) != 0))
		{
			value *= -1;
		}

		return value;
	}

	if (buttonOrAxis < 0)
	{
		return 0;
	}

	return SDL_GetGamepadButton(controller, (SDL_GamepadButton)buttonOrAxis) * 32767;
}

internal u8 NativeInput_AxisToByte(s32 axis)
{
	// Center small values: a drifting stick at rest must report neutral.
	if (abs(axis) <= g_cfg_gamepadDeadzone)
	{
		axis = 0;
	}

	s32 value = (axis / 256) + 128;

	if (value < 0)
	{
		return 0;
	}

	if (value > 0xff)
	{
		return 0xff;
	}

	return (u8)value;
}

internal s32 NativeInput_AxisIsActive(s32 axis)
{
	return abs(axis) > g_cfg_gamepadDeadzone;
}

internal void NativeInput_ApplyController(s32 slot)
{
	struct NativeInputController *nativeController = &s_controllers[slot];
	struct PlatformInputPadSnapshot *snapshot = &nativeController->snapshot;
	const struct NativeInputControllerMapping *mapping = &s_controllerMapping;
	SDL_Gamepad *controller = nativeController->controller;
	u16 buttons = 0xffff;

	if ((controller == NULL) || (SDL_GamepadConnected(controller) == 0))
	{
		return;
	}

	snapshot->connected = 1;
	snapshot->status = 0;
	snapshot->id = nativeController->analogEnabled ? NATIVE_INPUT_PAD_ANALOG : NATIVE_INPUT_PAD_DIGITAL;

	if (NativeInput_ControllerButtonState(controller, mapping->gc_square) > 16384)
	{
		buttons &= ~0x8000;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_circle) > 16384)
	{
		buttons &= ~0x2000;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_triangle) > 16384)
	{
		buttons &= ~0x1000;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_cross) > 16384)
	{
		buttons &= ~0x4000;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_l1) > 16384)
	{
		buttons &= ~0x400;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_r1) > 16384)
	{
		buttons &= ~0x800;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_l2) > 16384)
	{
		buttons &= ~0x100;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_r2) > 16384)
	{
		buttons &= ~0x200;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_dpad_up) > 16384)
	{
		buttons &= ~0x10;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_dpad_down) > 16384)
	{
		buttons &= ~0x40;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_dpad_left) > 16384)
	{
		buttons &= ~0x80;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_dpad_right) > 16384)
	{
		buttons &= ~0x20;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_l3) > 16384)
	{
		buttons &= ~0x2;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_r3) > 16384)
	{
		buttons &= ~0x4;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_select) > 16384)
	{
		buttons &= ~0x1;
	}
	if (NativeInput_ControllerButtonState(controller, mapping->gc_start) > 16384)
	{
		buttons &= ~0x8;
	}

	s32 rightX = NativeInput_ControllerButtonState(controller, mapping->gc_axis_right_x);
	s32 rightY = NativeInput_ControllerButtonState(controller, mapping->gc_axis_right_y);
	s32 leftX = NativeInput_ControllerButtonState(controller, mapping->gc_axis_left_x);
	s32 leftY = NativeInput_ControllerButtonState(controller, mapping->gc_axis_left_y);

	if ((buttons != 0xffff) || NativeInput_AxisIsActive(rightX) || NativeInput_AxisIsActive(rightY) || NativeInput_AxisIsActive(leftX) ||
	    NativeInput_AxisIsActive(leftY))
	{
		s_lastActiveControllerSlot = slot;
	}

	if (((buttons & 0x1) == 0) && ((buttons & 0x8) == 0))
	{
		buttons = 0xffff;
		if (nativeController->switchingAnalog == 0)
		{
			nativeController->analogEnabled = nativeController->analogEnabled == 0;
		}
		nativeController->switchingAnalog = 1;
	}
	else
	{
		nativeController->switchingAnalog = 0;
	}

	NativeInput_SetSnapshotButtons(snapshot, buttons);
	snapshot->analog[0] = NativeInput_AxisToByte(rightX);
	snapshot->analog[1] = NativeInput_AxisToByte(rightY);
	snapshot->analog[2] = NativeInput_AxisToByte(leftX);
	snapshot->analog[3] = NativeInput_AxisToByte(leftY);
}

internal u16 NativeInput_ReadKeyboard(void)
{
	const struct NativeInputKeyboardMapping *mapping = &s_keyboardMapping;
	u16 buttons = 0xffff;

	if (s_keyboardState == NULL)
	{
		return buttons;
	}

	if (s_keyboardState[mapping->kc_square])
	{
		buttons &= ~0x8000;
	}
	if (s_keyboardState[mapping->kc_circle])
	{
		buttons &= ~0x2000;
	}
	if (s_keyboardState[mapping->kc_triangle])
	{
		buttons &= ~0x1000;
	}
	if (s_keyboardState[mapping->kc_cross])
	{
		buttons &= ~0x4000;
	}
	if (s_keyboardState[mapping->kc_l1])
	{
		buttons &= ~0x400;
	}
	if (s_keyboardState[mapping->kc_l2])
	{
		buttons &= ~0x100;
	}
	if (s_keyboardState[mapping->kc_l3])
	{
		buttons &= ~0x2;
	}
	if (s_keyboardState[mapping->kc_r1])
	{
		buttons &= ~0x800;
	}
	if (s_keyboardState[mapping->kc_r2])
	{
		buttons &= ~0x200;
	}
	if (s_keyboardState[mapping->kc_r3])
	{
		buttons &= ~0x4;
	}
	if (s_keyboardState[mapping->kc_dpad_up])
	{
		buttons &= ~0x10;
	}
	if (s_keyboardState[mapping->kc_dpad_down])
	{
		buttons &= ~0x40;
	}
	if (s_keyboardState[mapping->kc_dpad_left])
	{
		buttons &= ~0x80;
	}
	if (s_keyboardState[mapping->kc_dpad_right])
	{
		buttons &= ~0x20;
	}
	if (s_keyboardState[mapping->kc_select])
	{
		buttons &= ~0x1;
	}
	if (s_keyboardState[mapping->kc_start])
	{
		buttons &= ~0x8;
	}

	return buttons;
}

internal s32 NativeInput_KeyboardSuppressed(void)
{
	if (s_keyboardState == NULL)
	{
		return 0;
	}

	return s_keyboardState[SDL_SCANCODE_RALT] || s_keyboardState[SDL_SCANCODE_LALT];
}

internal void NativeInput_ApplyKeyboard(s32 slot, u16 keyboardButtons)
{
	struct PlatformInputPadSnapshot *snapshot = &s_controllers[slot].snapshot;

	if (slot != s_keyboardControllerSlot)
	{
		return;
	}

	if (snapshot->connected == 0)
	{
		snapshot->connected = 1;
		snapshot->status = 0;
		snapshot->id = NATIVE_INPUT_PAD_DIGITAL;
	}

	u16 buttons = NativeInput_GetSnapshotButtons(snapshot);
	NativeInput_SetSnapshotButtons(snapshot, buttons & keyboardButtons);
}

internal s32 NativeInput_FindActiveControllerSlot(void)
{
	if (NativeInput_IsValidControllerSlot(s_lastActiveControllerSlot) && (s_controllers[s_lastActiveControllerSlot].controller != NULL))
	{
		return s_lastActiveControllerSlot;
	}

	for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		if (s_controllers[slot].controller != NULL)
		{
			return slot;
		}
	}

	return -1;
}

internal void NativeInput_SwapControllerSlots(s32 slotA, s32 slotB)
{
	if (!NativeInput_IsValidControllerSlot(slotA) || !NativeInput_IsValidControllerSlot(slotB) || (slotA == slotB))
	{
		return;
	}

	struct NativeInputController controller = s_controllers[slotA];
	s_controllers[slotA] = s_controllers[slotB];
	s_controllers[slotB] = controller;

	s32 mapping = s_controllerToSlotMapping[slotA];
	s_controllerToSlotMapping[slotA] = s_controllerToSlotMapping[slotB];
	s_controllerToSlotMapping[slotB] = mapping;

	if (s_lastActiveControllerSlot == slotA)
	{
		s_lastActiveControllerSlot = slotB;
	}
	else if (s_lastActiveControllerSlot == slotB)
	{
		s_lastActiveControllerSlot = slotA;
	}
}

internal s32 NativeInput_FindSlotForDeviceIndex(Sint32 deviceIndex)
{
	s32 slot;

	for (slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
		{
			if (s_controllerToSlotMapping[slot] == deviceIndex)
			{
				return slot;
			}
		}

		// Sticky reconnect: a slot that remembers this exact physical device takes
		// it back — a pad whose battery/cable dropped returns to the SAME player
		// slot instead of whichever slot happens to be free. A slot holding a
		// stale handle (pad vanished without a REMOVED event) also qualifies.
		{
			const char *path = SDL_GetGamepadPathForID((SDL_JoystickID)deviceIndex);

			if ((path != NULL) && (path[0] != '\0'))
			{
				for (slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
				{
					const struct NativeInputController *candidate = &s_controllers[slot];

					if (((candidate->controller == NULL) || (SDL_GamepadConnected(candidate->controller) == 0)) &&
					    (strcmp(s_controllerRememberedPath[slot], path) == 0))
					{
						return slot;
					}
				}
			}
		}

		for (slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		if ((s_controllerToSlotMapping[slot] < 0) && (s_controllers[slot].controller == NULL))
		{
			return slot;
		}
	}

	return -1;
}

internal void NativeInput_CloseController(s32 slot)
{
	if ((slot < 0) || (slot >= NATIVE_INPUT_MAX_CONTROLLERS))
	{
		return;
	}

	struct NativeInputController *controller = &s_controllers[slot];
		if (controller->controller != NULL)
		{
			if (s_controllerRememberedPath[slot][0] != '\0')
			{
				Platform_LogWarn("[CTR Native] pad slot %d disconnected (device remembered for auto-reconnect)\n", slot);
			}

			SDL_CloseGamepad(controller->controller);
		}

	controller->controller = NULL;
	controller->instanceId = -1;
	controller->analogEnabled = 0;
	controller->switchingAnalog = 0;
	s_controllerToSlotMapping[slot] = -1;

	if (s_lastActiveControllerSlot == slot)
	{
		s_lastActiveControllerSlot = -1;
	}
}

internal void NativeInput_OpenController(SDL_JoystickID instanceId, s32 slot)
{
	if ((slot < 0) || (slot >= NATIVE_INPUT_MAX_CONTROLLERS))
	{
		return;
	}

	if (SDL_IsGamepad(instanceId) == 0)
	{
		return;
	}

	// A device already open in ANY slot must never open again. SDL delivers
	// SDL_EVENT_GAMEPAD_ADDED for pads that were already connected at startup
	// (in addition to the initial enumeration), and without this guard one
	// physical pad landed in TWO slots — a single controller then drove both
	// PSX players at once (reported with Xbox controllers).
	for (s32 other = 0; other < NATIVE_INPUT_MAX_CONTROLLERS; other++)
	{
		if ((s_controllers[other].controller != NULL) && (s_controllers[other].instanceId == instanceId))
		{
			Platform_LogWarn("[CTR Native] duplicate gamepad add ignored (instance %d already in pad slot %d)\n", (s32)instanceId, other);
			return;
		}
	}

	struct NativeInputController *controller = &s_controllers[slot];

		// Stale handle (pad vanished without a REMOVED event): drop it so the
		// slot can take the device again.
		if ((controller->controller != NULL) && (SDL_GamepadConnected(controller->controller) == 0))
		{
			NativeInput_CloseController(slot);
		}

		if (controller->controller != NULL)
		{
			return;
		}

	controller->controller = SDL_OpenGamepad(instanceId);
	if (controller->controller == NULL)
	{
		return;
	}

	SDL_Joystick *joystick = SDL_GetGamepadJoystick(controller->controller);
	controller->instanceId = joystick != NULL ? SDL_GetJoystickID(joystick) : instanceId;
	controller->analogEnabled = (g_cfg_gamepadAnalog != 0) ? 1 : 0;
	controller->switchingAnalog = 0;

	// Record the device->slot mapping (was never written; the dedupe loop in
	// NativeInput_FindSlotForDeviceIndex depends on it).
	s_controllerToSlotMapping[slot] = (s32)controller->instanceId;

	NativeInput_MoveKeyboardOffControllerSlot(slot);

		{
			const char *padName = SDL_GetGamepadName(controller->controller);
			const char *padPath = SDL_GetGamepadPath(controller->controller);
			const bool wasKnown = (padPath != NULL) && (padPath[0] != '\0') && (strcmp(s_controllerRememberedPath[slot], padPath) == 0);

			snprintf(s_controllerRememberedPath[slot], sizeof(s_controllerRememberedPath[slot]), "%s", (padPath != NULL) ? padPath : "");
			Platform_LogWarn("[CTR Native] gamepad %s to pad slot %d: %s (instance %d)\n", wasKnown ? "reconnected" : "connected", slot, (padName != NULL) ? padName : "unknown", (s32)controller->instanceId);
		}
	}

internal void NativeInput_OpenKnownControllers(void)
{
	s32 count = 0;

	SDL_JoystickID *gamepads = SDL_GetGamepads(&count);
	for (s32 i = 0; i < count; i++)
	{
		s32 slot = NativeInput_FindSlotForDeviceIndex(gamepads[i]);

		if (slot >= 0)
		{
			NativeInput_OpenController(gamepads[i], slot);
		}
	}
	SDL_free(gamepads);
}

int Platform_InputInit(void)
{
	if (s_inputInitialized != 0)
	{
		return 1;
	}

	memset(s_controllers, 0, sizeof(s_controllers));
	memset(s_padSlotData, 0, sizeof(s_padSlotData));
	for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		s_controllers[slot].instanceId = -1;
		NativeInput_ResetSnapshot(slot);
		s_installedSnapshots[slot] = s_controllers[slot].snapshot;
	}

	NativeInput_DefaultMappings();

	if (g_cfg_keyboardSlot >= 0)
	{
		s_keyboardControllerSlot = g_cfg_keyboardSlot; // fixed player
	}
	else if (g_cfg_keyboardSlot == -1)
	{
		s_keyboardControllerSlot = NATIVE_INPUT_DEFAULT_KEYBOARD_SLOT; // auto (movable)
	}
	else
	{
		s_keyboardControllerSlot = -2; // pads only: keyboard drives no player
	}

	s_lastActiveControllerSlot = -1;
	s_installedSnapshotsActive = 0;
	s_keyboardState = SDL_GetKeyboardState(NULL);

	if (SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC) == 0)
	{
		fprintf(stderr, "[CTR Native] Failed to initialise SDL input subsystem: %s\n", SDL_GetError());
		return 0;
	}

	SDL_AddGamepadMappingsFromFile("gamecontrollerdb.txt");
		NativeInput_OpenKnownControllers();

		// Fix the pad-bus layout for the whole session. Deciding per frame let a
		// flaky pad (battery/cable) flip the multitap layout mid-game and break
		// the game's boot-time pad detection. g_cfg_padMode == 1 simulates the
		// 4-pad bus from startup so pads can attach into any slot as they appear.
		s_multitapLatched = 0;
		for (s32 slot = NATIVE_INPUT_PHYSICAL_SLOT_COUNT; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
		{
			if (s_controllers[slot].controller != NULL)
			{
				s_multitapLatched = 1;
			}
		}

		if (g_cfg_padMode == 1)
		{
			s_multitapLatched = 1;
		}
		else if (g_cfg_padMode == 2)
		{
			s_multitapLatched = 0;
		}

		s_inputInitialized = 1;
		return 1;
	}

void Platform_InputShutdown(void)
{
	for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		NativeInput_CloseController(slot);
	}

	if (s_inputInitialized != 0)
	{
		SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC);
	}

	s_inputInitialized = 0;
	s_installedSnapshotsActive = 0;
	s_keyboardControllerSlot = NATIVE_INPUT_DEFAULT_KEYBOARD_SLOT;
	s_lastActiveControllerSlot = -1;
	memset(s_padSlotData, 0, sizeof(s_padSlotData));
	s_keyboardState = NULL;
}

void Platform_InputUpdate(void)
{
	if (s_inputInitialized == 0)
	{
		return;
	}

	if (s_installedSnapshotsActive != 0)
	{
		// NOTE(aalhendi): replay/state installs PSX-shaped pad bytes here;
		// SDL host state is not serialized.
		NativeInput_WriteInstalledSnapshots();
		return;
	}

	if (g_padCommEnable == 0)
	{
		return;
	}

	SDL_PumpEvents();
	u16 keyboardButtons = NativeInput_KeyboardSuppressed() ? 0xffff : NativeInput_ReadKeyboard();

	for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
		{
			NativeInput_ResetSnapshot(slot);
			NativeInput_ApplyController(slot);
			NativeInput_ApplyKeyboard(slot, keyboardButtons);
		}

		// pad_mode 1 = "4 pads always on, even if they do not exist": present
		// empty slots as connected idle pads so pad drops/reconnects never change
		// what the game sees. Default-on per project requirement.
		if (g_cfg_padMode == 1)
		{
			for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
			{
				if (s_controllers[slot].snapshot.connected == 0)
				{
					NativeInput_MakeIdleConnectedSnapshot(&s_controllers[slot].snapshot);
				}
			}
		}

		NativeInput_WritePadBus();
	}

void Platform_InputControllerAdded(int deviceIndex)
{
	if (s_inputInitialized == 0)
	{
		return;
	}

	s32 slot = NativeInput_FindSlotForDeviceIndex(deviceIndex);
	if (slot >= 0)
	{
		NativeInput_OpenController(deviceIndex, slot);
	}
}

void Platform_InputControllerRemoved(int instanceId)
{
	for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		if (s_controllers[slot].instanceId == (SDL_JoystickID)instanceId)
		{
			NativeInput_CloseController(slot);
			return;
		}
	}
}

int Platform_InputCycleKeyboardController(void)
{
	// From "Pads only" (-2) or Auto (-1) the first press assigns player 1.
	if (s_keyboardControllerSlot < 0)
	{
		s_keyboardControllerSlot = 0;
	}
	else
	{
		s_keyboardControllerSlot = NativeInput_NextControllerSlot(s_keyboardControllerSlot);
	}

	return s_keyboardControllerSlot + 1;
}

int Platform_InputCycleGamepadController(void)
{
	s32 slot = NativeInput_FindActiveControllerSlot();

	if (slot < 0)
	{
		return 0;
	}

	s32 nextSlot = NativeInput_NextControllerSlot(slot);
	NativeInput_SwapControllerSlots(slot, nextSlot);
	NativeInput_WritePadBus();
	return nextSlot + 1;
}

void Platform_InputPadInit(int slot, unsigned char *padData)
{
	if ((slot < 0) || (slot >= NATIVE_INPUT_PHYSICAL_SLOT_COUNT))
	{
		return;
	}

	s_padSlotData[slot] = padData;
	NativeInput_WritePadBus();
}

int Platform_InputPadGetState(int port)
{
	s32 physicalSlot = (port >> 4) & 1;
	s32 tap = port & 3;
	s32 slot;

	if (NativeInput_UseMultitapBus() != 0)
	{
		if (physicalSlot != 0)
		{
			return PadStateDiscon;
		}
		slot = tap;
	}
	else
	{
		if (tap != 0)
		{
			return PadStateDiscon;
		}
		slot = physicalSlot;
	}

	if ((slot < 0) || (slot >= NATIVE_INPUT_MAX_CONTROLLERS))
	{
		return PadStateDiscon;
	}

	return s_controllers[slot].snapshot.connected ? PadStateStable : PadStateDiscon;
}

int Platform_InputCapturePadSnapshots(struct PlatformInputPadSnapshot *dst, int count)
{
	if ((dst == NULL) || (count < NATIVE_INPUT_MAX_CONTROLLERS))
	{
		return 0;
	}

	for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		dst[slot] = s_controllers[slot].snapshot;
	}

	return NATIVE_INPUT_MAX_CONTROLLERS;
}

int Platform_InputInstallPadSnapshots(const struct PlatformInputPadSnapshot *src, int count)
{
	if ((src == NULL) || (count < NATIVE_INPUT_MAX_CONTROLLERS))
	{
		return 0;
	}

	for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		s_installedSnapshots[slot] = src[slot];
	}

	s_installedSnapshotsActive = 1;
	NativeInput_WriteInstalledSnapshots();
	return NATIVE_INPUT_MAX_CONTROLLERS;
}

void Platform_InputClearInstalledPadSnapshots(void)
{
	s_installedSnapshotsActive = 0;
}

int Platform_InputGetStateSize(void)
{
	return (int)sizeof(struct NativeInputStateSnapshot);
}

int Platform_InputCaptureState(void *dst, int dstSize)
{
	struct NativeInputStateSnapshot *snapshot = (struct NativeInputStateSnapshot *)dst;

	if ((dst == NULL) || (dstSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->magic = NATIVE_INPUT_STATE_MAGIC;
	snapshot->version = NATIVE_INPUT_STATE_VERSION;
	snapshot->size = sizeof(*snapshot);
	snapshot->keyboardControllerSlot = s_keyboardControllerSlot;
	snapshot->lastActiveControllerSlot = s_lastActiveControllerSlot;
	snapshot->installedSnapshotsActive = s_installedSnapshotsActive;

	for (s32 slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		snapshot->installedSnapshots[slot] = s_installedSnapshots[slot];
		snapshot->controllers[slot].snapshot = s_controllers[slot].snapshot;
		snapshot->controllers[slot].analogEnabled = s_controllers[slot].analogEnabled;
		snapshot->controllers[slot].switchingAnalog = s_controllers[slot].switchingAnalog;
		snapshot->controllers[slot].controllerToSlotMapping = s_controllerToSlotMapping[slot];
	}

	return 1;
}

int Platform_InputRestoreState(const void *src, int srcSize)
{
	const struct NativeInputStateSnapshot *snapshot = (const struct NativeInputStateSnapshot *)src;
	s32 slot;

	if ((src == NULL) || (srcSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}
	if ((snapshot->magic != NATIVE_INPUT_STATE_MAGIC) || (snapshot->version != NATIVE_INPUT_STATE_VERSION) || (snapshot->size != sizeof(*snapshot)))
	{
		return 0;
	}
	if (!NativeInput_IsValidControllerSlot(snapshot->keyboardControllerSlot))
	{
		return 0;
	}
	if ((snapshot->lastActiveControllerSlot < -1) || (snapshot->lastActiveControllerSlot >= NATIVE_INPUT_MAX_CONTROLLERS))
	{
		return 0;
	}
	if ((snapshot->installedSnapshotsActive < 0) || (snapshot->installedSnapshotsActive > 1))
	{
		return 0;
	}
	for (slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		if ((snapshot->controllers[slot].analogEnabled < 0) || (snapshot->controllers[slot].analogEnabled > 1))
		{
			return 0;
		}
		if ((snapshot->controllers[slot].switchingAnalog < 0) || (snapshot->controllers[slot].switchingAnalog > 1))
		{
			return 0;
		}
		if (snapshot->controllers[slot].controllerToSlotMapping < -1)
		{
			return 0;
		}
	}

	s_keyboardControllerSlot = snapshot->keyboardControllerSlot;
	s_lastActiveControllerSlot = snapshot->lastActiveControllerSlot;
	s_installedSnapshotsActive = snapshot->installedSnapshotsActive != 0;

	for (slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		s_installedSnapshots[slot] = snapshot->installedSnapshots[slot];
		s_controllers[slot].snapshot = snapshot->controllers[slot].snapshot;
		s_controllers[slot].analogEnabled = snapshot->controllers[slot].analogEnabled;
		s_controllers[slot].switchingAnalog = snapshot->controllers[slot].switchingAnalog;
		s_controllerToSlotMapping[slot] = snapshot->controllers[slot].controllerToSlotMapping;
	}
	NativeInput_WritePadBus();

	return 1;
}

// Last actuator values pushed per slot — the game re-submits every frame, so
// this gates both the debug trace and the SDL call to real changes only.
static u16 s_vibrateLow[NATIVE_INPUT_MAX_CONTROLLERS];
static u16 s_vibrateHigh[NATIVE_INPUT_MAX_CONTROLLERS];

void Platform_InputPadVibrate(int port, unsigned char *table, int len)
{
	s32 physicalSlot = (port >> 4) & 1;
	s32 tap = port & 3;
	s32 slot;

	if (NativeInput_UseMultitapBus() != 0)
	{
		if (physicalSlot != 0)
		{
			return;
		}
		slot = tap;
	}
	else
	{
		if (tap != 0)
		{
			return;
		}
		slot = physicalSlot;
	}

	if ((slot < 0) || (slot >= NATIVE_INPUT_MAX_CONTROLLERS) || (table == NULL) || (len <= 0))
	{
		return;
	}

	struct NativeInputController *controller = &s_controllers[slot];
	if (controller->controller == NULL)
	{
		return;
	}

	u16 freqHigh = table[0] * 255;
	u16 freqLow = len > 1 ? table[1] * 255 : 0;

	if ((freqLow != 0) && (freqLow < 4096))
	{
		freqLow = 4096;
	}

	if ((freqHigh != 0) && (freqHigh < 4096))
	{
		freqHigh = 4096;
	}

	// Debug trace on change (verbose-gated via the [CTR Debug] prefix): shows
	// exactly what the game wants the pad to do and which slot it maps to.
	if ((s_vibrateLow[slot] != freqLow) || (s_vibrateHigh[slot] != freqHigh))
	{
		s_vibrateLow[slot] = freqLow;
		s_vibrateHigh[slot] = freqHigh;
		Platform_LogWarn("[CTR Debug] pad slot %d vibrate: low=%u high=%u (port=0x%X len=%d)\n",
		                 slot, (unsigned)freqLow, (unsigned)freqHigh, (unsigned)port, len);
	}

	if (g_cfg_gamepadRumble != 0)
	{
		if (SDL_RumbleGamepad(controller->controller, freqLow, freqHigh, 200) == false)
		{
			Platform_LogWarn("[CTR Native] SDL_RumbleGamepad failed: %s\n", SDL_GetError());
		}
	}
}

// Diagnostic tool (runtime key R): fires a short rumble on every connected pad
// so the SDL/pad path can be verified independently of any game event.
void Platform_InputRumbleTest(void)
{
	int slot;
	int fired = 0;

	for (slot = 0; slot < NATIVE_INPUT_MAX_CONTROLLERS; slot++)
	{
		if (s_controllers[slot].controller != NULL)
		{
			SDL_RumbleGamepad(s_controllers[slot].controller, 0x8000, 0x8000, 700);
			fired++;
		}
	}

	Platform_LogWarn("[CTR Native] rumble test: fired %d pad(s), 700ms\n", fired);
}
