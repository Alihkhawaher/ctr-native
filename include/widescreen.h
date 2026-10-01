#ifndef WIDESCREEN_H
#define WIDESCREEN_H

// True widescreen (Hor+), ported from thecodingbob/ctr-native PR #16
// ("Add widescreen option", 2026-06-28..07-01), adapted to this fork.
//
// The game still renders its 512-wide PSX frame. With true widescreen on and a
// presentation aspect wider than 4:3, the view-projection X row is scaled by
// factor/1000 (PushBuffer_SetMatrixVP) so a WIDER slice of the world is
// squeezed into that frame, the X frustum is widened to match
// (PushBuffer_UpdateFrustum), and the presenter stretches the frame back out:
// correct proportions, more horizontal field of view.
//
// 2D HUD quads are squeezed by the same factor around their own centre so
// they keep their shape after the stretch (positions are kept, so the HUD
// spreads toward the screen edges).
//
// factor = 1000 * (4/3) / presentedAspect, clamped to [250, 1000];
// 1000 = vanilla 4:3 (or true widescreen off: plain stretch).
int Widescreen_GetFactor(void);

#define WIDESCREEN_SQUEEZE_X4(xa, xb, xc, xd)                                  \
	do                                                                         \
	{                                                                          \
		const int wsFactor_ = Widescreen_GetFactor();                          \
		if (wsFactor_ < 1000)                                                  \
		{                                                                      \
			const int wsC_ = ((int)(xa) + (int)(xb) + (int)(xc) + (int)(xd)) / 4; \
			(xa) = wsC_ + (((int)(xa) - wsC_) * wsFactor_) / 1000;             \
			(xb) = wsC_ + (((int)(xb) - wsC_) * wsFactor_) / 1000;             \
			(xc) = wsC_ + (((int)(xc) - wsC_) * wsFactor_) / 1000;             \
			(xd) = wsC_ + (((int)(xd) - wsC_) * wsFactor_) / 1000;             \
		}                                                                      \
	} while (0)

#endif
