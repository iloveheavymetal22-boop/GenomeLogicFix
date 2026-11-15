#include "ArenaJudge.h"
#include "SocialLogic.h"
#include "PluginRuntime.h"
#include "GroupLogic.h"
#include "GroupBrawl.h"
#include "DebugLog.h"
#include "RequirementsCheck.h"
#include "FreezeFix.h"
#include "CrashDiag.h"

#include <g3sdk/Script.h>
#include <g3sdk/util/Hook.h>
#include <g3sdk/util/Logging.h>
#include <g3sdk/util/ScriptInterface.h>
#include <g3sdk/util/ScriptUtil.h>
#include <g3sdk/util/Util.h>

#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

//
// Genome Logic Fix: arena policy and centralized registration adapters.
//
namespace ArenaJudge::Punishment
{
    namespace
    {
        bCString g_strStage2Hold;
        GEU32 g_u32Stage2HoldTick = 0;
        bCPropertyID g_arrArenaWitnessID[96];
        GEInt g_iArenaWitnessCount = 0;
    }
    GEBool IsCurrentArenaWitness(Entity &npc);
    void NoteCurrentArenaWitness(Entity &npc);
}

namespace ArenaJudge::Implementation
{
    using PluginRuntime::GetConfig;
    using namespace PluginRuntime::HeroState;
    using namespace PluginRuntime::Conflict;
    using namespace ArenaJudge::Punishment;
    using PluginRuntime::Hook_SessionStart;
    using PluginRuntime::Hook_SessionStop;
    using PluginRuntime::SessionStart;
    using PluginRuntime::SessionStop;
    using PluginRuntime::ResetBlowHistory;
    using namespace GroupLogic::Brawl;
    using namespace GroupLogic::Combat;
    using SocialLogic::IsUnprovokedAssault;
    using SocialLogic::IsWandererNpc;
    using SocialLogic::IsTransformedHeroTarget;
    using SocialLogic::HasBeenHurtBy;
    using SocialLogic::IsSlaveTarget;
    using SocialLogic::IsSlaveVsNoneBlocked;
    using SocialLogic::IsWandererImmune;
    using SocialLogic::LogPoliticalFilter;
    using SocialLogic::IsFormWeaponBlocked;
    using SocialLogic::LogFormWeapon;

    using SocialLogic::OnCityAssault;
    using PluginRuntime::OnCityKnockout;
    using SocialLogic::ReleaseWatcherIfOver;
    using SocialLogic::ResetCityState;
    using SocialLogic::IsPeaceful;
    using SocialLogic::IsCityBrawlVictimAlive;

