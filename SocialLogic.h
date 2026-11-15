#pragma once
#include <g3sdk/Script.h>

// Existing social-conflict rules; arena and personal party brawls are separate.
namespace SocialLogic
{
    enum mEWitnessVerdict
    {
        mEWitnessVerdict_Ok = 0,
        mEWitnessVerdict_NotAWitness = 1,
        mEWitnessVerdict_TooFar = 2,
        mEWitnessVerdict_NoLineOfSight = 3,
    };
    GEBool IsUnprovokedAssault(Entity &victim, Entity &source);
    void OnCityAssault(Entity &victim, GEBool force = GEFalse, GEBool directWitnessAttack = GEFalse);
    void CapturePunishedConflict(Entity &winner);
    void ClearPunishedConflict();
    void OnCityKnockout(Entity &victim, GELPCChar where, Entity *source = nullptr);
    void ObserveNpc(Entity &npc);
    void ObserveTarget(Entity &npc, Entity &target, GEInt reason);
    bool IsAwaitingSecondAssault(Entity &npc, Entity &target);
    bool HoldFirstAssaultWarning(Entity &npc);
    class ImpactScope
    {
        Entity victim;
        GEBool outer = GEFalse;
        GEBool retaliating = GEFalse;
        GEBool notified = GEFalse;
        GEI32 hitPointsBefore = -1;
    public:
        ImpactScope(Entity &victim, Entity &source, GEBool notifyBeforeNative = GEFalse);
        ~ImpactScope();
        ImpactScope(ImpactScope const &) = delete;
        ImpactScope &operator=(ImpactScope const &) = delete;
    };
    GEBool ReleaseWatcherIfOver(Entity &npc);
    void ResetCityState(GELPCChar why);
    GEInt ResetBlowHistory();
    GEBool IsPeaceful(Entity &npc);
    GEBool IsCityBrawlVictimAlive();
    GEBool IsWatching(Entity &npc);
    GEBool IsAggro();
    GEBool IsSlaveSpectator(Entity &npc);
    GEBool ObserveSlaveWitness(Entity &npc, Entity &hero, Entity &victim);
    GEBool HandleSlaveWitnessTarget(Entity &npc, Entity &target, GEInt reason);

    // Social perception rules; shared hook wrappers retain their original order.
    GEBool IsWandererNpc(Entity &a_Npc);
    GEBool IsTransformedHeroTarget(Entity &a_Other);
    GEBool HasBeenHurtBy(Entity &a_Self, Entity &a_Other);
    GEBool IsSlaveTarget(Entity &a_Other);
    GEBool IsSlaveVsNoneBlocked(Entity &a_Self, Entity &a_Other);
    GEBool IsWandererImmune(Entity &a_Self, Entity &a_Other);
    void LogPoliticalFilter(GELPCChar a_strWhere, GELPCChar a_strRule, Entity &a_Self, Entity &a_Other);
    GEBool IsFormWeaponBlocked(Entity &a_Self, Entity &a_Other);
    void LogFormWeapon(GELPCChar a_strWhere, Entity &a_Self, Entity &a_Other);
    namespace Host
    {
        GEBool IsSentAgainstHero(Entity &npc);
        mEWitnessVerdict GetWitnessVerdict(Entity &npc, Entity &hero, Entity &victim, GELPCChar *reason = nullptr);
        void ForceAttack(Entity &npc, Entity &hero, gEAttackReason reason);
        void ArmSocialSpare(GELPCChar why);
        void ResetDeclareThrottle();
        void ResetSharedSessionState();
        GEBool IsHeroSide(Entity &npc);
        GEBool FindArenaOpponent(Entity &npc);
        GEBool FindRememberedOpponent(Entity &npc);
        GEBool HasRunningArenaQuest();
        GEBool IsHeroInsideArenaZone();
        GEBool HasTask(Entity &npc, GELPCChar task);
        GEBool HasReacted(Entity &npc, Entity &hero);
        GEBool IsAttackingHero(Entity &npc, Entity &hero);
        void ForceReaction(Entity &npc, Entity &hero);
        void NoteAggro(Entity &npc);
    }

    GEBool IgnorePerceivedTarget(Entity &self, Entity &other);
    GEBool AdjustPoliticalAttitude(Entity &self, Entity &other, GEInt nativeResult, GEInt &adjusted);
    GEBool AdjustPlayerPoliticalAttitude(Entity &self, GEInt &adjusted);

    GEInt AdjustRansack(Entity &npc, Entity &hero, GEInt nativeResult);

    void AlignWatcherAfterLoop(Entity &npc);
}






namespace SocialLogic::Hooks
{
    GEInt HostileMagic(gFScript original, GELPCChar name, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt AssessPlayerWeapon(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt AssessWarn(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt CanThreaten(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt StartSayAttackReason(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
}
