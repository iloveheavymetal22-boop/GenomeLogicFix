#pragma once
#include "GroupLogic.h"
#include <g3sdk/util/Hook.h>

// Internal bridge for the unchanged shared hook wrappers. No brawl arrays are public.
namespace GroupLogic::Brawl
{
struct DialogueMemberQuery {  DialogueMemberQuery(); ~DialogueMemberQuery(); };
struct PersonalRewardScope { GEInt depth; bCPropertyID victim; PersonalRewardScope(Entity &target, GEBool enabled); ~PersonalRewardScope(); };
struct PersonalKnockoutScope { GEInt depth; bCPropertyID victim; PersonalKnockoutScope(Entity &npc, GEBool enabled); ~PersonalKnockoutScope(); };
Entity BrawlMemoryOwner();
bCString BrawlEventKey(Entity &npc, GELPCChar kind, GEBool withEpoch = GEFalse);
void LoadBrawlEpoch();
GEBool BrawlEvent(Entity &npc, GELPCChar kind, GEBool withEpoch = GEFalse);
void SetBrawlEvent(Entity &npc, GELPCChar kind, GEBool enabled, GEBool withEpoch = GEFalse);
GEBool BrawlFullHealth(Entity &npc);
GEInt FindBrawlSlot(Entity &npc, GEBool create);
void ClearBrawls();
// THE UNCONDITIONAL RESET OF THE COMBAT STATE (knock-out of the master, time-out, the end of the fight). Nothing of the
        // fight is left on the creature, so it cannot hang in an unnatural pose: no target (the combat focus), no attacker,
        // no alternative target, no attack reason, no combat state; the routine is stopped and the creature is put back to
        // the peaceful task of following the hero (ContinueRoutine picks the native one, ZS_FollowPlayer is set by hand when
        // the routine has not left the combat / stumble task). A waiting creature keeps waiting.
        void ResetCombatState(Entity &a_Npc);
GEBool IsCompanionDownOrRising(Entity &npc);
GEBool IsRecoveringPersonalCompanion(Entity &npc);
void FilterDialogueMembers(bTValArray<eCEntity *> &members);
void UpdatePersonalRecovery(Entity &npc);
// The fight of this creature with the hero is over: the slot is closed and the combat state is reset.
        void EndBrawlOf(Entity &npc, GELPCChar why);
GEBool IsBrawlFighting(Entity &a_Npc);
GEBool CanReadBrawlDiagnostics();
void ResetBrawlDiagnostics();
void LogAimEntity(GELPCChar label, Entity &npc);
void LogMagicAim(GELPCChar where);
void ObserveRecovery(Entity &npc, GELPCChar where, GEBool begin = GEFalse, GEBool force = GEFalse);
void LogDialogueRecovery(GELPCChar where, eCEntity *first, eCEntity *second);
GEBool GE_STDCALL DiagnosticStartDialogue(gCInfoManager_PS *manager, eCEntity *first, eCEntity *second);
void GE_STDCALL DiagnosticProjectileShoot(PSProjectile *projectile);
void ReconcileBrawl(Entity &npc, GELPCChar why);
GEBool HeroHasDrawnWeapon();
void PollPersonalSheathe();
void BeginPersonalTeleport();
void PollPersonalTeleport();
void RestoreSavedBrawlState(Entity &npc);
GEInt FindPersonalSpectator(Entity &npc);
void ClearPersonalSpectators();
GEBool OwnsPersonalSpectatorTask(Entity &npc);
GEBool IsPersonalPickupTask(Entity &npc);
void CancelPersonalSpectator(Entity &npc, GEBool restore);
GEBool CanStartPersonalSpectator(Entity &npc, Entity &opponent);
void StartPersonalSpectators(Entity &opponent);
void FinishPersonalSpectators(Entity &opponent);
void PollPersonalSpectators();
GEBool IsPersonalBrawlPair(Entity &first, Entity &second);
GEBool CanDamageBrawlOpponent(Entity &attacker, Entity &victim);
GEBool IsFatalPersonalProjectile(Entity &source, Entity &victim);
GEBool PlayerSupportsPersonalKnockout(Entity &player);
void GE_STDCALL BrawlSetHitPoints(gCDamageReceiver_PS *self, GEI32 const *value);
GEBool ShouldSuppressBrawlXP(Entity &defeated);
// The SAFETY NET against a creature stuck in ZS_Stumble after a blow of the hero (it is called by PollFleeWatch about
        // one second after the blow). A creature that has accepted the fight goes on attacking the hero, any other one
        // goes back to the routine.
        void ReleaseStumbledNpc(Entity &a_Npc);
// THE MASTER IS KNOCKED OUT: a creature of his party that fights him (or still keeps him as a target) stops at once
        // and goes back to the routine of following. Nobody of the party finishes, robs or hits the fallen master.
        // Returns true when the creature was released.
        GEBool StandDownFromMaster(Entity &a_Npc, GELPCChar a_strWhere);
// A confirmed direct magic-projectile impact on a party member (from AssessHit).
        void OnHeroBlowOnArmy(Entity &a_Victim, bCPropertyID const &a_ProjectileID);
void PollBrawl();
// Called by the blow watch (PollFleeWatch) after the stumble release; a_iStage 0 is +300 ms, 1 is +1000 ms,
        // 2 is +2000 ms, 3 is +4000 ms after the last blow of the hero.
        void BrawlStageAction(Entity &a_Npc, GEInt a_iStage);
GEBool GE_STDCALL RunPersonalBrawlExit(bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU);
GEBool GE_STDCALL RunPersonalBrawlSpectator(bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU);
GEBool HandlePersonalAggressor(Entity &Self, Entity &Other);
void InitializeSessionAccess();
bTValArray<eCEntity *> *GE_STDCALL DialoguePartyMembers(PSParty *party, bTValArray<eCEntity *> *result, GEBool includeSelf);
eCEntity *GE_STDCALL PersonalSlotDrop(gCInventory_PS *inventory, gESlot slot, GEInt amount, bCMatrix const *matrix);
extern mCFunctionHook Hook_DialoguePartyMembers;
extern mCFunctionHook Hook_DiagnosticStartDialogue;
extern mCFunctionHook Hook_DiagnosticProjectileShoot;
extern mCFunctionHook Hook_BrawlSetHitPoints;
extern mCFunctionHook Hook_PersonalSlotDrop;
}