    using GroupLogic::IsHeroFormEntity;
    using GroupLogic::IsHeroControlled;
    using GroupLogic::IsHeroTransformedNow;
    //
    // Arena state. The duel is "IsArenaFight" when an NPC with AttackReason Arena targets the hero. The fight has
    // PHYSICALLY started when that opponent is in combat / arena AI mode and attacks the hero, or the hero stands in
    // the running arena zone (gCArena_PS.Status == Running).
    //
    namespace
    {
        // The duel opponent seen while the fight was on (by name). When the hero is knocked out, the attack reason of the
        // opponent can already have changed (the log of the game: Defeat found nobody), so he is looked up by this name.
        bCString g_strArenaOpponent;
        GEU32 g_u32TabooUntil = 0;            // the taboo of the duel opponent lasts until this tick after the quest
        GEU32 const TABOO_AFTER_MS = 30000;   // 30 seconds after the end of the arena / duel quest
        GEBool FindArenaOpponent(Entity &o_Opponent)
        {
            Entity Player = Entity::GetPlayer();
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc.IsDead())
                    continue;
                // The duel opponent: attack reason Arena (0x12) or Duel (0x11), the same pair the native Defeat checks.
                gEAttackReason const enuReason = Npc.NPC.AttackReason;
                if (enuReason != gEAttackReason_Arena && enuReason != gEAttackReason_Duel)
                    continue;
                // He fights the hero: the hero is his target, attacker or alternative target (the arena stores the
                // fighters in the alternative target), or the hero is simply the nearest fighter.
                if (Npc.NPC.GetCurrentTarget() == Player || Npc.NPC.GetCurrentAttacker() == Player ||
                    Npc.NPC.GetAlternativeTarget() == Player || Npc.GetDistanceTo(Player) < 3000.0f)
                {
                    o_Opponent = Npc;
                    g_strArenaOpponent = Npc.GetName();
                    return GETrue;
                }
            }
            return GEFalse;
        }

        // The remembered opponent, alive, when the attack-reason search above finds nobody any more.
        GEBool FindRememberedOpponent(Entity &o_Opponent)
        {
            if (g_strArenaOpponent.IsEmpty())
                return GEFalse;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (!Npc.IsDead() && Npc.GetName() == g_strArenaOpponent)
                {
                    o_Opponent = Npc;
                    return GETrue;
                }
            }
            return GEFalse;
        }

        // The fight has started physically: the opponent is in the fighting AI mode (combat / arena), or the arena the
        // hero stands in is Running.
        GEBool IsArenaFightStarted()
        {
            Entity Opponent;
            if (!FindArenaOpponent(Opponent))
                return GEFalse;

            gEAIMode const enuMode = Opponent.Routine.AIMode;
            if (enuMode == gEAIMode_Combat || enuMode == gEAIMode_Arena)
                return GETrue;

            Entity Zone = Entity::GetPlayer().GetZone();
            return Zone != None && Zone.Arena.IsValid() && Zone.Arena.Status == gEArenaStatus_Running;
        }

        // GEOMETRY: the entity physically stands inside the zone of the arena (the zone entity he is in carries the arena
        // property set). The duel quest is Running from the moment the duel is accepted, long before the hero enters the
        // ring, so the quest alone says nothing about where a cast happens.
        GEBool IsInsideArenaZone(Entity &a_Entity)
        {
            if (a_Entity == None)
                return GEFalse;
            Entity Zone = a_Entity.GetZone();
            return Zone != None && Zone.Arena.IsValid() ? GETrue : GEFalse;
        }

        GEBool IsHeroInsideArenaZone()
        {
            Entity Hero = Entity::GetPlayer();
            return IsInsideArenaZone(Hero);
        }

        // Diagnostics: a cast / hit that is ignored because the hero is outside the ring.
        void LogOutsideRing(GELPCChar a_strWhat)
        {
            static GEInt s_iLines = 0;
            if (s_iLines >= 40)
                return;
            ++s_iLines;
            Entity Zone = Entity::GetPlayer().GetZone();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] arena: %s ignored - the hero is outside the zone of the "
                                         "arena (zone '%s').\n",
                                         a_strWhat, Zone != None ? Zone.GetName().GetText() : "-");
        }
    } // namespace

    // Mercy for the knocked-out hero (anti-murder). No timers and no "episodes": only events and the geometry of the
    // world.
    //  ARMED by one of two events: (1) a violation of the arena rules (a consumable in a started fight, forbidden fire /
    //  ice / lightning, burning / freezing started on the opponent); (2) the second blow in a city (stage 2, total aggro).
    //  APPLIED when the hero lies knocked out: the place where he fell is remembered, and NOBODY within SPARE_RADIUS of
    //  that place may kill him - HasAttackReasonToKill answers "no" (hook below), so the native ZS_Attack_End goes on with
    //  WantToRansack / ZS_RansackBody: the NPC takes the weapon and the gold of the hero and walks away. The native
    //  exceptions of WantToRansack stay as they are.
    //  The state is dropped when the protected knock-out is over (the hero is on his feet again) and when the game state
    //  is reset (save game loaded, new game, session stop).
    //
    namespace
    {

        GEBool g_bOverLineSaid = GEFalse; // v1.0.60.0: the opponent has already spoken over the fallen hero (once per mercy)

        // The duel was already ended for the current knock-out of the hero (once per knock-out, whoever knocked him out).
        GEBool g_bDuelLossDone = GEFalse;
        void ResetDuelState(); // the companions waiting for the duel and the "lost to a third party" mark; defined below

        GEBool IsEnchantedMeleeEffect();           // v1.0.52.0: the status comes from an enchanted melee weapon; below
        void PunishRangedShot(GELPCChar a_strWhy); // v1.0.52.0: stage 2 for the bow / crossbow; below
        // v1.0.58.0: the crime + the duel quest is closed; below. v1.0.60.0: the opponent shouts a_strSvm (type a_u32Type) as he goes.
        void ArenaStageTwo(GELPCChar a_strWhy, GELPCChar a_strCrime, GELPCChar a_strSvm = "YouAskedForIt", GEU32 a_u32Type = 3);
        void CancelDuelForForbiddenWeapon(GELPCChar a_strWhy);         // v1.0.58.0: the timer is out: the duel is cancelled; below
        void PollWarnTimer();                                          // v1.0.58.0: the timer of the warning; below
        GEBool FindDuelOpponentAnywhere(Entity &o_Opponent);           // v1.0.60.0: the opponent also before the ring; below

        void BeginFormAggro(Entity &a_Opponent, GEBool a_bOpponent);   // v1.0.67.0: the crowd + the opponent go for the form; below
        void PollFormAggro();                                          // v1.0.67.0: the orders when the form body appears; below
        GEBool IsHeroHoldingMagic();                                   // a rune / a scroll / a bow / a crossbow is in the hand; below
        void ResetMagicJudge();     // v1.0.40.0: the state tracker of the magic in the arena; defined below
        void EndMagicThreat(GEBool a_bApprove = GEFalse); // v1.0.40.0: the hero put the magic away; defined below
                                                          // (v1.0.59.0: a_bApprove - the opponent approves aloud; v1.0.61.0: the line is "Good")
        GEBool IsWarnedOpponent(Entity &a_Npc);   // v1.0.41.0: the opponent in the "magic warning" pose; defined below
        GEBool HoldOpponentInWarning(Entity &a_Npc); // v1.0.41.0: keeps him in ZS_ReactToDamage; defined below
        GEBool PreemptMagicWarning(Entity &a_Npc);   // v1.0.47.0: the warning BEFORE the first native order; defined below
        void ReleaseWaitingCompanions(GELPCChar a_strWhy); // the squad follows the hero again; defined below
        GEBool IsArenaReleaseAllowed();                    // v1.0.43.0: the hero is on his feet; defined below
        // v1.0.68.0: a companion farther than this from the hero (another city) never runs across the map: he is not released into
        // ZS_FollowPlayer by the arena code, and after a teleport he is carried (AUTO) or stopped (not USER), never left to walk.
        GEFloat const FAR_COMPANION_DISTANCE = 10000.0f;
        // v1.0.68.0: the arena session of the plugin (see WaiterType below). g_bArenaSessionHold is NOT the quest status of the journal.
        GEBool IsArenaSessionGate();                       // the session was cancelled and no new one began: the ring checks of the quest sleep
        // a_bCancelled: the duel was cancelled / lost (the status Running that the game restores in the ring is then ignored).
        // a_iRelease: 0 - the AUTO waiters of the session stay AUTO (a teleport carries them), 1 - they follow the hero again as soon as
        // he may be released (IsArenaReleaseAllowed), 2 - they follow him again NOW.
        void EndArenaSession(GELPCChar a_strWhy, GEBool a_bCancelled, GEInt a_iRelease);
        void PumpArenaRelease();                           // the deferred release of the AUTO waiters of an ended session
        void PollSessionAdopt();                           // v1.1.1.0: the waiters of a duel that was running in the loaded save game; below
        GEBool IsSessionAutoWaiter(Entity &a_Npc);         // the companion waits by an AUTO order of the CURRENT arena session
        GEBool IsUserWaiterOrUnmarked(Entity &a_Npc);      // the wait is the player's (a mark USER, or no mark at all: an old wait)
        void MarkWaiterAutoPublic(Entity &a_Npc);          // the plugin / the game ordered this wait (see WaiterType below)
        void MarkWaiterUserPublic(Entity &a_Npc);          // the player ordered this wait in the dialogue
        void ClearWaiterMarksPublic(Entity &a_Npc);        // the companion follows the hero again: no mark
        using GroupLogic::IsHeroTransformedNow;                     // v1.0.48.0: the hero is in a beast body; defined below
        GEBool IsHeroDuelBlocked();                        // v1.0.48.0: the safety lock of the duel hooks; defined below
        GEBool IsTransformSpellInHand();                   // v1.0.48.0: the spell of the hero is a transformation; below
        void ResetDuelForTransform(GELPCChar a_strWhy);    // v1.0.48.0: the reset of the duel by a transformation; below
        // The opponent of the duel was knocked out by somebody who is NOT the hero (a witness, a summon, a stray blow): the
        // duel is lost, whatever the quest wants to do (see the Defeat hook and the SetStatus hook).
        GEBool g_bLostToThirdParty = GEFalse;
        GEBool g_bThirdPartyMessage = GEFalse;
        void AnnounceLossToThirdParty(); // the message "the fight is lost", once per duel; defined below
        void EndArenaDuelAfterKnockout();                       // defined below, with the arena helpers


        //
        // v1.0.60.0: THE VOICES OF THE ENDS OF THE ARENA VIOLATIONS (native SVM lines of Script_Game.dll 1.8.7):
        //  - "YouLeftTheArena" (type 7): the line of the native end of the arena ("Ты покинул арену - ты побежден!");
        //  - "YouAskedForIt" (type 3): the line of the native StartSayAttackReason for the reasons 9 / 10 ("Ты сам напросился!").
        // A fallen (IsDown) or dead body never speaks. The native plaque "GO_ArenaWarning" that goes with YouLeftTheArena (the text of the
        // escape from the arena, the message type Failure) is blocked for a while after the violation (hook of GUI2::PrintGameMessage).
        //
        GEBool HasTask(Entity &a_Npc, GELPCChar a_strTask); // defined below
        GEU32 g_u32PlaqueBlockUntil = 0;
        // v1.0.61.0: 5 s (was 15 s): enough to swallow the native plaque of the escape from the arena, the interface is not held for long.
        GEU32 const PLAQUE_BLOCK_MS = 5000;
        // v1.0.61.0: THE LINE OF APPROVAL when the hero puts the forbidden weapon away. "WiseMove" is voiced as an aggressive "come here" in
        // the Russian speech of the game; "WeaponAway" does not exist in the game (no such string in Script_Game.dll, no such file in
        // any of the 52 voice sets). "Good" ("Good!" / "Хорошо!") exists in every voice set and in SVMAdmin.dat.
        GELPCChar const ARENA_APPROVE_SVM = "Good";
        // v1.0.61.0: the opponent that is held in the fight against the hero after stage 2 (by name, empty: nobody), see PollStage2Hold.
        GEBool g_bOverLinePending = GEFalse; // the hero fell after a violation: the opponent says his line when the hero lies
        bCString g_strOverOpponent;
        GEU32 g_u32OverLineTick = 0;

        void BlockArenaPlaque()
        {
            g_u32PlaqueBlockUntil = (::GetTickCount() + PLAQUE_BLOCK_MS) | 1;
        }

        GEBool SayArenaLine(Entity &a_Speaker, Entity &a_Target, GELPCChar a_strSvm, GEU32 a_u32Type, GELPCChar a_strWhy)
        {
            // A body that lies (the flag, the Down mode or a native knock-out task) never speaks.
            if (a_Speaker == None || !a_Speaker.IsNPC() || a_Speaker.IsDead() || a_Speaker.IsDown() ||
                a_Speaker.Routine.AIMode == gEAIMode_Down || HasTask(a_Speaker, "ZS_Unconscious") ||
                HasTask(a_Speaker, "ZS_LieKnockDown") || HasTask(a_Speaker, "ZS_PiercedKO"))
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena voice '%s' (%s): the speaker is gone or lies unconscious - silent.\n",
                                             a_strSvm, a_strWhy);
                return GEFalse;
            }
            ::StartSaySVM(a_Speaker, a_Target, bCString(a_strSvm), a_u32Type);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena voice: '%s' says '%s' (%s).\n", a_Speaker.GetName().GetText(),
                                         a_strSvm, a_strWhy);
            return GETrue;
        }

        // The hero has fallen after a violation of the arena: the opponent will speak over him as soon as the hero lies on the sand.
        void ScheduleOverLine(Entity &a_Opponent)
        {
            if (g_bOverLineSaid || a_Opponent == None)
                return;
            g_bOverLinePending = GETrue;
            g_strOverOpponent = a_Opponent.GetName();
            g_u32OverLineTick = ::GetTickCount() | 1;
            BlockArenaPlaque(); // no text on the screen with the voice
        }

        // Polled from the tick of every NPC (one flag read when nothing is pending).
        void PollOverLine()
        {
            if (!g_bOverLinePending)
                return;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
            {
                g_bOverLinePending = GEFalse;
                return;
            }
            if (IsHeroKnockedOut())
            {
                g_bOverLinePending = GEFalse;
                g_bOverLineSaid = GETrue;
                Entity Opponent = g_strOverOpponent.IsEmpty() ? Entity() : Entity::GetEntity(g_strOverOpponent);
                SayArenaLine(Opponent, Hero, "YouLeftTheArena", 7, "over the fallen hero");
                return;
            }
            // The knock-out never showed (the technical net, the same as the fall grace of the mercy).
            if (static_cast<GEInt>(::GetTickCount() - g_u32OverLineTick) >= 8000)
                g_bOverLinePending = GEFalse;
        }

        //
        // v1.0.57.0: THE IRON IMMUNITY OF THE HERO. From the moment the hero falls until he has completely risen nobody can kill
        // him, whatever the mercy says (the mercy is a rule of the plugin about the crowd; this is a rule about the STATE of the
        // hero). "Down or rising" is read from the hero himself: the flag IsDown / the Down AI mode (IsHeroKnockedOut), or one of
        // the native knock-down tasks / states (ZS_Unconscious, ZS_Unconscious_Loop / _End, ZS_LieKnockDown, ZS_PiercedKO,
        // ZS_SitKnockDown*, ZS_StandUp, ZS_Fall). The immunity is lifted only when the hero is NOT down / rising any more AND his
        // routine is the ordinary one on his feet (the state PS_Normal_Loop / PS_Melee_Loop, also PS_Ranged_Walk_Loop and
        // PS_Magic_Loop when he stood up with a bow / a rune in the hand). No timer is involved except the technical net below.
        //
        // g_bSpareSawEnemy: after arming, somebody within SPARE_RADIUS really fought the hero (target = hero, a reason to
        // attack). The mercy ends by an EVENT: the hero is on his feet and nobody around him fights him any more.
        // IsArenaPunishment(): armed by an arena violation (ended by the arena code if nobody ever reacted).

        // The NPCs the plugin sent against the hero (by name). When the hero stands up after a protected knock-out, each
        // of them calms down as soon as he runs his own attack loop (also those who were too far to see the fall and
        // arrive later): no timer, the event "the hero is on his feet" releases them.

        // Witnesses of the CURRENT arena violation only, not the history of aggro.




        //
        // v1.0.67.0: THE CROWD AND THE OPPONENT ARE AIMED AT THE HERO'S FORM. A transformation in the ring removes the body PC_Hero, so
        // everybody who was sent against the hero loses the target; the engine drops their attack and, because the built-in weapon of
        // the form is filtered (IsFormWeaponBlocked), they do not come back. Once the transformation spell is cast in the ring the
        // plugin waits for the form body to APPEAR (an event seen by the tick of each NPC, no timer loop) and gives every NPC of the
        // crowd (the list of the aggro) and the opponent ONE order to attack the current body of the hero - the order they got for
        // the human body. An NPC that is idle again may be ordered again (at most FORM_ORDERS_MAX times, FORM_REORDER_MS apart).
        // The pursuit ends when the form is gone (the hero is a man again), the body is dead, the mercy is over, or the form does
        // not appear for FORM_WAIT_MS (the cast was interrupted).
        //
        GEBool g_bFormAggroPending = GEFalse;
        GEBool g_bFormSeen = GEFalse;
        GEU32 g_u32FormPendingSince = 0;
        GEU32 g_u32FormPollTick = 0;
        bCString g_strFormOpponent; // the duel opponent that stays in the fight against the form (by name, empty: nobody)
        bCString g_arrFormOrderName[AGGRO_MAX];
        GEInt g_arrFormOrderCount[AGGRO_MAX];
        GEU32 g_arrFormOrderTick[AGGRO_MAX];
        GEInt g_iFormOrderNames = 0;
        // Technical limits (not game timings): how long the form may take to appear, how often the crowd is looked at, how often and
        // how many times one NPC may be ordered again.
        GEU32 const FORM_WAIT_MS = 10000;
        GEU32 const FORM_POLL_MS = 400;
        GEU32 const FORM_REORDER_MS = 2500;
        GEInt const FORM_ORDERS_MAX = 3;

        void ResetFormAggro()
        {
            g_bFormAggroPending = GEFalse;
            g_bFormSeen = GEFalse;
            g_u32FormPendingSince = 0;
            g_u32FormPollTick = 0;
            g_strFormOpponent = bCString();
            for (GEInt i = 0; i < AGGRO_MAX; ++i)
            {
                g_arrFormOrderName[i] = bCString();
                g_arrFormOrderCount[i] = 0;
                g_arrFormOrderTick[i] = 0;
            }
            g_iFormOrderNames = 0;
        }


        // The NPC was sent against the hero by the plugin (the crowd of a crime, or the duel opponent that stays in the fight against the
        // form of the hero). The native filters of the built-in weapon of a form must not calm such an NPC down.
        GEBool IsSentAgainstHero(Entity &a_Npc)
        {
            if (!IsArmed() || a_Npc == None)
                return GEFalse;
            bCString const strName = a_Npc.GetName();
            if (FindAggro(strName) >= 0)
                return GETrue;
            return !g_strFormOpponent.IsEmpty() && g_strFormOpponent == strName ? GETrue : GEFalse;
        }





    } // namespace
    //
    // Consumables in a started arena fight (PunishArenaPotions): the hero LOSES the duel through the same native path as
    // after a real knock-out, nothing of the arena is edited by hand.
    //
    // Verified in Game.dll / Script_Game.dll 1.8.7:
    //  - the Status of the arena (gCArena_PS) is NOT free: gCQuest_PS::SetStatus mirrors it (Running <=> the duel quest is
    //    Running). Resetting it by hand (the previous version) left the quest Running and the arena None: the arena
    //    master lost his dialogue line and nobody could fight there any more.
    //  - a knock-out ends in Defeat -> PSQuestManager::OnNPCDefeated(hero) -> gCQuest_PS::OnNPCDefeated: for a duel quest
    //    (gEQuestType_Arena) that is Running and whose opponent list contains the CurrentAttacker of the hero the quest
    //    gets SetStatus(Lost), which also switches the arena off. Lost = the stake is gone, the duel has to be paid again,
    //    the chain does not advance.
    // So the plugin sets the opponent as CurrentAttacker of the hero and calls OnNPCDefeated(hero) on the quest manager
    // of the world, without Defeat (no knock-out animation, no dropped weapons, the hero keeps his AI mode).
    //
    // The opponent then ends the fight (ZS_Attack_Arena_End, as after "YouLeftTheArena") and returns to his routine.
    // (ZS_Attack_End decides who "won" from the AI mode of the hero: Friendly if he stands, Won if he is down. The hero
    // stays on his feet here, so the closing line of the opponent may still be the friendly one - see the log lines.)    //
    namespace
    {

        void LogArenaQuests(GELPCChar a_strWhen)
        {
            gCQuestManager_PS *pManager = gCQuestManager_PS::GetInstance();
            if (!pManager)
                return;
            bTObjArray<bTAutoPOSmartPtr<gCQuest_PS>> const &arrQuests = pManager->GetQuests();
            for (GEInt i = 0; i < arrQuests.GetCount(); ++i)
            {
                gCQuest_PS const *pQuest = static_cast<gCQuest_PS const *>(arrQuests.GetAt(i));
                if (!pQuest || pQuest->GetType() != gEQuestType_Arena)
                    continue;
                gEQuestStatus const enuStatus = pQuest->GetStatus();
                if (enuStatus == gEQuestStatus_Running || enuStatus == gEQuestStatus_Lost)
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] %s: duel quest '%s' status=%d\n", a_strWhen,
                                                 pQuest->GetName().GetText(), static_cast<int>(enuStatus));
            }
        }

        // a_bCompanions: the loss comes from companions / summons that entered the ring. Nothing is done to the crowd
        // then: no mercy is armed (nobody is sent against the hero), the spectators and the guards stay calm.
        // a_bMagic (v1.0.36.0): the loss comes from a cast of ANY spell / scroll / rune inside the ring.
        void ForceArenaDefeat(GEBool a_bCompanions = GEFalse, GEBool a_bMagic = GEFalse)
        {
            Entity Hero = Entity::GetPlayer();
            Entity Opponent;
            if (!FindArenaOpponent(Opponent) && !FindRememberedOpponent(Opponent))
                return;

            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] %s (opponent '%s').\n",
                                         a_bCompanions ? "The hero entered the arena with companions / summons"
                                                       : (a_bMagic ? "Magic cast during the arena duel"
                                                                   : "Consumable used during the arena fight"),
                                         Opponent.GetName().GetText());
            LogArenaQuests("before");
            if (!a_bCompanions)
                ArmSpare("a consumable in the arena fight", GETrue);

            // 1. The native loss of the duel: the quest manager of the world learns that the hero was defeated by him.
            Hero.NPC.SetCurrentAttacker(Opponent);
            Entity World = Entity::GetWorldEntity();
            World.QuestManager.OnNPCDefeated(Hero);
            LogArenaQuests("after");

            // 2. The opponent ends the fight and goes back to his routine (the same way as OnAttack after "YouLeftTheArena").
            Opponent.NPC.LastFightAgainstPlayer = gELastFightAgainstPlayer_Won;
            Opponent.Routine.FullStop();
            Opponent.NPC.SetCurrentAttacker(None);
            Opponent.NPC.SetCurrentTarget(None);
            Opponent.NPC.AttackReason = gEAttackReason_Arena;
            Opponent.Routine.SetState("ZS_Attack_Arena_End");
            // v1.0.59.0: NO artificial line on the screen. The disqualification (a consumable, a companion in the ring, magic) is silent:
            // the duel quest becomes Lost by the native chain above and the opponent leaves.
            // v1.0.68.0: the arena SESSION of the plugin ends here (the journal is not touched: the quest stays as the game keeps it, so the
            // duel can be taken again). The AUTO waiters are given back at once, or as soon as the hero may be released.
            EndArenaSession(a_bCompanions ? "the hero entered the ring with companions" : "the duel is lost in the ring", GETrue, 1);
            // v1.0.69.0 / v1.1.1.0: THE VERDICT OF THE OPPONENT. It covers both ways to lose here: a forbidden consumable (a potion, food, a mana
            // potion: PunishArenaPotions) and companions / summons (types 1..5) in the ring. Before, the gladiator dropped the weapon and left in
            // silence. Now he says the native line of the end of the arena "YouLeftTheArena" ("You left the arena - you are defeated"; the game
            // has no separate "YouLost" line: Script_Game.dll 1.8.7 knows only YouLeftTheArena for this verdict). The line is said AFTER the
            // session of the plugin is closed (g_bArenaSessionHold = false, see EndArenaSession) and after the opponent is already in the
            // peaceful ZS_Attack_Arena_End. It is a pure voice call (StartSaySVM through SayArenaLine): the native knock-out state of a human is
            // NOT started, so his AI cannot loop. ScheduleOverLine is NOT used: it speaks only when the hero LIES knocked out, here he stands.
            // The native plaque GO_ArenaWarning that goes with the line is held back (BlockArenaPlaque: only the voice is heard), and
            // g_bOverLineSaid is set so that a later fall of the hero in the same mercy does not repeat the line.
            if (!a_bMagic)
            {
                BlockArenaPlaque();
                if (SayArenaLine(Opponent, Hero, "YouLeftTheArena", 7,
                                 a_bCompanions ? "companions / summons in the ring" : "a forbidden consumable in the arena"))
                    g_bOverLineSaid = GETrue;
            }
        }

        using GroupLogic::IsHeroControlled; // defined below, with the rule for the controlled creatures
        using GroupLogic::IsHeroFormEntity;                  // defined below

        GEBool IsDuelTaboo(Entity &a_Npc);                       // defined below: the opponent of the duel / arena



        GEBool HasRunningArenaQuest();          // defined below
        void TrackDuelForCompanions();          // defined below: the companions wait while the duel quest runs
        void OnArenaQuestStarted(GEBool a_bFromLost = GEFalse); // defined below: the order to wait, ONCE, at the start of the quest

        // The ring is closed to companions: while the duel quest is Running, nobody who is under the control of the hero
        // (a companion, a summon, a tamed animal, a creature under Word of Control - IsHeroControlled) may stand inside the
        // zone of the arena together with the hero. The check is geometry + state only; a_bBlameOnly just reports.
        // Throttled technically (not a game timing): the loops that call it run on every tick of every participant.
        void CheckArenaCompanions()
        {
            static GEU32 s_u32Last = 0;
            GEU32 const u32Now = ::GetTickCount();
            if (s_u32Last != 0 && static_cast<GEInt>(u32Now - s_u32Last) < 300)
                return;
            s_u32Last = u32Now;

            if (!HasRunningArenaQuest() || !IsHeroInsideArenaZone())
                return;
            // v1.0.68.0: the duel was lost / cancelled and the game has put the quest back to Running by itself (so that the master can
            // offer it again): this is NOT a new duel, the companions that follow the hero cost nothing until the hero takes the duel
            // again from the master (OnArenaQuestStarted opens the new session).
            if (IsArenaSessionGate())
                return;
            Entity Hero = Entity::GetPlayer();
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc == Hero || Npc.IsDead() || !IsHeroControlled(Npc) || !IsInsideArenaZone(Npc))
                    continue;
                if (Npc.Party.Waiting)
                    continue; // a waiting companion never costs the duel (and does not fight: he stands and waits)
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] '%s' (controlled by the hero, party type %d) is inside the "
                                             "arena zone while the duel quest runs.\n",
                                             Npc.GetName().GetText(), static_cast<int>(Npc.Party.PartyMemberType));
                ForceArenaDefeat(GETrue);
                return;
            }
        }

    } // namespace

    //
    // v1.0.66.0: A CONSUMABLE COSTS THE DUEL ONLY IN THE RING, IN A FIGHT THAT HAS PHYSICALLY BEGUN. IsArenaFightStarted() is too wide for
    // this: it is true when the opponent is in the AI mode Arena (the mode of ZS_Arena, the walk to the ring) or when the arena zone is
    // Running (the quest status, Running from the moment the duel is taken from the arena master). A potion drunk after taking the quest
    // - on the way to the arena, in the city - cost the duel. Now BOTH must hold: the hero stands inside the zone of the arena, and the
    // opponent is IN THE DUEL FIGHT against him: the attack reason Arena / Duel, the hero is his target (current, alternative or
    // attacker), and his task / state is an attack one (ZS_Attack*, not an end state) - not ZS_Arena (the walk / the wait at the ring).
    //
    namespace
    {
        GEBool DuelFightStartsWith(bCString const &a_strText, GELPCChar a_strPrefix)
        {
            GELPCChar const strText = a_strText.GetText();
            return strText != nullptr && _strnicmp(strText, a_strPrefix, strlen(a_strPrefix)) == 0 ? GETrue : GEFalse;
        }

        GEBool IsArenaDuelFightBegun()
        {
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return GEFalse;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc == None || Npc == Hero || !Npc.IsNPC() || Npc.IsDead() || Npc.IsDown())
                    continue;
                gEAttackReason const enuReason = Npc.NPC.AttackReason;
                if (enuReason != gEAttackReason_Arena && enuReason != gEAttackReason_Duel)
                    continue;
                if (!(Npc.NPC.GetCurrentTarget() == Hero || Npc.NPC.GetAlternativeTarget() == Hero ||
                      Npc.NPC.GetCurrentAttacker() == Hero))
                    continue;
                if (HasTask(Npc, "ZS_Arena"))
                    continue; // still walking to / waiting at the ring: the fight has not begun
                bCString const strTask = Npc.Routine.GetCurrentTask();
                bCString const strState = Npc.Routine.GetCurrentState();
                GEBool const bAttack = DuelFightStartsWith(strTask, "ZS_Attack") || DuelFightStartsWith(strState, "ZS_Attack");
                GEBool const bEnding = (strTask.GetText() != nullptr && strstr(strTask.GetText(), "_End") != nullptr) ||
                                       (strState.GetText() != nullptr && strstr(strState.GetText(), "_End") != nullptr);
                if (bAttack && !bEnding)
                    return GETrue;
            }
            return GEFalse;
        }
    } // namespace

    void OnHeroConsumeStarted()
    {
        if (!GetConfig().PunishArenaPotions)
            return;
        // The quest is running, but the hero is not in the ring / the fight has not begun: healing is allowed.
        GEBool const bInside = IsHeroInsideArenaZone();
        if (!bInside || !IsArenaDuelFightBegun())
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 20 && HasRunningArenaQuest())
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Consumable used while the duel quest runs, but %s: no penalty.\n",
                                             bInside ? "the fight has not begun yet (nobody of the duel attacks the hero)"
                                                     : "the hero is outside the arena");
            }
            return;
        }
        ForceArenaDefeat();
    }
    //
    // Crimes in the witnesses' communication stack. The engine has no "gEPerception_Crime" (no such string in Game.dll /
    // Script.dll / Script_Game.dll); every crime reaches the witnesses through the perception scripts, which
    // DoLogicalDamage itself calls (SendPerceptionDefeat / SendPerceptionMurder / SendPerceptionFightSound). The plugin
    // sends SendPerceptionMurder with self = the hero: every NPC that can perceive him runs AssessMurder ->
    // AssessTarget and attacks him. Nothing is faked: no attitude, fame or enclave value is changed.
    //
    namespace
    {
        GEInt const CRIME_ASSAULT_ARG = 0x24; // AssessHit result that DoLogicalDamage maps to SendPerceptionMurder

        // Why SendPerceptionMurder alone is not enough (Script_Game.dll 1.8.7): it only loops over all NPCs and runs
        // CanReceivePerceptionFrom + AssessMurder, and AssessMurder drops the witness in many cases (he already has the
        // hero as attacker, flees, the "murder" does not concern him, attitude checks, ...), so the crowd stayed calm.
        // The reliable path is the one Script_G3Fixes uses for its own aggro (AssessActivePerception_FixAttack): every
        // witness that really sees the hero gets AssessTarget(witness, hero, reason) directly - the script that switches
        // an NPC to ZS_Attack - after the same IsFreeLOS test the engine uses.
        GEFloat const WITNESS_RADIUS = 2500.0f;

        enum mEWitnessVerdict
        {
            mEWitnessVerdict_Ok = 0,
            mEWitnessVerdict_NotAWitness = 1, // the hero / the opponent / dead / down / not a humanoid / a companion
            mEWitnessVerdict_TooFar = 2,
            mEWitnessVerdict_NoLineOfSight = 3,
        };

        mEWitnessVerdict GetWitnessVerdict(Entity &a_Npc, Entity &a_Hero, Entity &a_Opponent,
                                           GEBool a_bIgnoreLos = GEFalse, GELPCChar *o_pReason = nullptr)
        {
            auto verdict = [o_pReason](mEWitnessVerdict value, GELPCChar reason)
            { if (o_pReason) *o_pReason = reason; return value; };
            if (a_Npc == None)
                return verdict(mEWitnessVerdict_NotAWitness, "missing entity");
            if (a_Npc == a_Hero)
                return verdict(mEWitnessVerdict_NotAWitness, "the player");
            if (a_Npc == a_Opponent)
                return verdict(mEWitnessVerdict_NotAWitness, "the current duel opponent is handled separately");
            if (!a_Npc.IsNPC())
                return verdict(mEWitnessVerdict_NotAWitness, "not an NPC");
            if (a_Npc.IsDead() || a_Npc.IsDown())
                return verdict(mEWitnessVerdict_NotAWitness, "dead or knocked down");
            if (!AI_Decision::IsHumanoid(a_Npc))
                return verdict(mEWitnessVerdict_NotAWitness, "not humanoid");
            if (IsHeroSide(a_Npc) || IsHeroControlled(a_Npc))
                return verdict(mEWitnessVerdict_NotAWitness, "belongs to the player, including transformed leader");
            if (a_bIgnoreLos && (AI_Decision::IsSlave(a_Npc) || a_Npc.NPC.PoliticalAlignment == gEPoliticalAlignment_None))
                return verdict(mEWitnessVerdict_NotAWitness, "slave or no political faction at an arena violation");
            if (a_Npc.GetDistanceTo(a_Hero) > WITNESS_RADIUS)
                return verdict(mEWitnessVerdict_TooFar, "outside witness radius");
            if (!a_bIgnoreLos && !a_Npc.IsFreeLOS(a_Hero, GEFalse))
                return verdict(mEWitnessVerdict_NoLineOfSight, "no line of sight");
            return verdict(mEWitnessVerdict_Ok, "witness of the current violation");
        }

        // The current task of the NPC is exactly the given one. AssessTarget (Script_Game.dll 1.8.7) switches an NPC with
        // SetTask("ZS_Attack") / SetTask("ZS_Flee"), so the TASK is looked at, not the state.
        GEBool HasTask(Entity &a_Npc, GELPCChar a_strTask)
        {
            bCString const strTask = a_Npc.Routine.GetCurrentTask();
            return _stricmp(strTask.GetText(), a_strTask) == 0 ? GETrue : GEFalse;
        }

        // The witness already fights the hero (target = hero, task ZS_Attack): repeated hits must not restart it.
        GEBool IsAttackingHero(Entity &a_Witness, Entity &a_Hero)
        {
            return a_Witness.NPC.GetCurrentTarget() == a_Hero && HasTask(a_Witness, "ZS_Attack");
        }

        // The witness has reacted: he attacks the hero or runs away from him.
        GEBool HasReacted(Entity &a_Witness, Entity &a_Hero)
        {
            return IsAttackingHero(a_Witness, a_Hero) ||
                   (a_Witness.NPC.GetCurrentTarget() == a_Hero && HasTask(a_Witness, "ZS_Flee"));
        }

        // Fallback when the scripted AssessTarget dropped the witness (for a hero with a high reputation every attitude
        // check says "friend"). It is the same sequence AssessTarget itself runs: FullStop, target, SetTask.
        // Guards and armed residents fight; the unarmed ones run away (nobody is forced to attack against his nature).
        void ForceReaction(Entity &a_Witness, Entity &a_Hero)
        {
            NoteAggro(a_Witness);
            GEBool const bFights = AI_Decision::IsGuard(a_Witness) || AI_Decision::HasDangerousWeapon(a_Witness);
            a_Witness.Routine.FullStop();
            a_Witness.NPC.SetCurrentTarget(a_Hero);
            if (bFights)
                a_Witness.NPC.AttackReason = gEAttackReason_Enemy;
            a_Witness.Routine.SetTask(bFights ? "ZS_Attack" : "ZS_Flee");
        }

        void ForceAttack(Entity &a_Npc, Entity &a_Hero, gEAttackReason a_enuReason)
        {
            NoteAggro(a_Npc);
            a_Npc.Routine.FullStop();
            a_Npc.NPC.SetCurrentTarget(a_Hero);
            a_Npc.NPC.AttackReason = a_enuReason;
            a_Npc.Routine.SetTask("ZS_Attack");
        }


        GEU32 g_u32LastDeclare = 0;
        // a_bImmediate: the reaction must not wait for the 0.7 s pause between two searches for witnesses (the moment
        // a status effect starts is a single event: it is not repeated by the next hit).
        void DeclareHeroCriminal(GELPCChar a_strWhy, GEBool a_bImmediate = GEFalse)
        {
            // A beating is a series of hits: look for witnesses at most every 0.7 s.
            GEU32 const u32Now = ::GetTickCount();
            if (!a_bImmediate && g_u32LastDeclare != 0 && static_cast<GEInt>(u32Now - g_u32LastDeclare) < 700)
                return;
            g_u32LastDeclare = u32Now;

            Entity Hero = Entity::GetPlayer();
            Entity Opponent;
            GEBool const bArenaViolation = FindArenaOpponent(Opponent); // None outside the arena
            if (bArenaViolation)
                ArmSpare("a violation of the arena rules", GETrue);

            if (bArenaViolation)
                g_iArenaWitnessCount = 0; // Revalidate everyone for this violation.
            GEInt iAlerted = 0;
            GEInt iForced = 0;
            GEInt iTooFar = 0;
            GEInt iNoLos = 0;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Witness = arrNPCs.GetAt(i);
                // Arena violation: the whole crowd within the radius is told, nobody has to look at the very moment.
                GELPCChar reason = "unknown";
                mEWitnessVerdict const enuVerdict = GetWitnessVerdict(Witness, Hero, Opponent, bArenaViolation, &reason);
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][witness] '%s': %s (%s), arena=%d\n",
                    Witness != None ? Witness.GetName().GetText() : "<none>",
                    enuVerdict == mEWitnessVerdict_Ok ? "INCLUDED" : "EXCLUDED", reason, static_cast<int>(bArenaViolation));
                if (enuVerdict == mEWitnessVerdict_TooFar)
                    ++iTooFar;
                else if (enuVerdict == mEWitnessVerdict_NoLineOfSight)
                    ++iNoLos;
                if (enuVerdict != mEWitnessVerdict_Ok)
                    continue;

                // Reaching here means this NPC witnessed THIS incident.
                if (SocialLogic::ObserveSlaveWitness(Witness, Hero, Opponent))
                    continue;
                if (bArenaViolation)
                    NoteCurrentArenaWitness(Witness);
                if (HasReacted(Witness, Hero))
                {
                    NoteAggro(Witness);
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][witness] '%s': already reacting, registered for this incident.\n",
                                                   Witness.GetName().GetText());
                    continue;
                }

                // The crime is written down for the witness (gEPlayerCrime_Attack = 2, same call as in Defeat) ...
                AI_Memory::MemorizePlayerCrime(Witness, Hero, gEPlayerCrime_Attack);
                // ... and he reacts immediately (ZS_Attack / ZS_Flee through AssessTarget) ...
                AI_Perception::AssessTarget(Witness, Hero, gEAttackReason_Enemy);
                // ... and if the script dropped him anyway (e.g. a spectator of the arena stays in ZS_WatchFight), the
                // reaction is forced.
                if (!HasReacted(Witness, Hero))
                {
                    ForceReaction(Witness, Hero);
                    ++iForced;
                }
                else
                    NoteAggro(Witness);
                ++iAlerted;
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Crime: %s -> %d witness(es) react (%d of them forced), "
                                         "%d too far, %d without line of sight.\n",
                                         a_strWhy, iAlerted, iForced, iTooFar, iNoLos);
        }

        GEBool HasRunningArenaQuest()
        {
            gCQuestManager_PS *pManager = gCQuestManager_PS::GetInstance();
            if (!pManager)
                return GEFalse;
            bTObjArray<bTAutoPOSmartPtr<gCQuest_PS>> const &arrQuests = pManager->GetQuests();
            for (GEInt i = 0; i < arrQuests.GetCount(); ++i)
            {
                gCQuest_PS const *pQuest = static_cast<gCQuest_PS const *>(arrQuests.GetAt(i));
                if (pQuest && pQuest->GetType() == gEQuestType_Arena && pQuest->GetStatus() == gEQuestStatus_Running)
                    return GETrue;
            }
            return GEFalse;
        }

        // v1.0.43.0: WHEN THE QUARANTINE OF THE SQUAD MAY END. No artificial flag: the squad waits (ZS_WaitForPlayer, Waiting =
        // true, put ONCE by OnArenaQuestStarted) and the plugin touches the AI of the squad in no hook. The two detectors of the
        // end of the quest (TrackDuelForCompanions, PollArenaQuestEnd) notice that the native quest is not Running any more and
        // ask here whether the release is allowed NOW. It is NOT allowed:
        //  - while the hero lies knocked out (the guards ransack him, the squad only watches), whoever struck him down;
        //  - while the punishment is under way: the mercy of the arena violation is armed (ArmSpare) and the hero has not fallen
        //    yet, he stands in the ring (the silent Failed of the magic punishment closes the quest long before he falls).
        // It is allowed as soon as the hero is on his feet again - the very condition of the native event "Mercy ended: the
        // hero is on his feet again" (IsFallActive() && !IsHeroKnockedOut) -, when the hero won (no mercy armed), or when he
        // left the ring (the escape). Pure reads of the native state.
        GEBool IsArenaReleaseAllowed()
        {
            if (IsHeroKnockedOut())
                return GEFalse;
            if (IsArmed() && IsArenaPunishment() && !IsFallActive() && IsHeroInsideArenaZone())
                return GEFalse;
            return GETrue;
        }

        // v1.0.40.0: the old magic ban (the hook of the cast: SetLastCast / the summon and heal scripts -> one punishment)
        // is REMOVED. The arena judges magic by the STATES OF THE HERO now (OnHeroRoutineChange, at the end of this file):
        // stage 1 - the magic task PS_Magic in the ring is a threat; stage 2 - the cast state (PS_Magic_QuickCast ...) is the
        // punishment. There was no hook of gCMagic_PS::Cast in the code of the plugin (nothing to cut out of the sources).

        Entity ResolveEntity(GELPVoid a_pEntity, eCEntity *a_pFallback)
        {
            return a_pEntity ? *static_cast<Entity *>(a_pEntity) : Entity(a_pFallback);
        }


        //
        // DIAGNOSTICS v1.0.20.0 (reads only, nothing is written, no behaviour is changed).
        //  [flee]  the hero (in any form) hurts a member of his own army: the AI task / state of the victim right after
        //          the blow and then at +300 / +1000 / +2000 / +4000 ms (which native routine - ZS_Flee, ZS_Evade*,
        //          ZS_Stumble, ZS_AssessHit ... - keeps him walking backwards).
        //  [hit]   every blow on a member of the army (party types 1..5): who, whom, damage type, the native function
        //          that recorded it, and whether the attacker is an outsider (not of the army, not the hero).
        //
        GELPCChar DamageTypeName(GEInt a_iType)
        {
            switch (a_iType)
            {
                case gEDamageType_None: return "None";
                case gEDamageType_Impact: return "Impact";
                case gEDamageType_Blade: return "Blade";
                case gEDamageType_Missile: return "Missile";
                case gEDamageType_Fire: return "Fire";
                case gEDamageType_Ice: return "Ice";
                case gEDamageType_Lightning: return "Lightning";
                case gEDamageType_Physics: return "Physics";
                default: return "?";
            }
        }




        //
        // COMBAT RECORDS (v1.0.25.0). The native rules of the army: a creature of the party fights an outsider only after a
        // REAL blow - the outsider hurt the hero / a member of the army, or the hero / the army hurt him. A mob that has only
        // decided to attack the hero and is running at him gives no reason. The records are made where the engine stores
        // the attacker (PSNpc::SetCurrentAttacker / SetLastAttacker, which AssessHit calls on every landed blow).
        //  - combat record: the name of the outsider that is in a real fight with the hero's side;
        //  - hero hit record: the name of a creature of the army that the HERO himself (any form) has struck: for a while the
        //    plugin does not stand between it and the hero, the native counter of the blows / the brawl decides.
        //





        //
        // THE BRAWL OF THE HERO WITH A MEMBER OF HIS OWN ARMY (v1.0.30.0): the behaviour of an ordinary "green" NPC.
        //  - FIRST blow of the hero: the creature does not stay silent and does not attack. It reacts at once as a green NPC
        //    does (the native ZS_ReactToDamage: the weapon is drawn, the fighting stance, the manoeuvring and backing off in
        //    front of the hero, one on one). The endless retreat is stopped by the safety net (see BrawlStageAction and
        //    the ZS_Stumble release in PollFleeWatch).
        //  - SECOND blow within BRAWL_WINDOW_MS: the creature accepts the fight and attacks the hero (ZS_Attack). The fight
        //    stays inside the party: the name stays blue (GetFocusNameAttitude), nobody leaves the group.
        //  - The rest of the army keeps full neutrality (DefendAllies, AssessTarget and the master block do not let anybody
        //    in). When the hero is knocked out nobody loots him and nobody finishes him (LeaveMasterAlone); the fight ends
        //    and the creature goes back to the routine of following (PollBrawl).
        //
        GEU32 g_u32HeroCastSeq = 0;               // counts the casts of the hero (PSNpc::SetLastCast)

        // The fights are watched here (throttled): the hero knocked out / the creature down / the time over ends the fight;
        // while it goes on the timer "the hero struck" is kept alive (the master block stays off for this creature only).
        // v1.0.36.0: THE END OF THE ARENA QUEST RELEASES THE WAITING COMPANIONS. The status of the arena quest (gCQuestManager)
        // is watched from the background loop: when it WAS Running and is not any more (Success, Failed / Lost, Cancelled -
        // whatever the reason), every creature of the party (types 1..5) that still stands in ZS_WaitForPlayer gets the
        // native task of following back (Waiting = false, SetTask("ZS_FollowPlayer")). Reads only, throttled technically.
        GEBool g_bPollArenaWasRunning = GEFalse;
        // v1.0.40.0: the hero has been seen INSIDE the ring while the arena quest runs (the walk to the arena does not count as
        // an escape from it). Cleared when the quest is over.
        GEBool g_bRingSeen = GEFalse;

        void PollArenaQuestEnd()
        {
            static GEU32 s_u32Last = 0;
            GEU32 const u32Now = ::GetTickCount();
            if (s_u32Last != 0 && static_cast<GEInt>(u32Now - s_u32Last) < 250)
                return;
            s_u32Last = u32Now;

            GEBool const bRunning = HasRunningArenaQuest(); // the native status of the quest
            if (bRunning && IsHeroInsideArenaZone())
                g_bRingSeen = GETrue;
            else if (!bRunning)
            {
                g_bRingSeen = GEFalse;
                EndMagicThreat();
            }
            if (bRunning)
            {
                g_bPollArenaWasRunning = GETrue;
                return;
            }
            if (!g_bPollArenaWasRunning)
                return;
            // v1.0.43.0: the quest is over, but the squad is released only when the hero is on his feet again (not while he
            // lies knocked out and is ransacked, not while he is still beaten in the ring): the edge stays pending until then.
            if (!IsArenaReleaseAllowed())
                return;
            g_bPollArenaWasRunning = GEFalse;

            // v1.0.68.0: ONLY the AUTO waiters of THIS arena session (g_arrWaiters) are released. Before, every companion of the hero
            // that stood in ZS_WaitForPlayer anywhere in the world was sent back to ZS_FollowPlayer: the one who waited in another city
            // (told to wait by the player, or put there by a teleport) ran across the map to the hero. Nobody far away is touched here.
            Entity Hero = Entity::GetPlayer();
            GEInt iReleased = 0;
            GEInt iKept = 0;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc == None || Npc.IsDead() || !IsHeroControlled(Npc) || !HasTask(Npc, "ZS_WaitForPlayer"))
                    continue;
                if (!IsSessionAutoWaiter(Npc))
                {
                    ++iKept; // the player's wait, or a wait of another origin: it is not an arena waiter, it is left as it is
                    continue;
                }
                if (Hero != None && Npc.GetDistanceTo(Hero) > FAR_COMPANION_DISTANCE)
                {
                    MarkWaiterAutoPublic(Npc); // far (another city): he stays where he is, a teleport carries him to the hero
                    ++iKept;
                    continue;
                }
                ClearWaiterMarksPublic(Npc);
                Npc.Party.Waiting = GEFalse;
                Npc.Routine.FullStop();
                Npc.Routine.SetTask("ZS_FollowPlayer");
                ++iReleased;
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The arena quest is not running any more: %d waiting companion(s) of the arena "
                                         "session follow the hero again (ZS_FollowPlayer), %d other waiter(s) are left as they are.\n",
                                         static_cast<int>(iReleased), static_cast<int>(iKept));
        }



        // THE TABOO OF THE DUEL. While the duel / arena quest is Running, the one NPC who fights the hero in it (attack
        // reason Arena / Duel, or the opponent remembered by name when his reason has already changed) is invisible for the
        // whole army (types 1..5): nobody takes him for a target, neither at once nor for his blows on the hero. Every other
        // outsider is judged by the ordinary rules of the open world.
        GEBool IsDuelTaboo(Entity &a_Npc)
        {
            TrackDuelForCompanions(); // the end of the quest is noticed here too (throttled)
            if (a_Npc == None || !a_Npc.IsNPC() || IsHeroSide(a_Npc))
                return GEFalse;
            // After the end of the quest: 30 seconds, and as long as the opponent is still in the fight (ZS_Attack / the
            // combat modes) against the army - he does not attack the party of the hero (the summon, the companions).
            if (!HasRunningArenaQuest())
            {
                if (g_strArenaOpponent.IsEmpty() || !(a_Npc.GetName() == g_strArenaOpponent))
                    return GEFalse;
                GEBool const bTime = g_u32TabooUntil != 0 && static_cast<GEInt>(::GetTickCount() - g_u32TabooUntil) < 0;
                gEAIMode const enuMode = a_Npc.Routine.AIMode;
                GEBool const bStillFighting = (HasTask(a_Npc, "ZS_Attack") || enuMode == gEAIMode_Combat ||
                                               enuMode == gEAIMode_Arena) &&
                                              g_u32TabooUntil != 0 &&
                                              static_cast<GEInt>(::GetTickCount() - g_u32TabooUntil) < 120000;
                return (bTime || bStillFighting) ? GETrue : GEFalse;
            }
            gEAttackReason const enuReason = a_Npc.NPC.AttackReason;
            if (enuReason == gEAttackReason_Arena || enuReason == gEAttackReason_Duel)
            {
                g_strArenaOpponent = a_Npc.GetName();
                return GETrue;
            }
            if (!g_strArenaOpponent.IsEmpty() && a_Npc.GetName() == g_strArenaOpponent)
            {
                Entity Other;
                return FindArenaOpponent(Other) ? GEFalse : GETrue; // another opponent found: this one is no taboo
            }
            return GEFalse;
        }



    } // namespace

    namespace
    {

    } // namespace

    // The one hook for the arena magic and the city assaults: DoLogicalDamage is the choke point of every hit
    // (projectiles, burning ticks, ice). self = victim, other = the damaging object.
    // In the arena only two fighters exist: fire / ice / lightning damage to the opponent can only come from the hero,
    // unless the damaging object is explicitly owned by the opponent himself. So no fragile "who created the projectile"
    // chain is needed there. Freezing / igniting is also detected by the state of the opponent (IsFrozen / IsBurning)
    // before and after the original, because the status is applied inside AssessHit.
        ME_DEFINE_AND_HOOK_SCRIPT(DoLogicalDamage)
    {
        return PluginRuntime::Combat::DoLogicalDamage(DoLogicalDamage_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    //
    // Second, independent trigger of the assault reaction. DoLogicalDamage is also hooked by other plugins
    // (Script_NewBalance.dll), and a second inline hook on the same address can fail without any message. The
    // perception script AssessHit stores the hero as the attacker of every NPC he hurts through
    // PSNpc::SetCurrentAttacker / SetLastAttacker (Script.dll exports), so these two are hooked as well. The original
    // runs first, then the NPC is judged exactly like in DoLogicalDamage (IsUnprovokedAssault -> DeclareHeroCriminal).
    //
    namespace
    {
        // Why the damage type stored on the victim cannot be used here (verified in the log of the game): AssessHit calls
        // SetLastAttacker BEFORE it writes the type of the new damage, so PSDamageReceiver::DamageType still holds the
        // previous hit (Blade = 2) when the hero hits with a fireball / lightning bolt. What does tell magic from melee
        // at that moment is the hero himself: in magic mode his routine task is PS_Magic (PS_Melee with a weapon), and
        // PSNpc::LastSpell holds the template of the spell he cast ("Spl_Fireball", "Spl_Lightningbolt", ...). LastSpell
        // alone is not enough: it keeps the last spell after the hero went back to the sword.
        GEBool StartsWithNoCase(bCString const &a_strText, GELPCChar a_strPrefix)
        {
            return _strnicmp(a_strText.GetText(), a_strPrefix, strlen(a_strPrefix)) == 0 ? GETrue : GEFalse;
        }

        GEBool ContainsNoCase(bCString const &a_strText, GELPCChar a_strPart)
        {
            char strLower[128] = {};
            char strPart[64] = {};
            strncpy_s(strLower, a_strText.GetText(), _TRUNCATE);
            strncpy_s(strPart, a_strPart, _TRUNCATE);
            _strlwr_s(strLower);
            _strlwr_s(strPart);
            return strstr(strLower, strPart) != nullptr ? GETrue : GEFalse;
        }

        // The moment of the last cast of the hero (PSNpc::SetLastCast is called by the cast scripts). A projectile is
        // still flying when the hero already put the staff away, so a hit within CAST_WINDOW_MS after the cast is a
        // magic hit as well.
        GEU32 const CAST_WINDOW_MS = 2500;
        GEU32 g_u32HeroCastTick = 0;

        GEBool HeroCastRecently()
        {
            return g_u32HeroCastTick != 0 && static_cast<GEInt>(::GetTickCount() - g_u32HeroCastTick) < static_cast<GEInt>(CAST_WINDOW_MS);
        }

        // The hero casts: task or state of his routine is PS_Magic* (not the potion).
        GEBool IsHeroCasting(Entity &a_Hero)
        {
            bCString const strTask = a_Hero.Routine.GetCurrentTask();
            bCString const strState = a_Hero.Routine.GetCurrentState();
            auto const IsMagic = [](bCString const &a_strName)
            {
                return StartsWithNoCase(a_strName, "PS_Magic") && !StartsWithNoCase(a_strName, "PS_MagicPotion");
            };
            return IsMagic(strTask) || IsMagic(strState);
        }

        // v1.0.52.0: THE BOW / CROSSBOW. The ranged mode of the hero is the task / state PS_Ranged* (PS_Ranged, _Aim, _PowerAim,
        // _Aim_Loop, _Reload, _Shoot, _Walk, _Sneak, ...): the same family as PS_Magic* for the runes.
        GEU32 g_u32HeroShotTick = 0; // the moment of the last shot (PS_Ranged_Shoot); an arrow is still flying afterwards
        GEU32 const SHOT_WINDOW_MS = 4000;

        GEBool IsHeroRangedMode(Entity &a_Hero)
        {
            bCString const strTask = a_Hero.Routine.GetCurrentTask();
            bCString const strState = a_Hero.Routine.GetCurrentState();
            return StartsWithNoCase(strTask, "PS_Ranged") || StartsWithNoCase(strState, "PS_Ranged") ? GETrue : GEFalse;
        }

        GEBool HeroShotRecently()
        {
            return g_u32HeroShotTick != 0 &&
                   static_cast<GEInt>(::GetTickCount() - g_u32HeroShotTick) < static_cast<GEInt>(SHOT_WINDOW_MS);
        }

        // v1.0.52.0: AllowArenaEnchantedWeapons. GETrue: the burning / freezing of the opponent now comes from the ENCHANTED MELEE
        // WEAPON of the hero (a fire / ice sword) - he is not casting, did not cast and did not shoot a moment ago, he has no bow
        // in the hand. Such a status (and the elemental ticks it deals) is NOT a violation: the duel is the normal melee duel.
        // Anything that comes from a spell or from a bow stays what it was (the crime of the cast / of the shot is judged by the
        // states of the hero, stage 2).
        GEBool IsEnchantedMeleeEffect()
        {
            if (!GetConfig().AllowArenaEnchantedWeapons)
                return GEFalse;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return GEFalse;
            return !IsHeroCasting(Hero) && !HeroCastRecently() && !IsHeroRangedMode(Hero) && !HeroShotRecently() ? GETrue
                                                                                                                 : GEFalse;
        }

        // The template name of an elemental spell (fire / ice / lightning). Spl_Fireball, Spl_Firerain, Spl_Icelance,
        // Spl_IceBomb, Spl_FrostWave, Spl_Lightningbolt, Spl_Lightning, Spl_Freeze ... contain one of these parts.
        GEBool IsElementalSpell(bCString const &a_strSpell)
        {
            return ContainsNoCase(a_strSpell, "fire") || ContainsNoCase(a_strSpell, "ice") ||
                   ContainsNoCase(a_strSpell, "frost") || ContainsNoCase(a_strSpell, "freez") ||
                   ContainsNoCase(a_strSpell, "lightning") || ContainsNoCase(a_strSpell, "flame") ||
                   ContainsNoCase(a_strSpell, "burn");
        }

        // The damage type of a spell read from its OWN data, not from its name: the spell entity (found by the name the
        // game stored in LastSpell) has a gCMagic_PS with the spell type (projectile / target / summon) and the "Spawn"
        // entity (projectile, summon container, ...) whose gCDamage_PS holds the damage type. A custom spell made from a
        // summon (type Target, FuncOnCast MagicSummon, damage Lightning in the Smn_ template) is recognised this way.
        gEDamageType GetSpellDamageType(bCString const &a_strSpell, GEInt &o_iSpellType, bCString &o_strFunc,
                                        GEInt *o_pCastType = nullptr, bCString *o_pHitFunc = nullptr)
        {
            o_iSpellType = -1;
            if (o_pCastType)
                *o_pCastType = -1;
            if (a_strSpell.IsEmpty())
                return gEDamageType_None;
            eCSceneAdmin *pScene = FindModule<eCSceneAdmin>();
            if (!pScene)
                return gEDamageType_None;
            // LastSpell is the NAME OF A TEMPLATE ("Spl_..."), so the template entities are searched first.
            eCEntity *pSpellEntity = pScene->GetTemplateEntityByName(a_strSpell);
            if (!pSpellEntity)
                pSpellEntity = pScene->GetEntityByName(a_strSpell);
            if (!pSpellEntity)
                return gEDamageType_None;
            Entity Spell(pSpellEntity);
            if (!Spell.Magic.IsValid())
                return gEDamageType_None;
            o_iSpellType = static_cast<GEInt>(Spell.Magic.SpellType);
            o_strFunc = Spell.Magic.FuncOnCast;
            if (o_pCastType)
                *o_pCastType = static_cast<GEInt>(Spell.Magic.CastType);
            if (o_pHitFunc)
                *o_pHitFunc = Spell.Magic.FuncOnTargetHit;
            Entity Spawn = Spell.Magic.GetSpawn();
            if (Spawn != None && Spawn.Damage.IsValid())
                return Spawn.Damage.DamageType;
            return gEDamageType_None;
        }

        void OnHeroAttackerSet(PSNpc *a_pThis, Entity const *a_pAttacker, GELPCChar a_strWhere)
        {
            static GEBool s_bBusy = GEFalse; // AssessTarget of the witnesses must not re-enter
            if (s_bBusy || !a_pThis || !a_pAttacker || !a_pThis->m_pEngineEntityPropertySet)
                return;

            Entity Hero = Entity::GetPlayer();
            if (!(*a_pAttacker == Hero))
                return;
            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (!pOwner)
                return;
            Entity Victim(pOwner);
            if (Victim == Hero)
                return;
            // A companion / summon hit by the hero (an area spell, a stray blow) is no victim of an assault: no witnesses,
            // no watching crowd, no arena judging.
            if (IsHeroControlled(Victim))
                return;

            static GEBool s_bAliveLogged = GEFalse;
            if (!s_bAliveLogged)
            {
                s_bAliveLogged = GETrue;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] %s hook is alive.\n", a_strWhere);
            }

            s_bBusy = GETrue;

            Entity Opponent;
            GEBool const bOpponentHit = FindArenaOpponent(Opponent) && Victim == Opponent;
            if (bOpponentHit && !IsHeroInsideArenaZone())
                LogOutsideRing("a hit on the opponent");
            else if (bOpponentHit)
            {
                // v1.0.48.0: the hero in a monster body (or not a humanoid): no duel judging, nothing of him is read.
                if (IsHeroDuelBlocked())
                {
                    s_bBusy = GEFalse;
                    return;
                }
                // Arena: melee is allowed, fire / ice / lightning is not. The magic is recognised by the hero (see
                // above); burning / freezing are statuses of the victim and are reliable on their own (this also
                // covers the burning ticks after the fireball, when the hero already swung his sword again).
                gEDamageType const enuLast = Victim.DamageReceiver.IsValid() ? Victim.DamageReceiver.DamageType
                                                                             : gEDamageType_None;
                GEBool const bBurning = Victim.NPC.IsBurning();
                GEBool const bFrozen = Victim.NPC.IsFrozen();
                bCString const strLastSpell = Hero.NPC.LastSpell;
                GEBool const bCasting = IsHeroCasting(Hero) || HeroCastRecently();
                GEInt iSpellType = -1;
                bCString strSpellFunc;
                gEDamageType const enuSpellDamage = GetSpellDamageType(strLastSpell, iSpellType, strSpellFunc);
                // Name-free: a spell that is not a projectile (target / summon: the damage comes from the spell, not from a
                // missile), or whose data could not be read at all, is magic as well. "Summon" is the principle, not a name.
                GEBool const bNotProjectile = iSpellType == static_cast<GEInt>(gESpellType_Target) ||
                                              iSpellType == static_cast<GEInt>(gESpellType_Summon);
                GEBool const bUnknownSpell = iSpellType < 0 && !strLastSpell.IsEmpty();
                GEBool const bMagicHit = bCasting && (IsElementalSpell(strLastSpell) ||
                                                      IsElementalDamage(enuSpellDamage) || bNotProjectile ||
                                                      bUnknownSpell);
                // v1.0.52.0: AllowArenaEnchantedWeapons - the burning / freezing (and the elemental tick of it) that comes from an
                // enchanted MELEE weapon (no cast, no shot, no bow in the hands) is ignored: the duel is the normal melee duel.
                GEBool const bEnchantedMelee = (bBurning || bFrozen || IsElementalDamage(enuLast)) && IsEnchantedMeleeEffect();
                GEBool const bElemental = bMagicHit || (!bEnchantedMelee && (IsElementalDamage(enuLast) || bBurning || bFrozen));

                static GEInt s_iArenaDiagLines = 0;
                if (s_iArenaDiagLines < 200)
                {
                    ++s_iArenaDiagLines;
                    bCString const strTask = Hero.Routine.GetCurrentTask();
                    bCString const strState = Hero.Routine.GetCurrentState();
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] arena: hero hurt '%s' (%s): hero task='%s' "
                                                 "state='%s', last spell='%s' (type=%d, func='%s', spell damage=%d), "
                                                 "casting=%d, previous damage type=%d, "
                                                 "burning=%d, frozen=%d -> %s\n",
                                                 Victim.GetName().GetText(), a_strWhere, strTask.GetText(),
                                                 strState.GetText(), strLastSpell.GetText(),
                                                 static_cast<int>(iSpellType), strSpellFunc.GetText(),
                                                 static_cast<int>(enuSpellDamage),
                                                 static_cast<int>(bCasting), static_cast<int>(enuLast),
                                                 static_cast<int>(bBurning), static_cast<int>(bFrozen),
                                                 bElemental ? "ELEMENTAL, witnesses react"
                                                            : (bEnchantedMelee ? "enchanted melee weapon, allowed" : "melee, allowed"));
                }
                if (bElemental)
                    DeclareHeroCriminal("forbidden fire / ice / lightning damage in the arena");
            }
            s_bBusy = GEFalse;
        }



        mCFunctionHook Hook_NpcSetCurrentAttacker;
        void GE_STDCALL NpcSetCurrentAttacker(PSNpc *a_pThis, Entity const *a_pAttacker)
        {
            return PluginRuntime::Native::NpcSetCurrentAttacker(a_pThis, a_pAttacker,
                Hook_NpcSetCurrentAttacker.GetOriginalFunction<PluginRuntime::Native::NpcSetCurrentAttackerOriginal>());
        }

        mCFunctionHook Hook_NpcSetLastAttacker;
        void GE_STDCALL NpcSetLastAttacker(PSNpc *a_pThis, Entity const *a_pAttacker)
        {
            return PluginRuntime::Native::NpcSetLastAttacker(a_pThis, a_pAttacker,
                Hook_NpcSetLastAttacker.GetOriginalFunction<PluginRuntime::Native::NpcSetLastAttackerOriginal>());
        }

        //
        // The MOMENT a status effect starts on the arena opponent. A fire / ice sword hits as a plain blade blow (damage
        // type Blade), which the arena allows; the burning / freezing is switched on right AFTER that hit by AssessHit
        // (or by MagicBurn / MagicFreeze / ZS_Freeze for a spell), through PSNpc::EnableStatusEffects(mask, on)
        // (mask 1 = burning, 2 = frozen, 4 = poison, 8 = disease; EnableBurning / EnableFrozen are thin wrappers that
        // call it, so one inline hook covers them all). Waiting for the next blow means the crowd reacts only to the
        // second hit - or never, when the hero ignites the opponent and runs in circles. So the reaction starts here, in
        // the very frame the effect is on.
        //
        mCFunctionHook Hook_NpcEnableStatusEffects;
        void GE_STDCALL NpcEnableStatusEffects(PSNpc *a_pThis, GEInt a_iMask, GEBool a_bEnable)
        {
            return ArenaJudge::Native::NpcEnableStatusEffects(a_pThis, a_iMask, a_bEnable,
                Hook_NpcEnableStatusEffects.GetOriginalFunction<ArenaJudge::Native::NpcEnableStatusEffectsOriginal>());
        }

        // PSNpc::SetLastCast(): remembers when the hero cast a spell.
        mCFunctionHook Hook_NpcSetLastCast;
        void GE_STDCALL NpcSetLastCast(PSNpc *a_pThis)
        {
            return ArenaJudge::Native::NpcSetLastCast(a_pThis,
                Hook_NpcSetLastCast.GetOriginalFunction<ArenaJudge::Native::NpcSetLastCastOriginal>());
        }
    } // namespace

    //
    // The hero is knocked out in a started arena fight (by anybody: the opponent or a witness who attacked him for a
    // forbidden spell). The native Defeat of the hero (called by ZS_Unconscious / ZS_LieKnockDown / ZS_PiercedKO) calls
    // PSQuestManager::OnNPCDefeated(hero), and the duel quest becomes Lost only when the CurrentAttacker of the hero is
    // one of the opponents of the quest. A witness is not, so the duel stayed Running and the opponent attacked the hero
    // again when he got up. The fix is the same as for a consumable: the duel is lost, the hero has to pay the arena
    // master and start again.
    //  1. Before the original Defeat: if the knock-out is not by the opponent, the opponent is written as the
    //     CurrentAttacker of the hero, so the native code itself sets the quest to Lost.
    //  2. After it: if the duel quest still runs (a different quest layout), OnNPCDefeated is called by hand.
    //  3. The opponent ends the fight (ZS_Attack_Arena_End), as in ForceArenaDefeat.
    // A knock-out by the opponent himself is left to the native code completely.
    //
    namespace
    {
        void EndArenaFightOfOpponent(Entity &a_Opponent)
        {
            g_strStage2Hold = bCString(); // v1.0.61.0: the fight is ended on purpose: nothing holds the opponent in it any more
            {
                // What the opponent does at the moment he is told to end the fight (to find out, from the log, why he
                // sometimes stays in the middle of the arena and sometimes leaves).
                bCString const strTask = a_Opponent.Routine.GetCurrentTask();
                bCString const strState = a_Opponent.Routine.GetCurrentState();
                Entity Target = a_Opponent.NPC.GetCurrentTarget();
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] the opponent '%s' is told to end the fight: task='%s' "
                                             "state='%s' ai mode=%d attack reason=%d target='%s'\n",
                                             a_Opponent.GetName().GetText(), strTask.GetText(), strState.GetText(),
                                             static_cast<int>(a_Opponent.Routine.AIMode),
                                             static_cast<int>(a_Opponent.NPC.AttackReason),
                                             Target != None ? Target.GetName().GetText() : "-");
            }
            a_Opponent.NPC.LastFightAgainstPlayer = gELastFightAgainstPlayer_Won;
            a_Opponent.Routine.FullStop();
            a_Opponent.NPC.SetCurrentAttacker(None);
            a_Opponent.NPC.SetCurrentTarget(None);
            a_Opponent.NPC.AttackReason = gEAttackReason_Arena;
            a_Opponent.Routine.SetState("ZS_Attack_Arena_End");
        }

        // The duel is over for everybody once the hero is knocked out after a violation of the arena rules, WHOEVER knocked
        // him out (a witness, the opponent, a summoned creature): the quest is lost, the opponent leaves the arena.
        // Once per knock-out (g_bDuelLossDone).
        void EndArenaDuelAfterKnockout()
        {
            if (g_bDuelLossDone)
                return;
            Entity Hero = Entity::GetPlayer();
            Entity Opponent;
            if (!FindArenaOpponent(Opponent) && !FindRememberedOpponent(Opponent))
                return;
            g_bDuelLossDone = GETrue;

            Entity Attacker = Hero.NPC.GetCurrentAttacker();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The hero is knocked out after a violation in the arena (opponent "
                                         "'%s', knocked out by '%s'): the duel is over.\n",
                                         Opponent.GetName().GetText(),
                                         Attacker != None ? Attacker.GetName().GetText() : "-");
            if (HasRunningArenaQuest())
            {
                Hero.NPC.SetCurrentAttacker(Opponent);
                Entity World = Entity::GetWorldEntity();
                World.QuestManager.OnNPCDefeated(Hero);
                LogArenaQuests("after the knock-out");
            }
            EndArenaFightOfOpponent(Opponent);
            ScheduleOverLine(Opponent); // v1.0.60.0: the voice over the fallen hero (the plaque is blocked)
            EndArenaSession("the hero is knocked out in the arena", GETrue, 1); // v1.0.68.0: released when he stands again
        }
    } // namespace

        ME_DEFINE_AND_HOOK_SCRIPT(Defeat)
    {
        return PluginRuntime::Combat::Defeat(Defeat_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    //
    // The hero is down and an NPC decides whether to finish him off: HasAttackReasonToKill(npc, hero). It is called
    // directly by ZS_Attack_End (not through the script table); the script hook of the registry is an inline hook of the
    // function itself, so it covers the direct call as well. The original script runs first; "no" only replaces its
    // "yes". The answer is "no" for everybody within SPARE_RADIUS of the place where the hero fell once the mercy is
    // armed (see ArmSpare), and in a running arena fight. After "no" the native ZS_Attack_End goes on with
    // WantToRansack / ZS_RansackBody: the NPC takes the weapon and the gold of the hero (the native exceptions of
    // WantToRansack are untouched). Everything not covered (monsters, bandits outside the radius, quest kills) keeps
    // the native answer.
    //

    //
    // ZS_WatchFight_Loop is the state in which every watcher of a fight (the first stage of the city reaction above)
    // spends his time; the native loop never ends by itself (it only walks closer / back with ZS_WatchFight_Goto /
    // GoBack). The hook gives the watchers their routine back when the 10 s window ran out without a second blow
    // (ContinueRoutine, the call the native ZS_WatchFight itself uses to leave), and keeps them turned to the hero.
    //
    ME_DEFINE_AND_REGISTER_SCRIPT_AI_STATE(GLF_PersonalBrawlExit)
    {
        return GroupLogic::Brawl::RunPersonalBrawlExit(a_rRunTimeStack, a_pSPU);
    }

    ME_DEFINE_AND_REGISTER_SCRIPT_AI_STATE(GLF_PersonalBrawlSpectator)
    {
        return GroupLogic::Brawl::RunPersonalBrawlSpectator(a_rRunTimeStack, a_pSPU);
    }

        ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_WatchFight_Loop)
    {
        return PluginRuntime::Loops::ZS_WatchFight_Loop(ZS_WatchFight_Loop_Original, a_rRunTimeStack, a_pSPU);
    }

    //
    // ZS_Attack_Loop: the loop every attacker runs. The NPCs the plugin sent against the hero (list of NoteAggro) are
    // released here once the hero is on his feet after a protected knock-out (g_bAggroRelease, set by the event "the
    // hero stands again"): they drop the target and the attack reason and go back to their routine, also those who ran
    // too far to see the fall and only arrive now. No timer: the check is one flag and a name lookup.
    //
    // ZS_Arena_Loop is the state of the NPC who waits in / walks to the ring before the fight starts: the hero can enter
    // with his companions at that moment, when nobody runs the attack loop yet.
        ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Arena_Loop)
    {
        return PluginRuntime::Loops::ZS_Arena_Loop(ZS_Arena_Loop_Original, a_rRunTimeStack, a_pSPU);
    }

    //
    // v1.0.66.0: THE CRASH ON A TRANSFORMATION IN THE RING (the cause was found in the engine logs ge3log5 / 6 / 7 / 10: the same stack
    // every time). The hero casts the transformation scroll: the engine removes his own body ("PC_Hero") and spawns the beast, which
    // becomes the player. Everybody who was sent against the hero (the witnesses of the crime, the opponent) still has the removed body
    // as the target, and the NATIVE ZS_Attack_Loop reads the property sets of it without a check: GetAttackRange (SelectCombatMove.cpp
    // of Script_Game.dll) -> eCEntity::GetPropertySet with ECX = 0 = EXCEPTION_ACCESS_VIOLATION in Engine.dll. The only frame of the
    // plugin in that call stack is the call of ZS_Attack_Loop_Original below. Two layers:
    //  1. a target that is the removed body of the hero (or gone, for an NPC that the plugin sent against the hero) is put onto the
    //     CURRENT player entity (the beast) before the native loop runs; with no player at all the attack is dropped;
    //  2. the call of the native loop is made under a structured exception handler: if it still faults, the NPC drops the attack and
    //     returns to his routine instead of the game going down. (No polling and no standing order for anybody: one-time checks only.)
    //
    namespace
    {


    } // namespace

        ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Attack_Loop)
    {
        return PluginRuntime::Loops::ZS_Attack_Loop(ZS_Attack_Loop_Original, a_rRunTimeStack, a_pSPU);
    }

    //
    //
    // THE COMPANIONS WAIT WHILE THE DUEL RUNS. When the duel quest of the arena becomes Running, every companion of the
    // hero (types Party / Mob / Slave: those who follow him by the native follow state, not summons, which just exist) gets
    // the native "wait" status - PSParty.Waiting = true and the task ZS_WaitForPlayer, exactly what the dialogue "wait
    // here" does (WaitPlayerParty). The state is remembered by NAME. Nothing is forced to follow again by the plugin
    // while the duel goes on. When the quest is over (the hero won, was knocked out or lost it, or cancelled it) every
    // remembered companion gets the native "follow me" back (PSParty.Waiting = false, ContinueRoutine). A companion that
    // the player has already told "follow me" by hand (Waiting = false) is not touched any more. Summons and tamed
    // animals are not put to wait: they are dismissed or fight (nothing) by the rules of the arena above.
    //
    namespace
    {
        GEInt const WAITER_MAX = 32;
        bCString g_arrWaiters[WAITER_MAX];
        GEInt g_iWaiterCount = 0;
        GEBool g_bDuelWasActive = GEFalse;

        // EVERY creature that goes with the hero waits: companions (Party), mob / slave followers, controlled and tamed
        // creatures and summons - all the party member types 1..5 (IsHeroControlled knows the hidden body of the hero too).
        GEBool IsFollowingCompanion(Entity &a_Npc)
        {
            return IsHeroControlled(a_Npc);
        }

        // The game message that the native "wait here" shows (GO_SlaveDismiss: "$(name) is waiting..."). The text comes
        // from the string table of the game, so it is in the language of the game; $(name) is the focus name of the NPC.
        void PrintCompanionWaitMessage(Entity &a_Npc)
        {
            eCLocString const Loc(bCString("GO_SlaveDismiss"));
            bCUnicodeString strMessage(static_cast<bCUnicodeString const>(Loc));
            bCUnicodeString strName = a_Npc.GetFocusName();
            if (strName.IsEmpty())
                strName = bCUnicodeString(a_Npc.GetName().GetText());
            strMessage.Replace(L"$(name)", strName.GetText());
            gui2.PrintGameMessage(strMessage, gEGameMessageType_Yellow);
        }

        GEInt FindWaiter(bCString const &a_strName)
        {
            for (GEInt i = 0; i < g_iWaiterCount; ++i)
                if (g_arrWaiters[i] == a_strName)
                    return i;
            return -1;
        }

        //
        // v1.0.68.0: WHO ORDERED THE WAITING (WaiterType). A companion that waits (PSParty.Waiting) waits either because the PLAYER said
        // "wait here" in the dialogue (USER: he is never carried by a teleport and never released by the plugin), or because the plugin /
        // the game did it (AUTO: the start of the arena duel, a carry that failed, a companion left far away after a teleport). An AUTO
        // waiter is carried to the hero by the next teleport whatever the native flag Waiting says. A waiting companion with no mark at
        // all (his order was given before this session: a save game keeps no marks) counts as USER - his wait is never broken.
        // g_arrWaiters (above) is the AUTO list of the CURRENT arena session; g_arrAutoWait is the AUTO list of every other origin.
        //
        enum mEWaiterType
        {
            mEWaiterType_None = 0,
            mEWaiterType_User = 1,
            mEWaiterType_Auto = 2,
        };
        // The plugin's own arena session (NOT the status of the quest in the journal: the game sets that to Running by itself after a
        // loss, and it must stay as it is, or the duel could not be taken again). True from the start of the duel until a cancellation,
        // the end of the duel, a teleport or the departure of the hero from the arena.
        GEBool g_bArenaSessionHold = GEFalse;
        bCVector g_vArenaSessionStart;
        GEFloat const ARENA_SESSION_LEAVE_DISTANCE = 8000.0f; // the hero is this far from the place where the duel was taken: it is over
        GEFloat const ARENA_ADOPT_RADIUS = 6000.0f;           // a save game made in the middle of a duel: the waiters this near are adopted
        // The session was cancelled (a loss, a cancellation, a teleport): the status Running that the game puts back by itself in the
        // ring does NOT open a new session; only the dialogue with the master (or leaving the ring) does.
        GEBool g_bArenaSessionCancelled = GEFalse;
        GEBool g_bArenaReleasePending = GEFalse; // the session is over, the AUTO waiters are released as soon as the hero is on his feet

        GEInt FindNameIn(bCString const *a_pArray, GEInt a_iCount, bCString const &a_strName)
        {
            for (GEInt i = 0; i < a_iCount; ++i)
                if (a_pArray[i] == a_strName)
                    return i;
            return -1;
        }

        void AddNameTo(bCString *a_pArray, GEInt &a_iCount, GEInt a_iMax, bCString const &a_strName)
        {
            if (a_iCount >= a_iMax || FindNameIn(a_pArray, a_iCount, a_strName) >= 0)
                return;
            a_pArray[a_iCount++] = a_strName;
        }

        void RemoveNameFrom(bCString *a_pArray, GEInt &a_iCount, bCString const &a_strName)
        {
            GEInt const iIndex = FindNameIn(a_pArray, a_iCount, a_strName);
            if (iIndex < 0)
                return;
            a_pArray[iIndex] = a_pArray[a_iCount - 1];
            a_pArray[a_iCount - 1] = bCString();
            --a_iCount;
        }

        GEBool g_bArenaEntryLoaded = GEFalse;
        GEBool g_bArenaEntryPending = GEFalse;
        bCVector g_vArenaEntryOrigin;
        GEFloat const ARENA_ENTRY_LEAVE_DISTANCE = 10000.0f;
        void CancelAbandonedArena(GELPCChar why, GEBool keepWaiting);
        Entity ArenaEntryMemory()
        {
            Entity body = Entity::GetEntity(bCString("PC_Hero"));
            if (body != None && body.PlayerMemory.IsValid()) return body;
            return Entity::GetPlayer();
        }
        void StoreEntryPending(GEBool enabled)
        {
            g_bArenaEntryPending = enabled;
            Entity memory = ArenaEntryMemory();
            if (memory == None || !memory.PlayerMemory.IsValid()) return;
            bCString const key("GLF_ArenaEntryPending");
            if (enabled) memory.PlayerMemory.SetGameEvent(key);
            else memory.PlayerMemory.ClearGameEvent(key);
        }

        GEBool g_bArenaEntryCenterKnown = GEFalse;
        GEU32 g_u32ArenaCenterRetry = 0;
        void ConsiderArenaZone(Entity &candidate, Entity &zone, GEBool &ambiguous)
        {
            if (candidate == None || !candidate.Arena.IsValid()) return;
            if (zone == None) zone = candidate;
            else if (zone != candidate) ambiguous = GETrue;
        }
        void ConsiderArenaAnchor(Entity &anchor, Entity &zone, GEBool &ambiguous)
        {
            if (anchor == None || IsHeroSide(anchor)) return;
            ConsiderArenaZone(anchor, zone, ambiguous);
            Entity containing = anchor.GetZone();
            ConsiderArenaZone(containing, zone, ambiguous);
        }
        void ConsiderArenaQuestEntity(bCString const &name, Entity &zone, GEBool &ambiguous)
        {
            if (name.IsEmpty()) return;
            Entity entity = Entity::GetEntity(name);
            ConsiderArenaAnchor(entity, zone, ambiguous);
            if (entity == None || !entity.IsNPC() || !entity.Navigation.IsValid()) return;
            Entity destination = entity.Navigation.GetAIGotoDestination();
            ConsiderArenaAnchor(destination, zone, ambiguous);
            destination = entity.Navigation.GetCurrentDestinationPoint();
            ConsiderArenaAnchor(destination, zone, ambiguous);
        }
        GEBool ResolveArenaEntryCenter(bCVector &center)
        {
            gCQuestManager_PS *manager = gCQuestManager_PS::GetInstance();
            if (!manager) return GEFalse;
            gCQuest_PS *quest = nullptr;
            GEInt count = 0;
            bTObjArray<bTAutoPOSmartPtr<gCQuest_PS>> const &quests = manager->GetQuests();
            for (GEInt i = 0; i < quests.GetCount(); ++i)
            {
                gCQuest_PS *entry = const_cast<gCQuest_PS *>(static_cast<gCQuest_PS const *>(quests.GetAt(i)));
                if (entry && entry->GetType() == gEQuestType_Arena && entry->GetStatus() == gEQuestStatus_Running)
                { quest = entry; ++count; }
            }
            Entity zone;
            GEBool ambiguous = count != 1;
            GELPCChar method = "quest entity/target zone";
            if (!ambiguous)
            {
                ConsiderArenaQuestEntity(quest->GetDestinationEntity(), zone, ambiguous);
                bTObjArray<bCString> const &entities = quest->GetDeliveryEntities();
                for (GEInt i = 0; i < entities.GetCount(); ++i)
                    ConsiderArenaQuestEntity(entities.GetAt(i), zone, ambiguous);
                // A unique native Running zone is evidence only with one Running arena quest.
                if (zone == None && !ambiguous)
                {
                    method = "unique native running arena";
                    bTObjArray<Entity> all = Entity::GetEntities();
                    for (GEInt i = 0; i < all.GetCount(); ++i)
                    {
                        Entity candidate = all.GetAt(i);
                        if (candidate != None && candidate.Arena.IsValid() && candidate.Arena.Status == gEArenaStatus_Running)
                            ConsiderArenaZone(candidate, zone, ambiguous);
                    }
                }
            }
            if (ambiguous || zone == None)
            {
                static GEInt failures = 0;
                if (failures++ < 20)
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-entry] center unresolved: runningQuests=%d ambiguous=%d zone='%s'; no guessed cancellation.\n",
                        count, static_cast<int>(ambiguous), zone != None ? zone.GetName().GetText() : "-");
                return GEFalse;
            }
            bCBox const &bounds = zone.GetInstance()->GetWorldNodeBoundary();
            if (!bounds.IsValid() || bounds.GetMax().m_fX <= bounds.GetMin().m_fX ||
                bounds.GetMax().m_fZ <= bounds.GetMin().m_fZ)
            {
                static GEInt badBounds = 0;
                if (badBounds++ < 20) DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-entry] center unresolved: zone='%s' has invalid/empty world bounds.\n", zone.GetName().GetText());
                return GEFalse;
            }
            bCVector const candidate = bounds.GetCenter();
            if (!std::isfinite(candidate.m_fX) || !std::isfinite(candidate.m_fZ))
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-entry] center unresolved: non-finite world center.\n");
                return GEFalse;
            }
            center = candidate;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-entry] quest='%s' zone='%s' world-boundary center=(%.0f,%.0f,%.0f), via %s.\n",
                quest->GetName().GetText(), zone.GetName().GetText(), center.m_fX, center.m_fY, center.m_fZ, method);
            return GETrue;
        }
        void BeginArenaEntry(Entity &hero)
        {
            (void)hero;
            g_bArenaEntryLoaded = GETrue;
            g_bArenaEntryCenterKnown = GEFalse;
            g_u32ArenaCenterRetry = 0;
            Entity memory = ArenaEntryMemory();
            if (memory != None && memory.PlayerMemory.IsValid())
                memory.PlayerMemory.SetGameEvent(bCString("GLF_ArenaEntryKnown"));
            StoreEntryPending(!IsHeroInsideArenaZone());
        }
        GEBool PollArenaEntry()
        {
            if (!g_bArenaEntryLoaded)
            {
                Entity memory = ArenaEntryMemory();
                if (memory == None || !memory.PlayerMemory.IsValid()) return GEFalse;
                g_bArenaEntryLoaded = GETrue;
                GEBool const known = memory.PlayerMemory.IsSetGameEvent(bCString("GLF_ArenaEntryKnown"));
                g_bArenaEntryPending = known
                    ? memory.PlayerMemory.IsSetGameEvent(bCString("GLF_ArenaEntryPending"))
                    : HasRunningArenaQuest() && !IsHeroInsideArenaZone();
                if (!known)
                {
                    memory.PlayerMemory.SetGameEvent(bCString("GLF_ArenaEntryKnown"));
                    StoreEntryPending(g_bArenaEntryPending);
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-entry] legacy save adopted: first-entry history unknown, pending=%d; center will be resolved from arena.\n", static_cast<int>(g_bArenaEntryPending));
                }
            }
            if (!g_bArenaEntryPending) return GEFalse;
            if (!HasRunningArenaQuest() || IsHeroInsideArenaZone() || g_bRingSeen)
            {
                StoreEntryPending(GEFalse);
                return GEFalse;
            }
            Entity hero = Entity::GetPlayer();
            if (hero == None) return GETrue;
            if (!g_bArenaEntryCenterKnown)
            {
                GEU32 const now = ::GetTickCount();
                if (g_u32ArenaCenterRetry != 0 && static_cast<GEInt>(now - g_u32ArenaCenterRetry) < 1000) return GETrue;
                g_u32ArenaCenterRetry = now | 1;
                g_bArenaEntryCenterKnown = ResolveArenaEntryCenter(g_vArenaEntryOrigin);
                if (!g_bArenaEntryCenterKnown) return GETrue;
            }
            bCVector const position = hero.GetPosition();
            GEFloat const dx = position.m_fX - g_vArenaEntryOrigin.m_fX;
            GEFloat const dz = position.m_fZ - g_vArenaEntryOrigin.m_fZ;
            if (dx*dx + dz*dz > ARENA_ENTRY_LEAVE_DISTANCE*ARENA_ENTRY_LEAVE_DISTANCE)
                CancelAbandonedArena("more than 10000 horizontally from arena center before entry", GETrue);
            return GETrue;
        }

        GEFloat DistanceFromSessionStart(bCVector const &a_vPosition)
        {
            GEFloat const fDx = a_vPosition.m_fX - g_vArenaSessionStart.m_fX;
            GEFloat const fDy = a_vPosition.m_fY - g_vArenaSessionStart.m_fY;
            GEFloat const fDz = a_vPosition.m_fZ - g_vArenaSessionStart.m_fZ;
            return std::sqrt(fDx * fDx + fDy * fDy + fDz * fDz);
        }

        mEWaiterType GetWaiterType(Entity &a_Npc)
        { return static_cast<mEWaiterType>(GroupLogic::Waiting::GetWaiterType(a_Npc)); }

        GELPCChar WaiterTypeText(mEWaiterType a_enuType)
        { return GroupLogic::Waiting::WaiterTypeText(static_cast<GroupLogic::WaiterType>(a_enuType)); }

        void ClearWaiterMarks(Entity &a_Npc)
        { GroupLogic::Waiting::ClearWaiterMarks(a_Npc); }

        GEBool IsSessionAutoWaiter(Entity &a_Npc)
        {
            return a_Npc != None && FindWaiter(a_Npc.GetName()) >= 0 ? GETrue : GEFalse;
        }

        // A waiting companion whose wait the plugin / the game did NOT order (or a wait of an older session) is the player's.
        GEBool IsUserWaiterOrUnmarked(Entity &a_Npc)
        {
            return GetWaiterType(a_Npc) != mEWaiterType_Auto ? GETrue : GEFalse;
        }

        // The player ordered "wait here": the wait is his, the plugin never breaks it.
        void MarkWaiterUser(Entity &a_Npc)
        { GroupLogic::Waiting::MarkWaiterUser(a_Npc); }

        // The plugin / the game ordered the wait: the next teleport carries him.
        void MarkWaiterAuto(Entity &a_Npc)
        { GroupLogic::Waiting::MarkWaiterAuto(a_Npc); }

        void ResetWaiterMarks();
        void MarkWaiterAutoPublic(Entity &a_Npc) { MarkWaiterAuto(a_Npc); }
        void MarkWaiterUserPublic(Entity &a_Npc) { MarkWaiterUser(a_Npc); }
        void ClearWaiterMarksPublic(Entity &a_Npc) { ClearWaiterMarks(a_Npc); }

        void ResetWaiterMarks()
        {
            GroupLogic::Waiting::ResetWaiterMarks();
            g_bArenaSessionHold = GEFalse;
            g_bArenaSessionCancelled = GEFalse;
            g_bArenaReleasePending = GEFalse;
                }

        // Called ONCE at the start of the duel quest (v1.0.27.0): a creature that is put to wait is remembered by name.
        // (not loaded yet after a save game, left behind after a teleport) and comes to the hero later is put to wait at
        // that moment. Every creature is put to wait once per duel (remembered by name): the one whom the player calls
        // "follow me" by hand during the duel is not put to wait again - and loses the duel if he comes into the ring.
        void MakeCompanionsWait(GEBool a_bDuelStart)
        {
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            GEInt iNew = 0;
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (!IsFollowingCompanion(Npc) || Npc.IsDead())
                    continue;
                bCString const strName = Npc.GetName();
                if (FindWaiter(strName) >= 0 || g_iWaiterCount >= WAITER_MAX)
                    continue;
                if (Npc.Party.Waiting)
                    continue; // one the player already told to wait is left alone and is NOT given back later
                GroupLogic::Waiting::RemoveUserMark(strName); // v1.0.68.0: a stale USER mark of an earlier wait
                GroupLogic::Waiting::RemoveAutoMark(strName);
                g_arrWaiters[g_iWaiterCount++] = strName;           // v1.0.68.0: WaiterType = AUTO, the waiter of THIS arena session
                Npc.Party.Waiting = GETrue;
                Npc.Routine.FullStop();
                Npc.Routine.SetTask("ZS_WaitForPlayer");
                PrintCompanionWaitMessage(Npc);
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] '%s' (party type %d) waits: the duel quest runs.\n",
                                             strName.GetText(), static_cast<int>(Npc.Party.PartyMemberType));
                ++iNew;
            }
            if (a_bDuelStart || iNew > 0)
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The duel quest runs: %d companion(s) of the hero wait now "
                                             "(%d in all).\n",
                                             static_cast<int>(iNew), static_cast<int>(g_iWaiterCount));
        }

        // The quest of the arena was taken from the master of the arena (status Running): the companions get the native
        // order "wait here" and the message, ONCE.
        // The dialogue manager runs a dialogue right now (gCInfoManager_PS::m_bIsRunning, +0x14, checked by the SDK asserts).
        GEBool IsDialogueRunning()
        {
            gCInfoManager_PS *pManager = gCInfoManager_PS::GetInstance();
            return pManager && *(reinterpret_cast<GEU8 const *>(pManager) + 0x14) != 0 ? GETrue : GEFalse;
        }

        // v1.0.68.0: after a loss / a cancellation the GAME puts the quest back to Running by itself (the master must be able to offer
        // the duel again), with no dialogue and with the hero still in the ring. That is not a new duel. The genuine retake is made in the
        // dialogue with the master (or at least outside the ring). The quest status of the journal is never touched by the plugin.
        GEBool IsArenaSessionGate()
        {
            return g_bArenaSessionCancelled && !g_bArenaSessionHold ? GETrue : GEFalse;
        }

        // a_bFromLost: the quest was Lost (6) a moment ago: this is the status the game restores by itself after a loss. A first take
        // (Open -> Running) is always a new duel; a retake from the master (Lost -> Running) is a new duel only inside a dialogue.
        void OnArenaQuestStarted(GEBool a_bFromLost)
        {
            if (a_bFromLost && IsArenaSessionGate() && !IsDialogueRunning())
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena: the quest is Running again (Lost -> Running) after a lost / cancelled duel "
                                             "and no dialogue runs - the game restored it by itself, this is NOT a new duel (no session, "
                                             "the companions are not put to wait, the journal is not touched).\n");
                CrashDiag::Note("arena: Lost -> Running restored by the game, no dialogue: NOT a new duel, no session");
                return;
            }
            g_bLostToThirdParty = GEFalse;
            g_bThirdPartyMessage = GEFalse;
            g_u32TabooUntil = 0;
            g_bDuelWasActive = GETrue;
            g_bArenaSessionHold = GETrue;       // bArenaSessionHold: the arena session of the plugin begins
            {
                Entity SessionHero = Entity::GetPlayer();
                if (SessionHero != None)
                {
                    g_vArenaSessionStart = SessionHero.GetPosition();
                    g_bRingSeen = GEFalse;
                    BeginArenaEntry(SessionHero);
                }
            }
            g_bArenaSessionCancelled = GEFalse;
            g_bArenaReleasePending = GEFalse;
            MakeCompanionsWait(GETrue);
        }

        // The AUTO waiters of the CURRENT arena session (g_arrWaiters) follow the hero again. Nobody else is touched: not the USER waiters,
        // not the AUTO waiters of other origins, not a companion who stands in another city (he stays where he is, as an AUTO waiter).
        void ReleaseWaitingCompanions(GELPCChar a_strWhy)
        {
            g_bArenaReleasePending = GEFalse;
            if (g_iWaiterCount == 0)
                return;
            Entity Hero = Entity::GetPlayer();
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            GEInt iBack = 0;
            GEInt iFar = 0;
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (FindWaiter(Npc.GetName()) < 0)
                    continue;
                // Still waiting by our order and still his companion: he follows the hero again. If the player has
                // already said "follow me", Waiting is false and there is nothing to do.
                if (!Npc.IsDead() && IsFollowingCompanion(Npc) && Npc.Party.Waiting)
                {
                    if (GetWaiterType(Npc) == mEWaiterType_User)
                        continue; // the player took this wait over by his own order: it is never broken
                    if (Hero != None && Npc.GetDistanceTo(Hero) > FAR_COMPANION_DISTANCE)
                    {
                        // v1.0.68.0: he stays in another city: no run across the map; his wait stays AUTO, a teleport carries him.
                        RemoveNameFrom(g_arrWaiters, g_iWaiterCount, Npc.GetName());
                        MarkWaiterAuto(Npc);
                        ++iFar;
                        continue;
                    }
                    Npc.Party.Waiting = GEFalse;
                    Npc.Routine.FullStop();
                    Npc.Routine.ContinueRoutine();
                    ++iBack;
                }
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The duel is over (%s): %d companion(s) follow the hero "
                                         "again, %d far one(s) stay where they are.\n",
                                         a_strWhy, static_cast<int>(iBack), static_cast<int>(iFar));
            for (GEInt i = 0; i < WAITER_MAX; ++i)
                g_arrWaiters[i] = bCString();
            g_iWaiterCount = 0;
        }

        // The arena SESSION of the plugin ends (a_iRelease: 0 the AUTO waiters stay AUTO, 1 they follow the hero as soon as he may be
        // released, 2 they follow him now). The quest of the journal is NOT touched.
        void EndArenaSession(GELPCChar a_strWhy, GEBool a_bCancelled, GEInt a_iRelease)
        {
            GEBool const bHad = g_bArenaSessionHold || g_iWaiterCount > 0;
            g_bArenaSessionHold = GEFalse; // bArenaSessionHold = false
            if (a_bCancelled)
                g_bArenaSessionCancelled = GETrue;
            if (bHad)
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena session ended (%s): %d AUTO waiter(s) of the session, release mode %d.\n",
                                             a_strWhy, static_cast<int>(g_iWaiterCount), static_cast<int>(a_iRelease));
            if (a_iRelease >= 2)
                ReleaseWaitingCompanions(a_strWhy);
            else if (a_iRelease == 1 && g_iWaiterCount > 0)
                g_bArenaReleasePending = GETrue;
        }

        //
        // v1.1.1.0: THE SAVE GAME WITH A RUNNING DUEL QUEST. A save game keeps the quest status (Running) and the flag Waiting of the squad,
        // but not the marks of the plugin: after the load every waiting companion had NO mark, and a waiter with no mark counts as the
        // player's own wait (USER: never broken). So at the end / the cancellation of the duel the plugin wrote "2 other waiter(s) are left
        // as they are" and the squad stayed on the tribune for good. Here, after the load, a companion that waits while the arena quest is
        // natively Running (1) is taken as an AUTO waiter of the arena session (g_arrWaiters): when the duel ends or is cancelled he is
        // released into ZS_FollowPlayer like one who was put to wait by the plugin itself. The quest status is not touched.
        // Only when the quest is Running: a waiter in a save without a running duel stays a wait of the player.
        //
        GEBool g_bArenaAdoptPending = GEFalse;
        GEU32 g_u32ArenaAdoptTick = 0;

        void ArmArenaAdoptAfterLoad()
        {
            g_bArenaAdoptPending = GETrue;
            g_u32ArenaAdoptTick = ::GetTickCount() | 1;
        }

        // a_bMember: he stands in the member list of the party of the hero (the list the native teleport script walks).
        GEBool AdoptWaiterOfLoadedDuel(Entity &a_Npc, Entity &a_Hero, GEBool a_bMember)
        {
            if (a_Npc == None || a_Npc == a_Hero || !a_Npc.IsNPC() || a_Npc.IsDead() || !a_Npc.Party.Waiting)
                return GEFalse;
            gEPartyMemberType const enuType = a_Npc.Party.PartyMemberType;
            GEBool const bPermanent = enuType == gEPartyMemberType_Party || enuType == gEPartyMemberType_Mob ||
                                      enuType == gEPartyMemberType_Slave;
            if (!bPermanent || !(IsHeroControlled(a_Npc) || a_bMember))
                return GEFalse; // only the companions of the types 1..3; the tamed / summoned ones follow other rules
            bCString const strName = a_Npc.GetName();
            if (FindWaiter(strName) >= 0)
                return GEFalse; // already an AUTO waiter of the session
            if (GroupLogic::Waiting::HasUserMark(strName))
                return GEFalse; // the player has said "wait here" since the load: his order stays
            // Only a companion that waits NEAR the hero (on the tribune of the arena) belongs to the arena session. A companion that waits
            // far away (another city: the order of the player, or a stone) keeps his wait - the running arena quest has nothing to do with him.
            if (a_Npc.GetDistanceTo(a_Hero) > ARENA_ADOPT_RADIUS)
                return GEFalse;
            if (g_iWaiterCount >= WAITER_MAX)
                return GEFalse;
            GroupLogic::Waiting::RemoveAutoMark(strName);
            g_arrWaiters[g_iWaiterCount++] = strName; // WaiterType = AUTO, the waiter of the arena session
            char strLine[300] = {};
            _snprintf_s(strLine, sizeof(strLine), _TRUNCATE,
                        "[GenomeLogicFix][teleport] save game | '%s' | party type %d | Waiting=1 | distance %.0f | WaiterType=AUTO | ADOPTED: "
                        "the arena quest is natively Running in the loaded save, so his wait is the arena session's, not the player's",
                        strName.GetText(), static_cast<int>(enuType), static_cast<double>(a_Npc.GetDistanceTo(a_Hero)));
            DebugLog::GetLogger().LogFormat("%s\n", strLine);
            CrashDiag::Note(strLine);
            return GETrue;
        }

        void PollSessionAdopt()
        {
            if (!g_bArenaAdoptPending)
                return;
            Entity Hero = Entity::GetPlayer();
            GEBool const bGiveUp = static_cast<GEInt>(::GetTickCount() - g_u32ArenaAdoptTick) > 6000;
            if (Hero == None || !HasRunningArenaQuest())
            {
                // The world / the quests may still be filling up right after the load: look again on the next poll, but not for ever.
                if (bGiveUp)
                {
                    g_bArenaAdoptPending = GEFalse;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Save game: no arena quest is Running - the waiting companions keep the "
                                                 "wait of the player.\n");
                }
                return;
            }
            g_bArenaAdoptPending = GEFalse;
            GEInt iAdopted = 0;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (AdoptWaiterOfLoadedDuel(Npc, Hero, GEFalse))
                    ++iAdopted;
            }
            {
                bTValArray<eCEntity *> arrMembers = Hero.Party.GetMembers(GEFalse);
                for (GEInt i = 0; i < arrMembers.GetCount(); ++i)
                {
                    eCEntity *pMember = arrMembers.GetAt(i);
                    if (!pMember)
                        continue;
                    Entity Member(static_cast<eCEntity const *>(pMember));
                    if (AdoptWaiterOfLoadedDuel(Member, Hero, GETrue))
                        ++iAdopted;
                }
            }
            if (iAdopted > 0)
            {
                // The arena session of the plugin is on, as if the duel had been taken in this run: a cancellation or the end of the duel
                // releases the squad (ReleaseWaitingCompanions: g_arrWaiters only, never a companion farther than 10000).
                g_bArenaSessionHold = GETrue;
                g_bArenaSessionCancelled = GEFalse;
                g_bArenaReleasePending = GEFalse;
                g_bDuelWasActive = GETrue;
                g_vArenaSessionStart = Hero.GetPosition();
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Save game: the arena quest is Running, %d waiting companion(s) adopted as AUTO waiters "
                                         "of the arena session.\n",
                                         static_cast<int>(iAdopted));
        }

        // The deferred release of an ended session: it waits until the hero may be released (he does not lie, the crowd is not punishing him).
        void PumpArenaRelease()
        {
            if (!g_bArenaReleasePending)
                return;
            if (g_iWaiterCount == 0)
            {
                g_bArenaReleasePending = GEFalse;
                return;
            }
            if (!IsArenaReleaseAllowed())
                return;
            ReleaseWaitingCompanions("the arena session is over and the hero is on his feet");
        }

        // Called from the loops that run all the time (the attack loop, the watch loop, the arena loop): the edge of the
        // duel quest (not running -> running, running -> not running) is noticed here. Technically throttled.
        void TrackDuelForCompanions()
        {
            // v1.0.27.0: the order to wait is given ONCE, at the moment the quest is taken from the master of the arena
            // (OnArenaQuestStarted, called from the status hook). Nothing is repeated in the frame loop any more, so the
            // player can take a creature with him by the dialogue "follow me" during the quest. Here only the END of the
            // quest is noticed: the waiting companions are released and the duel taboo gets its 30 seconds.
            static GEU32 s_u32Last = 0;
            GEU32 const u32Now = ::GetTickCount();
            if (s_u32Last != 0 && static_cast<GEInt>(u32Now - s_u32Last) < 400)
                return;
            s_u32Last = u32Now;
            PollSessionAdopt(); // v1.1.1.0: the waiters of a duel that was Running in the loaded save game become AUTO (one flag read when idle)
            PumpArenaRelease(); // v1.0.68.0: the deferred release of the AUTO waiters of an ended session
            // v1.0.68.0: the hero has gone far from the place where the duel was taken (another city, a teleport that the session did not
            // see): the arena session is over, the AUTO waiters of it follow him (the far ones stay as AUTO, a teleport carries them).
            GEBool const awaitingEntry = g_bArenaAdoptPending ? GETrue : PollArenaEntry();
            if (g_bArenaSessionHold && !awaitingEntry)
            {
                Entity SessionHero = Entity::GetPlayer();
                if (SessionHero != None && DistanceFromSessionStart(SessionHero.GetPosition()) > ARENA_SESSION_LEAVE_DISTANCE)
                    EndArenaSession("the hero left the arena", GEFalse, 2);
            }
            GEBool const bActive = HasRunningArenaQuest(); // the native status of the quest
            if (bActive)
            {
                if (!g_bDuelWasActive)
                {
                    // The quest is Running but the status hook did not see it (a save game was loaded in the middle of it).
                    g_bLostToThirdParty = GEFalse;
                    g_bThirdPartyMessage = GEFalse;
                }
                g_bDuelWasActive = GETrue;
                return;
            }
            if (!g_bDuelWasActive)
                return;
            // v1.0.43.0: the quest is over, the squad is released only when the hero is on his feet again (see
            // IsArenaReleaseAllowed); until then the edge stays pending and the companions keep waiting.
            if (!IsArenaReleaseAllowed())
                return;
            {
                ReleaseWaitingCompanions("the hero is on his feet again, the duel quest is not running");
                g_bLostToThirdParty = GEFalse;
                g_bThirdPartyMessage = GEFalse;
                g_u32TabooUntil = ::GetTickCount() + TABOO_AFTER_MS;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The duel is over: the opponent '%s' keeps away from the army "
                                             "for %u ms.\n",
                                             g_strArenaOpponent.GetText(), static_cast<unsigned>(TABOO_AFTER_MS));
            }
            g_bDuelWasActive = GEFalse;
        }

        void AnnounceLossToThirdParty()
        {
            if (g_bThirdPartyMessage)
                return;
            g_bThirdPartyMessage = GETrue;
            // v1.0.59.0: the artificial line "Бой проигран! Соперника победил не игрок!" is removed: the loss is silent, only logged.
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The duel is lost: the opponent was not beaten by the hero.\n");
        }

        void ResetDuelState()
        {
            for (GEInt i = 0; i < WAITER_MAX; ++i)
                g_arrWaiters[i] = bCString();
            g_iWaiterCount = 0;
            g_bArenaEntryLoaded = GEFalse;
            g_bArenaEntryCenterKnown = GEFalse;
            g_u32ArenaCenterRetry = 0;
            g_bArenaEntryPending = GEFalse; // Persistent entry events are read from the next world's player memory.
            ResetWaiterMarks(); // v1.0.68.0: a new world: no marks, no arena session (a waiting companion without a mark counts as USER)
            g_bArenaAdoptPending = GEFalse; // v1.1.1.0: SessionStart arms it again right after the load, once the world is reset
            g_bDuelWasActive = GEFalse;
            g_bPollArenaWasRunning = GEFalse;
            g_bRingSeen = GEFalse;
            ResetMagicJudge();
            g_bLostToThirdParty = GEFalse;
            g_bThirdPartyMessage = GEFalse;
        }
    } // namespace

    //
    // The fight of a creature that the hero CONTROLS is a fight to the death only against a RED enemy of the hero. The
    // rule is made from a state, not from a faction: every way of putting a creature under the control of the hero ends
    // in the party of the hero with a party member type (the game writes it itself):
    //  Party (1) - a companion (JoinPlayerParty, ordinary and quest ones), Mob (2), Slave (3) - the other joins,
    //  Controlled (4) - Word of Control and Tame Animal, Summoned (5) - every summoned creature.
    //  - CONTROLLED = the party leader is the hero and the party member type is not None. The political alignment does not
    //    matter at all.
    //  - RED = the attitude of the creature to the hero is Hostile / Predator. A controlled creature may kill only a red
    //    one (a humanoid, a beast or a monster alike): the blow on anybody else is not deadly (IsDeadlyDamage) and the
    //    finishing blow is refused (HasAttackReasonToKill).
    //  - Primary aggression of the alignment None (wild animals, monsters): such a creature who is not red for the hero
    //    does not take a controlled creature (or the hero in the beast form) for an enemy (GetAttitude says Neutral).
    //
    namespace
    {
        // The hero is in a transformed body. PSNpc::IsTransformed (HasStatusEffects 0x10) is NOT reliable here: the log of
        // a test with the demon potion has no beast-form line at all, so the game's flag was false for GetPlayer(). The
        // sure sign is the body itself: the transformation makes the spawned creature the player (the hits of the hero came
        // from "Transform_Demon_Potion"), and the own body of the hero is the entity "PC_Hero".


        // v1.0.48.0: THE SAFETY LOCK of every duel hook (the preemptive warning, the detectors of the damage of the arena): the
        // hero is in a monster body, or is not a humanoid, or does not exist -> the duel checks are skipped and the original
        // code runs. The body of a form is not an NPC for the wrapper (Hero.NPC / Hero.Routine must not be touched then), so
        // nothing of the hero is read before this answer. Pure reads, no crash on a missing property set.
        GEBool IsHeroDuelBlocked()
        {
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return GETrue;
            if (IsHeroTransformedNow())
                return GETrue;
            if (Hero.IsNPC() && !AI_Decision::IsHumanoid(Hero))
                return GETrue;
            return GEFalse;
        }



    } // namespace

    //
    // Summon spells in the arena. A summon (creature, container of damage, a custom spell built on MagicSummon) does not
    // hit the opponent with a missile, so no hit event tells about it - the cast itself does. The cast functions of the
    // summon family (the FuncOnCast script of the spell, MagicSummon* / MagicCallAnimals) are hooked: when the hero casts
    // one while a duel is on, the crowd reacts at once, exactly as for a forbidden elemental spell. No spell name is used.
    //
    namespace
    {

    } // namespace

