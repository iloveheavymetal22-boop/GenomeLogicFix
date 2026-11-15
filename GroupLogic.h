#pragma once
#include <g3sdk/Script.h>

class mCFunctionHook;

// Group movement owns its teleport state. Arena hooks keep their original registration order.
namespace GroupLogic
{
    enum WaiterType { WaiterType_None = 0, WaiterType_User = 1, WaiterType_Auto = 2 };
    GEBool IsHeroTransformedNow();
    GEBool IsHeroFormEntity(Entity &npc);
    GEBool IsHeroControlled(Entity &npc);
    void CollectTeleportFollowers();
    void FinishTeleportFollowers(GELPCChar why);
    void PollTeleportFollowers();
    GEBool IsTeleportFollower(Entity &npc);
    GEBool TryLateCollect(Entity &npc);
    void LogTeleportRecord(GELPCChar stage, Entity &npc, Entity &hero, GELPCChar decision, GELPCChar reason);
    void SetTeleportScriptRunning(GEBool running);
    GEBool IsTeleportScriptRunning();

    // Boundary to the arena/session and waiting rules, still owned by ArenaJudge.
    namespace Host
    {
        GEBool IsDuelTaboo(Entity &npc);
        GEBool FindArenaOpponent(Entity &npc);
        GEBool HasRunningArenaQuest();
        GEBool IsInsideArenaZone(Entity &npc);
        GELPCChar DamageTypeName(GEInt type);
        void MarkWaiterUserPublic(Entity &npc);
        void ClearWaiterMarksPublic(Entity &npc);
        GEBool WasArenaDuelActive();
        GEBool IsHeroSide(Entity &npc);
        Entity GetDamageOwnerNpc(Entity &source);
        GEBool IsPointMagicSource(Entity *source);
        void StartFleeWatch(Entity &npc);
        GEBool HasTask(Entity &npc, GELPCChar task);
        void PollArenaQuestEnd();

        GEBool IsArenaSessionHeld();
        void EndArenaSession(GELPCChar why, GEBool cancelled, GEInt release);
        void PrintCompanionWaitMessage(Entity &npc);
        WaiterType GetWaiterType(Entity &npc);
        GELPCChar WaiterTypeText(WaiterType type);
        void ClearWaiterMarks(Entity &npc);
        void MarkWaiterAuto(Entity &npc);
    }
}
// Group combat/commands interface for shared wrappers; private records stay in GroupLogic.cpp.
namespace GroupLogic::Combat
{
GEBool IsHeroSide(Entity &a_Npc);
void LogAiSnapshot(GELPCChar a_strWhen, Entity &a_Npc);
void StartFleeWatch(Entity &a_Victim);
void PollFleeWatch();
void LogArmyHit(GELPCChar a_strFunction, Entity &a_Victim, Entity &a_Source, GEInt a_iDamageType);
void NoteName(bCString *a_pNames, GEU32 *a_pTicks, bCString const &a_strName, GEU32 a_u32Span);
GEBool HasName(bCString const *a_pNames, GEU32 const *a_pTicks, bCString const &a_strName, GEU32 a_u32Span);
void ClearCombatRecords();
GEBool HasCombatRecord(Entity &a_Outsider);
void RecordCombat(Entity &a_Victim, Entity &a_Attacker);
GEBool IsOrderedAttackReason(GEInt a_iReason);
GEBool IsInRealFight(Entity &a_Self, Entity &a_Other);
GEBool ArmyMustNotAttack(Entity &a_Self, Entity &a_Other, GEInt a_iReason);
GEBool IsRedForHero(Entity &a_Npc);
Entity GetDamageOwnerNpc(Entity &a_Source);
GEBool FollowerMustSpare(Entity &a_Attacker, Entity &a_Victim, GELPCChar a_strWhere);
GEBool IsHeroAttackerInForm(Entity &a_Attacker);
GEBool IsPointMagicSource(Entity *a_pSource);
GEBool IsFriendlyFire(Entity &a_Attacker, Entity &a_Victim, GEBool a_bAreaDamage = GEFalse, Entity *a_pSource = nullptr);
GEBool IsHeroInBeastForm();
GEBool IsPrimaryAggressionOnSummon(Entity &a_Self, Entity &a_Other);
GEBool IsDefendedTarget(bCString const &a_strName);
void ClearDefendedTargets();
void RememberDefendedTarget(bCString const &a_strName);
void NoteFocusRecord(bCString const &a_strEnemy, bCString const &a_strVictim);
GEBool HasFocusRecord(bCString const &a_strEnemy, bCString const &a_strVictim);
void ClearFocusRecords();
void DefendAllies(Entity &a_Victim, Entity &a_Attacker);
GEBool IsTamedOrDominated(Entity &a_Npc);
GEBool GetDialogPartner(gCInfo_PS const *a_pInfo, Entity &o_Npc);
void OrderWait(Entity &a_Npc);
void OrderFollow(Entity &a_Npc);
GEBool FindHumanBodyFor(Entity &a_Attacker, Entity &a_Body);
void ResetFleeWatch();

