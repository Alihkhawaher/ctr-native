#ifndef NATIVE_OVERLAY_H
#define NATIVE_OVERLAY_H

// Brief host-side overlay that shows the active graphics options for a couple
// of seconds after startup or an option change. Drawn directly to the default
// framebuffer before the buffer swap; never passes through the PSX pipeline.

void NativeOverlay_Init(void);
void NativeOverlay_Shutdown(void);
void NativeOverlay_Show(void);
void NativeOverlay_Draw(void);

#endif