#define GLF_SUMMON_HOOK(NAME) \
    ME_DEFINE_AND_HOOK_SCRIPT(NAME) \
    { \
        return GroupLogic::Lifecycle::SummonCast(NAME##_Original, #NAME, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs); \
    }

    //
    // Healing spells. The heal of the hero himself is the same as a healing consumable (a potion, food) and ends the duel
    // exactly like it (OnHeroConsumeStarted). The rule is made from the data of the spell, not from its name, and is
    // kept as one function so that it can be used elsewhere:
    //  - its hit script (FuncOnTargetHit) is MagicHeal: the spell restores the health of the caster (MagicHealOther,
    //    MagicCurePoison, MagicCureDisease ... are other scripts and are NOT healing in this sense);
    //  - its CastType is Regular (the normal cast). The fast variants (Power / Immediate), learned very late, are left
    //    alone on purpose.
    //
    namespace
    {


    } // namespace

#define GLF_HEAL_HOOK(NAME) \
    ME_DEFINE_AND_HOOK_SCRIPT(NAME) \
    { \
        return GroupLogic::Lifecycle::HealCast(NAME##_Original, #NAME, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs); \
    }

    GLF_HEAL_HOOK(MagicHeal)
    GLF_HEAL_HOOK(MagicCurePoison)
    GLF_HEAL_HOOK(MagicCureDisease)

    namespace
    {




    } // namespace

    //
    // PRIMARY AGGRESSION OF THE FACTION NONE AGAINST A SUMMON. A wild animal, a slave, a neutral (political alignment
    // None) must not be the first to attack, or to choose as its target, a creature summoned by the hero (party type 5:
    // skeleton, ogre, demon, ...). It ignores the summon until the summon hurts it first (the summon is then stored as
    // the current / last attacker of the NPC and the rule lets go). The other factions keep their native rules.
    //
    namespace
    {


        // ONE FOR ALL, ALL FOR ONE: the outsiders who attack a member of the army, remembered by name for a while.



    } // namespace
    namespace
    {
        // FOCUS RECORDS: who (an outsider) took whom (a member of the army) for a target in AssessTarget, and when.




    } // namespace
    //
    // v1.0.36.0: TWO POLITICAL FILTERS of the automatic aggression (AssessTarget / GetAttitude / GetAttitudeToPlayer).
    //  5. SLAVE vs NONE: a peaceful city NPC with PoliticalAlignment None never starts the aggression (and answers Neutral)
    //     against a target with PoliticalAlignment Slave when the target is (a) the hero in a transformed body, (b) a summon
    //     of the hero (party types 1..5) or (c) a tamed animal of the party (types 1..5). Another faction of the NPC: the
    //     plugin does not interfere.
    //  6. WANDERERS (Druids / Rangers): the engine has no faction enum value for them, so they are known by the name of the
    //     entity (the Wanderers are found by the political alignment Mid, not by name). Their automatic aggression against the
    //     hero in ANY form of transformation is blocked: for them it is a human in a beast skin, the attitude is the same
    //     as to the hero in his own body. It lasts until the hero attacks them first (the native fight rules then apply).
    //
    namespace
    {







    } // namespace

    //
    // v1.0.40.0: THE BUILT-IN WEAPONS OF THE FORMS ARE NO WEAPONS. The claws / fists / bites of a transformed hero (a demon, a
    // shadowbeast, a wolf ...) are for the engine a weapon of the melee that is always drawn, so every NPC of the world takes
    // it for a threat: the native perception scripts (disassembly of Script_Game.dll 1.8.7)
    //   AssessPlayerWeapon -> SetTask("ZS_ReactToWeapon") / SetTask("ZS_ObserveSuspect")   (the warning, the weapon is drawn)
    //   AssessWarn         -> SetTask("ZS_ReactToWeapon") / SetTask("ZS_ObserveSuspect")
    //   CanThreaten        -> the decision "may he threaten"
    // are cut off for the case where the target is the HERO in a form (Transform_*). Nothing else is touched: the answer is
    // "nothing happened" (0 / false). The filter lets go at once when the NPC already fights the hero or takes him for an enemy
    // (attitude Angry / Hostile / Predator, attack reason / target = the hero) or when the hero (any form) or his creature has
    // already hurt the NPC: after the first blow of the hero the native rules apply in full.
    //
    namespace
    {


    } // namespace

        ME_DEFINE_AND_HOOK_SCRIPT(AssessPlayerWeapon)
    {
        return SocialLogic::Hooks::AssessPlayerWeapon(AssessPlayerWeapon_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

        ME_DEFINE_AND_HOOK_SCRIPT(AssessWarn)
    {
        return SocialLogic::Hooks::AssessWarn(AssessWarn_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

        ME_DEFINE_AND_HOOK_SCRIPT(CanThreaten)
    {
        return SocialLogic::Hooks::CanThreaten(CanThreaten_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // The choice of a target (the script that switches an NPC to ZS_Attack).
    //  1. A creature under the control of the hero (party types 1..5) never takes the hero in ANY form (demon, wolf,
    //     ...), the body of the form or another creature of his army for a target.
    //  2. An NPC of the faction None never chooses a summon of the hero as a target before the summon hurts him first.
    //  3. When an outsider takes a member of the army for a target, the free members of the army defend him.
        ME_DEFINE_AND_HOOK_SCRIPT(AssessTarget)
    {
        return PluginRuntime::Combat::AssessTarget(AssessTarget_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    namespace
    {

    } // namespace

    // AssessHit(victim, damaging object): the reaction of a creature to a blow (OnDamage calls it directly, not only
    // through DoLogicalDamage). v1.0.23.0: the native rules of the brawl are RESTORED - the blow of the hero on his own
    // creature is registered by the engine as it is (no artificial ignoring). One thing is made for every form of the
    // hero: the native script judges the attacker by his weapon slots, his damage and his own attack counter, which a
    // transformed body (a demon, a wolf, ...) does not have. So a MELEE blow of the transformed hero on a creature of his
    // army is handed to the native script in the name of his human body PC_Hero: the engine then sees the standard hero.
    // A blow of an outsider on a member of the army calls the pack only when it was purposeful (see below).
        ME_DEFINE_AND_HOOK_SCRIPT(AssessHit)
    {
        return PluginRuntime::Combat::AssessHit(AssessHit_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // AssessAgressor(victim, attacker): called by every stumble state after a blow. THE NATIVE RULE (disassembly, 1.8.7): for
    // a creature of the party of the hero the answer to the hero is AssessTarget(self, hero, ReactToDamage) at once, i.e.
    // ZS_Attack from the FIRST blow (the log of the game: +359 ms after the arrow the skeleton was in ZS_Attack). A "green"
    // NPC does it otherwise: the first blow is borne (the native ZS_ReactToDamage: the weapon is drawn, the fighting
    // stance, the manoeuvring in front of the hero), and only the second blow within BRAWL_WINDOW_MS accepts the fight.
    //  - first blow  -> ZS_ReactToDamage against the hero (the native answer of the party is NOT run);
    //  - second blow -> the native answer (the fight; the brawl slot is open since OnHeroBlowOnArmy);
    //  - the hero is knocked out -> nothing is run, the creature goes back to the routine of following.
        ME_DEFINE_AND_HOOK_SCRIPT(AssessAgressor)
    {
        return PluginRuntime::Combat::AssessAgressor(AssessAgressor_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // ZS_Stumble: the native stumble of a creature that was hit. It takes its attacker from the creature itself
    // (GetCurrentAttacker) and the stumble animation is made relative to him. When the attacker is the transformed hero,
    // the engine gets his human body PC_Hero for the time of the call, exactly as for the standard hero.
        ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Stumble)
    {
        return GroupLogic::Lifecycle::ZS_Stumble(ZS_Stumble_Original, a_rRunTimeStack, a_pSPU);
    }

    // NO LOOTING AND NO FINISHING OF THE MASTER. When the hero is down, a creature of his own party (types 1..5) does
    // not search his pockets and does not strike the final blow: WantToRansack / HasAttackReasonToKill answer "no"
    // (see the hooks below), and the two routines themselves are left at once if they were started anyway.
    namespace
    {


    } // namespace

        ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_RansackBody)
    {
        return GroupLogic::Lifecycle::ZS_RansackBody(ZS_RansackBody_Original, a_rRunTimeStack, a_pSPU);
    }

        ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Attack_FinishingAttack)
    {
        return GroupLogic::Lifecycle::ZS_Attack_FinishingAttack(ZS_Attack_FinishingAttack_Original, a_rRunTimeStack, a_pSPU);
    }
    // AssessDefeat(self, other): the perception of a defeated fighter (it starts the looting / finishing reaction).
    // A creature of the party does not take its own fallen master for a victim.
        ME_DEFINE_AND_HOOK_SCRIPT(AssessDefeat)
    {
        return PluginRuntime::Combat::AssessDefeat(AssessDefeat_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

        ME_DEFINE_AND_HOOK_SCRIPT(AssessTalk)
    {
        return GroupLogic::Lifecycle::AssessTalk(AssessTalk_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    

        ME_DEFINE_AND_HOOK_SCRIPT(IsDeadlyDamage)
    {
        return PluginRuntime::Combat::IsDeadlyDamage(IsDeadlyDamage_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // The "human view" mirror of the beast form (clearing the transformed bit of the hero around GetAttitudeToPlayer) was
    // REMOVED: it wrote the status effects of the hero on every call (once per NPC per frame), which re-initialises the
    // visual animation of the transformed body - the crash of the game (EXCEPTION_ACCESS_VIOLATION in the Engine.dll
    // animation code) came right after it was added - and it was never verified that the checked object is the hero.

    //
    // GetAttitudeToPlayer(npc): the attitude of an NPC to the hero. The native aggression of the world is NOT touched
    // (a citizen, a guard, a slave attacks the hero in any form of transformation when the game says so). Only two
    // corrections are made, both for an aggressive answer:
    //  1. A creature under the control of the hero (party types 1..5, IsHeroControlled knows the hidden body PC_Hero)
    //     is never aggressive to him, in any form: Friendly.
    //  2. Primary aggression of the faction None (wild animals, slaves, neutrals) against a summon of the hero (party
    //     type 5) is not allowed: Neutral until the summon hurts the NPC first.
    //
        ME_DEFINE_AND_HOOK_SCRIPT(GetAttitudeToPlayer)
    {
        return PluginRuntime::Combat::GetAttitudeToPlayer(GetAttitudeToPlayer_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // GetAttitude(self, other): who is friend / enemy of whom.
    //  - Inside the army, and towards the master in ANY form of transformation: Friendly (always, not only when the
    //    native answer was aggressive - the name of a companion stays blue).
    //  - A creature of the army takes an enemy who attacks one of the army (see DefendAlly) for Hostile.
    //  - Faction None never takes a summon of the hero for an enemy before the summon hurts it first.
        ME_DEFINE_AND_HOOK_SCRIPT(GetAttitude)
    {
        return PluginRuntime::Combat::GetAttitude(GetAttitude_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // The colour of the name under the cursor. A creature of the army is blue (Friendly) in any form of the hero: the
    // native script compares the party leader with the player, which is the transformed body, not PC_Hero.
        ME_DEFINE_AND_HOOK_SCRIPT(GetFocusNameAttitude)
    {
        return PluginRuntime::Combat::GetFocusNameAttitude(GetFocusNameAttitude_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    //
    // The shouts of the witnesses of a brawl. StartSayAttackReason(npc, target) picks the voice line from the attack reason
    // of the NPC: Murder (13) -> "Murder", Enemy (12) -> "DieEnemy_0x". The plugin gives the victim the reason Murder and the
    // witnesses the reason Enemy in stage 2 of the city reaction, so they shout the lines of a killing although nobody died.
    // While the brawl goes on (stage 2) and its victim is alive, the reason is replaced with StopFight (8) for the time of
    // the call (the line "WillYouStopFighting", a shout about a fight) and put back at once. The reason itself, which
    // drives the behaviour of the NPC, is not changed. There is no state "ZS_CallGuards" and no SVM line "Guards!" /
    // "Help!" in Script_Game.dll, so no such shout can be made by the plugin.
    //
    namespace
    {


    } // namespace

        ME_DEFINE_AND_HOOK_SCRIPT(StartSayAttackReason)
    {
        return SocialLogic::Hooks::StartSayAttackReason(StartSayAttackReason_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    GLF_SUMMON_HOOK(MagicSummon)
    GLF_SUMMON_HOOK(MagicSummonDemon)
    GLF_SUMMON_HOOK(MagicSummonSkeleton)
    GLF_SUMMON_HOOK(MagicSummonGolem)
    GLF_SUMMON_HOOK(MagicSummonGoblin)
    GLF_SUMMON_HOOK(MagicSummonArmyOfDarkness)
    GLF_SUMMON_HOOK(MagicSummonMonster)
    GLF_SUMMON_HOOK(MagicSummonCompanion)
    GLF_SUMMON_HOOK(MagicSummonArmyOfCuteness)
    GLF_SUMMON_HOOK(MagicCallAnimals)

    // v1.0.50.0: THE LOSS OF CONTROL IS NATIVE. The plugin keeps no tracker of the tamed / summoned creatures and prints no
    // message about them (no "leaves the group"): the dismissal of the summons, the return home of the tamed ones and every
    // message of the game about it are made by the native scripts of Script_Game.dll alone.
    namespace
    {

        //
        // v1.0.41.0: THE TWO STAGES OF THE MAGIC IN THE ARENA, driven by the states of the hero (OnHeroRoutineChange).
        //  Stage 1, the warning: the arena quest is Running, the hero is in the ring and takes the magic in the hand (the task
        //  PS_Magic: a rune / a scroll). The opponent of the duel - and nobody else - answers exactly as a green NPC of the city
        //  answers the FIRST unanswered blow: the native state ZS_ReactToDamage (the attack is blocked, the weapon is drawn,
        //  he backs off and keeps the distance, he does not strike). At the moment he goes into the stance he says ONCE the
        //  native SVM "StopMagic" (the pool of the grey NPCs, speech type 9: the call of ZS_ReactToWeapon_WaitForSecondWarn).
        //  His attack reason of the duel (Arena / Duel) is NOT touched: he stays the opponent of the arena. When the hero puts
        //  the magic away, the opponent returns to the melee duel (ZS_Attack).
        //  Stage 2, the punishment: the cast state (PS_Magic_QuickCast / Cast / PowerCast / Summon) is the "second blow".
        //
        bCString g_strWarnOpponent;                           // the opponent in the warning stance (by name), empty: nobody
        GEBool g_bWarnWasFighting = GEFalse;                  // he was fighting (ZS_Attack / combat mode) when it began
        gEAttackReason g_enuWarnReason = gEAttackReason_None; // his attack reason of the duel, given back afterwards
        GEBool g_bWarnWasWalking = GEFalse;                       // he was still on his way to the ring (the state ZS_Arena)
        GEBool g_bMagicThreat = GEFalse;
        GEBool g_bMagicPunished = GEFalse; // the crime of the magic is declared for the quest that runs (cleared when it is over)
        GEBool g_bTransformReset = GEFalse; // v1.0.48.0: the duel was reset by a transformation spell (one reset per quest)
        // v1.0.58.0: THE TWO-STAGE CONTROL OF THE FORBIDDEN WEAPON (a rune / a scroll, a bow, a crossbow) IN THE RING. No cooldowns.
        //  1. The FIRST draw of the duel: the stance of stage 1 + the shout "StopMagic" and a timer WARN_TIMER_MS:
        //       a) the hero puts the weapon away within the timer -> the timer stops, the duel goes on as the melee duel;
        //       b) the hero still holds it when the timer is out -> the duel is CANCELLED (the quest is closed as Lost, the opponent
        //          leaves the combat and the ring, the squad is released; no crime).
        //  2. The SECOND draw of the same duel (after the warning was given and the weapon put away): no warning, no timer - stage 2
        //     at once: the local crime (Mercy armed, the witnesses around the ring attack the hero) and the duel quest is closed.
        //     The shot / the cast itself (PS_Ranged_Shoot, PS_Magic_*Cast) is stage 2 at once as well.
        GEU32 const WARN_TIMER_MS = 5000;
        GEBool g_bWarnUsed = GEFalse;    // the warning of this duel was already given: the next draw is stage 2
        GEU32 g_u32WarnDeadline = 0;     // 0: no timer; else the tick when the hero must have put the weapon away
        GEU32 g_u32WarnPollTick = 0;     // the previous poll of the timer (a gap = the game was paused: the timer waits)
        GEBool g_bAwaitPutAway = GEFalse; // the warning ended; the weapon is still read in the routine of the hero: not a new draw yet
        // v1.0.59.0: THE VOICE OF THE WARNING. The native state ZS_ReactToDamage (the stance of stage 1) says "WhatAreYouDoing" ("Эй, что
        // делаешь?") every time it is entered: Script_Game.dll 1.8.7, RVA 0x208A7 "push offset aWhatAreYouDoing" and then
        // StartSaySVM(type 10). The name of the line is read from that pointer, so PatchReactToDamageSvm (below) points the instruction at
        // the buffer g_arrReactSvm. The buffer holds "WhatAreYouDoing" (the native behaviour of every other NPC) except while the warning of
        // the arena lasts: then it holds the lines of the native warning of the weapons (ZS_ReactToWeapon_WaitForSecondWarn): "StopMagic"
        // for a rune / a scroll, "WeaponDown" for a bow / a crossbow. The opponent says it by the stance itself (once per entering it).
        // When the hero puts the weapon away the opponent says "Good" (EndMagicThreat; v1.0.61.0: was "WiseMove").
        GEBool g_bWarnRanged = GEFalse; // the forbidden weapon of the warning is a bow / a crossbow (else magic)
        char g_arrReactSvm[32] = "WhatAreYouDoing";

        void SetReactToDamageSvm(GELPCChar a_strName)
        {
            strncpy_s(g_arrReactSvm, sizeof(g_arrReactSvm), a_strName, _TRUNCATE);
        }

        GEBool g_bReactSvmPatched = GEFalse; // the "push offset" of ZS_ReactToDamage points at g_arrReactSvm

        // Points the operand of "push offset aWhatAreYouDoing" (Script_Game.dll 1.8.7, RVA 0x208A7: 68 <imm32>) at g_arrReactSvm. The
        // operand is checked first: it must lie inside the module and the string it points at must be exactly "WhatAreYouDoing"
        // (the DLL is rebased by the loader, so the value is compared through the string, not through the file address). On any
        // other build nothing is written (the stance then keeps its native line and BeginMagicThreat shouts the warning itself).
        void PatchReactToDamageSvm()
        {
            HMODULE hModule = ::GetModuleHandleA("Script_Game.dll");
            if (!hModule)
                return;
            GEU8 *pBase = reinterpret_cast<GEU8 *>(hModule);
            GEU8 *pPush = pBase + 0x208A7;
            if (pPush[0] != 0x68)
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] ZS_ReactToDamage: the code differs (opcode %02X): the line of the "
                                             "stance is left as it is.\n",
                                             pPush[0]);
                return;
            }
            GEU8 *const *ppOperand = reinterpret_cast<GEU8 *const *>(pPush + 1);
            GEU8 const *pString = *ppOperand;
            GEU8 const *pEnd = pBase + 0x139000; // the image of the module (SizeOfImage 0x139000)
            if (pString < pBase || pString + 16 > pEnd || strcmp(reinterpret_cast<char const *>(pString), "WhatAreYouDoing") != 0)
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] ZS_ReactToDamage: the operand does not point at \"WhatAreYouDoing\": "
                                             "the line of the stance is left as it is.\n");
                return;
            }
            DWORD dwOld = 0;
            if (!::VirtualProtect(pPush + 1, 4, PAGE_EXECUTE_READWRITE, &dwOld))
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] ZS_ReactToDamage: cannot unlock the code for the line patch.\n");
                return;
            }
            *reinterpret_cast<GEU8 const **>(pPush + 1) = reinterpret_cast<GEU8 const *>(g_arrReactSvm);
            DWORD dwDummy = 0;
            ::VirtualProtect(pPush + 1, 4, dwOld, &dwDummy);
            ::FlushInstructionCache(::GetCurrentProcess(), pPush + 1, 4);
            g_bReactSvmPatched = GETrue;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] ZS_ReactToDamage: the line of the stance is replaceable (patched).\n");
        }

        void ResetMagicJudge()
        {
            g_bWarnRanged = GEFalse;
            SetReactToDamageSvm("WhatAreYouDoing");
            g_bAwaitPutAway = GEFalse;
            g_bWarnUsed = GEFalse;
            g_u32WarnDeadline = 0;
            g_u32WarnPollTick = 0;
            g_strWarnOpponent = bCString();
            g_bWarnWasWalking = GEFalse;
            g_bWarnWasFighting = GEFalse;
            g_enuWarnReason = gEAttackReason_None;
            g_bMagicThreat = GEFalse;
            g_bMagicPunished = GEFalse;
            g_bTransformReset = GEFalse;
        }

        GEBool IsWarnedOpponent(Entity &a_Npc)
        {
            return g_bMagicThreat && !g_strWarnOpponent.IsEmpty() && a_Npc != None && a_Npc.GetName() == g_strWarnOpponent
                       ? GETrue
                       : GEFalse;
        }

        // The stance: the same three steps the native AI takes for the first blow (FullStop, the hero as the target, the task).
        void EngageWarning(Entity &a_Opponent, Entity &a_Hero)
        {
            a_Opponent.Routine.FullStop();
            a_Opponent.NPC.SetCurrentTarget(a_Hero);
            a_Opponent.Routine.SetTask("ZS_ReactToDamage");
        }

        // a_pHint (v1.0.47.0): the NPC that is known to be the opponent of the duel (the state ZS_Arena / the attack loop of the
        // arena runs on him) - used before the native code has written his attack reason, i.e. in the very first frame.
        void BeginMagicThreat(Entity *a_pHint = nullptr)
        {
            if (IsHeroDuelBlocked())
                return; // v1.0.48.0: the hero in a monster body: no duel checks, the original runs
            // v1.0.58.0: a call WITHOUT a hint is an event of the routine of the hero = a real draw of the weapon. A call WITH a hint
            // is a read of the state by a loop of the opponent: while the weapon of the ended warning is still read there (the
            // routine has not changed yet) it is the SAME weapon, not a second draw.
            if (!a_pHint)
                g_bAwaitPutAway = GEFalse;
            else if (g_bAwaitPutAway)
                return;
            if (g_bMagicThreat || g_bMagicPunished || !HasRunningArenaQuest() || !IsHeroInsideArenaZone() ||
                IsHeroKnockedOut())
                return; // after the crime of the magic there is no warning any more: everybody already fights
            // v1.0.58.0: THE SECOND DRAW of the same duel: no warning and no timer - stage 2 at once (the crime, the quest closed).
            if (g_bWarnUsed)
            {
                ArenaStageTwo("the forbidden weapon is drawn for the second time", "a forbidden weapon is drawn again in the arena");
                return;
            }
            Entity Hero = Entity::GetPlayer();
            Entity Opponent;
            if (a_pHint && *a_pHint != None)
                Opponent = *a_pHint;
            else if (!FindArenaOpponent(Opponent) && !FindRememberedOpponent(Opponent))
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, stage 1: the hero is in the magic mode, but the "
                                             "opponent is not found.\n");
                return;
            }
            if (Opponent.IsDead() || Opponent.IsDown())
                return;
            gEAIMode const enuMode = Opponent.Routine.AIMode;
            g_bWarnWasWalking = HasTask(Opponent, "ZS_Arena");
            g_bWarnWasFighting = HasTask(Opponent, "ZS_Attack") || enuMode == gEAIMode_Combat || enuMode == gEAIMode_Arena;
            g_enuWarnReason = Opponent.NPC.AttackReason;
            if (g_enuWarnReason == gEAttackReason_None)
                g_enuWarnReason = gEAttackReason_Arena; // not written yet by the native state: the duel reason is given back
            g_strWarnOpponent = Opponent.GetName();
            g_bMagicThreat = GETrue;
            g_bWarnUsed = GETrue; // v1.0.58.0: this duel has had its warning; the next draw is stage 2
            g_u32WarnDeadline = (::GetTickCount() + WARN_TIMER_MS) | 1; // v1.0.58.0: the timer of the warning
            g_u32WarnPollTick = ::GetTickCount() | 1;
            g_bWarnRanged = IsHeroRangedMode(Hero); // v1.0.59.0: a bow / a crossbow (PS_Ranged*) or magic
            // The stance says the line of the warning of the weapon INSTEAD of "WhatAreYouDoing" (see PatchReactToDamageSvm): the name is
            // set BEFORE the stance is taken, there is no second, explicit shout.
            GELPCChar const strSvm = g_bWarnRanged ? "WeaponDown" : "StopMagic";
            SetReactToDamageSvm(strSvm);
            EngageWarning(Opponent, Hero);
            // The code of the game could not be patched: the shout is made by hand (the native "WhatAreYouDoing" cannot be removed).
            if (!g_bReactSvmPatched)
                ::StartSaySVM(Opponent, Hero, bCString(strSvm), 9);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, stage 1: the hero holds a forbidden weapon (%s) - the opponent '%s' "
                                         "takes the stance of the first blow (ZS_ReactToDamage) and warns him (%s); "
                                         "%u ms to put the weapon away.\n",
                                         g_bWarnRanged ? "bow / crossbow" : "magic", Opponent.GetName().GetText(), strSvm,
                                         static_cast<unsigned>(WARN_TIMER_MS));
        }

        void EndMagicThreat(GEBool a_bApprove)
        {
            if (!HasRunningArenaQuest())
            {
                g_bMagicPunished = GEFalse; // the quest is over: the next one starts clean
                g_bTransformReset = GEFalse;
                g_bWarnUsed = GEFalse;      // v1.0.58.0: the next duel gets its own warning
                g_u32WarnDeadline = 0;
                g_bAwaitPutAway = GEFalse;
            }
            if (!g_bMagicThreat)
                return;
            // v1.0.58.0: the weapon is put away (or the warning ended otherwise): the timer stops; the same weapon still read from
            // the stale state of the routine must not be taken for a NEW draw until it is really away (g_bAwaitPutAway).
            g_u32WarnDeadline = 0;
            g_bAwaitPutAway = IsHeroHoldingMagic();
            g_bMagicThreat = GEFalse;
            SetReactToDamageSvm("WhatAreYouDoing"); // v1.0.59.0: the stance says its own native line again (for everybody else)
            g_bWarnRanged = GEFalse;
            Entity Opponent = g_strWarnOpponent.IsEmpty() ? Entity() : Entity::GetEntity(g_strWarnOpponent);
            GEBool const bFight = g_bWarnWasFighting;
            GEBool const bWalking = g_bWarnWasWalking;
            gEAttackReason const enuReason = g_enuWarnReason;
            g_strWarnOpponent = bCString(); // from now on nothing holds him: the code of the duel runs again
            g_bWarnWasWalking = GEFalse;
            g_bWarnWasFighting = GEFalse;
            g_enuWarnReason = gEAttackReason_None;
            if (Opponent == None || !Opponent.IsNPC() || Opponent.IsDead() || Opponent.IsDown() || IsHeroKnockedOut() ||
                !HasRunningArenaQuest())
                return;
            Entity Hero = Entity::GetPlayer();
            Opponent.Routine.FullStop();
            Opponent.NPC.SetCurrentTarget(Hero);
            if (enuReason != gEAttackReason_None)
                Opponent.NPC.AttackReason = enuReason;
            if (bFight)
                Opponent.Routine.SetTask("ZS_Attack"); // the melee duel goes on
            else if (bWalking)
                Opponent.Routine.SetTask("ZS_Arena"); // the duel has not started yet: the native walk to the ring goes on
            else
                Opponent.Routine.ContinueRoutine();
            // v1.0.59.0: the hero obeyed (the timer was running, the weapon is away): the opponent approves aloud and goes back to the duel.
            // v1.0.61.0: the line is "Good" ("Хорошо!"), NOT "WiseMove" (that one is voiced as an aggressive "come here").
            if (a_bApprove)
                SayArenaLine(Opponent, Hero, ARENA_APPROVE_SVM, 9, "the forbidden weapon is put away");
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena: the forbidden weapon is put away - the opponent '%s' %sgoes back "
                                         "to the duel.\n",
                                         Opponent.GetName().GetText(), a_bApprove ? "approves (Good) and " : "");
        }

        // The hero holds a rune / a scroll in the hand: the task PS_Magic, or one of its states (PS_Magic_Reload, PS_Magic_Loop,
        // the cast states). The health potion (PS_MagicPotion) is no magic in this sense. Pure reads of the routine of the hero.
        GEBool IsHeroHoldingMagic()
        {
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return GEFalse;
            if (HasTask(Hero, "PS_Magic"))
                return GETrue;
            // v1.0.52.0: a bow / a crossbow in the hands (PS_Ranged*) is the same stage 1 as a rune in the hand.
            if (IsHeroRangedMode(Hero))
                return GETrue;
            bCString const strState = Hero.Routine.GetCurrentState();
            return _strnicmp(strState.GetText(), "PS_Magic", 8) == 0 && _strnicmp(strState.GetText(), "PS_MagicPotion", 14) != 0
                       ? GETrue
                       : GEFalse;
        }

        // v1.0.47.0: THE WARNING AHEAD OF THE FIRST NATIVE ORDER. The event of the hero taking the magic (OnHeroRoutineChange) is
        // lost when he does it OUTSIDE the ring and walks in with the rune in the hand: nothing changes at the line of the zone,
        // and the native arena state sends the opponent to the hero at once. So the state is read, not the event: in every
        // place where the opponent of the duel is about to get or has just got his order (the arena loop that walks him to the
        // ring, the attack loop, the choice of the target, his tick) this function asks "the quest runs, the hero is inside the
        // ring, the magic is in his hand?" and, if so, puts the opponent into the stance of the warning (ZS_ReactToDamage +
        // StopMagic) BEFORE the native code runs - the order to come closer and strike is never carried out. Cheap filters first
        // (the attack reason is one property read), the heavy ones (the quests, the zone) only for the opponent of the duel.
        GEBool PreemptMagicWarning(Entity &a_Npc)
        {
            // v1.0.48.0: THE FIRST LINE - the hero is in a monster body (or is not a humanoid): the duel checks are skipped,
            // the native order of the opponent stays as it is (nothing of the hero is read, no null property set).
            if (IsHeroDuelBlocked())
                return GEFalse;
            if (g_bMagicThreat)
                return IsWarnedOpponent(a_Npc);
            if (g_bAwaitPutAway && !IsHeroHoldingMagic())
                g_bAwaitPutAway = GEFalse; // v1.0.58.0: the weapon is really away now: the next draw is a new one
            if (g_bMagicPunished || a_Npc == None || !a_Npc.IsNPC() || a_Npc.IsDead() || a_Npc.IsDown())
                return GEFalse;
            // The common condition (the quest runs, the hero is inside the ring, the magic is in his hand) is read ONCE per
            // frame for all the NPCs (a technical throttle of 30 ms, below one frame at 30 fps), not once per NPC.
            static GEU32 s_u32Checked = 0;
            static GEBool s_bReady = GEFalse;
            GEU32 const u32Now = ::GetTickCount();
            if (s_u32Checked == 0 || static_cast<GEInt>(u32Now - s_u32Checked) >= 30)
            {
                s_u32Checked = u32Now | 1;
                s_bReady = HasRunningArenaQuest() && IsHeroInsideArenaZone() && !IsHeroKnockedOut() && IsHeroHoldingMagic();
            }
            if (!s_bReady)
                return GEFalse;
            gEAttackReason const enuReason = a_Npc.NPC.AttackReason;
            if (enuReason != gEAttackReason_Arena && enuReason != gEAttackReason_Duel && !HasTask(a_Npc, "ZS_Arena"))
                return GEFalse; // not the opponent of the duel
            if (IsHeroSide(a_Npc) || IsHeroControlled(a_Npc))
                return GEFalse;
            BeginMagicThreat(&a_Npc);
            return IsWarnedOpponent(a_Npc);
        }

        // While the warning lasts the opponent stands in the stance: whatever puts him back into the fight (the arena state,
        // the native perception) is caught on his next tick / loop and the stance is taken again. True: he was put back.
        GEBool HoldOpponentInWarning(Entity &a_Npc)
        {
            if (!IsWarnedOpponent(a_Npc))
                return GEFalse;
            if (IsHeroDuelBlocked())
            {
                EndMagicThreat(); // v1.0.48.0: the hero became a monster: nobody holds the stance any more
                return GEFalse;
            }
            if (a_Npc.IsDead() || a_Npc.IsDown() || IsHeroKnockedOut() || !HasRunningArenaQuest() || !IsHeroInsideArenaZone())
            {
                EndMagicThreat();
                return GEFalse;
            }
            // The stance is the task ZS_ReactToDamage in any of its states (the loop, the step back, the end of the stance).
            if (HasTask(a_Npc, "ZS_ReactToDamage") && a_Npc.NPC.GetCurrentTarget() == Entity::GetPlayer())
                return GEFalse;
            Entity Hero = Entity::GetPlayer();
            EngageWarning(a_Npc, Hero);
            return GETrue;
        }

        // Stage 2, the punishment: the cast state is reached (PS_Magic_QuickCast / Cast / PowerCast / Summon), the "second blow".
        // v1.0.44.0: EXACTLY THE LOGIC OF THE FIRE / ICE / LIGHTNING DAMAGE (the crime of the arena violation): the plugin only
        // arms the mercy (ArmSpare inside DeclareHeroCriminal) and sends the crowd of the camp against the hero. The arena quest
        // is NOT touched - it stays Running and the game closes it natively at the two final points: the hero is knocked
        // out (Defeat, then the ransack) or he leaves the ring (the escape). So nothing restarts the chain of the quests in
        // the background, and the squad waits in ZS_WaitForPlayer until the hero is on his feet again (Mercy ended).
        // The opponent leaves the warning stance and goes back to the duel (as the burning opponent keeps fighting).
        void PunishMagicCast(GELPCChar a_strState)
        {
            if (IsHeroDuelBlocked())
                return; // v1.0.48.0: the hero in a monster body: no duel checks, the original runs
            if (!HasRunningArenaQuest())
                return;
            // v1.0.49.0: a transformation spell cancels the started duel (the quest is Running from the order given to the arena
            // master) WHEREVER the hero stands - before the ring it is only the cancellation, inside the ring it is a crime as
            // well: see ResetDuelForTransform. The other spells are judged only inside the ring.
            if (IsTransformSpellInHand())
            {
                ResetDuelForTransform(a_strState);
                return;
            }
            if (!IsHeroInsideArenaZone())
                return;
            if (g_bMagicPunished)
                return; // one crime per quest: the next states of the same cast (Cast, PowerCast) are the same blow
            ArenaStageTwo(a_strState, "a spell is cast in the arena"); // v1.0.58.0: the crime + the quest is closed
        }

        // v1.0.52.0: STAGE 2 FOR THE BOW / CROSSBOW, exactly the logic of the cast of a spell: the shot (the state PS_Ranged_Shoot,
        // or - the second net - an arrow of the hero that hits the opponent) declares the crime ONCE per quest (the same
        // g_bMagicPunished as the spell): the local crime of the arena (DeclareHeroCriminal: Mercy armed, the witnesses around the
        // ring attack) and the opponent leaves the warning stance for the duel. The quest stays Running.
        void PunishRangedShot(GELPCChar a_strWhy)
        {
            g_u32HeroShotTick = ::GetTickCount() | 1; // always remembered: the burning arrow is no "enchanted weapon"
            if (IsHeroDuelBlocked() || !HasRunningArenaQuest() || !IsHeroInsideArenaZone())
                return;
            ArenaStageTwo(a_strWhy, "a bow / crossbow is shot in the arena"); // v1.0.58.0: the crime + the quest is closed
        }

        // v1.0.48.0: THE TRANSFORMATION SPELL. The spells of the series Spl_Transform_* (and Spl_AnimalForm) are punished not by the
        // crowd but by the reset of the duel: the SAME chain the knock-out ends with, without the knock-out. Natively a defeat
        // is PSQuestManager::OnNPCDefeated(hero) with the opponent as the current attacker of the hero: the arena quest becomes
        // Lost (the stake is gone, the arena master starts the duel again by the dialogue - the log shows SetStatus(Running)
        // from the dialogue right after SetStatus(Lost)), the opponent goes to ZS_Attack_Arena_End and back to his routine. The
        // companions leave ZS_WaitForPlayer (ReleaseWaitingCompanions). Nothing else: no crime, no mercy, no witnesses - what the
        // camp does to a hero in a beast body is decided by the native rules of the transformation, the plugin does not interfere.

        // The spell the hero holds is a transformation: read from the DATA of the spell (the hit script of its template), not from
        // its name: the rune / scroll in the right hand -> its spell -> FuncOnTargetHit is MagicTransform / MagicAnimalForm. When
        // the item does not tell, the last cast spell is read (it can be an old one, so it is only the second choice).
        GEBool IsTransformScript(bCString const &a_strFunc)
        {
            return ContainsNoCase(a_strFunc, "MagicTransform") || ContainsNoCase(a_strFunc, "MagicAnimalForm");
        }

        GEBool IsTransformSpellInHand()
        {
            Entity Hero = Entity::GetPlayer();
            if (IsHeroDuelBlocked() || !Hero.Inventory.IsValid())
                return GEFalse;
            Entity Item = Hero.Inventory.GetItemFromSlot(gESlot_RightHand);
            if (Item != None)
            {
                Entity Spell = Item.Interaction.IsValid() ? Item.Interaction.GetSpell() : Entity();
                if (Spell != None && Spell.Magic.IsValid())
                    { bCString const strHit = Spell.Magic.FuncOnTargetHit; return IsTransformScript(strHit); }
                bCString const strItem = Item.GetName();
                if (ContainsNoCase(strItem, "Transform") || ContainsNoCase(strItem, "AnimalForm"))
                    return GETrue;
            }
            bCString const strLast = Hero.NPC.LastSpell;
            if (ContainsNoCase(strLast, "Transform") || ContainsNoCase(strLast, "AnimalForm"))
                return GETrue;
            return GEFalse;
        }

        // The opponent of the started duel, also BEFORE the hero stepped into the ring: the one with the attack reason of the
        // duel, the remembered one, or the one who still walks to the ring (the task ZS_Arena, his reason is not read by the
        // distance rule of FindArenaOpponent then).
        GEBool FindDuelOpponentAnywhere(Entity &o_Opponent)
        {
            if (FindArenaOpponent(o_Opponent) || FindRememberedOpponent(o_Opponent))
                return GETrue;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc == None || !Npc.IsNPC() || Npc.IsDead() || IsHeroSide(Npc) || IsHeroControlled(Npc))
                    continue;
                if (HasTask(Npc, "ZS_Arena") || HasTask(Npc, "ZS_Attack_Arena"))
                {
                    o_Opponent = Npc;
                    return GETrue;
                }
            }
            return GEFalse;
        }

        // v1.0.49.0: a transformation while the arena quest is Running (the duel was ordered from the arena master):
        //  - the duel is CANCELLED whether the hero has stepped into the ring or not: the same chain the knock-out ends with
        //    (the quest -> Lost by OnNPCDefeated, so the arena master offers the duel again; the opponent goes to
        //    ZS_Attack_Arena_End; the companions leave ZS_WaitForPlayer);
        //  - when the hero is INSIDE the ring he has broken the law: the crime is declared too (the camp reacts by the native
        //    rules of the game for a hero in a beast body); before the ring nothing but the cancellation happens.
        void ResetDuelForTransform(GELPCChar a_strWhy)
        {
            if (IsHeroDuelBlocked() || g_bTransformReset || !HasRunningArenaQuest())
                return;
            Entity Hero = Entity::GetPlayer();
            GEBool const bInRing = IsHeroInsideArenaZone();
            Entity Opponent;
            GEBool const bOpponent = FindDuelOpponentAnywhere(Opponent);
            ResetMagicJudge(); // the warning stance (if any) is dropped WITHOUT sending the opponent back into the fight
            g_bTransformReset = GETrue;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, transformation (%s): the duel is cancelled (%s), the quest is open "
                                         "for a new start.\n",
                                         a_strWhy, bInRing ? "the hero is in the ring: a crime" : "the hero is before the ring");
            // The crime FIRST: the search of the opponent inside needs his attack reason, which the end of the fight changes.
            if (bInRing)
            {
                // v1.0.67.0: the mercy is armed here EXPLICITLY (DeclareHeroCriminal arms it only when it finds the arena opponent by his
                // attack reason, and a mercy left over from an earlier episode is renewed by ArmSpare itself).
                ArmSpare("a transformation spell is cast in the arena", GETrue);
                DeclareHeroCriminal("a transformation spell is cast in the arena", GETrue);
                g_bDuelLossDone = GETrue; // the duel is closed below: the knock-out of the hero does not end it a second time
            }
            LogArenaQuests("transformation, before");
            if (bOpponent)
            {
                Hero.NPC.SetCurrentAttacker(Opponent);  // as the native Defeat does
                Entity World = Entity::GetWorldEntity();
                World.QuestManager.OnNPCDefeated(Hero); // the quest -> Lost (restartable by the dialogue)
            }
            if (HasRunningArenaQuest())
            {
                // The native path did not close the quest (no opponent found / not in his list): the same status it uses.
                gCQuestManager_PS *pManager = gCQuestManager_PS::GetInstance();
                if (pManager)
                {
                    bTObjArray<bTAutoPOSmartPtr<gCQuest_PS>> const &arrQuests = pManager->GetQuests();
                    for (GEInt i = 0; i < arrQuests.GetCount(); ++i)
                    {
                        gCQuest_PS *pQuest = const_cast<gCQuest_PS *>(static_cast<gCQuest_PS const *>(arrQuests.GetAt(i)));
                        if (pQuest && pQuest->GetType() == gEQuestType_Arena && pQuest->GetStatus() == gEQuestStatus_Running)
                        {
                            gEQuestStatus const enuLost = gEQuestStatus_Lost;
                            pQuest->SetStatus(enuLost);
                        }
                    }
                }
            }
            LogArenaQuests("transformation, after");
            // v1.0.67.0: IN THE RING (the crime is declared, the mercy is armed) the opponent does NOT leave: he and the crowd of the camp
            // fight the form exactly as they fought the man (BeginFormAggro: when the body of the form appears each of them gets ONE order
            // against it). Before the ring there is no crime and no fight: the opponent leaves as before.
            if (bInRing && IsArmed())
                BeginFormAggro(Opponent, bOpponent);
            else if (bOpponent)
                EndArenaFightOfOpponent(Opponent); // ZS_Attack_Arena_End: the opponent is calm again
            // v1.0.68.0: the arena session ends (the journal is not touched); the AUTO waiters of the session follow the hero as soon as he
            // may be released (he is punished by the crowd in the ring first).
            EndArenaSession("the hero transformed during the duel", GETrue, bInRing ? 1 : 2);
        }

        // v1.0.58.0: THE DUEL QUEST IS CLOSED AS LOST: the native chain of a defeat (OnNPCDefeated(hero) with the opponent as the
        // current attacker of the hero), the quest status Lost by hand when the native chain left it Running. The same two steps
        // as the cancellation after a transformation.
        void CloseArenaQuestAsLost(Entity &a_Opponent, GEBool a_bOpponent)
        {
            Entity Hero = Entity::GetPlayer();
            if (a_bOpponent && HasRunningArenaQuest())
            {
                Hero.NPC.SetCurrentAttacker(a_Opponent);
                Entity World = Entity::GetWorldEntity();
                World.QuestManager.OnNPCDefeated(Hero);
            }
            if (HasRunningArenaQuest())
            {
                gCQuestManager_PS *pManager = gCQuestManager_PS::GetInstance();
                if (pManager)
                {
                    bTObjArray<bTAutoPOSmartPtr<gCQuest_PS>> const &arrQuests = pManager->GetQuests();
                    for (GEInt i = 0; i < arrQuests.GetCount(); ++i)
                    {
                        gCQuest_PS *pQuest = const_cast<gCQuest_PS *>(static_cast<gCQuest_PS const *>(arrQuests.GetAt(i)));
                        if (pQuest && pQuest->GetType() == gEQuestType_Arena && pQuest->GetStatus() == gEQuestStatus_Running)
                        {
                            gEQuestStatus const enuLost = gEQuestStatus_Lost;
                            pQuest->SetStatus(enuLost);
                        }
                    }
                }
            }
        }

        void CancelAbandonedArena(GELPCChar why, GEBool keepWaiting)
        {
            static GEBool busy = GEFalse;
            if (busy || !HasRunningArenaQuest()) return;
            busy = GETrue;
            Entity opponent;
            GEBool const found = FindDuelOpponentAnywhere(opponent) || FindArenaOpponent(opponent) || FindRememberedOpponent(opponent);
            StoreEntryPending(GEFalse);
            ResetMagicJudge();
            if (keepWaiting)
            {
                // Convert only the session's automatic wait into ordinary AUTO waiting.
                // Clearing the arena list prevents the quest-end edge from releasing them.
                for (GEInt i = 0; i < g_iWaiterCount; ++i)
                {
                    Entity npc = Entity::GetEntity(g_arrWaiters[i]);
                    if (npc != None && npc.IsNPC() && !npc.IsDead() && npc.Party.Waiting &&
                        GetWaiterType(npc) != mEWaiterType_User)
                        MarkWaiterAuto(npc);
                }
                for (GEInt i = 0; i < WAITER_MAX; ++i) g_arrWaiters[i] = bCString();
                g_iWaiterCount = 0;
                g_bArenaReleasePending = GEFalse;
            }
            EndArenaSession(why, GETrue, keepWaiting ? 0 : 2);
            gCQuestManager_PS *manager = gCQuestManager_PS::GetInstance();
            if (manager)
            {
                bTObjArray<bTAutoPOSmartPtr<gCQuest_PS>> const &quests = manager->GetQuests();
                for (GEInt i = 0; i < quests.GetCount(); ++i)
                {
                    gCQuest_PS *quest = const_cast<gCQuest_PS *>(static_cast<gCQuest_PS const *>(quests.GetAt(i)));
                    if (quest && quest->GetType() == gEQuestType_Arena && quest->GetStatus() == gEQuestStatus_Running)
                    {
                        gEQuestStatus const lost = gEQuestStatus_Lost;
                        quest->SetStatus(lost);
                    }
                }
            }
            if (found && opponent != None && opponent.IsNPC() && !opponent.IsDead() && !opponent.IsDown())
                EndArenaFightOfOpponent(opponent);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-entry] fight cancelled: %s; opponent='%s'.\n",
                why, found ? opponent.GetName().GetText() : "-");
            busy = GEFalse;
        }

        // v1.0.58.0: STAGE 2 of the forbidden weapon: the LOCAL CRIME (Mercy armed, every witness around the ring attacks the hero)
        // and the duel quest is closed (Lost); the opponent ends the fight (ZS_Attack_Arena_End). Once per quest (g_bMagicPunished).
        // v1.0.61.0: the opponent does NOT leave the ring in stage 2: he is held in the attack on the hero together with the crowd of the
        // camp until the hero falls (ReleaseStage2Hold, from the Defeat hook / PollStage2Hold). The quest is closed at once, but the
        // native end of the duel is not run for him (ZS_Attack_Arena_End would turn him round and take him away from the fight).
        void ArenaStageTwo(GELPCChar a_strWhy, GELPCChar a_strCrime, GELPCChar a_strSvm, GEU32 a_u32Type)
        {
            if (IsHeroDuelBlocked() || g_bMagicPunished || !HasRunningArenaQuest())
                return;
            g_bMagicPunished = GETrue;
            // The warning (if any) is dropped WITHOUT sending the opponent back into the fight: he leaves the arena below.
            g_bMagicThreat = GEFalse;
            g_strWarnOpponent = bCString();
            g_bWarnWasWalking = GEFalse;
            g_bWarnWasFighting = GEFalse;
            g_enuWarnReason = gEAttackReason_None;
            g_u32WarnDeadline = 0;
            g_bAwaitPutAway = GEFalse;

            Entity Opponent;
            GEBool const bOpponent = FindDuelOpponentAnywhere(Opponent);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, stage 2: %s - the crime is declared, the duel quest is closed.\n",
                                         a_strWhy);
            // The crime FIRST: the search of the opponent inside needs his attack reason, which the end of the fight changes.
            DeclareHeroCriminal(a_strCrime, GETrue);
            LogArenaQuests("stage 2, before");
            CloseArenaQuestAsLost(Opponent, bOpponent);
            LogArenaQuests("stage 2, after");
            if (bOpponent && !Opponent.IsDead() && !Opponent.IsDown())
            {
                // v1.0.61.0: HELD IN THE FIGHT: the same order the witnesses get (FullStop, the hero as the target, a reason to attack,
                // ZS_Attack); he is also noted in the list of the NPCs sent against the hero (he calms down with the crowd, when the hero
                // is on his feet again).
                Entity Hero = Entity::GetPlayer();
                ForceAttack(Opponent, Hero, gEAttackReason_Enemy);
                g_strStage2Hold = Opponent.GetName();
                g_u32Stage2HoldTick = ::GetTickCount() | 1;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, stage 2: the opponent '%s' stays in the fight (ZS_Attack against the "
                                             "hero) until the hero falls.\n",
                                             Opponent.GetName().GetText());
            }
            // v1.0.60.0: the opponent does not turn to the crowd in silence: the native line of the fury ("YouAskedForIt").
            if (bOpponent && a_strSvm && a_strSvm[0])
            {
                Entity Hero = Entity::GetPlayer();
                SayArenaLine(Opponent, Hero, a_strSvm, a_u32Type, "stage 2: the crime is declared");
            }
            g_bDuelLossDone = GETrue; // the duel is already closed: the knock-out of the hero does not end it a second time
            EndArenaSession(a_strWhy, GETrue, 1); // v1.0.68.0: the session ends; the AUTO waiters come back when the hero may be released
        }



        // v1.0.67.0: the transformation is cast in the ring: from now the plugin waits for the body of the form (an event seen by the
        // tick of an NPC) and then gives the crowd + the opponent one order against it (PollFormAggro).
        void BeginFormAggro(Entity &a_Opponent, GEBool a_bOpponent)
        {
            for (GEInt i = 0; i < AGGRO_MAX; ++i)
            {
                g_arrFormOrderName[i] = bCString();
                g_arrFormOrderCount[i] = 0;
                g_arrFormOrderTick[i] = 0;
            }
            g_iFormOrderNames = 0;
            g_bFormAggroPending = GETrue;
            g_bFormSeen = GEFalse;
            g_u32FormPendingSince = ::GetTickCount() | 1;
            g_u32FormPollTick = 0;
            g_strFormOpponent = a_bOpponent && a_Opponent != None ? a_Opponent.GetName() : bCString();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, transformation: the opponent '%s' and the crowd (%d) stay in the fight - "
                                         "they are aimed at the body of the form when it appears.\n",
                                         g_strFormOpponent.IsEmpty() ? "-" : g_strFormOpponent.GetText(),
                                         static_cast<int>(g_iArenaWitnessCount));
        }

        // v1.0.67.0: one NPC of the crowd / the opponent: does he need an order against the form now (and has he got the slot for it)?
        GEBool TakeFormOrderSlot(bCString const &a_strName, GEU32 a_u32Now)
        {
            GEInt iSlot = -1;
            for (GEInt j = 0; j < g_iFormOrderNames; ++j)
                if (g_arrFormOrderName[j] == a_strName)
                {
                    iSlot = j;
                    break;
                }
            if (iSlot < 0)
            {
                if (g_iFormOrderNames >= AGGRO_MAX)
                    return GEFalse;
                iSlot = g_iFormOrderNames++;
                g_arrFormOrderName[iSlot] = a_strName;
                g_arrFormOrderCount[iSlot] = 0;
                g_arrFormOrderTick[iSlot] = 0;
            }
            if (g_arrFormOrderCount[iSlot] >= FORM_ORDERS_MAX)
                return GEFalse;
            if (g_arrFormOrderTick[iSlot] != 0 &&
                static_cast<GEInt>(a_u32Now - g_arrFormOrderTick[iSlot]) < static_cast<GEInt>(FORM_REORDER_MS))
                return GEFalse;
            ++g_arrFormOrderCount[iSlot];
            g_arrFormOrderTick[iSlot] = a_u32Now | 1;
            return GETrue;
        }

        // v1.0.67.0: the body of the form is there: every NPC the plugin sent against the hero (the list of the aggro) and the duel opponent
        // that is not fighting him now gets ONE order (FullStop, the CURRENT player entity as the target, a reason, ZS_Attack) - the order
        // they got for the human body. No standing hold: an NPC that is idle again may be ordered again (TakeFormOrderSlot). The pursuit
        // is over when the form is gone (the hero is a man again), the body is dead, or the mercy ended.
        void PollFormAggro()
        {
            if (!g_bFormAggroPending)
                return;
            GEU32 const u32Now = ::GetTickCount();
            if (g_u32FormPollTick != 0 && static_cast<GEInt>(u32Now - g_u32FormPollTick) < static_cast<GEInt>(FORM_POLL_MS))
                return;
            g_u32FormPollTick = u32Now | 1;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None || !IsArmed())
            {
                ResetFormAggro();
                return;
            }
            if (!IsHeroTransformedNow())
            {
                if (g_bFormSeen)
                {
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, transformation: the form is gone (the hero is a man again) - the "
                                                 "pursuit of the form is over.\n");
                    ResetFormAggro();
                }
                else if (static_cast<GEInt>(u32Now - g_u32FormPendingSince) >= static_cast<GEInt>(FORM_WAIT_MS))
                {
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, transformation: no form appeared (the cast was interrupted).\n");
                    ResetFormAggro();
                }
                return;
            }
            g_bFormSeen = GETrue;
            if (Hero.IsDead())
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, transformation: the body of the form is dead - the pursuit is over.\n");
                ResetFormAggro();
                return;
            }

            GEInt iOrdered = 0;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc == None || Npc == Hero || !Npc.IsNPC() || Npc.IsDead() || Npc.IsDown())
                    continue;
                bCString const strName = Npc.GetName();
                GEBool const bOpponent = !g_strFormOpponent.IsEmpty() && g_strFormOpponent == strName;
                if (!bOpponent && !IsCurrentArenaWitness(Npc))
                    continue;
                if (IsHeroSide(Npc) || IsHeroControlled(Npc))
                    continue;
                // He fights the form already, or runs away from it as he ran from the man: nothing to repeat.
                if (IsAttackingHero(Npc, Hero) || HasTask(Npc, "ZS_Flee"))
                    continue;
                if (Npc.GetDistanceTo(Hero) > WITNESS_RADIUS && !Npc.IsFreeLOS(Hero, GEFalse))
                    continue; // the form is completely out of his sight
                if (!TakeFormOrderSlot(strName, u32Now))
                    continue;
                ForceAttack(Npc, Hero, gEAttackReason_Enemy);
                ++iOrdered;
            }
            static GEInt s_iLines = 0;
            if (iOrdered > 0 && s_iLines < 40)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, transformation: %d NPC(s) of the crowd / the opponent are ordered to "
                                             "attack the form '%s'.\n",
                                             static_cast<int>(iOrdered), Hero.GetName().GetText());
            }
        }

        // v1.0.58.0: THE TIMER IS OUT AND THE HERO STILL HOLDS THE FORBIDDEN WEAPON: the duel is CANCELLED. No crime (he was warned and
        // the timer ran out - the offence is the refusal of the rules, not violence): the quest is closed as Lost, the opponent
        // leaves the combat mode and the ring (ZS_Attack_Arena_End), the squad leaves ZS_WaitForPlayer.
        void CancelDuelForForbiddenWeapon(GELPCChar a_strWhy)
        {
            if (IsHeroDuelBlocked() || !HasRunningArenaQuest())
                return;
            Entity Opponent;
            GEBool const bOpponent = FindDuelOpponentAnywhere(Opponent);
            ResetMagicJudge(); // the warning stance is dropped WITHOUT sending the opponent back into the fight; flags are clean
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena: %s - the duel is cancelled, the opponent leaves the ring.\n",
                                         a_strWhy);
            LogArenaQuests("cancelled, before");
            BlockArenaPlaque(); // v1.0.60.0: no text of the escape from the arena on the screen, only the voice
            CloseArenaQuestAsLost(Opponent, bOpponent);
            LogArenaQuests("cancelled, after");
            if (bOpponent)
                EndArenaFightOfOpponent(Opponent);
            // v1.0.60.0: the peaceful leaving: the opponent says the native line of the end of the arena.
            if (bOpponent)
            {
                Entity Hero = Entity::GetPlayer();
                SayArenaLine(Opponent, Hero, "YouLeftTheArena", 7, "the duel is cancelled");
            }
            EndArenaSession("the duel is cancelled (the forbidden weapon is held)", GETrue, 2);
        }

        // v1.0.58.0: the timer of the warning. Called from the tick of every NPC (one flag read when there is no timer). While the
        // game is paused the ticks of the NPCs stop: the gap since the previous poll moves the deadline, so a pause is not counted.
        void PollWarnTimer()
        {
            if (g_u32WarnDeadline == 0 || !g_bMagicThreat)
                return;
            GEU32 const u32Now = ::GetTickCount();
            if (g_u32WarnPollTick != 0)
            {
                GEU32 const u32Gap = u32Now - g_u32WarnPollTick;
                if (u32Gap > 1500)
                    g_u32WarnDeadline += u32Gap;
            }
            g_u32WarnPollTick = u32Now | 1;
            if (static_cast<GEInt>(u32Now - g_u32WarnDeadline) < 0)
                return; // the time is not out yet
            g_u32WarnDeadline = 0;
            if (!HasRunningArenaQuest() || !IsHeroInsideArenaZone() || IsHeroKnockedOut() || !IsHeroHoldingMagic())
            {
                // the weapon is away (the event was missed) / the hero left the ring: the warning is simply over
                EndMagicThreat(!IsHeroHoldingMagic() && HasRunningArenaQuest() && IsHeroInsideArenaZone() && !IsHeroKnockedOut());
                return;
            }
            CancelDuelForForbiddenWeapon("the forbidden weapon was held longer than the warning timer");
        }
    } // namespace

    // v1.0.47.0: THE TELEPORT OF THE HERO IS NATIVE AGAIN. Nothing of the distance / of the coordinates / of the disconnect is
    // done by the plugin any more: the native MagicTeleport (the teleport stone, the scroll) and the native OnTick decide
    // about every creature of the party, as in the pure game (disassembly of Script_Game.dll 1.8.7, MagicTeleport):
    //  - the types 1..3 (companions, mob, slaves) get the native script WaitPlayerParty ("wait here") and stay where they were;
    //  - the type 4 (tamed / dominated) loses the leader, FullStop, ContinueRoutine; HandleControlledReturnHome then makes it
    //    wild (type None) when it stands at its place;
    //  - the type 5 (summons) goes to DismissPlayerParty (it disappears, ZS_RagDollDead).
    // The ONLY addition: the PERMANENT COMPANIONS (the types 1..3: Party / Mob / Slave - Cyrus is a type 3) follow the hero
    // through the stone; the types 4 and 5 never. The way the original game
    // moves a character silently and at once is the info-script command "MoveTo" (ECommand_MoveTo = 37, the command of the
    // quest of general Lee when he is sent away): its executor is Entity::MoveTo(Entity) - the pose of the character is set to
    // the pose of the target, no cast animation, no item in the inventory of the character (the state ZS_Lee_Teleport, in
    // contrast, takes the item It_Teleport_VangardTempel and plays _AI_Cast, so it is not used). The same call is made here.
    // Who is moved is decided BEFORE the native script runs (it puts the types 1..3 to wait): only a type 1 that follows the
    // hero (is not told to wait). After the move he follows the hero again (ZS_FollowPlayer), the native "wait" is taken off.
    namespace
    {
        // v1.0.49.0: WHO FOLLOWS. The log of 1.0.48.0 shows that Cyrus is the party type 3 (Slave), not 1 ("AssessDefeat: 'Cyrus'
        // (party type 3)"), so the filter "strictly the type 1" found nobody. The permanent companions are the types 1..3
        // (Party, Mob, Slave): exactly the ones the native MagicTeleport puts to wait (WaitPlayerParty, type <= 3). The types
        // 4 and 5 are never moved.
        using GroupLogic::IsTeleportFollower;
        using GroupLogic::TryLateCollect;
        using GroupLogic::LogTeleportRecord;
        using GroupLogic::CollectTeleportFollowers;
        using GroupLogic::FinishTeleportFollowers;
        using GroupLogic::PollTeleportFollowers;
    } // namespace

    // v1.0.59.0: THE STANCE SAYS THE LINE OF THE WARNING. The native state ZS_ReactToDamage speaks its line when it is entered; the
    // name of the line is read from the buffer g_arrReactSvm (PatchReactToDamageSvm). Here, per NPC, right before the native state runs,
    // the buffer is set: the opponent of the warning gets "StopMagic" (a rune / a scroll) or "WeaponDown" (a bow / a crossbow), every
    // other NPC (a citizen who is hit, ...) keeps the native "WhatAreYouDoing". The state runs synchronously inside this hook.
    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_ReactToDamage)
    {
        if (g_bReactSvmPatched)
        {
            Entity Self(a_pSPU->GetSelfEntity());
            GELPCChar const strLine = (Self != None && IsWarnedOpponent(Self)) ? (g_bWarnRanged ? "WeaponDown" : "StopMagic")
                                                                               : "WhatAreYouDoing";
            SetReactToDamageSvm(strLine);
        }
        return ZS_ReactToDamage_Original(a_rRunTimeStack, a_pSPU);
    }

        ME_DEFINE_AND_HOOK_SCRIPT(MagicTeleport)
    {
        return GroupLogic::Waiting::MagicTeleport(MagicTeleport_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // v1.0.50.0: THE PLAQUE "<name> IS WAITING". The native MagicTeleport runs the script WaitPlayerParty for every member of
    // the party of the types 1..3 (disassembly of Script_Game.dll: the script 100A8F40 prints the text GO_SlaveDismiss with
    // GUI2::PrintGameMessage, then Waiting = true, FullStop, ZS_WaitForPlayer). A companion that the plugin has collected is
    // carried to the hero, so for him the script is not run at all: no plaque, no waiting, an unbroken interface. For the
    // ones that are not collected (told to wait by the hero in the dialogue, a type 4 / 5) the original runs unchanged, and
    // for a collected one whose move failed the plugin shows the plaque itself (RestoreNativeWait). Outside the teleport
    // spell (g_bTeleportScriptRuns is false) the script is never touched.
        ME_DEFINE_AND_HOOK_SCRIPT(WaitPlayerParty)
    {
        return GroupLogic::Waiting::WaitPlayerParty(WaitPlayerParty_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // The native OnTick of every NPC runs unchanged (the distance, the dismissal of the summons, the return home of the
    // tamed): the plugin does not look at the creatures under control any more.
        ME_DEFINE_AND_HOOK_SCRIPT(OnTick)
    {
        return PluginRuntime::Loops::OnTick(OnTick_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

        ME_DEFINE_AND_HOOK_SCRIPT(HasAttackReasonToKill)
    {
        return PluginRuntime::Combat::HasAttackReasonToKill(HasAttackReasonToKill_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    //
    // WantToRansack(npc, hero): the NPC takes the weapon and the gold of the knocked-out hero. The native script refuses
    // for a class Mage / Paladin, but the hero was robbed anyway after the mercy of the plugin. So the exception is
    // enforced here: a Mage / Paladin (the hero or the NPC himself) is never ransacked while the mercy is applied.
    //
        ME_DEFINE_AND_HOOK_SCRIPT(GiveXP)
    {
        return GroupLogic::Lifecycle::GiveXP(GiveXP_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

        ME_DEFINE_AND_HOOK_SCRIPT(JoinPlayerParty)
    {
        return GroupLogic::Lifecycle::JoinPlayerParty(JoinPlayerParty_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    

    

    

        ME_DEFINE_AND_HOOK_SCRIPT(WantToRansack)
    {
        return PluginRuntime::Combat::WantToRansack(WantToRansack_Original, a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    //
    // Stage 4: no difficulty speed-up of the arena opponent. The animation speed of an NPC is set through
    // Entity::SetTimeScale / PSRoutine::SetTimeScale (Script.dll). During an arena fight a value above 1.0 for the
    // opponent is replaced by the base speed 1.0. A value below 1.0 (weakness slows the NPC down, freezing sets 0) is
    // never touched, so a weakened opponent is honestly slowed. (Game 1.8.7 has no gECharacterState_Weak enum.)
    //
    namespace
    {
        GEFloat ClampOpponentTimeScale(eCEntity *a_pOwner, GEFloat a_fScale)
        {
            // v1.0.59.0: EnableHardDifficultyAttackSpeed=true: the native speed-up of the arena opponent returns (no clamp).
            if (GetConfig().EnableHardDifficultyAttackSpeed || a_fScale <= 1.0f || !a_pOwner)
                return a_fScale;
            Entity Owner(a_pOwner);
            Entity Opponent;
            if (FindArenaOpponent(Opponent) && Owner == Opponent)
                return 1.0f;
            return a_fScale;
        }

        // gCSession::Start(mode) / gCSession::Stop() (Game.dll): a save game is loaded (mode LoadGame = 1) or a new game
        // starts (mode NewGame = 0) / the session ends. The state of the beating lives in the plugin only, so it is
        // forgotten here, before the original runs (the original also stops a running session itself).
        // v1.0.60.0: GUI2::PrintGameMessage (Script.dll). The native end of the arena (ZS_Arena_Loop / ZS_Attack_Loop: the hero left the
        // ring) says the line "YouLeftTheArena" and prints the text GO_ArenaWarning with the message type Failure (13; disassembly of
        // Script_Game.dll 1.8.7: push 0Dh before the call). While the plaque is blocked (the voice of the end of the arena is heard
        // instead of it: the cancelled duel, the voice over the fallen hero) a Failure message is swallowed; every other message and every
        // message outside the window passes untouched. The plugin itself prints no Failure message any more.
        mCFunctionHook Hook_GuiPrintGameMessage;
        void GE_STDCALL GuiPrintGameMessage(GUI2 *a_pThis, bCUnicodeString const *a_pText, gEGameMessageType a_enuType)
        {
            return ArenaJudge::Native::GuiPrintGameMessage(a_pThis, a_pText, a_enuType,
                Hook_GuiPrintGameMessage.GetOriginalFunction<ArenaJudge::Native::GuiPrintGameMessageOriginal>());
        }










        mCFunctionHook Hook_EntitySetTimeScale;
        void GE_STDCALL EntitySetTimeScale(Entity *a_pThis, GEFloat a_fScale)
        {
            return ArenaJudge::Native::EntitySetTimeScale(a_pThis, a_fScale,
                Hook_EntitySetTimeScale.GetOriginalFunction<ArenaJudge::Native::EntitySetTimeScaleOriginal>());
        }

        mCFunctionHook Hook_RoutineSetTimeScale;
        void GE_STDCALL RoutineSetTimeScale(PSRoutine *a_pThis, GEFloat a_fScale)
        {
            return ArenaJudge::Native::RoutineSetTimeScale(a_pThis, a_fScale,
                Hook_RoutineSetTimeScale.GetOriginalFunction<ArenaJudge::Native::RoutineSetTimeScaleOriginal>());
        }

        // PSParty::GetPartyLeader (Script.dll): THE PARTY FOLLOWS THE MASTER IN ANY FORM. A transformation (scroll,
        // potion) makes a separate creature the player (Entity::GetPlayer()), while the party of the hero stays bound to
        // his own human body "PC_Hero", which is hidden meanwhile. Every native check "the leader is the player" (follow
        // state, the blue name in the group, "is a companion") then fails: the companions turn grey, drop out of the
        // group and stop following. Here the leader that is the hidden body PC_Hero is reported as the player, but only
        // for a creature that really belongs to the party (types 1..5); the body itself and strangers are untouched.
        mCFunctionHook Hook_PartyGetLeader;
        Entity *GE_STDCALL PartyGetLeader(PSParty *a_pThis, Entity *a_pRet)
        {
            return GroupLogic::Lifecycle::PartyGetLeader(a_pThis, a_pRet,
                Hook_PartyGetLeader.GetOriginalFunction<GroupLogic::Lifecycle::PartyGetLeaderOriginal>());
        }

        // PSNpc::CanHit(minDuration) (Script.dll): may the NPC hit now? minDuration is MinHitDuration of ge3.ini (the
        // callers pass it); the answer is "enough time has passed since the last hit". The native function skips the whole
        // delay for an NPC whose attack reason is Arena (0x12) (and for a zombie), on every difficulty, so the arena
        // opponent attacked without any pause. For the time of the call the reason Arena is hidden behind Duel (0x11, which
        // CanHit does not know), so the opponent keeps the delay of the ini like every other NPC, and it is put back at
        // once. Nothing else reads the reason inside this call.
        mCFunctionHook Hook_NpcCanHit;
        GEBool GE_STDCALL NpcCanHit(PSNpc *a_pThis, GEU32 a_u32MinDuration)
        {
            return ArenaJudge::Native::NpcCanHit(a_pThis, a_u32MinDuration,
                Hook_NpcCanHit.GetOriginalFunction<ArenaJudge::Native::NpcCanHitOriginal>());
        }

        // gCQuest_PS::SetStatus(gEQuestStatus const &) (Game.dll, called directly by OnNPCDefeated and the dialogue
        // scripts). When the opponent of the duel was knocked out by somebody who is not the hero, the quest of the arena
        // must not become Success / Won: it becomes Lost (the status that also switches the arena off). Every other
        // quest and every other status pass through untouched. This is the hook that worked in the first versions.
        mCFunctionHook Hook_QuestSetStatus;
        GEBool GE_STDCALL QuestSetStatus(gCQuest_PS *a_pThis, gEQuestStatus const *a_penuStatus)
        {
            return ArenaJudge::Native::QuestSetStatus(a_pThis, a_penuStatus,
                Hook_QuestSetStatus.GetOriginalFunction<ArenaJudge::Native::QuestSetStatusOriginal>(), _ReturnAddress());
        }

        // Entity::DoDamage(source, amount, type) (Script.dll): the damage dealt by script code directly, bypassing
        // DoLogicalDamage - the area spells (MagicAreaDamageTick: meteor, fire rain, hail ...) use it for every victim of
        // the explosion. It is a thin wrapper: it forwards (source, 0, amount, type, 0, iterator) to the virtual
        // gCEntity::DoDamage of the entity "this". The check below is symmetric (both sides must be under the control of
        // the hero), so it does not matter which side is which. The source is the owner of the spell (the chain of owners
        // is followed by GetDamageOwnerNpc). Blocked: the call is not forwarded at all, so no damage is done.
        mCFunctionHook Hook_EntityDoDamage;
        void GE_STDCALL EntityDoDamage(Entity *a_pThis, Entity const *a_pSource, GEU32 a_u32Amount,
                                       gEDamageType a_enuType)
        {
            return PluginRuntime::Native::EntityDoDamage(a_pThis, a_pSource, a_u32Amount, a_enuType,
                Hook_EntityDoDamage.GetOriginalFunction<PluginRuntime::Native::EntityDoDamageOriginal>());
        }

        // The animation speed of the arena opponent on the hard difficulty. The script function that returns the animation
        // speed of an NPC (Script_Game.dll 1.8.7, RVA 0x42A0, called by every _AI_*Attack / parade / stumble) ends with
        //   if (attackReason == Arena && GetCurrentDifficulty() == Hard) speed *= 1.25;
        // i.e. at RVA 0x4DB6 "cmp eax,12h / jne", at 0x4DB8 "call GetCurrentDifficulty", at 0x4DBE "cmp eax,2" and at 0x4DC1
        // "jne 0x4DE3" (skip the multiplication). Only this ONE place is touched: the conditional jump becomes an
        // unconditional one (75 -> EB, the same distance), so for the arena opponent the multiplication is never done, and
        // the speed stays what it is on the medium difficulty. The base speed, the weakness slow-down (x0.75, a few
        // instructions above) and every other difficulty rule are untouched. All bytes are checked before the write; on any
        // other build of Script_Game.dll nothing is patched.
        void RemoveArenaHardSpeedUp()
        {
            HMODULE hModule = ::GetModuleHandleA("Script_Game.dll");
            if (!hModule)
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Script_Game.dll is not loaded: the arena speed-up of the "
                                             "hard difficulty is left as it is.\n");
                return;
            }
            GEU8 *pBase = reinterpret_cast<GEU8 *>(hModule);
            // Bytes 2..5 are the ABSOLUTE address of the import slot of GetCurrentDifficulty. The DLL is not loaded at its
            // preferred base (the crash log of the game shows Script_Game.dll at 0F7C0000, the file says 10000000), so the
            // loader has rebased this address: it differs from the file and is NOT compared (the first version of this
            // check compared it and therefore never patched anything - "differs from the expected one" in the log).
            GEU8 const arrExpected[] = {0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, // call dword ptr [GetCurrentDifficulty]
                                        0x83, 0xF8, 0x02,                   // cmp eax, 2 (Hard)
                                        0x75, 0x20};                        // jne +0x20 (skip the multiplication)
            GEU8 *pPlace = pBase + 0x4DB8;
            GEBool bSame = GETrue;
            for (size_t i = 0; i < sizeof(arrExpected); ++i)
            {
                if (i >= 2 && i < 6)
                    continue;
                if (pPlace[i] != arrExpected[i])
                    bSame = GEFalse;
            }
            if (!bSame)
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The code of Script_Game.dll differs from the expected one "
                                             "(bytes %02X %02X .. %02X %02X %02X %02X %02X): the arena speed-up of the "
                                             "hard difficulty is left as it is.\n",
                                             pPlace[0], pPlace[1], pPlace[6], pPlace[7], pPlace[8], pPlace[9], pPlace[10]);
                return;
            }
            DWORD dwOld = 0;
            GEU8 *pJump = pBase + 0x4DC1;
            if (!::VirtualProtect(pJump, 1, PAGE_EXECUTE_READWRITE, &dwOld))
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Cannot unlock the code for the arena speed-up patch.\n");
                return;
            }
            *pJump = 0xEB; // jne -> jmp short
            DWORD dwDummy = 0;
            ::VirtualProtect(pJump, 1, dwOld, &dwDummy);
            ::FlushInstructionCache(::GetCurrentProcess(), pJump, 1);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The x1.25 attack speed of the arena opponent on the hard "
                                         "difficulty is removed.\n");
        }
    } // namespace

    //
    // v1.0.35.0: THE DIALOGUE WITH A TAMED / DOMINATED CREATURE ("Tame Animal", "Word of Control": party member type 4,
    // the hero is the party leader). The engine refuses to talk to monsters and slaves, so the two native lines of the
    // hero (owner "PC_Hero") are taken over for such a partner only:
    //  - the info of the condition type Wait (34) becomes the line "Wait here"  -> task ZS_WaitForPlayer, Waiting = true;
    //  - the info of the condition type Join (14) becomes the line "Follow me"  -> the combat state is reset, the task
    //    ZS_FollowPlayer (and ContinueRoutine) is set, Waiting = false. It is offered at any time, so the hero can pull
    //    a creature out of a combat stupor (after a fight, after a teleport).
    // The native scripts of these two infos (CmdWait / CmdPartyJoin) are NOT run for such a partner: JoinPlayerParty would
    // rewrite the party member type of the creature. Every other partner / info is left to the native code.
    // Layout of gCInfo_PS (SDK asserts): m_bInfoGiven +0x18, m_enuCondType +0x28.
    //
    namespace
    {











        // gCInfo_PS::AreConditionsFulfilled() const (Game.dll): the line is offered for such a partner whatever the native
        // rules (no talking to monsters / slaves) say. "Wait here" only while he does not wait yet.
        mCFunctionHook Hook_InfoConditions;


        GEBool GE_STDCALL InfoConditions(gCInfo_PS *a_pThis)
        {
            return GroupLogic::Waiting::InfoConditions(a_pThis,
                Hook_InfoConditions.GetOriginalFunction<GroupLogic::Waiting::InfoConditionsOriginal>());
        }

        // gCInfo_PS::Execute(gCInfoManager_PS *) (Game.dll): the chosen line. The order is given at once, the native
        // script is skipped and the dialogue is closed.
        mCFunctionHook Hook_InfoExecute;
        GEBool GE_STDCALL InfoExecute(gCInfo_PS *a_pThis, gCInfoManager_PS *a_pManager)
        {
            return GroupLogic::Waiting::InfoExecute(a_pThis, a_pManager,
                Hook_InfoExecute.GetOriginalFunction<GroupLogic::Waiting::InfoExecuteOriginal>());
        }

        // gCInfo_PS::GetDialogDescription() const: the text of the line (hidden return buffer).
        mCFunctionHook Hook_InfoDescription;
        bCUnicodeString *GE_STDCALL InfoDescription(gCInfo_PS *a_pThis, bCUnicodeString *a_pRet)
        {
            return GroupLogic::Waiting::InfoDescription(a_pThis, a_pRet,
                Hook_InfoDescription.GetOriginalFunction<GroupLogic::Waiting::InfoDescriptionOriginal>());
        }

        // Entity::HasInfo(other) const (Script.dll): "the hero has something to say to this NPC". The perception scripts
        // (AssessTalk, the focus) ask it before the creature can be spoken to. True for such a creature.
        mCFunctionHook Hook_EntityHasInfo;
        GEBool GE_STDCALL EntityHasInfo(Entity *a_pThis, Entity const *a_pOther)
        {
            return GroupLogic::Waiting::EntityHasInfo(a_pThis, a_pOther,
                Hook_EntityHasInfo.GetOriginalFunction<GroupLogic::Waiting::EntityHasInfoOriginal>());
        }
    } // namespace

} // namespace ArenaJudge::Implementation

namespace GroupLogic::Host
{

    GEBool IsArenaSessionHeld() { return ArenaJudge::Implementation::g_bArenaSessionHold; }
    void EndArenaSession(GELPCChar why, GEBool cancelled, GEInt release)
    { ArenaJudge::Implementation::EndArenaSession(why, cancelled, release); }
    void PrintCompanionWaitMessage(Entity &npc) { ArenaJudge::Implementation::PrintCompanionWaitMessage(npc); }
    WaiterType GetWaiterType(Entity &npc)
    { return static_cast<WaiterType>(ArenaJudge::Implementation::GetWaiterType(npc)); }
    GELPCChar WaiterTypeText(WaiterType type)
    { return ArenaJudge::Implementation::WaiterTypeText(static_cast<ArenaJudge::Implementation::mEWaiterType>(type)); }
    void ClearWaiterMarks(Entity &npc) { ArenaJudge::Implementation::ClearWaiterMarks(npc); }
    void MarkWaiterAuto(Entity &npc) { ArenaJudge::Implementation::MarkWaiterAuto(npc); }
}
namespace SocialLogic::Host
{
    GEBool IsHeroSide(Entity &npc) { return ArenaJudge::Implementation::IsHeroSide(npc); }
    GEBool FindArenaOpponent(Entity &npc) { return ArenaJudge::Implementation::FindArenaOpponent(npc); }
    GEBool FindRememberedOpponent(Entity &npc) { return ArenaJudge::Implementation::FindRememberedOpponent(npc); }
    GEBool HasRunningArenaQuest() { return ArenaJudge::Implementation::HasRunningArenaQuest(); }
    GEBool IsHeroInsideArenaZone() { return ArenaJudge::Implementation::IsHeroInsideArenaZone(); }
    GEBool HasTask(Entity &npc, GELPCChar task) { return ArenaJudge::Implementation::HasTask(npc, task); }
    GEBool HasReacted(Entity &npc, Entity &hero) { return ArenaJudge::Implementation::HasReacted(npc, hero); }
    GEBool IsAttackingHero(Entity &npc, Entity &hero) { return ArenaJudge::Implementation::IsAttackingHero(npc, hero); }
    void ForceReaction(Entity &npc, Entity &hero) { ArenaJudge::Implementation::ForceReaction(npc, hero); }


    void ResetDeclareThrottle() { ArenaJudge::Implementation::g_u32LastDeclare = 0; }

}

namespace SocialLogic::Host
{
    mEWitnessVerdict GetWitnessVerdict(Entity &npc, Entity &hero, Entity &victim, GELPCChar *reason)
    { return static_cast<mEWitnessVerdict>(ArenaJudge::Implementation::GetWitnessVerdict(npc, hero, victim, GEFalse, reason)); }
    void ForceAttack(Entity &npc, Entity &hero, gEAttackReason reason)
    { ArenaJudge::Implementation::ForceAttack(npc, hero, reason); }
}

namespace GroupLogic::Host
{
    GEBool IsHeroSide(Entity &npc) { return ArenaJudge::Implementation::IsHeroSide(npc); }
    Entity GetDamageOwnerNpc(Entity &source) { return ArenaJudge::Implementation::GetDamageOwnerNpc(source); }
    GEBool IsPointMagicSource(Entity *source) { return ArenaJudge::Implementation::IsPointMagicSource(source); }
    void StartFleeWatch(Entity &npc) { ArenaJudge::Implementation::StartFleeWatch(npc); }
    void PollArenaQuestEnd() { ArenaJudge::Implementation::PollArenaQuestEnd(); }
}

namespace GroupLogic::Host { GEBool HasTask(Entity &npc, GELPCChar task) { return ArenaJudge::Implementation::HasTask(npc, task); } }

namespace GroupLogic::Host
{
    GEBool IsDuelTaboo(Entity &npc) { return ArenaJudge::Implementation::IsDuelTaboo(npc); }
    GEBool FindArenaOpponent(Entity &npc) { return ArenaJudge::Implementation::FindArenaOpponent(npc); }
    GEBool HasRunningArenaQuest() { return ArenaJudge::Implementation::HasRunningArenaQuest(); }
    GEBool IsInsideArenaZone(Entity &npc) { return ArenaJudge::Implementation::IsInsideArenaZone(npc); }
    GELPCChar DamageTypeName(GEInt type) { return ArenaJudge::Implementation::DamageTypeName(type); }
    void MarkWaiterUserPublic(Entity &npc) { ArenaJudge::Implementation::MarkWaiterUserPublic(npc); }
    void ClearWaiterMarksPublic(Entity &npc) { ArenaJudge::Implementation::ClearWaiterMarksPublic(npc); }
    GEBool WasArenaDuelActive() { return ArenaJudge::Implementation::g_bDuelWasActive; }
}

namespace SocialLogic::Host
{
    GEBool IsSentAgainstHero(Entity &npc) { return ArenaJudge::Implementation::IsSentAgainstHero(npc); }
}

namespace PluginRuntime::Host
{
    void ArmArenaAdoptAfterLoad() { ArenaJudge::Implementation::ArmArenaAdoptAfterLoad(); }

    void ResetFormAggro() { ArenaJudge::Implementation::ResetFormAggro(); }

    void ResetDuelState() { ArenaJudge::Implementation::ResetDuelState(); }
}

namespace PluginRuntime::Host
{
    NativeHookBinding const *GetNativeHookBindings(GEU32 &count)
    {
        static NativeHookBinding const bindings[] = {
            {&ArenaJudge::Implementation::Hook_GuiPrintGameMessage, "?PrintGameMessage@GUI2@@QAEXABVbCUnicodeString@@W4gEGameMessageType@@@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::GuiPrintGameMessage), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_SessionStart, "?Start@gCSession@@QAEXW4gESession_StartMode@@@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::SessionStart), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_SessionStop, "?Stop@gCSession@@QAEXXZ", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::SessionStop), "Game.dll"},
            {&PluginRuntime::Hook_SessionProcess, "?Process@gCSession@@UAEXXZ", reinterpret_cast<GELPVoid>(&PluginRuntime::SessionProcess), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_PersonalSlotDrop, "?DropItemsFromSlot@gCInventory_PS@@QAEPAVeCEntity@@W4gESlot@@HPBVbCMatrix@@@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::PersonalSlotDrop), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_DialoguePartyMembers, "?GetMembers@PSParty@@QBE?AV?$bTValArray@PAVeCEntity@@@@_N@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::DialoguePartyMembers), "Script.dll"},
            {&GroupLogic::Lifecycle::Hook_SecondarySummonType, "?SetPartyMemberType@gCParty_PS@@QAEXABV?$bTPropertyContainer@W4gEPartyMemberType@@@@@Z", reinterpret_cast<GELPVoid>(&GroupLogic::Lifecycle::SecondarySummonType), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_DiagnosticStartDialogue, "?StartInfoManager@gCInfoManager_PS@@QAE_NAAVeCEntity@@0@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::DiagnosticStartDialogue), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_BrawlSetHitPoints, "?SetHitPoints@gCDamageReceiver_PS@@QAEXABJ@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::BrawlSetHitPoints), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_NpcSetCurrentAttacker, "?SetCurrentAttacker@PSNpc@@QAEXABVEntity@@@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::NpcSetCurrentAttacker), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_NpcSetLastAttacker, "?SetLastAttacker@PSNpc@@QAEXABVEntity@@@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::NpcSetLastAttacker), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_NpcSetLastCast, "?SetLastCast@PSNpc@@QAEXXZ", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::NpcSetLastCast), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_NpcEnableStatusEffects, "?EnableStatusEffects@PSNpc@@QAEXH_N@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::NpcEnableStatusEffects), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_EntitySetTimeScale, "?SetTimeScale@Entity@@QAEXM@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::EntitySetTimeScale), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_RoutineSetTimeScale, "?SetTimeScale@PSRoutine@@QAEXM@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::RoutineSetTimeScale), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_NpcCanHit, "?CanHit@PSNpc@@QAE_NK@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::NpcCanHit), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_QuestSetStatus, "?SetStatus@gCQuest_PS@@QAE_NABW4gEQuestStatus@@@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::QuestSetStatus), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_EntityDoDamage, "?DoDamage@Entity@@QAEXABV1@KW4gEDamageType@@@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::EntityDoDamage), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_DiagnosticProjectileShoot, "?Shoot@PSProjectile@@QAEXXZ", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::DiagnosticProjectileShoot), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_PartyGetLeader, "?GetPartyLeader@PSParty@@QBE?AVEntity@@XZ", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::PartyGetLeader), "Script.dll"},
            {&ArenaJudge::Implementation::Hook_InfoConditions, "?AreConditionsFulfilled@gCInfo_PS@@IBE_NXZ", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::InfoConditions), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_InfoExecute, "?Execute@gCInfo_PS@@QAE_NPAVgCInfoManager_PS@@@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::InfoExecute), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_InfoDescription, "?GetDialogDescription@gCInfo_PS@@QBE?AVbCUnicodeString@@XZ", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::InfoDescription), "Game.dll"},
            {&ArenaJudge::Implementation::Hook_EntityHasInfo, "?HasInfo@Entity@@QBE_NABV1@@Z", reinterpret_cast<GELPVoid>(&ArenaJudge::Implementation::EntityHasInfo), "Script.dll"},
        };
        count = static_cast<GEU32>(sizeof(bindings) / sizeof(bindings[0]));
        return bindings;
    }
    void RemoveArenaHardSpeedUp() { ArenaJudge::Implementation::RemoveArenaHardSpeedUp(); }
    void PatchReactToDamageSvm() { ArenaJudge::Implementation::PatchReactToDamageSvm(); }
}

