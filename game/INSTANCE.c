#include <common.h>

void INSTANCE_Birth(struct Instance *inst, struct Model *model, const char *name, struct Thread *th, u32 flags)
{
	s32 i;
	struct GameTracker *gGT;
	char *dst = inst->name;
	char *last = &inst->name[sizeof(inst->name) - 1];

#if defined(CTR_NATIVE)
	// NOTE(aalhendi): Retail copies a fixed 15-byte field. Native also accepts
	// NULL and short C strings, so stop at their terminator and zero-pad.
	while (dst < last && name != NULL && *name != '\0')
#else
	while (dst < last)
#endif
	{
		*dst++ = *name++;
	}
	while (dst <= last)
		*dst++ = '\0';

	inst->depthBiasNormal = 0xfe;
	inst->depthBiasSecondary = 0xc;
	inst->animIndex = 0;
	inst->specLightX = 1;

	gGT = GAME_TRACKER;
	inst->model = model;

	inst->scale.x = 0x1000;
	inst->scale.y = 0x1000;
	inst->scale.z = 0x1000;

	inst->flags = flags;
	inst->alphaScale = 0;
	inst->colorRGBA = 0;
	inst->instDef = 0;

	inst->animFrame = 0;
	inst->vertSplit = 0;
	inst->reflectionRGBA = 0x7f7f7f;

	inst->thread = th;
	inst->compressedNormalAndDriverIndex = 0;

	for (i = 0; i < gGT->numPlyrCurrGame; i++)
	{
		inst->idpp[i].mh = 0;
		inst->idpp[i].pushBuffer = &gGT->pushBuffer[i];
		inst->idpp[i].instFlags = 0;
	}
}


struct Instance *INSTANCE_Birth3D(struct Model *model, const char *name, struct Thread *th)
{
	struct Instance *inst = (struct Instance *)JitPool_Add(&GAME_TRACKER->JitPools.instance);

	if (inst != 0)
	{
		INSTANCE_Birth(inst, model, name, th, DRAW_COLLISION_MASK);
	}

	return inst;
}


struct Instance *INSTANCE_Birth2D(struct Model *model, const char *name, struct Thread *th)
{
	struct GameTracker *gGT;
	struct Instance *inst;
	s32 i;

	inst = (struct Instance *)JitPool_Add(&GAME_TRACKER->JitPools.instance);

	if (inst != NULL)
	{
		INSTANCE_Birth(inst, model, name, th, 0x40f);
	}
#if defined(CTR_NATIVE)
	else
	{
		// NOTE(aalhendi): Retail assumes capacity; native cannot write through
		// the null instance when the shared pool is exhausted.
		return NULL;
	}
#endif

	gGT = GAME_TRACKER;
	inst->idpp[0].pushBuffer = &gGT->pushBuffer_UI;

	i = 1;
	if (i < gGT->numPlyrCurrGame)
	{
		// NOTE(aalhendi): Keep retail's separate pre-loop and loop pointer lifetimes.
		struct GameTracker *loopTracker = gGT;

		do
		{
			inst->idpp[i].pushBuffer = 0;
			i++;
		} while (i < loopTracker->numPlyrCurrGame);
	}

	return inst;
}


#if defined(CTR_NATIVE)
static void INSTANCE_RollbackThreadBirth(struct Thread *t, struct Thread *relativeTh)
{
	struct GameTracker *gGT = GAME_TRACKER;

	if (relativeTh == NULL)
	{
		gGT->threadBuckets[t->flags & 0xff].thread = t->siblingThread;
	}
	else if ((t->flags & SELF_SIBLING) != 0)
	{
		relativeTh->siblingThread = t->siblingThread;
	}
	else if ((t->flags & CHILD_BETWEEN) != 0)
	{
		relativeTh->childThread = t->childThread;
	}
	else
	{
		relativeTh->childThread = t->siblingThread;
	}

	PROC_DestroyObject(t->object, t->flags);
	LIST_AddFront(&gGT->JitPools.thread.free, (struct Item *)t);
}
#endif

struct Instance *INSTANCE_BirthWithThread(s32 modelID, const char *name, s32 poolType, s32 bucket, void *funcThTick, s32 objSize, struct Thread *parent)
{
	struct Model *lookupModel;
	struct Model *model;
	struct Thread *t;
	struct Instance *inst;
	u32 remainder;
	register u32 sizeFlags CTR_PSX_REGISTER("$2");

