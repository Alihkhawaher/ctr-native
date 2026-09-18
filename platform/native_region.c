#include "platform/native_region.h"

#include <macros.h>

#include <platform/native_disc_image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

global_variable char s_nativeRegionBootId[64] = "";

internal int NativeRegion_ExtractBootId(const char *cnfText, char *out, size_t outSize)
{
	const char *cursor = strstr(cnfText, "BOOT");
	const char *value;
	size_t length = 0;

	if (cursor == NULL)
	{
		return 0;
	}

	// "BOOT = cdrom:\SCUS_944.26;1" — keep the id, drop path prefix and ;1.
	value = strstr(cursor, "cdrom:");
	if (value == NULL)
	{
		return 0;
	}

	value += 6;
	while ((*value == '\\') || (*value == '/') || (*value == ' '))
	{
		value++;
	}

	while ((value[length] != '\0') && (value[length] != ';') && (value[length] != '\r') && (value[length] != '\n') && ((length + 1) < outSize))
	{
		out[length] = value[length];
		length++;
	}

	out[length] = '\0';
	return length != 0;
}

internal enum NativeRegion NativeRegion_ClassifyBootId(const char *bootId)
{
	// Prefix families: SCUS_/SLUS_ = NTSC-U, SCES_/SLES_ = PAL, SCPS_/SLPS_ = NTSC-J.
	if ((strncmp(bootId, "SCUS_", 5) == 0) || (strncmp(bootId, "SLUS_", 5) == 0))
	{
		return NATIVE_REGION_NTSC_U;
	}

	if ((strncmp(bootId, "SCES_", 5) == 0) || (strncmp(bootId, "SLES_", 5) == 0))
	{
		return NATIVE_REGION_PAL;
	}

	if ((strncmp(bootId, "SCPS_", 5) == 0) || (strncmp(bootId, "SLPS_", 5) == 0))
	{
		return NATIVE_REGION_NTSC_J;
	}

	return NATIVE_REGION_UNKNOWN;
}

enum NativeRegion NativeRegion_DetectFromDisc(void)
{
	u8 *cnfData = NULL;
	int size = 0;
	char bootId[64];
	enum NativeRegion region;

	s_nativeRegionBootId[0] = '\0';

	if (!NativeDiscImage_ReadFileBytes("SYSTEM.CNF", 0, &cnfData, &size) || (cnfData == NULL) || (size <= 0))
	{
		free(cnfData);
		return NATIVE_REGION_UNKNOWN;
	}

	if (NativeRegion_ExtractBootId((const char *)cnfData, bootId, sizeof(bootId)))
	{
		snprintf(s_nativeRegionBootId, sizeof(s_nativeRegionBootId), "%s", bootId);
		region = NativeRegion_ClassifyBootId(bootId);
	}
	else
	{
		region = NATIVE_REGION_UNKNOWN;
	}

	free(cnfData);
	return region;
}

const char *NativeRegion_Name(enum NativeRegion region)
{
	switch (region)
	{
	case NATIVE_REGION_NTSC_U:
		return "NTSC-U";
	case NATIVE_REGION_PAL:
		return "PAL";
	case NATIVE_REGION_NTSC_J:
		return "NTSC-J";
	default:
		return "unknown";
	}
}

const char *NativeRegion_GetLastBootId(void)
{
	return s_nativeRegionBootId;
}

int NativeRegion_IsSupported(enum NativeRegion region)
{
	// UNKNOWN = no disc / unreadable SYSTEM.CNF (extracted-asset setups): the
	// engine has always supported those, keep allowing them.
	return (region == NATIVE_REGION_NTSC_U) || (region == NATIVE_REGION_UNKNOWN);
}
