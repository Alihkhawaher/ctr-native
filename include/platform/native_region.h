#ifndef NATIVE_REGION_H
#define NATIVE_REGION_H

// Disc-region detection and dispatch policy.
//
// The shipped game code is the NTSC-U decompilation (SCUS-94426) ONLY. Foreign
// discs (PAL SCES-02105, NTSC-J SCPS-10118) carry their own game executables
// — which a source port cannot run — and different data layouts (PAL BIGFILE
// is ~30 MB larger from its six languages), so the NTSC-U code segfaults on
// them. The engine therefore detects the disc's BOOT id up front and refuses
// foreign discs with a clear message instead of crashing; experiments can
// override with --allow-foreign-disc.
//
// Extracted-asset setups (no disc image) report UNKNOWN and stay allowed.

enum NativeRegion
{
	NATIVE_REGION_UNKNOWN = 0,
	NATIVE_REGION_NTSC_U,
	NATIVE_REGION_PAL,
	NATIVE_REGION_NTSC_J,
};

// Reads SYSTEM.CNF from the loaded disc image and classifies the BOOT id.
enum NativeRegion NativeRegion_DetectFromDisc(void);
const char *NativeRegion_Name(enum NativeRegion region);
// The raw BOOT id from the last detection ("SCUS_944.26", ...), or "".
const char *NativeRegion_GetLastBootId(void);
// Whether the shipped game code can run this region (NTSC-U and UNKNOWN).
int NativeRegion_IsSupported(enum NativeRegion region);

#endif