	lookupModel = GAME_TRACKER->modelPtr[modelID];

	if (lookupModel == NULL)
	{
		return NULL;
	}

	// NOTE(aalhendi): Keep the validated lookup separate from the model retained
	// across allocation, preserving retail's register lifetime at the alignment branch.
	CTR_PSX_OBSERVE_VALUE(lookupModel);
	model = lookupModel;

	// Round payloads such as TalkingMask up to a word, then pack the size field.
	remainder = objSize & 3;
	if (remainder != 0)
	{
		remainder -= 4;
		sizeFlags = ((u32)objSize - remainder) << 16;
	}
	else
	{
		sizeFlags = (u32)objSize << 16;
	}

	// NOTE(aalhendi): Retail combines the pool bits in v0 before the bucket.
	sizeFlags = poolType | sizeFlags;
	CTR_PSX_OBSERVE_VALUE(sizeFlags);
	t = PROC_BirthWithObject(sizeFlags | bucket, funcThTick, name, parent);

#if defined(CTR_NATIVE)
	// NOTE(aalhendi): Retail assumes the thread and instance pools have capacity.
	// Native returns failure instead of writing through PS1 low memory.
	if (t == NULL)
	{
		return NULL;
	}
#endif

	t->modelIndex = modelID;
	inst = INSTANCE_Birth3D(model, name, t);

#if defined(CTR_NATIVE)
	if (inst == NULL)
	{
		INSTANCE_RollbackThreadBirth(t, parent);
		return NULL;
	}
#endif

	t->inst = inst;

	return inst;
}


struct Instance *INSTANCE_BirthWithThread_Stack(const struct InstanceBirthParams *params)
{
	return INSTANCE_BirthWithThread(params->modelID, params->name, params->poolType, params->bucket, params->funcThTick, params->objSize, params->parent);
}


void INSTANCE_Death(struct Instance *inst)
{
	JitPool_Remove(&GAME_TRACKER->JitPools.instance, (struct Item *)inst);
}


