#ifndef CTR_MATCHING_OVERLAY_225_RETAIL_SYMBOLS_H
#define CTR_MATCHING_OVERLAY_225_RETAIL_SYMBOLS_H

#include <common.h>

// NOTE(aalhendi): Overlay 225 addresses resident EXE state absolutely rather
// than through gp. Keep these artifact bindings out of shared layout headers.
#define VB_GAME_TRACKER_ASM_NAME    "sdata_static+832"
#define VB_GAME_TRACKER_PAGE        0x80090000u
#define VB_GAME_TRACKER_PAGE_OFFSET (-0x2d54)
#define VB_STANDINGS_SUFFIX_PAGE    ((s16 *)0x800a0000u)
// NOTE(aalhendi): suffixBase is pinned to $2 in shared C. Keep this basic asm
// register-specific so GCC 2.8.1 does not extend the value's live range.
#define VB_ADD_STANDINGS_SUFFIX_LOW(value)                         \
	do                                                             \
	{                                                              \
		(void)sizeof(value);                                       \
		__asm__("addiu $2,$2,%lo(s_standingsSuffixStringIds225)"); \
	} while (0)
// NOTE(aalhendi): Retail keeps the GameTracker page in $3 at the viewport-loop
// tail. The page-relative load preserves that lifetime without leaking the
// artifact offset into shared source.
#define VB_PLAYER_COUNT_FROM_PAGE(page)      (CTR_PSX_PAGE_LVALUE(struct GameTracker *, (page), VB_GAME_TRACKER_PAGE_OFFSET, VB_GAME_TRACKER)->numPlyrCurrGame)
#define VB_VIEW_TYPE                         struct GameTracker
#define VB_VIEW_FROM_OFFSET(offset)          ((struct GameTracker *)((u8 *)VB_GAME_TRACKER + (offset)))
#define VB_VIEW_PUSH_BUFFER(view)            ((view)->pushBuffer[0])
#define VB_PUSH_BUFFER_FROM_OFFSET(offset)   (((struct GameTracker *)((u8 *)VB_GAME_TRACKER + (offset)))->pushBuffer[0])

// NOTE(aalhendi): GCC 2.8.1 needs this otherwise unused lifetime to preserve
// the retail row-state spill homes and setup schedule without emitted code.
#define VB_MATCH_ROW_ALLOCATION_BEGIN(value) __asm__ volatile("" : "=g"(value))
// NOTE(aalhendi): The shared C produces retail's row-preheader instructions,
// but GCC schedules those independent instructions differently. Replace only
// that preheader with retail's order; $L250 restores the skipped loop label.
#define VB_MATCH_ROW_SCHEDULE_BEGIN()        __asm__ volatile(".if 0")
#define VB_MATCH_ROW_SCHEDULE_END()                                  \
	__asm__ volatile(".endif\n\t"                                    \
	                 "lui $30,%hi(" VB_GAME_TRACKER_ASM_NAME ")\n\t" \
	                 "sw $3,132($sp)\n\t"                            \
	                 "addu $9,$3,-2\n\t"                             \
	                 "sw $9,136($sp)\n\t"                            \
	                 "slt $9,$3,3\n\t"                               \
	                 "move $22,$20\n\t"                              \
	                 "sw $9,140($sp)\n\t"                            \
	                 "li $9,30\n\t"                                  \
	                 "sw $9,144($sp)\n\t"                            \
	                 "li $9,5\n\t"                                   \
	                 "sw $9,148($sp)\n\t"                            \
	                 "$L250:")
#define VB_MATCH_ROW_SETUP_ORDER(rowCount, configIndex) __asm__ volatile("" : "+g"(rowCount), "+g"(configIndex))
#define VB_MATCH_ROW_ALLOCATION_END(value)              __asm__ volatile("" : : "g"(value))

// NOTE(aalhendi): Keep the hand-scheduled retail preheader tied to the shared
// C constants. GCC 2.8.1 supports this array-bound assertion even though
// CTR_STATIC_ASSERT intentionally emits nothing for that compiler.
#define VB_VALIDATE_MATCHING_CONSTANTS()                 \
	typedef char vb_matching_constants_must_match_retail \
	    [(VB_MIN_PLAYERS == 2 && VB_STANDINGS_EXPANDED_MIN_ENTRIES == 3 && VB_ROW_INITIAL_DELAY_FRAMES == 30 && VB_ROW_STAGGER_FRAMES == 5) ? 1 : -1];

extern struct GameTracker *vb_gameTracker asm(VB_GAME_TRACKER_ASM_NAME);
extern s32 vb_framesSinceRaceEnded asm("sdata_static+1472");
extern char **vb_languageStrings asm("sdata_static+2316");
extern s32 vb_menuReady asm("sdata_static+1360");
// NOTE(aalhendi): Retail passes this packed color by address at this call site.
// Native copies the resident u32 through its adapter.
extern Color vb_battleColor asm("sdata_static+1228");
extern struct MetaDataCHAR vb_characterMetadata[16] asm("data+25572");
extern s16 vb_characterIDs[8] asm("data+25828");

#define VB_GAME_TRACKER            vb_gameTracker
#define VB_FRAMES_SINCE_RACE_ENDED vb_framesSinceRaceEnded
#define VB_LANGUAGE_STRINGS        vb_languageStrings
#define VB_MENU_READY              vb_menuReady
#define VB_BATTLE_COLOR_PTR        (&vb_battleColor)
#define VB_CHARACTER_METADATA      vb_characterMetadata
#define VB_CHARACTER_IDS           vb_characterIDs
#define VB_DRAW_OUTER_RECT         RECTMENU_DrawOuterRect_HighLevel
#define VB_DRAW_POLY_FT4           DecalHUD_DrawPolyFT4

#endif