    GEInt ResolveTarget(Entity &self, Entity &other, Entity &hero, gFScript original, gCScriptProcessingUnit *spu, GELPVoid selfArg, GELPVoid otherArg, GEInt args);
    GEInt AdjustAttitude(Entity &self, Entity &other, GEInt nativeResult);
    void ZeroPendingDamage(Entity &victim);

    GEBool IsMasterDownVictim(Entity &self, Entity &other);
    GEBool LeaveMasterAlone(gCScriptProcessingUnit *spu, GELPCChar where);
}

namespace GroupLogic::Waiting
{
    using InfoExecuteOriginal = GEBool(GE_STDCALL *)(gCInfo_PS *, gCInfoManager_PS *);
    WaiterType GetWaiterType(Entity &npc);
    GELPCChar WaiterTypeText(WaiterType type);
    void ClearWaiterMarks(Entity &npc);
    void MarkWaiterUser(Entity &npc);
    void MarkWaiterAuto(Entity &npc);
    void ResetWaiterMarks();
    void RemoveUserMark(bCString const &name);
    void RemoveAutoMark(bCString const &name);
    GEBool HasUserMark(bCString const &name);
    GEBool HasAutoMark(bCString const &name);
    GEInt MagicTeleport(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt WaitPlayerParty(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEBool GE_STDCALL InfoExecute(gCInfo_PS *info, gCInfoManager_PS *manager, InfoExecuteOriginal original);

    using InfoConditionsOriginal = GEBool(GE_STDCALL *)(gCInfo_PS *);
    using InfoDescriptionOriginal = bCUnicodeString *(GE_STDCALL *)(gCInfo_PS *, bCUnicodeString *);
    using EntityHasInfoOriginal = GEBool(GE_STDCALL *)(Entity *, Entity const *);
    GEBool GE_STDCALL InfoConditions(gCInfo_PS *info, InfoConditionsOriginal original);
    bCUnicodeString *GE_STDCALL InfoDescription(gCInfo_PS *info, bCUnicodeString *result, InfoDescriptionOriginal original);
    GEBool GE_STDCALL EntityHasInfo(Entity *first, Entity const *second, EntityHasInfoOriginal original);
}
namespace GroupLogic::Host
{
    GEBool HasArenaWaiterName(bCString const &name);
    void RemoveArenaWaiterName(bCString const &name);
}






namespace GroupLogic::Lifecycle
{
    using AIStateOriginal = GEBool(GE_STDCALL *)(bTObjStack<gScriptRunTimeSingleState> &, gCScriptProcessingUnit *);
    GEInt AssessTalk(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt GiveXP(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt JoinPlayerParty(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEBool ZS_RansackBody(AIStateOriginal original, bTObjStack<gScriptRunTimeSingleState> &stack, gCScriptProcessingUnit *spu);
    GEBool ZS_Attack_FinishingAttack(AIStateOriginal original, bTObjStack<gScriptRunTimeSingleState> &stack, gCScriptProcessingUnit *spu);
    GEInt SummonCast(gFScript original, GELPCChar name, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt HealCast(gFScript original, GELPCChar name, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);

    void ResetHeroLossSummons();
    void CaptureHeroLossSummons(bool keepPrevious = false);
    void ReviewSummonTargets();
    extern mCFunctionHook Hook_SecondarySummonType;
    void GE_STDCALL SecondarySummonType(gCParty_PS *party, bTPropertyContainer<gEPartyMemberType> const &type);
    void UpdateSecondarySummons();
    void ResetSecondarySummons();
    void AdjustSummonPartyMembers(PSParty *party, bTValArray<eCEntity *> &members);
    void DismissSummonsOnHeroLoss(GEBool confirmedDefeat = GEFalse);
    void OnHeroDefeated(Entity &defeated);
}



namespace GroupLogic::Combat
{
    void LogAttackerSet(GELPCChar where, PSNpc *npc, Entity const *attacker);
    void OnArmyAttacked(PSNpc *npc, Entity const *attacker);
}
namespace GroupLogic::Lifecycle
{
    GEBool ZS_Stumble(AIStateOriginal original, bTObjStack<gScriptRunTimeSingleState> &stack, gCScriptProcessingUnit *spu);
    using PartyGetLeaderOriginal = Entity *(GE_STDCALL *)(PSParty *, Entity *);
    Entity * PartyGetLeader(PSParty *a_pThis, Entity *a_pRet, PartyGetLeaderOriginal original);
}

namespace GroupLogic::Combat
{
    GEBool IsCreatedByHero(Entity &victim, Entity &source);
    GEBool IsElementalDamage(gEDamageType type);
    gEDamageType GetHitDamageType(Entity &source, Entity &victim);
    gEDamageType GetSourceElementalType(Entity &source);
}