namespace PluginRuntime::Host
{
    GEBool IsArenaKnockout(Entity &victim)
    {
        using namespace ArenaJudge::Implementation;
        if (victim == None || !victim.IsNPC() || !HasRunningArenaQuest())
            return GEFalse;
        Entity opponent;
        return IsHeroInsideArenaZone() ||
            ((FindArenaOpponent(opponent) || FindRememberedOpponent(opponent)) && victim == opponent);
    }
}

namespace PluginRuntime::Host
{
    void OnArenaRoutineChange(GELPCChar a_strName, GEBool a_bIsTask)
    {
        using namespace ArenaJudge::Implementation;
        if (_strnicmp(a_strName, "PS_MagicPotion", 14) == 0)
            return;
        // v1.0.52.0: THE BOW / CROSSBOW (PS_Ranged*), the same two stages as the magic:
        //  - the task PS_Ranged (the bow / crossbow is taken in the hand) -> stage 1, the warning;
        //  - PS_Ranged_Shoot (the arrow / bolt is released) -> stage 2, the crime;
        //  - the other PS_Ranged_* names (aim, reload, walk, ...) change nothing and do NOT end the warning.
        if (_strnicmp(a_strName, "PS_Ranged", 9) == 0)
        {
            if (_stricmp(a_strName, "PS_Ranged_Shoot") == 0)
                PunishRangedShot(a_strName);
            else if (a_bIsTask && _stricmp(a_strName, "PS_Ranged") == 0)
                BeginMagicThreat();
            return;
        }
        GEBool const bMagicName = _strnicmp(a_strName, "PS_Magic", 8) == 0;
        if (!bMagicName)
        {
            // The task changed to something else: the magic mode is over.
            if (a_bIsTask)
                EndMagicThreat(GETrue); // v1.0.59.0: the hero put the weapon away during the warning: the opponent approves
            return;
        }
        if (_stricmp(a_strName, "PS_Magic") == 0)
        {
            if (a_bIsTask)
                BeginMagicThreat();
            return;
        }
        if (_stricmp(a_strName, "PS_Magic_QuickCast") == 0 || _stricmp(a_strName, "PS_Magic_Cast") == 0 ||
            _stricmp(a_strName, "PS_Magic_PowerCast") == 0 || _stricmp(a_strName, "PS_Magic_Summon") == 0)
            PunishMagicCast(a_strName);
        }

}

