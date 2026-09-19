#ifndef PLATFORM_H
#define PLATFORM_H

struct PlatformMempackArena
{
	void *base;
	void *start;
	void *endOfMemory;
	int size;
	int backingSize;
};

void Platform_Init(const char *title, int width, int height, int fullscreen);
void Platform_Shutdown(void);
void Platform_InitScratchpad(void);
const struct PlatformMempackArena *Platform_InitMempackArena(void);
const struct PlatformMempackArena *Platform_GetMempackArena(void);
void Platform_BeginFrame(void);
int Platform_BeginScene(void);
void Platform_EndScene(void);
void Platform_EndFrame(void);
void Platform_PresentVRAMDisplay(void);
void Platform_PinVRAMDisplayFrames(int frameCount);
void Platform_PinVRAMDisplayRect(int x, int y, int w, int h, int frameCount);
int Platform_GetVBlankCount(void);
void Platform_WaitUntilVBlank(int targetVBlank);
void Platform_PollHostEvents(void);
int Platform_PollInput(void);

// PGXP memory cache: bind a prim field that just received raw GTE output
// (packed SXY) to the full-precision transform, keyed by its address.
// Called by the RenderBucket prim writers; see platform/native_gpu.c.
void Pgxp_NoteSxyStore(const void *addrField, unsigned int packedSxy);
void Pgxp_NotePrimWrite(void *dstField, const void *srcField, unsigned int packed);
void Pgxp_NoteTransformCopy(void *dstField, const void *srcField);
void Pgxp_SetPackedSource(const void *field, unsigned int packed);

#if defined(CTR_NATIVE)
int NikoGetEnterKey(void);
#endif

#endif
