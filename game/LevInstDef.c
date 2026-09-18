#include <common.h>

#if defined(CTR_NATIVE)
#include <platform/native_log.h>
#endif

// NOTE(aalhendi): These lists alternate between InstDef* and Instance* in place.
// Both records keep their peer at 0x2c. Shared PVS lists therefore toggle once
// per reference, just as retail does; do not deduplicate the traversal.
// Alias-qualified slots and byte-safe peer reads support either declared type.
typedef void *LevInstDefLink CTR_MAY_ALIAS;
CTR_STATIC_ASSERT(offsetof(struct InstDef, ptrInstance) == offsetof(struct Instance, instDef));

static inline void *LevInstDef_Peer(const void *record)
{
	return (void *)CTR_ReadU32AlignedLE((const u8 *)record + offsetof(struct InstDef, ptrInstance));
}


void LevInstDef_UnPack(struct mesh_info *ptr_mesh_info)
{
	struct QuadBlock *qbCurr;
	struct QuadBlock *qbEnd;
	LevInstDefLink *visInstSrc;
	struct Level *level1;
#if defined(CTR_NATIVE)
	int dbgBadQB = 0;
#endif

	qbCurr = ptr_mesh_info->ptrQuadBlockArray;
	qbEnd = qbCurr + ptr_mesh_info->numQuadBlock;

#if defined(CTR_NATIVE)
	Platform_LogWarn("[CTR Debug] UnPack enter: mesh=%p qbArr=%p numQB=%d level1=%p instArr=%p\n", (void *)ptr_mesh_info,
	                 (void *)ptr_mesh_info->ptrQuadBlockArray, (int)ptr_mesh_info->numQuadBlock, (void *)GAME_TRACKER->level1,
	                 (void *)(GAME_TRACKER->level1 ? GAME_TRACKER->level1->ptrInstDefPtrArray : 0));
#endif

	// loop through all quadblocks
	for (; qbCurr < qbEnd; qbCurr++)
	{
		if ((qbCurr->pvs != 0) && (qbCurr->pvs->visInstSrc != 0))
		{
			// loop through all instance pointers visible on quadblock
			for (visInstSrc = (LevInstDefLink *)qbCurr->pvs->visInstSrc; visInstSrc[0] != NULL; visInstSrc++)
			{
#if defined(CTR_NATIVE)
				// A list whose first word is its own address is the engine's
				// "empty list" sentinel (the renderer skips such lists by the
				// same self-check). Toggling it would read non-record data at
				// +0x2C and corrupt the sentinel; PSX tolerates the junk read,
				// hosts fault on it. Preserve the sentinel.
				if (visInstSrc[0] == (void *)visInstSrc)
				{
					Platform_LogWarn("[CTR Debug] UnPack: preserved self-sentinel list=%p qb=%p\n", (void *)visInstSrc, (void *)qbCurr);
					continue;
				}
				if (((u32)visInstSrc[0] < 0x00400000u) || ((u32)visInstSrc[0] >= 0x10000000u))
				{
					dbgBadQB++;
					if (dbgBadQB <= 10)
					{
						Platform_LogError("[CTR Debug] UnPack BAD qb-entry: list=%p idx=%d val=%p qb=%p\n", (void *)visInstSrc,
						                  (int)(visInstSrc - (LevInstDefLink *)qbCurr->pvs->visInstSrc), visInstSrc[0], (void *)qbCurr);
					}
					continue;
				}
#endif
				visInstSrc[0] = LevInstDef_Peer(visInstSrc[0]);
			}
		}
	}

	level1 = GAME_TRACKER->level1;
	visInstSrc = (LevInstDefLink *)level1->ptrInstDefPtrArray;

	if (visInstSrc != NULL)
	{
		// loop through all instDef pointers in the LEV
		for (; visInstSrc[0] != 0; visInstSrc++)
		{
#if defined(CTR_NATIVE)
			if (((u32)visInstSrc[0] < 0x00400000u) || ((u32)visInstSrc[0] >= 0x10000000u))
			{
				dbgBadQB++;
				if (dbgBadQB <= 10)
				{
					Platform_LogError("[CTR Debug] UnPack BAD lev-entry: list=%p idx=%d val=%p\n", (void *)visInstSrc,
					                  (int)(visInstSrc - (LevInstDefLink *)level1->ptrInstDefPtrArray), visInstSrc[0]);
				}
				continue;
			}
#endif
			visInstSrc[0] = LevInstDef_Peer(visInstSrc[0]);
		}
	}

#if defined(CTR_NATIVE)
	if (dbgBadQB != 0)
	{
		Platform_LogError("[CTR Debug] UnPack: skipped %d bad entries total\n", dbgBadQB);
	}
#endif
}


void LevInstDef_RePack(struct mesh_info *ptr_mesh_info, b32 boolAdvHub)
{
	struct QuadBlock *qbCurr;
	struct QuadBlock *qbEnd;
	LevInstDefLink *visInstSrc;
	struct Level *level1;
	struct Thread *th;

	qbCurr = ptr_mesh_info->ptrQuadBlockArray;
	qbEnd = qbCurr + ptr_mesh_info->numQuadBlock;

	// loop through all quadblocks
	for (; qbCurr < qbEnd; qbCurr++)
	{
		if ((qbCurr->pvs != 0) && (qbCurr->pvs->visInstSrc != 0))
		{
			// loop through all instance pointers visible on quadblock
			for (visInstSrc = (LevInstDefLink *)qbCurr->pvs->visInstSrc; visInstSrc[0] != NULL; visInstSrc++)
			{
#if defined(CTR_NATIVE)
				// Preserve the "empty list" self-sentinel, same as UnPack.
				if (visInstSrc[0] == (void *)visInstSrc)
				{
					continue;
				}
#endif
				visInstSrc[0] = LevInstDef_Peer(visInstSrc[0]);
			}
		}
	}

	level1 = GAME_TRACKER->level1;
	visInstSrc = (LevInstDefLink *)level1->ptrInstDefPtrArray;

	if (visInstSrc != NULL)
	{
		// loop through all instDef pointers in the LEV
		for (; visInstSrc[0] != NULL; visInstSrc++)
		{
			struct Instance *inst = visInstSrc[0];
			struct InstDef *instDef = inst->instDef;

			// if on adv hub
			if (boolAdvHub != 0)
			{
				th = inst->thread;
				if (th != 0)
				{
					th->flags |= THREAD_FLAG_DEAD;
				}

				// Return the level-owned instance to the free pool.
				LIST_AddFront(&GAME_TRACKER->JitPools.instance.free, (struct Item *)visInstSrc[0]);
			}

			// go back to instDef
			visInstSrc[0] = instDef;
		}
	}

	PROC_CheckAllForDead();
}