namespace GroupLogic::Host
{
    GEBool HasArenaWaiterName(bCString const &name) { return ArenaJudge::Implementation::FindWaiter(name) >= 0; }
    void RemoveArenaWaiterName(bCString const &name)
    { ArenaJudge::Implementation::RemoveNameFrom(ArenaJudge::Implementation::g_arrWaiters, ArenaJudge::Implementation::g_iWaiterCount, name); }
}



namespace ArenaJudge::Punishment
{

    GEBool FinishIfHeroDead()
    {
        using namespace ArenaJudge::Implementation;
        static GEBool finishing = GEFalse;
        Entity hero = Entity::GetPlayer();
        if (finishing || !IsArmed() || !IsArenaPunishment() || hero == None || !hero.IsDead())
            return GEFalse;
        finishing = GETrue;
        bCString const heldOpponent = g_strStage2Hold;
        g_strStage2Hold = bCString();
        g_u32Stage2HoldTick = 0;
        g_bOverLinePending = GEFalse;
        GEInt released = 0;
        bTObjArray<Entity> npcs = Entity::GetNPCs();
        for (GEInt i = 0; i < npcs.GetCount(); ++i)
        {
            Entity npc = npcs.GetAt(i);
            if (npc == None || !npc.IsNPC() || npc.IsDead() || npc.IsDown()) continue;
            GEBool const official = (!heldOpponent.IsEmpty() && npc.GetName() == heldOpponent) ||
                (!g_strArenaOpponent.IsEmpty() && npc.GetName() == g_strArenaOpponent);
            if (!(official || IsCurrentArenaWitness(npc))) continue;
            Entity target = npc.NPC.GetCurrentTarget();
            // End only the punishment of this hero; preserve unrelated combat.
            if (target != hero && !(official && target == None)) continue;
            if (official)
                EndArenaFightOfOpponent(npc);
            else
            {
                npc.Routine.FullStop();
                npc.NPC.SetCurrentTarget(None);
                npc.NPC.SetAlternativeTarget(None);
                npc.NPC.SetCurrentAttacker(None);
                npc.NPC.AttackReason = gEAttackReason_None;
                npc.NPC.CombatState = 0;
                npc.Routine.ContinueRoutine();
            }
            ++released;
        }
        ResetSpare();
        ClearAggro();
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-punishment] hero dead: punishment ended, %d attacker(s) released.\n", released);
        finishing = GEFalse;
        return GETrue;
    }
        GEBool IsCurrentArenaWitness(Entity &npc)
        {
            using namespace ArenaJudge::Implementation;

            if (npc == None)
                return GEFalse;
            for (GEInt i = 0; i < g_iArenaWitnessCount; ++i)
                if (g_arrArenaWitnessID[i] == npc.GetInstance()->GetID())
                    return GETrue;
            return GEFalse;
        }
        void NoteCurrentArenaWitness(Entity &npc)
        {
            using namespace ArenaJudge::Implementation;

            if (g_iArenaWitnessCount < AGGRO_MAX && !IsCurrentArenaWitness(npc))
                g_arrArenaWitnessID[g_iArenaWitnessCount++] = npc.GetInstance()->GetID();
        }

        // The arena owns this exception; social and group rules outside it stay native.
        GEBool IsArenaPunishmentActor(Entity &npc)
        {
            using namespace ArenaJudge::Implementation;

            return IsArmed() && IsArenaPunishment() && npc != None && npc.IsNPC() &&
                (IsCurrentArenaWitness(npc) ||
                 (!g_strArenaOpponent.IsEmpty() && npc.GetName() == g_strArenaOpponent));
        }
        GEBool IsArenaPunishmentBlow(Entity &victim, Entity &attacker)
        {
            using namespace ArenaJudge::Implementation;

            return (IsHeroSide(victim) && IsArenaPunishmentActor(attacker)) ||
                   (IsHeroSide(attacker) && IsArenaPunishmentActor(victim));
        }
        GEBool IgnoreArenaPunishmentTarget(Entity &observer, Entity &actor)
        {
            using namespace ArenaJudge::Implementation;

            if (observer == None || !observer.IsNPC() || observer == actor ||
                !IsArenaPunishmentActor(actor) ||
                !(IsHeroControlled(observer) || IsArenaPunishmentActor(observer)))
                return GEFalse;
            // Direct attacks on this NPC still permit self-defence.
            if (observer.NPC.GetCurrentAttacker() == actor || observer.NPC.GetLastAttacker() == actor)
                return GEFalse;
            return GETrue;
        }
        void LogArenaPunishmentFilter(Entity &observer, Entity &actor, GELPCChar where)
        {
            using namespace ArenaJudge::Implementation;

            static GEInt lines = 0;
            if (lines++ < 80)
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-punishment] %s: '%s' ignores lawful punishment by '%s'.\n",
                    where, observer.GetName().GetText(), actor.GetName().GetText());
        }

        // v1.0.61.0: the hero fell (stage 2): the opponent stops the fight and goes away, and says the line over the hero that lies on the
        // sand (the voice and the silence of a lying opponent are decided by ScheduleOverLine / SayArenaLine; no text on the screen).
        void ReleaseStage2Hold(GELPCChar a_strWhy)
        {
            using namespace ArenaJudge::Implementation;

            if (g_strStage2Hold.IsEmpty())
                return;
            bCString const strName = g_strStage2Hold;
            g_strStage2Hold = bCString();
            Entity Opponent = Entity::GetEntity(strName);
            if (Opponent == None || !Opponent.IsNPC() || Opponent.IsDead())
                return;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, stage 2: the hero is down (%s) - the opponent '%s' stops the fight "
                                         "and leaves.\n",
                                         a_strWhy, Opponent.GetName().GetText());
            ScheduleOverLine(Opponent); // the plaque is blocked, the line is said when the hero lies; silent if he lies himself
            if (!Opponent.IsDown())
                EndArenaFightOfOpponent(Opponent);
        }

        // v1.0.61.0: keeps the opponent of stage 2 in the attack (the engine may take him out of ZS_Attack when the duel quest is closed:
        // the log showed ZS_ReactToDamage_Loop). Called from the tick of every NPC; one flag read when idle, a technical throttle.
        void PollStage2Hold()
        {
            if (FinishIfHeroDead()) return;

            using namespace ArenaJudge::Implementation;

            if (g_strStage2Hold.IsEmpty())
                return;
            GEU32 const u32Now = ::GetTickCount();
            if (g_u32Stage2HoldTick != 0 && static_cast<GEInt>(u32Now - g_u32Stage2HoldTick) < 400)
                return;
            g_u32Stage2HoldTick = u32Now | 1;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None || !IsArmed())
            {
                g_strStage2Hold = bCString(); // the mercy is over: nobody holds him
                return;
            }
            if (IsHeroKnockedOut())
            {
                ReleaseStage2Hold("the knock-out is seen"); // the Defeat hook did not run for this fall
                return;
            }
            Entity Opponent = Entity::GetEntity(g_strStage2Hold);
            if (Opponent == None || !Opponent.IsNPC() || Opponent.IsDead() || Opponent.IsDown())
            {
                g_strStage2Hold = bCString();
                return;
            }
            if (IsAttackingHero(Opponent, Hero))
                return;
            ForceAttack(Opponent, Hero, gEAttackReason_Enemy);
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Arena, stage 2: the opponent '%s' is put back into the attack on the "
                                             "hero (the engine took him out of it).\n",
                                             Opponent.GetName().GetText());
            }
        }
    void OnArenaNpcTick(Entity &Self)
    {
        using namespace ArenaJudge::Implementation;
        if (FinishIfHeroDead()) return;
        PollWarnTimer();         // v1.0.58.0: the 5 s timer of the warning of the forbidden weapon (one flag read when idle)
        PollOverLine();          // v1.0.60.0: the voice of the opponent over the fallen hero (one flag read when idle)
        PollStage2Hold();        // v1.0.61.0: the opponent of stage 2 stays in the attack on the hero (one flag read when idle)
        PollFormAggro();         // v1.0.67.0: the crowd + the opponent are aimed at the body of the form (one flag read when idle)

        // v1.0.41.0: the opponent in the warning stance (the magic in the hand of the hero) is held in it on every tick.
        // v1.0.47.0: the first frame of the duel: the opponent that has just got the arena order is put into the stance now.
        if (g_bMagicThreat)
            HoldOpponentInWarning(Self);
        else
            PreemptMagicWarning(Self);

    }
}

