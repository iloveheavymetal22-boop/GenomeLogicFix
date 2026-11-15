#pragma once
#include <g3sdk/Script.h>

// Arena policy inside the existing DLL. Hook installation remains centralized.
namespace ArenaJudge::Punishment
{
    GEBool IsArenaPunishmentActor(Entity &npc);
    GEBool IsArenaPunishmentBlow(Entity &victim, Entity &attacker);
    GEBool IgnoreArenaPunishmentTarget(Entity &observer, Entity &actor);
    void LogArenaPunishmentFilter(Entity &observer, Entity &actor, GELPCChar where);
    void ReleaseStage2Hold(GELPCChar why);
    void PollStage2Hold();
    GEBool FinishIfHeroDead();
    void OnArenaNpcTick(Entity &npc);
}

namespace ArenaJudge::Combat
{
    struct DamageContext
    {
        Entity Opponent;
        GEBool bArenaVictim, bWasFrozen, bWasBurning, bDuelBlocked;
        GELPCChar strReason;
        GEBool bRangedHit;
    };
    struct DefeatContext
    {
        GEBool bThirdParty = GEFalse, bForcedLoss = GEFalse;
        Entity Opponent;
    };
    void OnHeroConsumeStarted();
    DamageContext BeforeDamage(Entity &victim, Entity &source);
    void AfterDamage(DamageContext &state, Entity &victim);
    DefeatContext BeforeDefeat(Entity &defeated, Entity &hero);
    void AfterDefeat(DefeatContext &state, Entity &defeated, Entity &hero);
    GEBool IsDuelTaboo(Entity &npc);
    GEBool IsWarnedOpponent(Entity &npc);
    GEBool PreemptMagicWarning(Entity &npc);
    GEInt FinishHeroDecision(Entity &npc, GEInt nativeResult);
    void OnHeroSummonCast(GELPCChar name, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other);
    void OnHeroHealCast(GELPCChar name, gCScriptProcessingUnit *spu, GELPVoid self);
}

namespace ArenaJudge::Loops
{
    GEBool FixStaleHeroTarget(Entity &npc);
    GEBool ReleaseInvalidPunishmentTarget(gCScriptProcessingUnit *spu);
    GEBool ReleaseProtectedPartyTarget(gCScriptProcessingUnit *spu);
    void PollCompanions();
    GEBool HoldWarning(Entity &npc);
}

namespace ArenaJudge::ConflictHooks
{
    GEBool IsFormTransitionPending();
    void ResetBeforeMercy();
    void ResetDuelLoss();
    void ClearWitnesses();
    void EndDuelAfterKnockout();
}

namespace ArenaJudge::Loops
{
    void OnHeroTeleported();
}

namespace ArenaJudge::Native
{
    void OnHeroAttackerSet(PSNpc *npc, Entity const *attacker, GELPCChar where);
    using NpcEnableStatusEffectsOriginal = void(GE_STDCALL *)(PSNpc *, GEInt, GEBool);
    void NpcEnableStatusEffects(PSNpc *a_pThis, GEInt a_iMask, GEBool a_bEnable, NpcEnableStatusEffectsOriginal original);
    using NpcSetLastCastOriginal = void(GE_STDCALL *)(PSNpc *);
    void NpcSetLastCast(PSNpc *a_pThis, NpcSetLastCastOriginal original);
    using GuiPrintGameMessageOriginal = void(GE_STDCALL *)(GUI2 *, bCUnicodeString const *, gEGameMessageType);
    void GuiPrintGameMessage(GUI2 *a_pThis, bCUnicodeString const *a_pText, gEGameMessageType a_enuType, GuiPrintGameMessageOriginal original);
    using EntitySetTimeScaleOriginal = void(GE_STDCALL *)(Entity *, GEFloat);
    void EntitySetTimeScale(Entity *a_pThis, GEFloat a_fScale, EntitySetTimeScaleOriginal original);
    using RoutineSetTimeScaleOriginal = void(GE_STDCALL *)(PSRoutine *, GEFloat);
    void RoutineSetTimeScale(PSRoutine *a_pThis, GEFloat a_fScale, RoutineSetTimeScaleOriginal original);
    using NpcCanHitOriginal = GEBool(GE_STDCALL *)(PSNpc *, GEU32);
    GEBool NpcCanHit(PSNpc *a_pThis, GEU32 a_u32MinDuration, NpcCanHitOriginal original);
    using QuestSetStatusOriginal = GEBool(GE_STDCALL *)(gCQuest_PS *, gEQuestStatus const *);
    GEBool QuestSetStatus(gCQuest_PS *a_pThis, gEQuestStatus const *a_penuStatus, QuestSetStatusOriginal original, GELPVoid caller);
}