// param1 - pointer to Instance Descriptions
// param2 - number of instances
void INSTANCE_LevInitAll(struct InstDef *levInstDef, int numInst)
{
	u16 modelID;
	int *dst;
	int *src;
	struct Instance *inst;
	struct MetaDataMODEL *meta;
	struct GameTracker *gGT = sdata->gGT;
	s32 i;

	for (i = 0; i < numInst; i++)
	{
		struct InstDrawPerPlayer *idpp;
		s32 j;
		b32 boolArcadeOnly;
		b32 boolRelicOnly;

		// get first free item in Instance Pool
		inst = (struct Instance *)LIST_RemoveFront(&gGT->JitPools.instance.free);

		// NOT writing to model
		// InstDef + 0x10 + 0x1c
		// InstDef -> 0x2C = ptrInstance
		levInstDef->ptrInstance = inst;

		// if allocation failed
		if (inst == NULL)
		{
			return;
		}

		// pointer to InstDef in LEV
		src = (int *)levInstDef;

		// pointer to instance in pool,
		// add 8 bytes to skip Prev and Next
		dst = (int *)((int)inst + 8);

		// copy InstDef data from LEV to instance pool
		while (src != (int *)((int)levInstDef + 0x20))
		{
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
			dst[3] = src[3];
			src += 4;
			dst += 4;
		}

		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];

		// 0x10 + (5 * 4) = 0x24
		inst->depthBiasNormal = levInstDef->unk24 - 2;
		inst->depthBiasSecondary = levInstDef->unk24 + 12;

		// reflect color
		inst->reflectionRGBA = 0x7f7f7f;

		inst->animIndex = 0;
		inst->animFrame = 0;

		// instace -> instDef
		// the two are now linked on both ends
		inst->instDef = levInstDef;

		inst->vertSplit = 0;
		inst->specLightX = 1;
		inst->compressedNormalAndDriverIndex = 0;

		// converted to TEST in rebuildPS1
		ConvertRotToMatrix(&inst->matrix, &levInstDef->rot);

		// instance posX and posY
		CTR_COPY_VEC3(inst->matrix.t, CTR_VECTOR_DATA(&(levInstDef->pos)));

		inst->thread = NULL;
		idpp = INST_GETIDPP(inst);

		// loop through InstDrawPerPlayer
		for (j = 0; j < gGT->numPlyrCurrGame; j++)
		{
			idpp[j].mh = 0;
			idpp[j].pushBuffer = &gGT->pushBuffer[j];
		}

		modelID = levInstDef->model->id;

		// can be -1
		if ((s16)modelID > 0)
		{
			// Only continue if LEV instances are enabled,
			// they may be disabled due to podium scene on adv hub
			if ((gGT->gameMode2 & NO_LEV_INSTANCE) == 0)
			{
				meta = COLL_LevModelMeta(modelID);

				if (meta->LInB != NULL)
				{
					// call funcLevInstDefBirth, make thread for this instance
					meta->LInB(inst);
				}
			}
		}

		boolArcadeOnly = ((((u32)modelID - PU_FRUIT_CRATE) < 2) || (modelID == PU_WUMPA_FRUIT));

		boolRelicOnly = ((((u32)modelID - STATIC_TIME_CRATE_02) < 2) || (modelID == STATIC_TIME_CRATE_01));

		if (((gGT->gameMode1 & TIME_TRIAL) != 0) && (boolArcadeOnly || boolRelicOnly))
		{
			inst->flags &= ~DRAW_COLLISION_MASK;
		}
		else if ((gGT->gameMode1 & RELIC_RACE) != 0)
		{
			if (boolRelicOnly)
			{
				gGT->timeCratesInLEV++;
			}
			else if (boolArcadeOnly)
			{
				inst->flags &= ~DRAW_COLLISION_MASK;
			}
		}
		else if (boolRelicOnly)
		{
			inst->flags &= ~DRAW_COLLISION_MASK;
		}

		if ((gGT->gameMode1 & CRYSTAL_CHALLENGE) != 0)
		{
			if (modelID == STATIC_CRYSTAL)
			{
				gGT->numCrystalsInLEV++;
			}
			else if (modelID == PU_FRUIT_CRATE)
			{
				inst->flags &= ~DRAW_COLLISION_MASK;
			}
		}

		// If NOT crystal challenge
		else
		{
			// Disable LevInst for Crystal, TNT, Nitro
			if ((modelID == STATIC_CRYSTAL) || (modelID == STATIC_CRATE_TNT) || (modelID == PU_EXPLOSIVE_CRATE))
			{
				inst->flags &= ~DRAW_COLLISION_MASK;
			}
		}

		if (
		    // If not in Adventure Mode, or CTR Token Race
		    ((gGT->gameMode1 & ADVENTURE_MODE) == 0) || ((gGT->gameMode2 & TOKEN_RACE) == 0))
		{
			// disable C-T-R letters
			if ((u32)(modelID - STATIC_C) < 3)
			{
				inst->flags &= ~DRAW_COLLISION_MASK;
			}
		}

		// next InstDef
		levInstDef++;
	}
}


void INSTANCE_LevDelayedLInBs(struct InstDef *instDef, int numInstances)
{
	s32 i;

	for (i = 0; i < numInstances; i++)
	{
		struct MetaDataMODEL *meta = COLL_LevModelMeta(instDef->model->id);

		if ((meta != NULL) && (meta->LInB != NULL))
		{
			meta->LInB(instDef->ptrInstance);
		}

		instDef++;
	}
}


/// @brief Obtain number of actual animation data frames in the first lod entry of the passed model.
/// @param pInstance - pointer to Instance
/// @param animIndex - animation index to check
s32 INSTANCE_GetNumAnimFrames(struct Instance *pInstance, int animIndex)
{
	struct Model *pModel;
	struct ModelHeader *pHeader;
	struct ModelAnim *pAnim;

	// get model from instance and validate
	if (pModel = pInstance->model, pModel != NULL)
	{
		// if model got headers
		if (pModel->numHeaders > 0)
		{
			// get first header ptr and validate
			if (pHeader = pModel->headers, pHeader != NULL)
			{
				// if header got animations
				if (pHeader->ptrAnimations != NULL)
				{
					// validate anim index param
					if (animIndex < (int)pHeader->numAnimations)
					{
						// get proper animation ptr and validate
						if (pAnim = *(pHeader->ptrAnimations + animIndex), pAnim != NULL)
						{
							// we're finally there, get number of frames
							// remember it's masked due to interp flag
							return pAnim->numFrames & 0x7fff;
						}
					}
				}
			}
		}
	}

	// any other case
	return 0;
}