namespace ArenaJudge::Combat
{
    void OnHeroConsumeStarted() { ArenaJudge::Implementation::OnHeroConsumeStarted(); }

    DamageContext BeforeDamage(Entity &Victim, Entity &Source)
    {
        using namespace ArenaJudge::Implementation;
        Entity Opponent;
        // The duel opponent is protected from the hero's elemental magic from the moment the duel is on, not only once the
        // fight physically started (the hit itself can be the first action of the hero).
        GEBool const bArenaVictim = FindArenaOpponent(Opponent) && Victim == Opponent && IsHeroInsideArenaZone();
        GEBool const bWasFrozen = bArenaVictim && Victim.NPC.IsFrozen();
        GEBool const bWasBurning = bArenaVictim && Victim.NPC.IsBurning();
        // v1.0.48.0: the hero in a monster body: the duel judging below is skipped (the native code runs as it is).
        GEBool const bDuelBlocked = bArenaVictim && IsHeroDuelBlocked();

        GELPCChar strReason = nullptr;
        GEBool bRangedHit = GEFalse; // v1.0.52.0: an arrow / bolt of the hero hits the opponent in the ring
        if (bArenaVictim)
        {
            gEDamageType const enuType = GetHitDamageType(Source, Victim);
            GEBool const bOwnedByOpponent = Source != None && Source.Interaction.IsValid() &&
                                            Source.Interaction.GetOwner() == Opponent;
            static GEInt s_iArenaDiagLines = 0;
            if (s_iArenaDiagLines < 200)
            {
                ++s_iArenaDiagLines;
                GEInt const iOwnType = Source != None && Source.Damage.IsValid()
                                           ? static_cast<GEInt>(Source.Damage.DamageType)
                                           : -1;
                GEBool const bOwnedByHero = Source != None && Source.Interaction.IsValid() &&
                                            Source.Interaction.GetOwner() == Entity::GetPlayer();
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] arena hit on '%s': source='%s' (own type=%d, "
                                             "owned by hero=%d), damageType=%d, ownedByOpponent=%d, "
                                             "burning=%d, frozen=%d, fightStarted=%d\n",
                                             Victim.GetName().GetText(),
                                             Source != None ? Source.GetName().GetText() : "-", iOwnType,
                                             static_cast<int>(bOwnedByHero), static_cast<int>(enuType),
                                             static_cast<int>(bOwnedByOpponent),
                                             static_cast<int>(Victim.NPC.IsBurning()),
                                             static_cast<int>(Victim.NPC.IsFrozen()),
                                             static_cast<int>(IsArenaFightStarted()));
            }
            // v1.0.52.0: the elemental damage of a burning / frozen opponent that comes from an enchanted melee weapon is allowed
            // (AllowArenaEnchantedWeapons); a cast, a shot or a bow in the hands keeps it forbidden.
            // v1.0.52.0: the second net of the shot of a bow / crossbow: an arrow / bolt of the hero reaches the opponent.
            if (enuType == gEDamageType_Missile && !bOwnedByOpponent && !bDuelBlocked && Source != None &&
                Source.Interaction.IsValid() && Source.Interaction.GetOwner() == Entity::GetPlayer())
                bRangedHit = GETrue;
            GEBool const bEnchantedTick = (Victim.NPC.IsBurning() || Victim.NPC.IsFrozen()) && IsEnchantedMeleeEffect();
            if (IsElementalDamage(enuType) && !bOwnedByOpponent && !bDuelBlocked && !bEnchantedTick)
                strReason = "forbidden fire / ice / lightning damage in the arena";
        }
        return {Opponent, bArenaVictim, bWasFrozen, bWasBurning, bDuelBlocked, strReason, bRangedHit};
    }
    void AfterDamage(DamageContext &state, Entity &Victim)
    {
        using namespace ArenaJudge::Implementation;
        auto &bArenaVictim = state.bArenaVictim;
        auto &bWasFrozen = state.bWasFrozen;
        auto &bWasBurning = state.bWasBurning;
        auto &bDuelBlocked = state.bDuelBlocked;
        auto &strReason = state.strReason;
        auto &bRangedHit = state.bRangedHit;
        if (!strReason && bArenaVictim && !bDuelBlocked && !Victim.IsDead())
        {
            if (!IsEnchantedMeleeEffect()) // v1.0.52.0: the status of an enchanted melee weapon is allowed
            {
                if (!bWasFrozen && Victim.NPC.IsFrozen())
                    strReason = "the arena opponent was frozen";
                else if (!bWasBurning && Victim.NPC.IsBurning())
                    strReason = "the arena opponent was set on fire";
            }
        }
        if (bRangedHit)
            PunishRangedShot("an arrow / a bolt hits the opponent");
        if (strReason)
            DeclareHeroCriminal(strReason);
    }
    DefeatContext BeforeDefeat(Entity &Self, Entity &Hero)
    {
        using namespace ArenaJudge::Implementation;
        DefeatContext state;
        if (Self == Hero && IsArenaPunishment())
        {
            // v1.0.60.0: the hero falls after a violation of the arena (the crowd punishes him): the opponent will speak over him.
            Entity ViolationOpponent;
            if (FindArenaOpponent(ViolationOpponent) || FindRememberedOpponent(ViolationOpponent) ||
                FindDuelOpponentAnywhere(ViolationOpponent))
                ScheduleOverLine(ViolationOpponent);
        }
        // v1.0.61.0: stage 2: the hero is knocked out - the opponent held in the fight stops it and leaves (the voice over the hero).
        if (Self == Hero)
            ReleaseStage2Hold("Defeat of the hero");
        if (Self == Hero)
        {
            PluginRuntime::Conflict::BeginNewFall(); // a new knock-out: the fall point is taken anew
            BeginHeroImmunity();      // v1.0.57.0: from now until he stands on his feet nobody may kill the hero
            RememberHeroFall(GEFalse); // mercy: the place where the hero fell (the arena loss is handled right below)
        }

        // The OPPONENT of the duel is knocked out by somebody who is not the hero (a witness, a companion, a summon): the
        // quest must not become a victory - the fight is lost. The one who dealt the blow is the current attacker of the
        // knocked-out NPC (the last attacker when it is empty); nobody written down (a burning tick of the hero's fire) is
        // not a third party.
        GEBool &bThirdParty = state.bThirdParty;
        if (!(Self == Hero) && Self.IsNPC() && HasRunningArenaQuest())
        {
            Entity OppSeen;
            GEBool bIsOpponent = FindArenaOpponent(OppSeen) && OppSeen == Self;
            if (!bIsOpponent && !g_strArenaOpponent.IsEmpty() && Self.GetName() == g_strArenaOpponent)
                bIsOpponent = GETrue;
            if (bIsOpponent)
            {
                Entity Who = Self.NPC.GetCurrentAttacker();
                if (Who == None)
                    Who = Self.NPC.GetLastAttacker();
                if (Who != None && !(Who == Hero) && Who.IsNPC())
                {
                    bThirdParty = GETrue;
                    g_bLostToThirdParty = GETrue; // the status hook turns Success / Won into Lost
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The arena opponent '%s' is knocked out by '%s', not by "
                                                 "the hero: the duel is lost.\n",
                                                 Self.GetName().GetText(), Who.GetName().GetText());
                }
            }
        }

        Entity &Opponent = state.Opponent;
        GEBool &bForcedLoss = state.bForcedLoss;
        // The arena fight is on: the opponent is found by his attack reason, or - when the reason already changed
        // (witnesses attack the hero too) - by the remembered name while the duel quest still runs.
        GEBool bArenaOn = GEFalse;
        if (Self == Hero)
        {
            GEBool const bQuest = HasRunningArenaQuest();
            bArenaOn = (IsArenaFightStarted() && FindArenaOpponent(Opponent)) ||
                       (bQuest && (FindArenaOpponent(Opponent) || FindRememberedOpponent(Opponent)));
            static GEInt s_iDefeatLines = 0;
            if (s_iDefeatLines < 60)
            {
                ++s_iDefeatLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] Defeat of the hero: duel quest running=%d, "
                                             "arena fight=%d, opponent '%s' -> %s\n",
                                             static_cast<int>(bQuest), static_cast<int>(IsArenaFightStarted()),
                                             Opponent != None ? Opponent.GetName().GetText() : "-",
                                             bArenaOn ? "arena knock-out" : "not an arena fight");
            }
        }
        if (bArenaOn)
        {
            Entity Attacker = Hero.NPC.GetCurrentAttacker();
            GEBool const bByOpponent = Attacker == Opponent;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The hero is knocked out in the arena fight (opponent '%s', "
                                         "knocked out by '%s').\n",
                                         Opponent.GetName().GetText(),
                                         Attacker != None ? Attacker.GetName().GetText() : "-");
            if (!bByOpponent)
            {
                bForcedLoss = GETrue;
                LogArenaQuests("before knock-out");
                Hero.NPC.SetCurrentAttacker(Opponent);
            }
        }

        return state;
    }
    void AfterDefeat(DefeatContext &state, Entity &Self, Entity &Hero)
    {
        using namespace ArenaJudge::Implementation;
        auto &bThirdParty = state.bThirdParty;
        auto &bForcedLoss = state.bForcedLoss;
        auto &Opponent = state.Opponent;
        if (bThirdParty)
        {
            // The native Defeat of the opponent has reported the "victory" to the quest manager; if the quest still runs
            // (the status hook did not have to act), the loss is reported the way a consumable does it.
            if (HasRunningArenaQuest())
            {
                Hero.NPC.SetCurrentAttacker(Self);
                Entity World = Entity::GetWorldEntity();
                World.QuestManager.OnNPCDefeated(Hero);
            }
            LogArenaQuests("opponent knocked out by a third party");
            AnnounceLossToThirdParty();
        }

        if (bForcedLoss)
        {
            if (HasRunningArenaQuest())
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The duel quest is still running after Defeat: "
                                             "OnNPCDefeated is called by hand.\n");
                Hero.NPC.SetCurrentAttacker(Opponent);
                Entity World = Entity::GetWorldEntity();
                World.QuestManager.OnNPCDefeated(Hero);
            }
            LogArenaQuests("after knock-out");
            EndArenaFightOfOpponent(Opponent);
            g_bDuelLossDone = GETrue; // done, the common end of the duel below must not repeat it
            // v1.0.59.0: the artificial line "Бой проигран!" is removed: the loss is silent.
        }
        // After a violation of the arena rules the duel ends for everybody WHOEVER knocked the hero out - also when the
        // opponent himself did it (the native code then did not stop him, he attacked the hero again after he got up).
        if (Self == Hero && IsArenaPunishment() && !bForcedLoss)
            EndArenaDuelAfterKnockout();
    }
    GEBool IsDuelTaboo(Entity &npc) { return ArenaJudge::Implementation::IsDuelTaboo(npc); }
    GEBool IsWarnedOpponent(Entity &npc) { return ArenaJudge::Implementation::IsWarnedOpponent(npc); }
    GEBool PreemptMagicWarning(Entity &npc) { return ArenaJudge::Implementation::PreemptMagicWarning(npc); }
}

