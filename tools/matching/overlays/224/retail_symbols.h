#ifndef CTR_MATCHING_OVERLAY_224_RETAIL_SYMBOLS_H
#define CTR_MATCHING_OVERLAY_224_RETAIL_SYMBOLS_H

#include <common.h>

// NOTE(aalhendi): Overlay 224 addresses resident EXE state absolutely rather
// than through gp. Keep those artifact bindings out of shared layout headers.
// The [0] access shape is also intentional: scalar aliases change GCC 2.8.1's
// allocation across this translation unit.
extern struct GameTracker *tt_gameTracker[3] asm("sdata_static+832");
extern char **tt_languageStrings[3] asm("sdata_static+2316");
extern s32 tt_framesSinceRaceEnded[3] asm("sdata_static+1472");
extern u32 tt_flags[3] asm("sdata_static+2592");
extern struct GameProgress tt_gameProgress asm("sdata_static+6012");
extern s32 tt_menuReady[3] asm("sdata_static+1360");
extern s32 tt_anyPlayerTap[3] asm("sdata_static+2532");
extern b16 tt_ghostTooBig[3] asm("sdata_static+2008");
extern Color tt_menuHighlight asm("sdata_static+2528");
extern struct MetaDataCHAR tt_characterMetadata[16] asm("data+25572");

// NOTE(aalhendi): These declarations preserve the retail callers' argument
// layout without imposing it on the native renderer interfaces.
extern void tt_drawPolyGT4(struct Icon *icon, s16 posX, s32 posY, struct PrimMem *primMem, u32 *ot, Color color0, Color color1, Color color2, Color color3,
                           s8 transparency, s16 scale) asm("RECTMENU_DrawPolyGT4");
extern void tt_drawClearBox(const RECT *rect, const Color *color, s32 transparency, u32 *ot, struct PrimMem *primMem) asm("CTR_Box_DrawClearBox");
extern void tt_drawLineWideX(char *str, s32 posX, s16 posY, s16 fontType, s16 flags) asm("DecalFont_DrawLine");

#define TT_GAME_TRACKER            tt_gameTracker[0]
#define TT_LANGUAGE_STRINGS        tt_languageStrings[0]
#define TT_FRAMES_SINCE_RACE_ENDED tt_framesSinceRaceEnded[0]
#define TT_FLAGS                   tt_flags[0]
#define TT_GAME_PROGRESS           tt_gameProgress
#define TT_MENU_READY              tt_menuReady[0]
#define TT_ANY_PLAYER_TAP          tt_anyPlayerTap[0]
#define TT_GHOST_TOO_BIG           tt_ghostTooBig[0]
#define TT_MENU_HIGHLIGHT          tt_menuHighlight
#define TT_CHARACTER_METADATA      tt_characterMetadata
#define TT_DRAW_POLY_GT4           tt_drawPolyGT4
#define TT_DRAW_CLEAR_BOX          tt_drawClearBox
#define TT_DRAW_LINE_WIDE_X        tt_drawLineWideX

#endif