namespace ArenaJudge::Combat
{
    GEInt FinishHeroDecision(Entity &Self, GEInt iResult)
    {
        using namespace ArenaJudge::Implementation;
        // v1.0.57.0: THE IRON IMMUNITY. The hero is down, in the animation of the fall or only rising: the answer is NO for
        // everybody, wherever the NPC stands and whatever the mercy / the arena / the attack reason are. It is lifted only by
        // UpdateHeroImmunity (the hero is up and in the routine PS_Normal_Loop / PS_Melee_Loop ...).
        if (UpdateHeroImmunity())
        {
            static GEInt s_iImmuneLines = 0;
            if (s_iImmuneLines < 60)
            {
                ++s_iImmuneLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] '%s' asks whether to kill the hero: attack reason=%d, the hero "
                                             "is down / rising -> the plugin: NO\n",
                                             Self.GetName().GetText(), static_cast<int>(Self.NPC.AttackReason));
            }
            return 0;
        }

        // Mercy: armed by a violation / the second blow, the hero is down, the NPC stands within SPARE_RADIUS of the
        // place where the hero fell. A running arena fight is protected as before.
        GEFloat fDistance = -1.0f;
        GEBool const bArenaFight = IsArenaFightStarted();
        GEBool const bMercy = SpareApplies(Self, fDistance);

        static GEInt s_iAskLines = 0;
        if (s_iAskLines < 120)
        {
            ++s_iAskLines;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] '%s' asks whether to kill the hero: attack reason=%d, "
                                         "mercy armed=%d, distance from the fall point=%.0f, arena fight=%d -> %s\n",
                                         Self.GetName().GetText(), static_cast<int>(Self.NPC.AttackReason),
                                         static_cast<int>(IsArmed()), static_cast<double>(fDistance),
                                         static_cast<int>(bArenaFight),
                                         (bMercy || bArenaFight) ? "the plugin: NO" : "the plugin: leaves yes");
        }
        if (!bMercy && !bArenaFight)
            return iResult;

        DebugLog::GetLogger().LogFormat("[GenomeLogicFix] '%s' does not finish the hero off (attack reason %d): he takes "
                                     "the weapon and the gold instead.\n",
                                     Self.GetName().GetText(), static_cast<int>(Self.NPC.AttackReason));
        return 0;
        }
        void OnHeroSummonCast(GELPCChar a_strScript, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity,
                              GELPVoid a_pOtherEntity)
        {
            using namespace ArenaJudge::Implementation;

            static GEBool s_bBusy = GEFalse;
            if (s_bBusy)
                return;
            Entity Hero = Entity::GetPlayer();
            Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
            Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
            // The caster is the hero, or the hero is in his magic task (the script may run on the spell entity).
            if (!(Self == Hero) && !(Other == Hero) && !IsHeroCasting(Hero))
                return;
            // v1.0.40.0: the punishment is given by the state tracker (PS_Magic_Summon / PS_Magic_QuickCast), not here: this
            // hook only logs a summon cast inside the ring.
            if (!HasRunningArenaQuest() || !IsHeroInsideArenaZone())
                return; // no duel / outside the ring: summoning is free
            static GEInt s_iLines = 0;
            if (s_iLines < 60)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] arena: the hero casts a summon (%s, self='%s', "
                                             "other='%s', last spell='%s') -> the crowd reacts at once\n",
                                             a_strScript, Self != None ? Self.GetName().GetText() : "-",
                                             Other != None ? Other.GetName().GetText() : "-",
                                             bCString(Hero.NPC.LastSpell).GetText());
            }
            s_bBusy = GETrue;
            s_bBusy = GEFalse;
        }
        GEBool IsHeroHealSpell(bCString const &a_strSpell, GEInt &o_iCastType, bCString &o_strHitFunc)
        {
            using namespace ArenaJudge::Implementation;

            GEInt iSpellType = -1;
            bCString strCastFunc;
            GetSpellDamageType(a_strSpell, iSpellType, strCastFunc, &o_iCastType, &o_strHitFunc);
            if (iSpellType < 0)
                return GEFalse;
            // The spells that replace a consumable: heal himself, cure poison, cure weakness (disease). The potions with
            // these effects are forbidden in the arena as well. MagicHealOther is NOT in the list.
            return (_stricmp(o_strHitFunc.GetText(), "MagicHeal") == 0 ||
                    _stricmp(o_strHitFunc.GetText(), "MagicCurePoison") == 0 ||
                    _stricmp(o_strHitFunc.GetText(), "MagicCureDisease") == 0) &&
                   o_iCastType == static_cast<GEInt>(gECastType_Regular);
        }

        void OnHeroHealCast(GELPCChar a_strScript, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity)
        {
            using namespace ArenaJudge::Implementation;

            Entity Hero = Entity::GetPlayer();
            Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
            // The hero casts it himself (his magic task; a potion is PS_MagicPotion and is handled by the consumable hook).
            if (!(Self == Hero) || !IsHeroCasting(Hero))
                return;
            // v1.0.40.0: with a Running arena quest and the hero in the ring the magic is judged by the state tracker (the
            // cast state is the punishment, for every spell). The old consumable rule below only covers a duel without quest.
            if (HasRunningArenaQuest() && IsHeroInsideArenaZone())
                return;
            if (!IsArenaFightStarted() || !IsHeroInsideArenaZone())
                return;
            GEInt iCastType = -1;
            bCString strHitFunc;
            bCString const strSpell = Hero.NPC.LastSpell;
            GEBool const bHeal = IsHeroHealSpell(strSpell, iCastType, strHitFunc);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] arena: the hero casts %s (spell '%s', cast type=%d, "
                                         "hit script '%s') -> %s\n",
                                         a_strScript, strSpell.GetText(), static_cast<int>(iCastType),
                                         strHitFunc.GetText(),
                                         bHeal ? "a heal / cure like a consumable" : "left alone (fast variant / unknown)");
            if (bHeal)
                OnHeroConsumeStarted();
        }
}

namespace ArenaJudge::Loops
{
        // True: the NPC was handled (the attack was dropped), the native loop must not run in this tick.
        GEBool FixStaleHeroTarget(Entity &a_Npc)
        {
            using namespace ArenaJudge::Implementation;

            if (a_Npc == None || !a_Npc.IsNPC())
                return GEFalse;
            Entity Target = a_Npc.NPC.GetCurrentTarget();
            Entity Player = Entity::GetPlayer();
            GEBool bStale = GEFalse;
            if (Target != None)
            {
                // The removed body of the hero: it is not the player any more (the beast is).
                bCString const strTarget = Target.GetName();
                bStale = !(Target == Player) && strTarget.GetText() != nullptr && _stricmp(strTarget.GetText(), "PC_Hero") == 0;
            }
            else
            {
                // No target at all, but the plugin sent him against the hero (the crime): the target is lost. v1.0.67.0: only while the
                // hero is in a form (or the form is awaited): after a knock-out or a normal end of a fight an empty target is the native
                // end of the attack and is not repaired.
                if (IsHeroKnockedOut() || !(g_bFormAggroPending || IsHeroTransformedNow()))
                    return GEFalse;
                bStale = FindAggro(a_Npc.GetName()) >= 0 && a_Npc.NPC.AttackReason != gEAttackReason_None;
            }
            if (!bStale)
                return GEFalse;

            static GEInt s_iLines = 0;
            if (Player != None && !Player.IsDead())
            {
                a_Npc.NPC.SetCurrentTarget(Player);
                if (s_iLines < 30)
                {
                    ++s_iLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] ZS_Attack_Loop: '%s' had the removed body of the hero as the target - "
                                                 "he is aimed at the current body of the hero '%s'.\n",
                                                 a_Npc.GetName().GetText(), Player.GetName().GetText());
                }
                return GEFalse;
            }
            a_Npc.Routine.FullStop();
            a_Npc.NPC.SetCurrentTarget(None);
            a_Npc.NPC.AttackReason = gEAttackReason_None;
            a_Npc.Routine.ContinueRoutine();
            if (s_iLines < 30)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] ZS_Attack_Loop: '%s' has no body of the hero to attack - the attack is "
                                             "dropped.\n",
                                             a_Npc.GetName().GetText());
            }
            return GETrue;
        }
    GEBool ReleaseInvalidPunishmentTarget(gCScriptProcessingUnit *a_pSPU)
    {
        using namespace ArenaJudge::Implementation;
        {
            Entity Fighter(a_pSPU->GetSelfEntity());
            Entity target = Fighter != None && Fighter.IsNPC() ? Fighter.NPC.GetCurrentTarget() : Entity();
            if (IgnoreArenaPunishmentTarget(Fighter, target))
            {
                LogArenaPunishmentFilter(Fighter, target, "ZS_Attack_Loop");
                Fighter.Routine.FullStop();
                Fighter.NPC.SetCurrentTarget(None);
                Fighter.NPC.SetAlternativeTarget(None);
                Fighter.NPC.AttackReason = gEAttackReason_None;
                Fighter.NPC.CombatState = 0;
                if (IsHeroControlled(Fighter))
                    Fighter.Routine.SetTask(Fighter.Party.Waiting ? "ZS_WaitForPlayer" : "ZS_FollowPlayer");
                else
                    Fighter.Routine.ContinueRoutine();
                return GETrue;
            }
        }
        return GEFalse;
    }
    GEBool ReleaseProtectedPartyTarget(gCScriptProcessingUnit *a_pSPU)
    {
        using namespace ArenaJudge::Implementation;
        {
            Entity Gladiator(a_pSPU->GetSelfEntity());
            Entity GTarget = Gladiator != None ? Gladiator.NPC.GetCurrentTarget() : Entity();
            if (Gladiator != None && GTarget != None && IsHeroControlled(GTarget) && IsDuelTaboo(Gladiator))
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The duel opponent '%s' leaves '%s' (party of the hero) alone.\n",
                                             Gladiator.GetName().GetText(), GTarget.GetName().GetText());
                Gladiator.Routine.FullStop();
                Gladiator.NPC.SetCurrentTarget(None);
                Gladiator.NPC.SetCurrentAttacker(None);
                Gladiator.NPC.AttackReason = gEAttackReason_None;
                Gladiator.Routine.ContinueRoutine();
                return GETrue;
            }
        }
        return GEFalse;
    }

    void PollCompanions()
    {
        ArenaJudge::Implementation::CheckArenaCompanions();
        ArenaJudge::Implementation::TrackDuelForCompanions();
    }

    GEBool HoldWarning(Entity &npc)
    {
        return ArenaJudge::Implementation::PreemptMagicWarning(npc) || ArenaJudge::Implementation::HoldOpponentInWarning(npc);
    }
}

namespace ArenaJudge::ConflictHooks
{
    GEBool IsFormTransitionPending() { return ArenaJudge::Implementation::g_bFormAggroPending; }
    void ResetBeforeMercy()
    {
        ArenaJudge::Implementation::ResetFormAggro();
        ArenaJudge::Punishment::g_strStage2Hold = bCString();
        ArenaJudge::Implementation::g_bOverLineSaid = GEFalse;
        ArenaJudge::Implementation::g_bOverLinePending = GEFalse;
    }
    void ResetDuelLoss() { ArenaJudge::Implementation::g_bDuelLossDone = GEFalse; }
    void ClearWitnesses() { ArenaJudge::Punishment::g_iArenaWitnessCount = 0; }
    void EndDuelAfterKnockout() { ArenaJudge::Implementation::EndArenaDuelAfterKnockout(); }
}

namespace ArenaJudge::Loops
{
    void OnHeroTeleported()
    {
        ArenaJudge::Implementation::CancelAbandonedArena("confirmed hero teleport", GEFalse);
    }
}

namespace ArenaJudge::Native
{
    void OnHeroAttackerSet(PSNpc *npc, Entity const *attacker, GELPCChar where)
    { ArenaJudge::Implementation::OnHeroAttackerSet(npc, attacker, where); }
    void NpcEnableStatusEffects(PSNpc *a_pThis, GEInt a_iMask, GEBool a_bEnable, NpcEnableStatusEffectsOriginal original)
    {
        using namespace ArenaJudge::Implementation;

            using mFOriginal = void(GE_STDCALL *)(PSNpc *, GEInt, GEBool);
            GEBool const bWatched = a_pThis && a_bEnable && (a_iMask & 3) != 0 && a_pThis->m_pEngineEntityPropertySet;
            GEBool const bWasBurning = bWatched && (a_iMask & 1) ? a_pThis->IsBurning() : GEFalse;
            GEBool const bWasFrozen = bWatched && (a_iMask & 2) ? a_pThis->IsFrozen() : GEFalse;

            original(a_pThis, a_iMask, a_bEnable);

            static GEBool s_bBusy = GEFalse; // the witnesses' AssessTarget must not re-enter
            if (!bWatched || s_bBusy)
                return;
            GEBool const bBurningStarted = (a_iMask & 1) && !bWasBurning && a_pThis->IsBurning();
            GEBool const bFrozenStarted = (a_iMask & 2) && !bWasFrozen && a_pThis->IsFrozen();
            if (!bBurningStarted && !bFrozenStarted)
                return;

            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (!pOwner)
                return;
            Entity Victim(pOwner);
            Entity Hero = Entity::GetPlayer();
            Entity Opponent;
            if (Victim == Hero || !FindArenaOpponent(Opponent) || !(Victim == Opponent))
                return;
            if (IsHeroDuelBlocked())
                return; // v1.0.48.0: the hero in a monster body: no duel judging, the original already ran
            if (!IsHeroInsideArenaZone())
            {
                LogOutsideRing("setting the opponent on fire / freezing him");
                return;
            }
            // v1.0.52.0: AllowArenaEnchantedWeapons: the status of a fire / ice SWORD (no cast, no shot) is no violation.
            if (IsEnchantedMeleeEffect())
            {
                static GEInt s_iEnchantedLines = 0;
                if (s_iEnchantedLines < 40)
                {
                    ++s_iEnchantedLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] arena: %s started on '%s' by an enchanted melee weapon "
                                                 "(AllowArenaEnchantedWeapons=true): allowed, the melee duel goes on.\n",
                                                 bBurningStarted ? "BURNING" : "FREEZING", Victim.GetName().GetText());
                }
                return;
            }

            // Only the hero can set the opponent on fire / freeze him in the arena. If somebody else is already written
            // down as the attacker, it is not the hero's doing.
            Entity Attacker = Victim.NPC.GetLastAttacker();
            Entity Current = Victim.NPC.GetCurrentAttacker();
            GEBool const bOther = (Attacker != None && !(Attacker == Hero)) || (Current != None && !(Current == Hero));
            static GEInt s_iStatusLines = 0;
            if (s_iStatusLines < 100)
            {
                ++s_iStatusLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] arena: %s started on '%s' (last attacker '%s', "
                                             "current attacker '%s', last spell '%s') -> %s\n",
                                             bBurningStarted ? "BURNING" : "FREEZING", Victim.GetName().GetText(),
                                             Attacker != None ? Attacker.GetName().GetText() : "-",
                                             Current != None ? Current.GetName().GetText() : "-",
                                             bCString(Hero.NPC.LastSpell).GetText(),
                                             bOther ? "not the hero, ignored" : "the crowd reacts at once");
            }
            if (bOther)
                return;

            s_bBusy = GETrue;
            DeclareHeroCriminal(bBurningStarted ? "the arena opponent was set on fire"
                                                : "the arena opponent was frozen",
                                GETrue);
            s_bBusy = GEFalse;
            }
    void NpcSetLastCast(PSNpc *a_pThis, NpcSetLastCastOriginal original)
    {
        using namespace ArenaJudge::Implementation;

            using mFOriginal = void(GE_STDCALL *)(PSNpc *);
            original(a_pThis);
            if (!a_pThis || !a_pThis->m_pEngineEntityPropertySet)
                return;
            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (pOwner && Entity(pOwner) == Entity::GetPlayer())
            {
                g_u32HeroCastTick = ::GetTickCount();
                if (g_u32HeroCastTick == 0)
                    g_u32HeroCastTick = 1;
                ++g_u32HeroCastSeq;
                LogMagicAim("SetLastCast after native update");
                static GEInt s_iCastLines = 0;
                if (s_iCastLines < 30)
                {
                    ++s_iCastLines;
                    bCString const strSpell = Entity::GetPlayer().NPC.LastSpell;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] hero cast '%s'\n", strSpell.GetText());
                }
            }
            }
    void GuiPrintGameMessage(GUI2 *a_pThis, bCUnicodeString const *a_pText, gEGameMessageType a_enuType, GuiPrintGameMessageOriginal original)
    {
        using namespace ArenaJudge::Implementation;

            using mFOriginal = void(GE_STDCALL *)(GUI2 *, bCUnicodeString const *, gEGameMessageType);
            if (a_enuType == gEGameMessageType_Failure && g_u32PlaqueBlockUntil != 0 &&
                static_cast<GEInt>(::GetTickCount() - g_u32PlaqueBlockUntil) < 0)
            {
                static GEInt s_iLines = 0;
                if (s_iLines < 20)
                {
                    ++s_iLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The native plaque of the end of the arena (Failure) is blocked: "
                                                 "only the voice is heard.\n");
                }
                return;
            }
            original(a_pThis, a_pText, a_enuType);
            }
    void EntitySetTimeScale(Entity *a_pThis, GEFloat a_fScale, EntitySetTimeScaleOriginal original)
    {
        using namespace ArenaJudge::Implementation;

            using mFOriginal = void(GE_STDCALL *)(Entity *, GEFloat);
            original(
                a_pThis, ClampOpponentTimeScale(a_pThis ? a_pThis->GetInstance() : nullptr, a_fScale));
            }
    void RoutineSetTimeScale(PSRoutine *a_pThis, GEFloat a_fScale, RoutineSetTimeScaleOriginal original)
    {
        using namespace ArenaJudge::Implementation;

            using mFOriginal = void(GE_STDCALL *)(PSRoutine *, GEFloat);
            eCEntity *pOwner = a_pThis ? static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity() : nullptr;
            original(a_pThis, ClampOpponentTimeScale(pOwner, a_fScale));
            }
    GEBool NpcCanHit(PSNpc *a_pThis, GEU32 a_u32MinDuration, NpcCanHitOriginal original)
    {
        using namespace ArenaJudge::Implementation;

            using mFOriginal = GEBool(GE_STDCALL *)(PSNpc *, GEU32);
            eCEntity *pOwner = a_pThis && a_pThis->m_pEngineEntityPropertySet
                                   ? static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity()
                                   : nullptr;
            // v1.0.59.0: DisableMinHitDurationOnHard=true: the native "no pause between the blows" of the arena opponent returns.
            if (pOwner && !GetConfig().DisableMinHitDurationOnHard)
            {
                Entity Owner(pOwner);
                if (Owner.IsNPC() && Owner.NPC.AttackReason == gEAttackReason_Arena)
                {
                    Owner.NPC.AttackReason = gEAttackReason_Duel;
                    GEBool const bResult = original(a_pThis, a_u32MinDuration);
                    Owner.NPC.AttackReason = gEAttackReason_Arena;
                    return bResult;
                }
            }
            return original(a_pThis, a_u32MinDuration);
            }
    GEBool ClearNewArenaAttemptCounters(gCQuest_PS *quest, gEQuestStatus const *requested,
                                        bTValArray<GEI32> &saved)
    {
        if (!quest || !requested || quest->GetType() != gEQuestType_Arena ||
            *requested != gEQuestStatus_Running ||
            (quest->GetStatus() != gEQuestStatus_Open && quest->GetStatus() != gEQuestStatus_Lost))
            return GEFalse;
        auto &counters = quest->AccessDeliveryCounter();
        auto const &targets = quest->GetDeliveryEntities();
        if (counters.GetCount() != targets.GetCount() ||
            counters.GetCount() != quest->GetDeliveryAmounts().GetCount())
        {
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-attempt] '%s': counter/target sizes differ; reset skipped.\n",
                quest->GetName().GetText());
            return GEFalse;
        }
        saved = counters;
        for (GEInt i = 0; i < counters.GetCount(); ++i)
        {
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-attempt] '%s': target[%d]='%s' progress %d -> 0 for new attempt.\n",
                quest->GetName().GetText(), i, targets[i].GetText(), counters[i]);
            counters[i] = 0;
        }
        return GETrue;
    }

    GEBool QuestSetStatus(gCQuest_PS *a_pThis, gEQuestStatus const *a_penuStatus, QuestSetStatusOriginal original, GELPVoid caller)
    {
        using namespace ArenaJudge::Implementation;

            using mFOriginal = GEBool(GE_STDCALL *)(gCQuest_PS *, gEQuestStatus const *);
            // v1.0.43.0 diagnostics: every status change of an arena quest, with the state of the hero (who restarts the duel
            // quest after the ransack shows up here).
            if (a_pThis && a_penuStatus && a_pThis->GetType() == gEQuestType_Arena)
            {
                static GEInt s_iStatusLines = 0;
                if (s_iStatusLines < 80)
                {
                    ++s_iStatusLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] arena quest '%s': SetStatus(%d), now %d, hero down=%d, "
                                                 "mercy armed=%d active=%d, caller %p\n",
                                                 a_pThis->GetName().GetText(), static_cast<int>(*a_penuStatus),
                                                 static_cast<int>(a_pThis->GetStatus()), static_cast<int>(IsHeroKnockedOut()),
                                                 static_cast<int>(IsArmed()), static_cast<int>(IsFallActive()),
                                                 caller);
                }
            }
            if (a_pThis && a_penuStatus && g_bLostToThirdParty &&
                (*a_penuStatus == gEQuestStatus_Success || *a_penuStatus == gEQuestStatus_Won) &&
                a_pThis->GetType() == gEQuestType_Arena)
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] duel quest '%s': status %d replaced with Lost (the opponent "
                                             "was not beaten by the hero).\n",
                                             a_pThis->GetName().GetText(), static_cast<int>(*a_penuStatus));
                gEQuestStatus const enuLost = gEQuestStatus_Lost;
                GEBool const bResult = original(a_pThis, &enuLost);
                AnnounceLossToThirdParty();
                return bResult;
            }
            GEBool const bWasLost = a_pThis && a_pThis->GetStatus() == gEQuestStatus_Lost ? GETrue : GEFalse; // v1.0.68.0
            bTValArray<GEI32> savedCounters;
            GEBool const bCleared = ClearNewArenaAttemptCounters(a_pThis, a_penuStatus, savedCounters);
            GEBool const bSetResult = original(a_pThis, a_penuStatus);
            if (bCleared && !bSetResult)
            {
                a_pThis->AccessDeliveryCounter() = savedCounters;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][arena-attempt] '%s': start rejected; counters restored.\n",
                    a_pThis->GetName().GetText());
            }
            if (bSetResult && a_pThis && a_penuStatus && *a_penuStatus == gEQuestStatus_Running &&
                a_pThis->GetStatus() == gEQuestStatus_Running && a_pThis->GetType() == gEQuestType_Arena)
                OnArenaQuestStarted(bWasLost); // the dialogue with the master of the arena: the companions wait NOW
            return bSetResult;
            }
}


ME_DEFINE_AND_HOOK_SCRIPT(MagicPoison)
{
    return SocialLogic::Hooks::HostileMagic(MagicPoison_Original, "MagicPoison", a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
}

ME_DEFINE_AND_HOOK_SCRIPT(MagicDisease)
{
    return SocialLogic::Hooks::HostileMagic(MagicDisease_Original, "MagicDisease", a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
}

ME_DEFINE_AND_HOOK_SCRIPT(MagicFear)
{
    return SocialLogic::Hooks::HostileMagic(MagicFear_Original, "MagicFear", a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
}

ME_DEFINE_AND_HOOK_SCRIPT(MagicSleep)
{
    return SocialLogic::Hooks::HostileMagic(MagicSleep_Original, "MagicSleep", a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
}

ME_DEFINE_AND_HOOK_SCRIPT(MagicFreeze)
{
    return SocialLogic::Hooks::HostileMagic(MagicFreeze_Original, "MagicFreeze", a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
}

ME_DEFINE_AND_HOOK_SCRIPT(MagicStun)
{
    return SocialLogic::Hooks::HostileMagic(MagicStun_Original, "MagicStun", a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
}
