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

//
// Genome Logic Fix: arena judging (stages 2, 3, 4) and the INI configuration (stage 6).
//
namespace RequirementsCheck
{
    //
    // Configuration: <Gothic 3 folder>\Ini\Script_GenomeLogicFix.ini, section [Main], line "PunishArenaPotions=true".
    //
    namespace
    {
        GELPCChar const INI_FILE_NAME = "Script_GenomeLogicFix.ini";
        GELPCChar const INI_SECTION = "Main";
        GELPCChar const INI_KEY_PUNISH_POTIONS = "PunishArenaPotions";
        GELPCChar const INI_KEY_IGNORE_TELEPORT = "IgnoreTeleportNames"; // v1.0.51.0
        GELPCChar const INI_MISSING_MARK = "<<missing>>";                // tells "no such key" from "the key is empty"
        GELPCChar const INI_KEY_ENCHANTED = "AllowArenaEnchantedWeapons"; // v1.0.52.0
        GELPCChar const INI_KEY_HARD_SPEED = "EnableHardDifficultyAttackSpeed"; // v1.0.59.0
        GELPCChar const INI_KEY_MIN_HIT = "DisableMinHitDurationOnHard";        // v1.0.59.0
        GELPCChar const INI_KEY_CRASH_DIAG = "CrashDiagnostics";                 // v1.0.66.0

        GEBool ParseIniBool(GELPCChar a_strValue)
        {
            return !(_stricmp(a_strValue, "false") == 0 || strcmp(a_strValue, "0") == 0 || _stricmp(a_strValue, "no") == 0 ||
                     _stricmp(a_strValue, "off") == 0);
        }

        Config g_Config;

        void BuildIniPath(char *o_pBuffer, size_t a_uSize)
        {
            char strExe[MAX_PATH] = {};
            ::GetModuleFileNameA(nullptr, strExe, MAX_PATH);
            char *pSlash = strrchr(strExe, '\\');
            if (pSlash)
                *pSlash = 0;
            _snprintf_s(o_pBuffer, a_uSize, _TRUNCATE, "%s\\Ini\\%s", strExe, INI_FILE_NAME);
        }
    } // namespace

    Config &GetConfig()
    {
        return g_Config;
    }

    GEBool SaveConfig()
    {
        char strPath[MAX_PATH + 64] = {};
        BuildIniPath(strPath, sizeof(strPath));
        GEBool const bPotions = ::WritePrivateProfileStringA(INI_SECTION, INI_KEY_PUNISH_POTIONS,
                                                             g_Config.PunishArenaPotions ? "true" : "false", strPath)
                                    ? GETrue
                                    : GEFalse;
        GEBool const bNames = ::WritePrivateProfileStringA(INI_SECTION, INI_KEY_IGNORE_TELEPORT,
                                                           g_Config.IgnoreTeleportNames, strPath)
                                  ? GETrue
                                  : GEFalse;
        GEBool const bEnchanted = ::WritePrivateProfileStringA(INI_SECTION, INI_KEY_ENCHANTED,
                                                               g_Config.AllowArenaEnchantedWeapons ? "true" : "false", strPath)
                                      ? GETrue
                                      : GEFalse;
        GEBool const bHardSpeed = ::WritePrivateProfileStringA(INI_SECTION, INI_KEY_HARD_SPEED,
                                                               g_Config.EnableHardDifficultyAttackSpeed ? "true" : "false", strPath)
                                      ? GETrue
                                      : GEFalse;
        GEBool const bMinHit = ::WritePrivateProfileStringA(INI_SECTION, INI_KEY_MIN_HIT,
                                                            g_Config.DisableMinHitDurationOnHard ? "true" : "false", strPath)
                                   ? GETrue
                                   : GEFalse;
        GEBool const bCrashDiag = ::WritePrivateProfileStringA(INI_SECTION, INI_KEY_CRASH_DIAG,
                                                               g_Config.CrashDiagnostics ? "true" : "false", strPath)
                                      ? GETrue
                                      : GEFalse;
        return bPotions && bNames && bEnchanted && bHardSpeed && bMinHit && bCrashDiag ? GETrue : GEFalse;
    }

    void LoadConfig()
    {
        char strPath[MAX_PATH + 64] = {};
        BuildIniPath(strPath, sizeof(strPath));

        // Every key is read first, and the file is written once at the end when any key was missing (the defaults of the
        // missing ones, the values of the existing ones are kept).
        GEBool bWrite = GEFalse;

        char strValue[32] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_PUNISH_POTIONS, "", strValue, sizeof(strValue), strPath);
        if (strValue[0] == 0)
            bWrite = GETrue; // File or key missing: keep the default (true) and write it.
        else
        {
            g_Config.PunishArenaPotions = !(_stricmp(strValue, "false") == 0 || strcmp(strValue, "0") == 0 ||
                                            _stricmp(strValue, "no") == 0 || _stricmp(strValue, "off") == 0);
            GetDefaultLogger().LogFormat("[GenomeLogicFix] PunishArenaPotions=%s\n",
                                         g_Config.PunishArenaPotions ? "true" : "false");
        }

        // v1.0.51.0: IgnoreTeleportNames. An empty value is a valid answer ("nobody is blacklisted"), a missing key is not.
        char strNames[sizeof(g_Config.IgnoreTeleportNames)] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_IGNORE_TELEPORT, INI_MISSING_MARK, strNames, sizeof(strNames), strPath);
        if (strcmp(strNames, INI_MISSING_MARK) == 0)
            bWrite = GETrue; // keep the default of the structure and write it
        else
            strncpy_s(g_Config.IgnoreTeleportNames, sizeof(g_Config.IgnoreTeleportNames), strNames, _TRUNCATE);
        GetDefaultLogger().LogFormat("[GenomeLogicFix] IgnoreTeleportNames=%s\n", g_Config.IgnoreTeleportNames);

        // v1.0.52.0: AllowArenaEnchantedWeapons (default true; a missing key is written with the default).
        char strEnchanted[32] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_ENCHANTED, "", strEnchanted, sizeof(strEnchanted), strPath);
        if (strEnchanted[0] == 0)
            bWrite = GETrue;
        else
            g_Config.AllowArenaEnchantedWeapons = ParseIniBool(strEnchanted);
        GetDefaultLogger().LogFormat("[GenomeLogicFix] AllowArenaEnchantedWeapons=%s\n",
                                     g_Config.AllowArenaEnchantedWeapons ? "true" : "false");

        // v1.0.59.0: EnableHardDifficultyAttackSpeed / DisableMinHitDurationOnHard (default false; a missing key is written).
        char strHardSpeed[32] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_HARD_SPEED, "", strHardSpeed, sizeof(strHardSpeed), strPath);
        if (strHardSpeed[0] == 0)
            bWrite = GETrue;
        else
            g_Config.EnableHardDifficultyAttackSpeed = ParseIniBool(strHardSpeed);
        char strMinHit[32] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_MIN_HIT, "", strMinHit, sizeof(strMinHit), strPath);
        if (strMinHit[0] == 0)
            bWrite = GETrue;
        else
            g_Config.DisableMinHitDurationOnHard = ParseIniBool(strMinHit);
        GetDefaultLogger().LogFormat("[GenomeLogicFix] EnableHardDifficultyAttackSpeed=%s, DisableMinHitDurationOnHard=%s\n",
                                     g_Config.EnableHardDifficultyAttackSpeed ? "true" : "false",
                                     g_Config.DisableMinHitDurationOnHard ? "true" : "false");

        // v1.0.66.0: CrashDiagnostics (default true; a missing key is written).
        char strCrashDiag[32] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_CRASH_DIAG, "", strCrashDiag, sizeof(strCrashDiag), strPath);
        if (strCrashDiag[0] == 0)
            bWrite = GETrue;
        else
            g_Config.CrashDiagnostics = ParseIniBool(strCrashDiag);
        GetDefaultLogger().LogFormat("[GenomeLogicFix] CrashDiagnostics=%s\n", g_Config.CrashDiagnostics ? "true" : "false");

        if (bWrite)
            SaveConfig();
    }

    // v1.0.51.0: the name is in the black list of the teleport (the list is split by commas / semicolons, the spaces around the
    // names are cut, the case does not matter). The empty name is never in the list.
    GEBool IsTeleportNameIgnored(GELPCChar a_strName)
    {
        if (!a_strName || !a_strName[0])
            return GEFalse;
        char const *pRead = g_Config.IgnoreTeleportNames;
        while (*pRead)
        {
            while (*pRead == ',' || *pRead == ';' || *pRead == ' ' || *pRead == '\t')
                ++pRead;
            char const *pStart = pRead;
            while (*pRead && *pRead != ',' && *pRead != ';')
                ++pRead;
            char const *pEnd = pRead;
            while (pEnd > pStart && (pEnd[-1] == ' ' || pEnd[-1] == '\t'))
                --pEnd;
            size_t const uLength = static_cast<size_t>(pEnd - pStart);
            if (uLength > 0 && uLength == strlen(a_strName) && _strnicmp(pStart, a_strName, uLength) == 0)
                return GETrue;
        }
        return GEFalse;
    }

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
            GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] arena: %s ignored - the hero is outside the zone of the "
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
        GEFloat const SPARE_RADIUS = 2500.0f;
        GEBool g_bSpareArmed = GEFalse;
        GEBool g_bSpareActive = GEFalse; // a protected knock-out is going on: the fall point below is valid
        GEBool g_bOverLineSaid = GEFalse; // v1.0.60.0: the opponent has already spoken over the fallen hero (once per mercy)
        GEBool g_bSpareHeroSeenDown = GEFalse; // v1.0.54.0: the hero has really lain down since the fall was remembered
        GEU32 g_u32SpareFallTick = 0;          // v1.0.54.0: the moment the fall was remembered
        GEU32 const SPARE_FALL_GRACE_MS = 8000; // technical limit: the knock-out animation of the hero never takes this long
        GEFloat g_fFallX = 0.0f;
        GEFloat g_fFallY = 0.0f;
        GEFloat g_fFallZ = 0.0f;
        // The duel was already ended for the current knock-out of the hero (once per knock-out, whoever knocked him out).
        GEBool g_bDuelLossDone = GEFalse;
        void ResetDuelState(); // the companions waiting for the duel and the "lost to a third party" mark; defined below
        void ResetBlowHistory(GELPCChar a_strWhy); // v1.0.51.0: the clean slate of every blow counter (Mercy ended); below
        GEBool IsEnchantedMeleeEffect();           // v1.0.52.0: the status comes from an enchanted melee weapon; below
        void PunishRangedShot(GELPCChar a_strWhy); // v1.0.52.0: stage 2 for the bow / crossbow; below
        // v1.0.58.0: the crime + the duel quest is closed; below. v1.0.60.0: the opponent shouts a_strSvm (type a_u32Type) as he goes.
        void ArenaStageTwo(GELPCChar a_strWhy, GELPCChar a_strCrime, GELPCChar a_strSvm = "YouAskedForIt", GEU32 a_u32Type = 3);
        void CancelDuelForForbiddenWeapon(GELPCChar a_strWhy);         // v1.0.58.0: the timer is out: the duel is cancelled; below
        void PollWarnTimer();                                          // v1.0.58.0: the timer of the warning; below
        GEBool FindDuelOpponentAnywhere(Entity &o_Opponent);           // v1.0.60.0: the opponent also before the ring; below
        void ReleaseStage2Hold(GELPCChar a_strWhy);                    // v1.0.61.0: the hero fell: the opponent stops the fight; below
        void PollStage2Hold();                                         // v1.0.61.0: the opponent is held in the attack; below
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
        GEBool IsHeroTransformedNow();                     // v1.0.48.0: the hero is in a beast body; defined below
        GEBool IsHeroDuelBlocked();                        // v1.0.48.0: the safety lock of the duel hooks; defined below
        GEBool IsTransformSpellInHand();                   // v1.0.48.0: the spell of the hero is a transformation; below
        void ResetDuelForTransform(GELPCChar a_strWhy);    // v1.0.48.0: the reset of the duel by a transformation; below
        // The opponent of the duel was knocked out by somebody who is NOT the hero (a witness, a summon, a stray blow): the
        // duel is lost, whatever the quest wants to do (see the Defeat hook and the SetStatus hook).
        GEBool g_bLostToThirdParty = GEFalse;
        GEBool g_bThirdPartyMessage = GEFalse;
        void AnnounceLossToThirdParty(); // the message "the fight is lost", once per duel; defined below
        void EndArenaDuelAfterKnockout();                       // defined below, with the arena helpers
        void RememberHeroFall(GEBool a_bEndDuel = GETrue);      // defined below

        GEBool IsHeroKnockedOut()
        {
            Entity Hero = Entity::GetPlayer();
            return Hero.IsDown() || Hero.Routine.AIMode == gEAIMode_Down;
        }

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
        bCString g_strStage2Hold;
        GEU32 g_u32Stage2HoldTick = 0;
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena voice '%s' (%s): the speaker is gone or lies unconscious - silent.\n",
                                             a_strSvm, a_strWhy);
                return GEFalse;
            }
            ::StartSaySVM(a_Speaker, a_Target, bCString(a_strSvm), a_u32Type);
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena voice: '%s' says '%s' (%s).\n", a_Speaker.GetName().GetText(),
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
        GEBool g_bHeroImmune = GEFalse;
        GEBool g_bHeroImmuneSeenDown = GEFalse; // the hero was really seen down / rising since the immunity began
        GEU32 g_u32HeroImmuneTick = 0;

        GEBool IsHeroDownOrRising()
        {
            if (IsHeroKnockedOut())
                return GETrue;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return GEFalse;
            bCString const strTask = Hero.Routine.GetCurrentTask();
            bCString const strState = Hero.Routine.GetCurrentState();
            static GELPCChar const arrDown[] = {"ZS_Unconscious", "ZS_LieKnockDown", "ZS_PiercedKO",
                                                "ZS_SitKnockDown", "ZS_StandUp",     "ZS_Fall"};
            for (GELPCChar strPrefix : arrDown)
            {
                size_t const uLen = strlen(strPrefix);
                if (_strnicmp(strTask.GetText(), strPrefix, uLen) == 0 || _strnicmp(strState.GetText(), strPrefix, uLen) == 0)
                    return GETrue;
            }
            return GEFalse;
        }

        // The hero stands on his feet in an ordinary routine of the player: the loop state of the player routine (PS_Normal_Loop,
        // PS_Melee_Loop, PS_Ranged_Walk_Loop, PS_Magic_Loop, ...), i.e. a state PS_*_Loop. The knock-down / rising states are ZS_*.
        GEBool IsHeroOnFeetRoutine()
        {
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return GEFalse;
            bCString const strState = Hero.Routine.GetCurrentState();
            GELPCChar const strText = strState.GetText();
            size_t const uLen = strlen(strText);
            return uLen > 8 && _strnicmp(strText, "PS_", 3) == 0 && _stricmp(strText + uLen - 5, "_Loop") == 0 ? GETrue : GEFalse;
        }

        GEU32 g_u32HeroUpSince = 0; // the moment the hero was first seen NOT down / rising after the fall (0: he is down)
        GEU32 const HERO_UP_NET_MS = 6000; // technical net: a rising that never reaches a loop state (an unknown state name)
        GEU32 const HERO_UP_DEBOUNCE_MS = 600; // technical debounce of the "on his feet" reading (not a game timing)

        void ResetHeroImmunity()
        {
            g_bHeroImmune = GEFalse;
            g_bHeroImmuneSeenDown = GEFalse;
            g_u32HeroImmuneTick = 0;
            g_u32HeroUpSince = 0;
        }

        // The Defeat hook: the hero is about to fall (the knock-out itself is not visible yet).
        void BeginHeroImmunity()
        {
            if (!g_bHeroImmune)
                GetDefaultLogger().LogFormat("[GenomeLogicFix] The hero is down: nobody can kill him until he has risen.\n");
            g_bHeroImmune = GETrue;
            g_bHeroImmuneSeenDown = IsHeroDownOrRising();
            g_u32HeroImmuneTick = ::GetTickCount() | 1;
        }

        // True: the hero is immune NOW (down, rising, or the fall has just begun). Called wherever the death of the hero is decided
        // and from the housekeeping; it also starts the immunity when the hero is seen down by a path that did not go through Defeat.
        GEBool UpdateHeroImmunity()
        {
            if (Entity::GetPlayer() == None)
                return GEFalse;
            if (IsHeroDownOrRising())
            {
                if (!g_bHeroImmune)
                {
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] The hero is down: nobody can kill him until he has risen.\n");
                    g_u32HeroImmuneTick = ::GetTickCount() | 1;
                }
                g_bHeroImmune = GETrue;
                g_bHeroImmuneSeenDown = GETrue;
                g_u32HeroUpSince = 0; // down (again): the count of the technical net starts over
                return GETrue;
            }
            if (!g_bHeroImmune)
                return GEFalse;
            if (g_bHeroImmuneSeenDown)
            {
                if (g_u32HeroUpSince == 0)
                    g_u32HeroUpSince = ::GetTickCount() | 1;
                GEU32 const u32UpFor = ::GetTickCount() - g_u32HeroUpSince;
                GEBool const bNet = static_cast<GEInt>(u32UpFor) >= static_cast<GEInt>(HERO_UP_NET_MS);
                // The loop state must hold for HERO_UP_DEBOUNCE_MS: a one-frame flicker of the routine (the log shows PS_Melee_Loop
                // right after Defeat) is not a hero who stands.
                GEBool const bStands = IsHeroOnFeetRoutine() && static_cast<GEInt>(u32UpFor) >= static_cast<GEInt>(HERO_UP_DEBOUNCE_MS);
                if (!bStands && !bNet)
                    return GETrue; // up, but still in the animation of rising / not yet in the ordinary routine
                {
                    bCString const strState = Entity::GetPlayer().Routine.GetCurrentState();
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] The hero has risen (state '%s'%s): the immunity is over.\n",
                                                 strState.GetText(), bNet ? ", technical net" : "");
                }
                ResetHeroImmunity();
                return GEFalse;
            }
            // The fall was announced by the Defeat hook but the knock-out never showed (the technical net, not a game timing).
            if (static_cast<GEInt>(::GetTickCount() - g_u32HeroImmuneTick) >= static_cast<GEInt>(SPARE_FALL_GRACE_MS))
            {
                ResetHeroImmunity();
                return GEFalse;
            }
            return GETrue;
        }

        // g_bSpareSawEnemy: after arming, somebody within SPARE_RADIUS really fought the hero (target = hero, a reason to
        // attack). The mercy ends by an EVENT: the hero is on his feet and nobody around him fights him any more.
        // g_bSpareArena: armed by an arena violation (ended by the arena code if nobody ever reacted).
        GEBool g_bSpareSawEnemy = GEFalse;
        GEBool g_bSpareArena = GEFalse;
        GEU32 g_u32SpareScan = 0; // technical throttle of the scan below (not a game timing)

        // The NPCs the plugin sent against the hero (by name). When the hero stands up after a protected knock-out, each
        // of them calms down as soon as he runs his own attack loop (also those who were too far to see the fall and
        // arrive later): no timer, the event "the hero is on his feet" releases them.
        GEInt const AGGRO_MAX = 96;
        bCString g_arrAggroNames[AGGRO_MAX];
        GEInt g_iAggroCount = 0;
        GEBool g_bAggroRelease = GEFalse;

        GEInt FindAggro(bCString const &a_strName)
        {
            for (GEInt i = 0; i < g_iAggroCount; ++i)
                if (g_arrAggroNames[i] == a_strName)
                    return i;
            return -1;
        }

        void NoteAggro(Entity &a_Npc)
        {
            g_bAggroRelease = GEFalse; // a new conflict: the old release is void
            bCString const strName = a_Npc.GetName();
            if (g_iAggroCount >= AGGRO_MAX || FindAggro(strName) >= 0)
                return;
            g_arrAggroNames[g_iAggroCount++] = strName;
        }

        void ClearAggro()
        {
            for (GEInt i = 0; i < AGGRO_MAX; ++i)
                g_arrAggroNames[i] = bCString();
            g_iAggroCount = 0;
            g_bAggroRelease = GEFalse;
        }

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

        void ResetSpare(); // defined below

        // The NPC was sent against the hero by the plugin (the crowd of a crime, or the duel opponent that stays in the fight against the
        // form of the hero). The native filters of the built-in weapon of a form must not calm such an NPC down.
        GEBool IsSentAgainstHero(Entity &a_Npc)
        {
            if (!g_bSpareArmed || a_Npc == None)
                return GEFalse;
            bCString const strName = a_Npc.GetName();
            if (FindAggro(strName) >= 0)
                return GETrue;
            return !g_strFormOpponent.IsEmpty() && g_strFormOpponent == strName ? GETrue : GEFalse;
        }

        void ArmSpare(GELPCChar a_strWhy, GEBool a_bArena = GEFalse)
        {
            // v1.0.67.0: A STALE MERCY. A fall of the hero that is long over (he stands on his feet, he is not rising) but whose mercy was
            // never closed - the mercy is closed by an attack loop of somebody who fights him, and nobody did after that fall - must not
            // be taken for the fall of the NEW violation. Before, the first attack loop of the new fight found "the hero is on his feet
            // again", wrote Mercy ended and calmed down the whole crowd in the same frame (the log of the transformation in the ring).
            // The fall that has just been remembered (the knock-out is not visible yet) is not stale.
            GEBool const bFalling = !g_bSpareHeroSeenDown &&
                                    static_cast<GEInt>(::GetTickCount() - g_u32SpareFallTick) < static_cast<GEInt>(SPARE_FALL_GRACE_MS);
            if (g_bSpareActive && !bFalling && !IsHeroDownOrRising())
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Mercy: the old fall of the hero is over (he stands) - a new mercy starts.\n");
                ResetSpare();
                ResetHeroImmunity();
            }
            if (!g_bSpareArmed)
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Mercy armed: %s.\n", a_strWhy);
            g_bSpareArmed = GETrue;
            if (a_bArena)
                g_bSpareArena = GETrue;
            if (!IsHeroKnockedOut())
                g_bDuelLossDone = GEFalse; // a new violation: the next knock-out ends the duel again
        }

        void ResetSpare()
        {
            ResetFormAggro(); // v1.0.67.0: the mercy is over: nobody is aimed at a form any more
            g_strStage2Hold = bCString(); // v1.0.61.0: the mercy is over: the opponent is not held in the fight any more
            g_bOverLineSaid = GEFalse;
            g_bOverLinePending = GEFalse;
            g_bSpareArmed = GEFalse;
            g_bSpareActive = GEFalse;
            g_bSpareHeroSeenDown = GEFalse;
            g_u32SpareFallTick = 0;
            g_bSpareSawEnemy = GEFalse;
            g_bSpareArena = GEFalse;
            g_bDuelLossDone = GEFalse;
        }

        // The hero stands again after a protected knock-out: the mercy and the fall point are dropped at once.
        // Armed but not yet used: it ends when somebody fought the hero and now nobody does.
        void SpareHousekeeping()
        {
            // v1.0.54.0: THE MERCY ENDS ONLY AFTER THE HERO HAS REALLY LAIN DOWN AND STOOD UP AGAIN. The fall is remembered in the
            // Defeat hook BEFORE the native Defeat runs, i.e. while the hero is not yet "down": the first loop of an attacker that
            // asked "is the hero on his feet?" ended the mercy in the very frame of the fall, and the crowd killed the hero as he
            // went down (mercy armed=0 -> "leaves yes"). Now the mercy waits until the knock-out is seen (g_bSpareHeroSeenDown);
            // a technical safety limit (the knock-out that never comes) ends it anyway.
            if (g_bSpareActive && IsHeroKnockedOut())
                g_bSpareHeroSeenDown = GETrue;
            if (g_bSpareActive && !IsHeroKnockedOut() && !g_bSpareHeroSeenDown &&
                static_cast<GEInt>(::GetTickCount() - g_u32SpareFallTick) < static_cast<GEInt>(SPARE_FALL_GRACE_MS))
                return; // the hero is going down: the mercy stays
            // v1.0.57.0: the mercy ends together with the immunity - when the hero has completely risen (not while he is rising).
            if (g_bSpareActive && !IsHeroKnockedOut() && !UpdateHeroImmunity())
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Mercy ended: the hero is on his feet again; the NPCs sent "
                                             "against him (%d) calm down.\n",
                                             static_cast<int>(g_iAggroCount));
                ResetSpare();
                g_bAggroRelease = g_iAggroCount > 0;
                ResetBlowHistory("Mercy ended: the hero is on his feet again"); // v1.0.51.0: a new fight starts from scratch
                return;
            }
            // The knock-out of the hero is noticed here as well, not only in Defeat: a knock-out by a witness does not go
            // through the Defeat hook (no log line in the game), and the duel must end whoever knocked the hero out.
            if (g_bSpareArmed && !g_bSpareActive && IsHeroKnockedOut())
            {
                RememberHeroFall();
                return;
            }
            if (!g_bSpareArmed || g_bSpareActive || IsHeroKnockedOut())
                return;
            // v1.0.67.0: between the cast of the transformation and the appearance of the form body nobody has the NEW body as the target
            // yet (they still aim at the removed PC_Hero): that is not "nobody fights the hero any more", the mercy must not end here.
            if (g_bFormAggroPending)
                return;
            GEU32 const u32Now = ::GetTickCount();
            if (g_u32SpareScan != 0 && static_cast<GEInt>(u32Now - g_u32SpareScan) < 500)
                return;
            g_u32SpareScan = u32Now;

            Entity Hero = Entity::GetPlayer();
            GEBool bFight = GEFalse;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount() && !bFight; ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc == Hero || !Npc.IsNPC() || Npc.IsDead() || Npc.IsDown())
                    continue;
                if (Npc.GetDistanceTo(Hero) > SPARE_RADIUS)
                    continue;
                if (Npc.NPC.GetCurrentTarget() == Hero && Npc.NPC.AttackReason != gEAttackReason_None)
                    bFight = GETrue;
            }
            if (bFight)
                g_bSpareSawEnemy = GETrue;
            else if (g_bSpareSawEnemy)
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Mercy ended: nobody fights the hero any more.\n");
                ResetSpare();
                ResetBlowHistory("Mercy ended: nobody fights the hero any more");
            }
        }

        // The place where the hero fell (called when the hero is knocked out, and again by the first NPC that asks).
        void RememberHeroFall(GEBool a_bEndDuel)
        {
            if (!g_bSpareArmed || g_bSpareActive)
                return;
            bCVector const vFall = Entity::GetPlayer().GetPosition();
            g_fFallX = vFall.m_fX;
            g_fFallY = vFall.m_fY;
            g_fFallZ = vFall.m_fZ;
            g_bSpareActive = GETrue;
            g_bSpareHeroSeenDown = IsHeroKnockedOut();
            g_u32SpareFallTick = ::GetTickCount() | 1;
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The hero fell at (%.0f, %.0f, %.0f): nobody within %.0f may "
                                         "kill him.\n",
                                         static_cast<double>(g_fFallX), static_cast<double>(g_fFallY),
                                         static_cast<double>(g_fFallZ), static_cast<double>(SPARE_RADIUS));
            // A violation of the arena rules (not the city): the duel is over for everybody, the opponent leaves.
            if (a_bEndDuel && g_bSpareArena)
                EndArenaDuelAfterKnockout();
        }

        // True: this NPC must not kill the knocked-out hero (armed, hero down, NPC within SPARE_RADIUS of the fall point).
        GEBool SpareApplies(Entity &a_Npc, GEFloat &o_fDistance)
        {
            o_fDistance = -1.0f;
            SpareHousekeeping();
            // v1.0.54.0: while the hero is still going down (the fall is remembered, the knock-out not yet seen) the mercy applies too.
            GEBool const bGoingDown = g_bSpareActive && !g_bSpareHeroSeenDown;
            if (!g_bSpareArmed || (!IsHeroKnockedOut() && !bGoingDown))
                return GEFalse;
            RememberHeroFall();
            bCVector const vNpc = a_Npc.GetPosition();
            GEFloat const fDx = vNpc.m_fX - g_fFallX;
            GEFloat const fDy = vNpc.m_fY - g_fFallY;
            GEFloat const fDz = vNpc.m_fZ - g_fFallZ;
            o_fDistance = std::sqrt(fDx * fDx + fDy * fDy + fDz * fDz);
            return o_fDistance <= SPARE_RADIUS;
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
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] %s: duel quest '%s' status=%d\n", a_strWhen,
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

            GetDefaultLogger().LogFormat("[GenomeLogicFix] %s (opponent '%s').\n",
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

        GEBool IsHeroControlled(Entity &a_Npc); // defined below, with the rule for the controlled creatures
        GEBool IsHeroFormEntity(Entity &a_Npc);                  // defined below
        GEBool IsHeroSide(Entity &a_Npc);                        // defined below
        GEBool IsDuelTaboo(Entity &a_Npc);                       // defined below: the opponent of the duel / arena
        void DefendAllies(Entity &a_Victim, Entity &a_Attacker); // defined below: one for all, all for one
        void ClearDefendedTargets();                             // defined below
        void ClearFocusRecords();                                // defined below
        Entity GetDamageOwnerNpc(Entity &a_Source);                       // defined below
        GEBool IsFriendlyFire(Entity &a_Attacker, Entity &a_Victim, GEBool a_bAreaDamage = GEFalse,
                              Entity *a_pSource = nullptr); // defined below
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] '%s' (controlled by the hero, party type %d) is inside the "
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
        if (!g_Config.PunishArenaPotions)
            return;
        // The quest is running, but the hero is not in the ring / the fight has not begun: healing is allowed.
        GEBool const bInside = IsHeroInsideArenaZone();
        if (!bInside || !IsArenaDuelFightBegun())
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 20 && HasRunningArenaQuest())
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Consumable used while the duel quest runs, but %s: no penalty.\n",
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
                                           GEBool a_bIgnoreLos = GEFalse)
        {
            if (a_Npc == a_Hero || a_Npc == a_Opponent || a_Npc == None)
                return mEWitnessVerdict_NotAWitness;
            if (!a_Npc.IsNPC() || a_Npc.IsDead() || a_Npc.IsDown())
                return mEWitnessVerdict_NotAWitness;
            if (!AI_Decision::IsHumanoid(a_Npc))
                return mEWitnessVerdict_NotAWitness;
            // The companions of the hero never testify against him.
            if (a_Npc.Party.GetPartyLeader() == a_Hero)
                return mEWitnessVerdict_NotAWitness;
            // The arena is no business of slaves and of people without any faction.
            if (a_bIgnoreLos && (AI_Decision::IsSlave(a_Npc) || a_Npc.NPC.PoliticalAlignment == gEPoliticalAlignment_None))
                return mEWitnessVerdict_NotAWitness;
            if (a_Npc.GetDistanceTo(a_Hero) > WITNESS_RADIUS)
                return mEWitnessVerdict_TooFar;
            return (a_bIgnoreLos || a_Npc.IsFreeLOS(a_Hero, GEFalse)) ? mEWitnessVerdict_Ok
                                                                       : mEWitnessVerdict_NoLineOfSight;
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

            GEInt iAlerted = 0;
            GEInt iForced = 0;
            GEInt iTooFar = 0;
            GEInt iNoLos = 0;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Witness = arrNPCs.GetAt(i);
                // Arena violation: the whole crowd within the radius is told, nobody has to look at the very moment.
                mEWitnessVerdict const enuVerdict = GetWitnessVerdict(Witness, Hero, Opponent, bArenaViolation);
                if (enuVerdict == mEWitnessVerdict_TooFar)
                    ++iTooFar;
                else if (enuVerdict == mEWitnessVerdict_NoLineOfSight)
                    ++iNoLos;
                if (enuVerdict != mEWitnessVerdict_Ok)
                    continue;

                if (HasReacted(Witness, Hero))
                    continue;

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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Crime: %s -> %d witness(es) react (%d of them forced), "
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
        // hero is on his feet again" (g_bSpareActive && !IsHeroKnockedOut) -, when the hero won (no mercy armed), or when he
        // left the ring (the escape). Pure reads of the native state.
        GEBool IsArenaReleaseAllowed()
        {
            if (IsHeroKnockedOut())
                return GEFalse;
            if (g_bSpareArmed && g_bSpareArena && !g_bSpareActive && IsHeroInsideArenaZone())
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

        // The TRUE creator of a damaging object. For a spell the damage object is the projectile entity (or the
        // spell container / burning effect), and its Owner can be the projectile itself or a spell container, not the
        // hero. The chain is followed link by link (Owner -> User -> parent Spell, at most MAX_LINKS deep) and the
        // hero is looked for in every link; the last inflictor stored on the victim is the final fallback.
        GEBool IsCreatedByHero(Entity &a_Victim, Entity &a_Source)
        {
            Entity Hero = Entity::GetPlayer();
            if (a_Source == Hero)
                return GETrue;

            Entity Current = a_Source;
            for (GEInt iLink = 0; iLink < 4 && Current != None; ++iLink)
            {
                if (!Current.Interaction.IsValid())
                    break;

                Entity Owner = Current.Interaction.GetOwner();
                Entity User = Current.Interaction.GetUser();
                if (Owner == Hero || User == Hero)
                    return GETrue;

                Entity Spell = Current.Interaction.GetSpell();
                if (Spell != None && Spell != Current && Spell.Interaction.IsValid() &&
                    (Spell.Interaction.GetOwner() == Hero || Spell.Interaction.GetUser() == Hero))
                    return GETrue;

                // Next link: the owner (the caster or the projectile that carries the spell).
                if (Owner == None || Owner == Current)
                    break;
                Current = Owner;
            }

            if (a_Victim.DamageReceiver.IsValid())
            {
                Entity Inflictor = a_Victim.DamageReceiver.GetLastInflictor();
                if (Inflictor == Hero)
                    return GETrue;
            }
            return GEFalse;
        }

        //
        // Assault of a hero with a high reputation. Script_Game.dll 1.8.7 (GetAttitudeToPlayer / GetZoneAttitudeToPlayer)
        // compares the political fame and the city (enclave) fame of the hero with 0x4B = 75: from 75 up (the maximum is
        // 100) every resident is "Friendly" to him, and the perception scripts (AssessHit / AssessMurder) then ignore
        // the beating of a fellow citizen. Below 75 the game reacts by itself, so the plugin only steps in from 75.
        //
        GEI32 const REPUTATION_MIN = 75;
        GEU32 const ASSAULT_MEMORY_MS = 30000;
        bCString g_strAssaultVictim;
        GEU32 g_u32AssaultUntil = 0;

        // The hero has a reputation of REPUTATION_MIN or more in the city / political group of the NPC.
        GEBool HasHighReputation(Entity &a_Npc, GEI32 &o_iEnclaveFame, GEI32 &o_iPoliticalFame)
        {
            o_iEnclaveFame = -1;
            o_iPoliticalFame = -1;

            Entity City = a_Npc.NPC.GetEnclave();
            if (City != None && City.Enclave.IsValid())
                o_iEnclaveFame = City.Enclave.PlayerFame;

            Entity Hero = Entity::GetPlayer();
            if (Hero.PlayerMemory.IsValid())
            {
                gEPoliticalAlignment const enuAlignment = a_Npc.NPC.PoliticalAlignment;
                o_iPoliticalFame = Hero.PlayerMemory.GetPoliticalFame(enuAlignment);
            }
            return o_iEnclaveFame >= REPUTATION_MIN || o_iPoliticalFame >= REPUTATION_MIN;
        }

        void RememberAssaultVictim(Entity &a_Victim)
        {
            g_strAssaultVictim = a_Victim.GetName();
            g_u32AssaultUntil = ::GetTickCount() + ASSAULT_MEMORY_MS;
            if (g_u32AssaultUntil == 0)
                g_u32AssaultUntil = 1;
        }

        // The beating goes on: the same victim is hit again within ASSAULT_MEMORY_MS.
        GEBool IsRememberedAssaultVictim(Entity &a_Victim)
        {
            if (g_u32AssaultUntil == 0 || static_cast<GEInt>(::GetTickCount() - g_u32AssaultUntil) >= 0)
                return GEFalse;
            return g_strAssaultVictim == a_Victim.GetName();
        }

        // The hero (with a reputation of 75+) hurts a peaceful humanoid and no legal duel is running (arena, duel quest).
        GEBool IsUnprovokedAssault(Entity &a_Victim, Entity &a_Source)
        {
            Entity Hero = Entity::GetPlayer();
            if (a_Victim == None || a_Victim == Hero || !a_Victim.IsNPC() || a_Victim.IsDead())
                return GEFalse;

            // Diagnostics (limited): why a hit on an NPC is, or is not, judged.
            static GEInt s_iDiagLines = 0;
            GEBool const bByHero = a_Source != None && IsCreatedByHero(a_Victim, a_Source);
            GEBool const bHumanoid = AI_Decision::IsHumanoid(a_Victim);
            if (bByHero && s_iDiagLines < 300)
            {
                ++s_iDiagLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] hero hits '%s' (humanoid=%d, source='%s')\n",
                                             a_Victim.GetName().GetText(), static_cast<int>(bHumanoid),
                                             a_Source.GetName().GetText());
            }

            if (!bByHero || !bHumanoid)
                return GEFalse;

            // A legal duel is no assault: the victim is the opponent of the duel, or the hero stands inside the ring. The
            // duel quest is Running from the moment the duel is accepted, long before the hero enters the ring, so the
            // quest alone does not make a blow on a bystander (Kulak, a guard, ...) legal - those are judged as usual.
            Entity Opponent;
            GEBool const bOpponentHit = FindArenaOpponent(Opponent) && a_Victim == Opponent;
            GEBool const bInRing = HasRunningArenaQuest() && IsHeroInsideArenaZone();
            if (bOpponentHit || bInRing)
            {
                if (s_iDiagLines < 300)
                {
                    ++s_iDiagLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] ignored: a legal duel (the victim is the opponent=%d, "
                                                 "the hero is in the ring=%d)\n",
                                                 static_cast<int>(bOpponentHit), static_cast<int>(bInRing));
                }
                return GEFalse;
            }

            GEI32 iEnclaveFame = 0;
            GEI32 iPoliticalFame = 0;
            GEBool const bHighReputation = HasHighReputation(a_Victim, iEnclaveFame, iPoliticalFame);
            GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] hero hit '%s': enclave fame=%d, political fame=%d, "
                                         "attackReason=%d, combatState=%u, attitude=%d -> %s\n",
                                         a_Victim.GetName().GetText(), static_cast<int>(iEnclaveFame),
                                         static_cast<int>(iPoliticalFame),
                                         static_cast<int>(a_Victim.NPC.AttackReason),
                                         static_cast<unsigned>(a_Victim.NPC.CombatState),
                                         static_cast<int>(AI_Decision::GetAttitudeToPlayer(a_Victim)),
                                         bHighReputation ? "reputation 75+" : "reputation below 75, the game reacts");
            if (!bHighReputation)
                return GEFalse;

            // Further hits on the victim of a beating that already started: the witnesses are asked again.
            if (IsRememberedAssaultVictim(a_Victim))
                return GETrue;

            // A legal fight: the victim already fights the hero, or is his enemy anyway.
            // ReactToDamage is only the reaction to a blow: an NPC who answers the hero's hit is still a victim (the
            // fighters like Kulak already stand in this state when the second blow comes).
            // CombatState is NOT looked at: Kulak has combatState=1 from his very first hit with attackReason=0 (log),
            // so it says nothing about a fight against the hero. Only a real attack reason does.
            GEBool const bReactsToHit = a_Victim.NPC.AttackReason == gEAttackReason_ReactToDamage;
            if (!bReactsToHit && a_Victim.NPC.AttackReason != gEAttackReason_None)
                return GEFalse;
            if (a_Victim.NPC.LastFightAgainstPlayer == gELastFightAgainstPlayer_Running)
                return GEFalse;
            gEAttitude const enuAttitude = AI_Decision::GetAttitudeToPlayer(a_Victim);
            if (enuAttitude != gEAttitude_Friendly && enuAttitude != gEAttitude_Neutral)
                return GEFalse;

            RememberAssaultVictim(a_Victim);
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Assault on '%s': enclave fame=%d, political fame=%d, "
                                         "attitude=%d.\n",
                                         a_Victim.GetName().GetText(), static_cast<int>(iEnclaveFame),
                                         static_cast<int>(iPoliticalFame), static_cast<int>(enuAttitude));
            return GETrue;
        }

        //
        // Two-stage reaction to a beating in a friendly city (reputation 75+, no arena).
        //  1. FIRST BLOW - attention. The witnesses do NOT attack. Every peaceful witness in line of sight drops his
        //     routine (sitting, smoking, forging), turns to the hero and watches: the sequence the native perception
        //     script itself uses to make a watcher (FullStop, current target = hero, alternative target = victim,
        //     SetTask("ZS_WatchFight")). The victim is left alone: the native reaction makes him draw the weapon, step
        //     back and put it away again. A window of WATCH_MS starts. When it is over without a second blow the watchers
        //     return to their routine (ContinueRoutine); the release is done by the hook of ZS_WatchFight_Loop, which
        //     every watcher runs on every tick.
        //  2. SECOND BLOW within the window - total aggro. The second blow is a blow on the same victim OR on another
        //     peaceful resident who lives in the same enclave (city) / belongs to the same political faction as the first
        //     victim and stands within WITNESS_RADIUS of the hero (a pogrom of different victims is no loophole). The
        //     attack reason of the victims is overwritten with Murder, and they, all watchers and the guards in sight get
        //     SetTask("ZS_Attack").
        //
        enum mECityPhase
        {
            mECityPhase_Idle = 0,
            mECityPhase_Watching = 1,
            mECityPhase_Aggro = 2,
        };

        GEU32 const WATCH_MS = 10000;  // the window for the second blow
        GEU32 const BLOW_GAP_MS = 600; // one blow raises several events (damage, attacker set): count it once
        GEInt const WATCH_MAX = 64;
        mECityPhase g_enuCityPhase = mECityPhase_Idle;
        bCString g_strCityVictim;                                          // the last victim of the current beating
        bCString g_strCityEnclave;                                         // enclave of the first victim (empty: none)
        gEPoliticalAlignment g_enuCityAlignment = gEPoliticalAlignment_None; // faction of the first victim
        GEU32 g_u32WatchDeadline = 0;
        GEU32 g_u32AggroUntil = 0;
        GEU32 g_u32BlowTick = 0;
        GEU32 g_u32LastAggroPass = 0;
        bCString g_arrWatchNames[WATCH_MAX];
        GEInt g_iWatchCount = 0;

        GEBool TickReached(GEU32 a_u32Now, GEU32 a_u32Time)
        {
            return a_u32Time == 0 || static_cast<GEInt>(a_u32Now - a_u32Time) >= 0;
        }

        GEInt FindWatcher(bCString const &a_strName)
        {
            for (GEInt i = 0; i < g_iWatchCount; ++i)
                if (g_arrWatchNames[i] == a_strName)
                    return i;
            return -1;
        }

        GEBool IsWatcher(Entity &a_Npc)
        {
            return FindWatcher(a_Npc.GetName()) >= 0;
        }

        void AddWatcher(Entity &a_Npc)
        {
            bCString const strName = a_Npc.GetName();
            if (FindWatcher(strName) >= 0 || g_iWatchCount >= WATCH_MAX)
                return;
            g_arrWatchNames[g_iWatchCount++] = strName;
        }

        void RemoveWatcher(GEInt a_iIndex)
        {
            if (a_iIndex < 0 || a_iIndex >= g_iWatchCount)
                return;
            g_arrWatchNames[a_iIndex] = g_arrWatchNames[g_iWatchCount - 1];
            --g_iWatchCount;
        }

        // An NPC that does his own business: he fights nobody and has no reason to fight.
        GEBool IsPeaceful(Entity &a_Npc)
        {
            return a_Npc.NPC.AttackReason == gEAttackReason_None && a_Npc.NPC.CombatState == 0;
        }

        // Hard attack order: the NPC drops what he does and attacks the hero (what AssessTarget does for a fighter).
        void ForceAttack(Entity &a_Npc, Entity &a_Hero, gEAttackReason a_enuReason)
        {
            NoteAggro(a_Npc);
            a_Npc.Routine.FullStop();
            a_Npc.NPC.SetCurrentTarget(a_Hero);
            a_Npc.NPC.AttackReason = a_enuReason;
            a_Npc.Routine.SetTask("ZS_Attack");
        }

        // Stage 1: the witnesses in sight stop what they do, turn to the hero and watch.
        void StartWatching(Entity &a_Hero, Entity &a_Victim)
        {
            GEInt iNew = 0;
            GEInt iAlready = 0;
            GEInt iFailed = 0;
            GEInt iBusy = 0;
            GEInt iTooFar = 0;
            GEInt iNoLos = 0;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            // Forget the watchers of an earlier beating who are not watching any more.
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Known = arrNPCs.GetAt(i);
                GEInt const iKnown = FindWatcher(Known.GetName());
                if (iKnown >= 0 && !HasTask(Known, "ZS_WatchFight"))
                    RemoveWatcher(iKnown);
            }
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Witness = arrNPCs.GetAt(i);
                mEWitnessVerdict const enuVerdict = GetWitnessVerdict(Witness, a_Hero, a_Victim);
                if (enuVerdict == mEWitnessVerdict_TooFar)
                    ++iTooFar;
                else if (enuVerdict == mEWitnessVerdict_NoLineOfSight)
                    ++iNoLos;
                if (enuVerdict != mEWitnessVerdict_Ok)
                    continue;

                if (HasTask(Witness, "ZS_WatchFight"))
                {
                    AddWatcher(Witness);
                    ++iAlready;
                    continue;
                }
                // Somebody who already fights (a guard against a monster, a witness who attacks the hero) is left alone.
                if (!IsPeaceful(Witness) || HasReacted(Witness, a_Hero))
                {
                    ++iBusy;
                    continue;
                }

                Witness.Routine.FullStop();
                Witness.NPC.SetCurrentTarget(a_Hero);
                Witness.NPC.SetAlternativeTarget(a_Victim);
                Witness.Routine.SetTask("ZS_WatchFight");
                Witness.SetAlignmentTarget(a_Hero); // turn to the hero at once, the loop keeps him turned to the fight
                if (HasTask(Witness, "ZS_WatchFight"))
                {
                    AddWatcher(Witness);
                    ++iNew;
                }
                else
                    ++iFailed;
            }
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Assault, stage 1 (attention): '%s' was hit, %d witness(es) "
                                         "watch now (%d already watched), %d busy, %d could not switch, %d too far, "
                                         "%d without line of sight; window %u ms.\n",
                                         a_Victim.GetName().GetText(), static_cast<int>(iNew),
                                         static_cast<int>(iAlready), static_cast<int>(iBusy),
                                         static_cast<int>(iFailed), static_cast<int>(iTooFar), static_cast<int>(iNoLos),
                                         static_cast<unsigned>(WATCH_MS));
        }

        // Stage 2: total aggro. The victim gets a criminal attack reason, the watchers and the guards in sight attack.
        // a_strFirstVictim: the victim of the first blow when the second blow hit somebody else (empty otherwise): he
        // is ordered to attack as well, he is the one who was beaten first.
        // v1.0.53.0: a_bInstantKnockout - the victim lay down from the very FIRST blow (the counter was wound on to the second one).
        // The honest second blow reaches the crowd through the watchers of stage 1 (forced to ZS_Attack) and, for the rest, through
        // the native AssessTarget, where the reputation 75+ of the hero "forgives" the beating. A one-shot knock-out has no
        // time for stage 1 to gather everybody, so here EVERY witness that sees the hero is treated as a watcher: forced to attack
        // (FullStop, the hero as the target, ZS_Attack), whatever the reputation, the weapon or the nature of the witness. The
        // number of the attackers is that of the honest second blow with the same crowd around.
        void StartTotalAggro(Entity &a_Hero, Entity &a_Victim, bCString const &a_strFirstVictim, GEBool a_bFirstTime,
                             GEBool a_bInstantKnockout = GEFalse)
        {
            GEInt iForced = 0;
            GEInt iReact = 0;
            GEInt iTooFar = 0;
            GEInt iNoLos = 0;

            // The second blow: from now on nobody within SPARE_RADIUS of the place where the hero falls may kill him.
            ArmSpare("the second blow in a city (total aggro)");

            // v1.0.52.0: a victim who lies in the knock-out (by the flag, the Down mode or the native knock-out task) is never
            // ordered to get up and fight - the witnesses carry the second blow, he lies on the ground.
            GEBool const bVictimLies = a_Victim.IsDown() || a_Victim.Routine.AIMode == gEAIMode_Down ||
                                       HasTask(a_Victim, "ZS_Unconscious") || HasTask(a_Victim, "ZS_LieKnockDown") ||
                                       HasTask(a_Victim, "ZS_PiercedKO");
            if (!a_Victim.IsDead() && !bVictimLies && !IsAttackingHero(a_Victim, a_Hero))
            {
                ForceAttack(a_Victim, a_Hero, gEAttackReason_Murder);
                ++iForced;
            }

            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Witness = arrNPCs.GetAt(i);
                if (!a_strFirstVictim.IsEmpty() && !(Witness == a_Victim) && Witness.IsNPC() &&
                    Witness.GetName() == a_strFirstVictim)
                {
                    if (!Witness.IsDead() && !Witness.IsDown() && !IsAttackingHero(Witness, a_Hero))
                    {
                        ForceAttack(Witness, a_Hero, gEAttackReason_Murder);
                        ++iForced;
                    }
                    continue;
                }
                mEWitnessVerdict const enuVerdict = GetWitnessVerdict(Witness, a_Hero, a_Victim);
                if (enuVerdict == mEWitnessVerdict_NotAWitness)
                    continue;
                // v1.0.53.0: after a one-shot knock-out everybody who sees the hero counts as a watcher (see above).
                GEBool const bWatcher = IsWatcher(Witness) || (a_bInstantKnockout && enuVerdict == mEWitnessVerdict_Ok);
                // The watchers have seen everything even if they stepped aside meanwhile; the others must see the hero now.
                if (!bWatcher && enuVerdict != mEWitnessVerdict_Ok)
                {
                    if (enuVerdict == mEWitnessVerdict_TooFar)
                        ++iTooFar;
                    else
                        ++iNoLos;
                    continue;
                }
                if (IsAttackingHero(Witness, a_Hero))
                    continue;

                AI_Memory::MemorizePlayerCrime(Witness, a_Hero, gEPlayerCrime_Attack);
                if (bWatcher || AI_Decision::IsGuard(Witness))
                {
                    ForceAttack(Witness, a_Hero, gEAttackReason_Enemy);
                    ++iForced;
                }
                else
                {
                    AI_Perception::AssessTarget(Witness, a_Hero, gEAttackReason_Enemy);
                    if (!HasReacted(Witness, a_Hero))
                    {
                        ForceReaction(Witness, a_Hero);
                        ++iForced;
                    }
                    else
                        NoteAggro(Witness);
                    ++iReact;
                }
            }

            g_iWatchCount = 0; // nobody watches any more, everybody fights
            g_enuCityPhase = mECityPhase_Aggro;
            g_u32AggroUntil = ::GetTickCount() + ASSAULT_MEMORY_MS;
            if (g_u32AggroUntil == 0)
                g_u32AggroUntil = 1;
            RememberAssaultVictim(a_Victim); // the beating goes on: keep the victim in the memory
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Assault, stage 2 (%s): '%s' - %d ordered to ZS_Attack, "
                                         "%d reacted by themselves, %d too far, %d without line of sight.\n",
                                         a_bFirstTime ? "total aggro" : "aggro goes on", a_Victim.GetName().GetText(),
                                         static_cast<int>(iForced), static_cast<int>(iReact), static_cast<int>(iTooFar),
                                         static_cast<int>(iNoLos));
        }

        // Name of the enclave (city) the NPC lives in; empty if he belongs to none.
        bCString GetEnclaveName(Entity &a_Npc)
        {
            Entity City = a_Npc.NPC.GetEnclave();
            if (City != None && City.Enclave.IsValid())
                return City.GetName();
            return bCString();
        }

        // The victim belongs to the community of the first victim of the current beating (same enclave, or the same
        // political faction) and is within the witness radius of the hero.
        GEBool IsSameCommunityNearHero(Entity &a_Victim, Entity &a_Hero)
        {
            if (a_Victim.GetDistanceTo(a_Hero) > WITNESS_RADIUS)
                return GEFalse;
            bCString const strEnclave = GetEnclaveName(a_Victim);
            if (!strEnclave.IsEmpty() && !g_strCityEnclave.IsEmpty() && strEnclave == g_strCityEnclave)
                return GETrue;
            gEPoliticalAlignment const enuAlignment = a_Victim.NPC.PoliticalAlignment;
            return enuAlignment != gEPoliticalAlignment_None && enuAlignment == g_enuCityAlignment;
        }

        // Forgets everything about the beating of the hero: the phase (attention / aggro), the victims, the watchers, the
        // memory of the assault and the NPCs marked as sparing. Called when a save game is loaded / a new game starts /
        // the session stops: the state lives in the plugin, not in the save game, so without the reset the first blow
        // after loading an earlier save would still be counted as the second one.
        void ResetCityState(GELPCChar a_strWhy)
        {
            GEBool const bHadState = g_enuCityPhase != mECityPhase_Idle || g_iWatchCount != 0 ||
                                     g_u32AssaultUntil != 0 || g_u32BlowTick != 0;
            g_enuCityPhase = mECityPhase_Idle;
            g_strCityVictim = bCString();
            g_strCityEnclave = bCString();
            g_enuCityAlignment = gEPoliticalAlignment_None;
            g_u32WatchDeadline = 0;
            g_u32AggroUntil = 0;
            g_u32BlowTick = 0;
            g_u32LastAggroPass = 0;
            g_u32LastDeclare = 0;
            g_iWatchCount = 0;
            for (GEInt i = 0; i < WATCH_MAX; ++i)
                g_arrWatchNames[i] = bCString();
            g_strAssaultVictim = bCString();
            g_u32AssaultUntil = 0;
            ResetSpare();
            ResetFormAggro();    // v1.0.67.0: nobody is aimed at a form of the previous session
            ResetHeroImmunity(); // v1.0.57.0: a loaded save / a new session starts without the hold
            ClearAggro();
            ClearDefendedTargets();
            ResetDuelState();
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The assault counter is reset (%s)%s.\n", a_strWhy,
                                         bHadState ? "" : ", nothing was pending");
        }

        // Entry point: the hero hit a peaceful resident of a friendly city (IsUnprovokedAssault said yes).
        void OnCityAssault(Entity &a_Victim)
        {
            Entity Hero = Entity::GetPlayer();
            GEU32 const u32Now = ::GetTickCount();

            // Second line of defence next to the session hooks: a loaded save game creates the hero entity anew, so a
            // different hero instance than at the previous blow means "another game state" - the old counter is void.
            static eCEntity *s_pHeroInstance = nullptr;
            eCEntity *pHeroInstance = Hero.GetInstance();
            if (pHeroInstance != s_pHeroInstance)
            {
                if (s_pHeroInstance != nullptr)
                    ResetCityState("the hero entity was replaced");
                s_pHeroInstance = pHeroInstance;
            }
            bCString const strVictim = a_Victim.GetName();

            // A phase that ran out is over.
            if (g_enuCityPhase == mECityPhase_Watching && TickReached(u32Now, g_u32WatchDeadline))
                g_enuCityPhase = mECityPhase_Idle;
            if (g_enuCityPhase == mECityPhase_Aggro && TickReached(u32Now, g_u32AggroUntil))
                g_enuCityPhase = mECityPhase_Idle;

            // One blow raises several events (the damage script, SetCurrentAttacker, SetLastAttacker): count it once.
            if (g_u32BlowTick != 0 && static_cast<GEInt>(u32Now - g_u32BlowTick) < static_cast<GEInt>(BLOW_GAP_MS))
                return;

            // The same victim, or - while a beating is going on - another resident of the same community near the hero.
            // The victim of the first blow is remembered by name; "the same community" is the same enclave or the same
            // political faction as the first victim, within the witness radius of the hero. A pogrom of different victims
            // is therefore the second blow, not a new "peaceful" first blow.
            GEBool const bSameVictim = g_enuCityPhase != mECityPhase_Idle && g_strCityVictim == strVictim;
            GEBool const bOtherVictim = g_enuCityPhase != mECityPhase_Idle && !bSameVictim &&
                                        IsSameCommunityNearHero(a_Victim, Hero);
            bCString const strPrevious = bOtherVictim ? g_strCityVictim : bCString();

            if ((bSameVictim || bOtherVictim) && g_enuCityPhase == mECityPhase_Aggro)
            {
                // The fight goes on: those who calmed down or came later are called again (at most every 0.7 s).
                g_u32BlowTick = u32Now;
                if (bOtherVictim || g_u32LastAggroPass == 0 ||
                    static_cast<GEInt>(u32Now - g_u32LastAggroPass) >= 700)
                {
                    g_u32LastAggroPass = u32Now;
                    StartTotalAggro(Hero, a_Victim, strPrevious, GEFalse);
                }
                return;
            }

            if ((bSameVictim || bOtherVictim) && g_enuCityPhase == mECityPhase_Watching)
            {
                // A burning / frozen victim is hurt again and again by the fire itself: that is not a new blow.
                if (bSameVictim && (a_Victim.NPC.IsBurning() || a_Victim.NPC.IsFrozen()))
                    return;
                g_u32BlowTick = u32Now;
                g_u32LastAggroPass = u32Now;
                if (bOtherVictim)
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] Assault: second blow within the window on ANOTHER "
                                                 "resident of the same community: '%s' (first victim '%s').\n",
                                                 strVictim.GetText(), strPrevious.GetText());
                else
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] Assault: second blow on '%s' within the window.\n",
                                                 strVictim.GetText());
                StartTotalAggro(Hero, a_Victim, strPrevious, GETrue);
                return;
            }

            // The first blow (or the window / the aggro of the previous beating is over, or the victim is a stranger).
            g_u32BlowTick = u32Now;
            g_strCityVictim = strVictim;
            g_strCityEnclave = GetEnclaveName(a_Victim);
            g_enuCityAlignment = a_Victim.NPC.PoliticalAlignment;
            g_enuCityPhase = mECityPhase_Watching;
            g_u32WatchDeadline = u32Now + WATCH_MS;
            if (g_u32WatchDeadline == 0)
                g_u32WatchDeadline = 1;
            StartWatching(Hero, a_Victim);
        }

        // v1.0.35.0: HARD BLOCK OF THE LOOTING EXPLOIT. A green / neutral resident of a friendly city (reputation 75+) who
        // falls unconscious (down / ZS_Unconscious) at the hands of the hero is, AT ONCE and without any timer, the SECOND
        // blow: the 10 s window (WATCH_MS) is ignored, the full crime is declared (StartTotalAggro: every witness in sight
        // and the guards draw the weapon and attack the hero natively). Called from AssessHit, AssessAgressor, the
        // knock-out scripts (Defeat) and the damage hook, so no path of the knock-out is missed. Once per victim and fall.
        void OnCityKnockout(Entity &a_Victim, GELPCChar a_strWhere)
        {
            Entity Hero = Entity::GetPlayer();
            if (a_Victim == None || a_Victim == Hero || !a_Victim.IsNPC() || a_Victim.IsDead())
                return;
            // "Fell unconscious": the down flag, the Down AI mode, or one of the native knock-out tasks.
            GEBool const bDown = a_Victim.IsDown() || a_Victim.Routine.AIMode == gEAIMode_Down ||
                                 HasTask(a_Victim, "ZS_Unconscious") || HasTask(a_Victim, "ZS_LieKnockDown") ||
                                 HasTask(a_Victim, "ZS_PiercedKO");
            if (!bDown)
                return;
            if (IsHeroSide(a_Victim) || IsHeroControlled(a_Victim) || !AI_Decision::IsHumanoid(a_Victim))
                return;

            // The hero (any form) must be the one who knocked him out.
            Entity Attacker = a_Victim.NPC.GetCurrentAttacker();
            if (Attacker == None)
                Attacker = a_Victim.NPC.GetLastAttacker();
            Entity Inflictor = a_Victim.DamageReceiver.IsValid() ? a_Victim.DamageReceiver.GetLastInflictor() : Entity();
            GEBool const bByHero = (Attacker != None && IsHeroSide(Attacker)) || IsRememberedAssaultVictim(a_Victim) ||
                                   (Inflictor != None && IsHeroSide(Inflictor));
            if (!bByHero)
                return;

            // v1.0.37.0: THE LAW OF THE CITY. The general exception "the NPC attacked first" is REMOVED (neither the attack
            // reason nor the attitude of the victim is looked at): a resident of a friendly city who falls unconscious at the
            // hands of the hero ALWAYS raises the total aggression. Only two legitimate situations are exempt, both read from
            // the quest manager (the arena quest is Running):
            //  1. the hero stands inside the ring of the arena;
            //  2. it is the official duel 1 on 1: the victim is the opponent of the duel quest.
            if (HasRunningArenaQuest())
            {
                if (IsHeroInsideArenaZone())
                    return;
                Entity Opponent;
                if ((FindArenaOpponent(Opponent) || FindRememberedOpponent(Opponent)) && a_Victim == Opponent)
                    return;
            }

            GEI32 iEnclaveFame = 0;
            GEI32 iPoliticalFame = 0;
            if (!HasHighReputation(a_Victim, iEnclaveFame, iPoliticalFame))
                return; // below 75 the game reacts by itself

            // v1.0.52.0: THE KNOCK-OUT FROM THE FIRST BLOW IS THE SECOND BLOW (the blind spot of 1.0.51.0, where the witnesses
            // stayed silent because the counter of the blows had only just been started when the victim already lay down).
            // OnCityAssault runs BEFORE the original damage code, so by now the first blow is counted (the phase is Watching:
            // the witnesses stopped and look). A phase that is not a live Aggro here means the knock-out came with the very
            // first blow: the counter is wound on to the second blow at once, right AFTER it was started (the victim, the
            // community and the blow time are written as the second blow does), and StartTotalAggro below arms the mercy and
            // sends every witness (and the guards) at the hero. The victim himself is NOT touched: he lies in the native
            // knock-out (StartTotalAggro orders nobody who is down), he is not ordered to get up and fight.
            GEU32 const u32Check = ::GetTickCount();
            GEBool const bAggroLive = g_enuCityPhase == mECityPhase_Aggro && !TickReached(u32Check, g_u32AggroUntil);
            GEBool const bFromFirstBlow = !bAggroLive;

            // Once per fall of the same victim.
            static bCString s_strLastVictim;
            static GEU32 s_u32LastTick = 0;
            GEU32 const u32Now = ::GetTickCount();
            bCString const strVictim = a_Victim.GetName();
            if (s_u32LastTick != 0 && static_cast<GEInt>(u32Now - s_u32LastTick) < 5000 && s_strLastVictim == strVictim)
                return;
            s_strLastVictim = strVictim;
            s_u32LastTick = u32Now;

            if (bFromFirstBlow)
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Assault: '%s' fell unconscious from the FIRST blow (%s): the "
                                             "counter is wound on - the knock-out counts as the SECOND blow, the witnesses "
                                             "attack the hero (Mercy armed), the victim stays down.\n",
                                             strVictim.GetText(), a_strWhere);
            else
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Assault: '%s' fell unconscious at the hands of the hero (%s): "
                                             "counted as the SECOND blow at once, the %u ms window is ignored.\n",
                                             strVictim.GetText(), a_strWhere, static_cast<unsigned>(WATCH_MS));
            g_u32BlowTick = u32Now;
            g_strCityVictim = strVictim;
            g_strCityEnclave = GetEnclaveName(a_Victim);
            g_enuCityAlignment = a_Victim.NPC.PoliticalAlignment;
            g_u32LastAggroPass = u32Now;
            StartTotalAggro(Hero, a_Victim, bCString(), GETrue, bFromFirstBlow);
        }

        // Called by the hook of ZS_WatchFight_Loop for the NPC that runs it. True: the watcher was released and has
        // already got his routine back (the state must not go on).
        GEBool ReleaseWatcherIfOver(Entity &a_Self)
        {
            GEInt const iIndex = FindWatcher(a_Self.GetName());
            if (iIndex < 0)
                return GEFalse;
            GEU32 const u32Now = ::GetTickCount();
            if (g_enuCityPhase == mECityPhase_Watching && !TickReached(u32Now, g_u32WatchDeadline))
                return GEFalse;

            RemoveWatcher(iIndex);
            if (g_iWatchCount == 0 && g_enuCityPhase == mECityPhase_Watching)
                g_enuCityPhase = mECityPhase_Idle;
            if (!HasTask(a_Self, "ZS_WatchFight"))
                return GEFalse;
            a_Self.NPC.SetCurrentTarget(None);
            a_Self.NPC.SetAlternativeTarget(None);
            a_Self.Routine.ContinueRoutine();

            static GEInt s_iReleaseLines = 0;
            if (s_iReleaseLines < 60)
            {
                ++s_iReleaseLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Assault: no second blow within %u ms, '%s' goes back to "
                                             "his routine (%d watcher(s) left).\n",
                                             static_cast<unsigned>(WATCH_MS), a_Self.GetName().GetText(),
                                             static_cast<int>(g_iWatchCount));
            }
            return GETrue;
        }


        GEBool IsElementalDamage(gEDamageType a_enuType)
        {
            return a_enuType == gEDamageType_Fire || a_enuType == gEDamageType_Ice || a_enuType == gEDamageType_Lightning;
        }

        // The damage type of a hit. A spell reaches DoLogicalDamage as a projectile / spell container / burning effect:
        // the elemental type is searched in every link (object itself, its spell, its owner), then the first known type
        // and finally the type stored on the victim are used.
        gEDamageType GetHitDamageType(Entity &a_Source, Entity &a_Victim)
        {
            gEDamageType enuFirst = gEDamageType_None;
            Entity Current = a_Source;
            for (GEInt iLink = 0; iLink < 4 && Current != None; ++iLink)
            {
                if (Current.Damage.IsValid())
                {
                    gEDamageType const enuType = Current.Damage.DamageType;
                    if (IsElementalDamage(enuType))
                        return enuType;
                    if (enuFirst == gEDamageType_None)
                        enuFirst = enuType;
                }
                if (!Current.Interaction.IsValid())
                    break;

                Entity Spell = Current.Interaction.GetSpell();
                if (Spell != None && Spell != Current && Spell.Damage.IsValid() &&
                    IsElementalDamage(Spell.Damage.DamageType))
                    return Spell.Damage.DamageType;

                Entity Owner = Current.Interaction.GetOwner();
                if (Owner == None || Owner == Current)
                    break;
                Current = Owner;
            }
            if (a_Victim.DamageReceiver.IsValid() && IsElementalDamage(a_Victim.DamageReceiver.DamageType))
                return a_Victim.DamageReceiver.DamageType;
            return enuFirst;
        }

        // The elemental damage type of the damaging object ITSELF (its own chain: object, its spell, its owners). Unlike
        // GetHitDamageType the type stored on the victim is NOT used: it is the type of the PREVIOUS hit and would let a
        // sword blow pass as a spell. gEDamageType_None when the object is not elemental.
        gEDamageType GetSourceElementalType(Entity &a_Source)
        {
            // v1.0.34.0: the walk STOPS at the first creature (the owner of the projectile, the hero). His own Damage property
            // and his own last spell (Interaction.GetSpell) are not the type of THIS projectile: through them an arrow of the
            // hero was taken for a magic bolt (an elemental weapon / the last cast spell of the hero) and passed the filter.
            Entity Current = a_Source;
            for (GEInt iLink = 0; iLink < 4 && Current != None; ++iLink)
            {
                if (Current.IsNPC() || IsHeroSide(Current))
                    break;
                if (Current.Damage.IsValid() && IsElementalDamage(Current.Damage.DamageType))
                    return Current.Damage.DamageType;
                if (!Current.Interaction.IsValid())
                    break;
                Entity Spell = Current.Interaction.GetSpell();
                if (Spell != None && Spell != Current && Spell.Damage.IsValid() && IsElementalDamage(Spell.Damage.DamageType))
                    return Spell.Damage.DamageType;
                Entity Owner = Current.Interaction.GetOwner();
                if (Owner == None || Owner == Current)
                    break;
                Current = Owner;
            }
            return gEDamageType_None;
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

        // The hero in any form: the player entity, the body of the form, the hidden human body.
        GEBool IsHeroSide(Entity &a_Npc)
        {
            if (a_Npc == None)
                return GEFalse;
            if (a_Npc == Entity::GetPlayer() || IsHeroFormEntity(a_Npc))
                return GETrue;
            return _stricmp(a_Npc.GetName().GetText(), "PC_Hero") == 0 ? GETrue : GEFalse;
        }

        void LogAiSnapshot(GELPCChar a_strWhen, Entity &a_Npc)
        {
            Entity Hero = Entity::GetPlayer();
            Entity Leader = a_Npc.Party.GetPartyLeader();
            Entity Target = a_Npc.NPC.GetCurrentTarget();
            Entity Attacker = a_Npc.NPC.GetCurrentAttacker();
            bCString const strTask = a_Npc.Routine.GetCurrentTask();
            bCString const strState = a_Npc.Routine.GetCurrentState();
            GetDefaultLogger().LogFormat(
                "[GenomeLogicFix][diag][flee] %s: '%s' (%p, party type %d, leader '%s', waiting=%d) task='%s' state='%s' "
                "aiMode=%d attackReason=%d combatState=%u target='%s' currentAttacker='%s' distToHero=%.0f dead=%d "
                "down=%d\n",
                a_strWhen, a_Npc.GetName().GetText(), static_cast<void *>(a_Npc.GetInstance()),
                static_cast<int>(a_Npc.Party.PartyMemberType), Leader != None ? Leader.GetName().GetText() : "-",
                static_cast<int>(a_Npc.Party.Waiting), strTask.GetText(), strState.GetText(),
                static_cast<int>(a_Npc.Routine.AIMode), static_cast<int>(a_Npc.NPC.AttackReason),
                static_cast<unsigned>(a_Npc.NPC.CombatState), Target != None ? Target.GetName().GetText() : "-",
                Attacker != None ? Attacker.GetName().GetText() : "-",
                static_cast<double>(Hero != None ? a_Npc.GetDistanceTo(Hero) : -1.0f),
                static_cast<int>(a_Npc.IsDead()), static_cast<int>(a_Npc.IsDown()));
        }
        GEInt const FLEE_WATCH_MAX = 8;
        GEInt const FLEE_STAGES = 4;
        GEU32 const g_arrFleeOffsets[FLEE_STAGES] = {300, 1000, 2000, 4000};
        bCString g_arrFleeNames[FLEE_WATCH_MAX];
        GEU32 g_arrFleeStart[FLEE_WATCH_MAX] = {};
        GEInt g_arrFleeStage[FLEE_WATCH_MAX] = {};
        GEBool g_bFleeWatchActive = GEFalse;

        void StartFleeWatch(Entity &a_Victim)
        {
            bCString const strName = a_Victim.GetName();
            GEInt iSlot = -1;
            for (GEInt i = 0; i < FLEE_WATCH_MAX; ++i)
            {
                if (g_arrFleeStart[i] != 0 && g_arrFleeNames[i] == strName)
                {
                    iSlot = i; // hit again: the watch starts anew
                    break;
                }
                if (iSlot < 0 && g_arrFleeStart[i] == 0)
                    iSlot = i;
            }
            if (iSlot < 0)
                iSlot = 0;
            g_arrFleeNames[iSlot] = strName;
            g_arrFleeStart[iSlot] = ::GetTickCount() | 1;
            g_arrFleeStage[iSlot] = 0;
            g_bFleeWatchActive = GETrue;
        }

        // Called from the hooks that run all the time (attitude / target). Technically throttled, reads only.
        void PollBrawl();                                // defined below (v1.0.30.0)
        void ReleaseStumbledNpc(Entity &a_Npc);          // defined below
        void BrawlStageAction(Entity &a_Npc, GEInt a_iStage); // defined below
        void OnHeroBlowOnArmy(Entity &a_Victim);         // defined below
        void PollFleeWatch()
        {
            PollBrawl();
            if (!g_bFleeWatchActive)
                return;
            static GEU32 s_u32Last = 0;
            GEU32 const u32Now = ::GetTickCount();
            if (s_u32Last != 0 && static_cast<GEInt>(u32Now - s_u32Last) < 100)
                return;
            s_u32Last = u32Now;
            GEBool bAny = GEFalse;
            for (GEInt i = 0; i < FLEE_WATCH_MAX; ++i)
            {
                if (g_arrFleeStart[i] == 0)
                    continue;
                GEInt const iStage = g_arrFleeStage[i];
                if (iStage >= FLEE_STAGES)
                {
                    g_arrFleeStart[i] = 0;
                    continue;
                }
                bAny = GETrue;
                GEU32 const u32Elapsed = u32Now - g_arrFleeStart[i];
                if (static_cast<GEInt>(u32Elapsed) < static_cast<GEInt>(g_arrFleeOffsets[iStage]))
                    continue;
                g_arrFleeStage[i] = iStage + 1;
                bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
                for (GEInt k = 0; k < arrNPCs.GetCount(); ++k)
                {
                    Entity Npc = arrNPCs.GetAt(k);
                    if (!(Npc.GetName() == g_arrFleeNames[i]))
                        continue;
                    char strWhen[64] = {};
                    _snprintf_s(strWhen, sizeof(strWhen), _TRUNCATE, "+%u ms after the blow of the hero",
                                static_cast<unsigned>(u32Elapsed));
                    LogAiSnapshot(strWhen, Npc);
                    if (iStage >= 1 && IsHeroControlled(Npc) && HasTask(Npc, "ZS_Stumble"))
                    {
                        ReleaseStumbledNpc(Npc); // THE SAFETY NET: an army member left in ZS_Stumble about 1 s after a blow is released
                        GetDefaultLogger().LogFormat("[GenomeLogicFix] '%s' was stuck in ZS_Stumble after a blow: released.\n",
                                                     Npc.GetName().GetText());
                    }
                    BrawlStageAction(Npc, iStage);
                    break;
                }
            }
            if (!bAny)
                g_bFleeWatchActive = GEFalse;
        }
        // One line per blow on a member of the army. a_strFunction: the native function that recorded the blow.
        // a_pSource may be the damaging object (projectile) - the owner chain gives the real attacker.
        void LogArmyHit(GELPCChar a_strFunction, Entity &a_Victim, Entity &a_Source, GEInt a_iDamageType)
        {
            static GEInt s_iLines = 0;
            if (s_iLines >= 600 || a_Victim == None || !a_Victim.IsNPC())
                return;
            if (!IsHeroControlled(a_Victim))
                return;
            Entity Attacker = GetDamageOwnerNpc(a_Source);
            if (Attacker == None)
                Attacker = a_Source;
            Entity Hero = Entity::GetPlayer();
            GEBool const bHeroSide = Attacker != None && IsHeroSide(Attacker);
            GEBool const bArmy = Attacker != None && IsHeroControlled(Attacker);
            GELPCChar strKind = bHeroSide ? "HERO" : (bArmy ? "ARMY" : "OUTSIDER");
            ++s_iLines;
            GetDefaultLogger().LogFormat(
                "[GenomeLogicFix][diag][hit] %s: attacker='%s' (%p, %s, faction %d, party type %d) -> victim='%s' (%p, "
                "party type %d) damage=%s(%d) source object='%s' hero(GetPlayer)='%s'\n",
                a_strFunction, Attacker != None ? Attacker.GetName().GetText() : "-",
                Attacker != None ? static_cast<void *>(Attacker.GetInstance()) : nullptr, strKind,
                Attacker != None && Attacker.IsNPC() ? static_cast<int>(Attacker.NPC.PoliticalAlignment) : -1,
                Attacker != None && Attacker.IsNPC() ? static_cast<int>(Attacker.Party.PartyMemberType) : -1,
                a_Victim.GetName().GetText(), static_cast<void *>(a_Victim.GetInstance()),
                static_cast<int>(a_Victim.Party.PartyMemberType), DamageTypeName(a_iDamageType), a_iDamageType,
                a_Source != None ? a_Source.GetName().GetText() : "-", Hero.GetName().GetText());
            // The hero (in any form) hurts his own creature: the AI is watched for the next seconds.
            if (bHeroSide)
            {
                LogAiSnapshot("right after the blow of the hero", a_Victim);
                StartFleeWatch(a_Victim);
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
        GEInt const REC_MAX = 32;
        GEU32 const COMBAT_MS = 60000;
        bCString g_arrCombatNames[REC_MAX];
        GEU32 g_arrCombatTick[REC_MAX] = {};

        void NoteName(bCString *a_pNames, GEU32 *a_pTicks, bCString const &a_strName, GEU32 a_u32Span)
        {
            GEU32 const u32Now = ::GetTickCount();
            GEInt iFree = -1;
            for (GEInt i = 0; i < REC_MAX; ++i)
            {
                if (a_pTicks[i] != 0 && a_pNames[i] == a_strName)
                {
                    a_pTicks[i] = u32Now | 1;
                    return;
                }
                if (iFree < 0 && (a_pTicks[i] == 0 || static_cast<GEInt>(u32Now - a_pTicks[i]) >= static_cast<GEInt>(a_u32Span)))
                    iFree = i;
            }
            if (iFree < 0)
                iFree = 0;
            a_pNames[iFree] = a_strName;
            a_pTicks[iFree] = u32Now | 1;
        }

        GEBool HasName(bCString const *a_pNames, GEU32 const *a_pTicks, bCString const &a_strName, GEU32 a_u32Span)
        {
            GEU32 const u32Now = ::GetTickCount();
            for (GEInt i = 0; i < REC_MAX; ++i)
                if (a_pTicks[i] != 0 && a_pNames[i] == a_strName &&
                    static_cast<GEInt>(u32Now - a_pTicks[i]) < static_cast<GEInt>(a_u32Span))
                    return GETrue;
            return GEFalse;
        }

        void ClearCombatRecords()
        {
            for (GEInt i = 0; i < REC_MAX; ++i)
            {
                g_arrCombatNames[i] = bCString();
                g_arrCombatTick[i] = 0;
            }
        }

        GEBool HasCombatRecord(Entity &a_Outsider)
        {
            return HasName(g_arrCombatNames, g_arrCombatTick, a_Outsider.GetName(), COMBAT_MS);
        }

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
        GEInt const BRAWL_MAX = 8;
        GEU32 const BRAWL_WINDOW_MS = 10000; // the second blow must come within this time, otherwise the counter is reset
        GEU32 const BRAWL_MAX_MS = 90000;    // a fight never lasts longer than this
        GEU32 const BLOW_DEBOUNCE_MS = 1200; // the burning ticks of one fireball are NOT a second blow: counted once
        bCString g_arrBrawlNames[BRAWL_MAX];
        GEInt g_arrBrawlCount[BRAWL_MAX] = {};
        GEU32 g_arrBrawlLastBlow[BRAWL_MAX] = {}; // 0: the slot is free
        GEU32 g_arrBrawlStart[BRAWL_MAX] = {};    // 0: the creature does not fight the hero

        GEBool g_arrBrawlReacted[BRAWL_MAX] = {}; // the reaction of a green NPC to the first blow has been made
        GEU32 g_arrBrawlCastSeq[BRAWL_MAX] = {};  // the number of the cast of the first blow (the second blow = a NEW cast)
        GEU32 g_u32HeroCastSeq = 0;               // counts the casts of the hero (PSNpc::SetLastCast)

        GEInt FindBrawlSlot(bCString const &a_strName, GEBool a_bCreate)
        {
            GEU32 const u32Now = ::GetTickCount();
            GEInt iFree = -1;
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
            {
                if (g_arrBrawlLastBlow[i] != 0 && g_arrBrawlNames[i] == a_strName)
                    return i;
                GEBool const bExpired = g_arrBrawlLastBlow[i] == 0 ||
                                        (g_arrBrawlStart[i] == 0 && static_cast<GEInt>(u32Now - g_arrBrawlLastBlow[i]) >
                                                                        static_cast<GEInt>(BRAWL_WINDOW_MS));
                if (iFree < 0 && bExpired)
                    iFree = i;
            }
            if (!a_bCreate)
                return -1;
            if (iFree < 0)
                iFree = 0;
            g_arrBrawlNames[iFree] = a_strName;
            g_arrBrawlCount[iFree] = 0;
            g_arrBrawlStart[iFree] = 0;
            g_arrBrawlLastBlow[iFree] = 0;
            g_arrBrawlReacted[iFree] = GEFalse;
            g_arrBrawlCastSeq[iFree] = 0;
            return iFree;
        }

        void ClearBrawls()
        {
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
            {
                g_arrBrawlNames[i] = bCString();
                g_arrBrawlCount[i] = 0;
                g_arrBrawlLastBlow[i] = 0;
                g_arrBrawlStart[i] = 0;
                g_arrBrawlReacted[i] = GEFalse;
                g_arrBrawlCastSeq[i] = 0;
            }
        }

        // v1.0.51.0: THE CLEAN SLATE (called by the event "Mercy ended"). Every memory of the blows of the hero is dropped for
        // EVERY NPC of the location: the phase / victim / timers of the city beating (the FIRST / SECOND blow of the residents),
        // the watchers, the memory of the assault, the counters of the brawl of the army (blow count, started fight, reaction),
        // the blow watch, the records of the fights. The NPCs that still watch the old beating (ZS_WatchFight) get their
        // routine back first - the native loop never ends by itself. The list of the NPCs sent against the hero (g_arrAggro*)
        // and the release flag are NOT touched: the same event calms those down in ZS_Attack_Loop. The next blow is the first one.
        void ResetBlowHistory(GELPCChar a_strWhy)
        {
            GEInt iReleased = 0;
            if (g_iWatchCount > 0)
            {
                bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
                for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
                {
                    Entity Npc = arrNPCs.GetAt(i);
                    if (Npc == None || !Npc.IsNPC() || Npc.IsDead() || !IsWatcher(Npc) || !HasTask(Npc, "ZS_WatchFight"))
                        continue;
                    Npc.NPC.SetCurrentTarget(None);
                    Npc.NPC.SetAlternativeTarget(None);
                    Npc.Routine.ContinueRoutine();
                    ++iReleased;
                }
            }

            g_enuCityPhase = mECityPhase_Idle;
            g_strCityVictim = bCString();
            g_strCityEnclave = bCString();
            g_enuCityAlignment = gEPoliticalAlignment_None;
            g_u32WatchDeadline = 0;
            g_u32AggroUntil = 0;
            g_u32BlowTick = 0;
            g_u32LastAggroPass = 0;
            g_u32LastDeclare = 0;
            g_iWatchCount = 0;
            for (GEInt i = 0; i < WATCH_MAX; ++i)
                g_arrWatchNames[i] = bCString();
            g_strAssaultVictim = bCString();
            g_u32AssaultUntil = 0;

            ClearBrawls();
            for (GEInt i = 0; i < FLEE_WATCH_MAX; ++i)
            {
                g_arrFleeNames[i] = bCString();
                g_arrFleeStart[i] = 0;
                g_arrFleeStage[i] = 0;
            }
            g_bFleeWatchActive = GEFalse;
            ClearFocusRecords();
            ClearCombatRecords();
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Blow history reset (%s): FIRST / SECOND blow counters of all NPCs, the "
                                         "city beating and its watchers (%d released) are cleared.\n",
                                         a_strWhy, static_cast<int>(iReleased));
        }

        // THE UNCONDITIONAL RESET OF THE COMBAT STATE (knock-out of the master, time-out, the end of the fight). Nothing of the
        // fight is left on the creature, so it cannot hang in an unnatural pose: no target (the combat focus), no attacker,
        // no alternative target, no attack reason, no combat state; the routine is stopped and the creature is put back to
        // the peaceful task of following the hero (ContinueRoutine picks the native one, ZS_FollowPlayer is set by hand when
        // the routine has not left the combat / stumble task). A waiting creature keeps waiting.
        void ResetCombatState(Entity &a_Npc)
        {
            a_Npc.Routine.FullStop();
            a_Npc.NPC.SetCurrentTarget(None);
            a_Npc.NPC.SetCurrentAttacker(None);
            a_Npc.NPC.SetLastAttacker(None);
            a_Npc.NPC.SetAlternativeTarget(None);
            a_Npc.NPC.AttackReason = gEAttackReason_None;
            a_Npc.NPC.CombatState = 0;
            a_Npc.Routine.ContinueRoutine();
            // Forced back to the peaceful task of following (a creature that was told to wait keeps waiting).
            if (!a_Npc.Party.Waiting)
                a_Npc.Routine.SetTask("ZS_FollowPlayer");
        }

        // The fight of this creature with the hero is over: the slot is closed and the combat state is reset.
        void EndBrawlOf(Entity &a_Npc, GELPCChar a_strWhy)
        {
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The brawl of '%s' with his master is over (%s): the combat state is "
                                         "reset, back to the routine of following.\n",
                                         a_Npc.GetName().GetText(), a_strWhy);
            GEInt const iSlot = FindBrawlSlot(a_Npc.GetName(), GEFalse);
            if (iSlot >= 0)
            {
                g_arrBrawlStart[iSlot] = 0;
                g_arrBrawlCount[iSlot] = 0;
                g_arrBrawlLastBlow[iSlot] = 0;
                g_arrBrawlReacted[iSlot] = GEFalse;
            }
            ResetCombatState(a_Npc);
        }

        GEBool IsBrawlFighting(Entity &a_Npc)
        {
            GEInt const iSlot = FindBrawlSlot(a_Npc.GetName(), GEFalse);
            return iSlot >= 0 && g_arrBrawlStart[iSlot] != 0;
        }

        // The SAFETY NET against a creature stuck in ZS_Stumble after a blow of the hero (it is called by PollFleeWatch about
        // one second after the blow). A creature that has accepted the fight goes on attacking the hero, any other one
        // goes back to the routine.
        void ReleaseStumbledNpc(Entity &a_Npc)
        {
            a_Npc.Routine.FullStop();
            if (IsBrawlFighting(a_Npc) && !IsHeroKnockedOut())
            {
                a_Npc.NPC.SetCurrentTarget(Entity::GetPlayer());
                a_Npc.NPC.AttackReason = gEAttackReason_Angry;
                a_Npc.Routine.SetTask("ZS_Attack");
                return;
            }
            a_Npc.NPC.SetCurrentTarget(None);
            a_Npc.Routine.ContinueRoutine();
        }

        // THE MASTER IS KNOCKED OUT: a creature of his party that fights him (or still keeps him as a target) stops at once
        // and goes back to the routine of following. Nobody of the party finishes, robs or hits the fallen master.
        // Returns true when the creature was released.
        GEBool StandDownFromMaster(Entity &a_Npc, GELPCChar a_strWhere)
        {
            if (a_Npc == None || !a_Npc.IsNPC() || !IsHeroControlled(a_Npc) || !IsHeroKnockedOut())
                return GEFalse;
            // Fighting the master in any way: a started brawl, the master as the target or as the current attacker.
            Entity Target = a_Npc.NPC.GetCurrentTarget();
            Entity Attacker = a_Npc.NPC.GetCurrentAttacker();
            GEBool const bAgainstMaster = IsBrawlFighting(a_Npc) || (Target != None && IsHeroSide(Target)) ||
                                          (Attacker != None && IsHeroSide(Attacker));
            if (!bAgainstMaster)
                return GEFalse;
            EndBrawlOf(a_Npc, a_strWhere);
            return GETrue;
        }

        // A blow of the hero (any form) on a member of his army has passed (called from RecordCombat).
        void OnHeroBlowOnArmy(Entity &a_Victim)
        {
            if (a_Victim == None || !a_Victim.IsNPC() || a_Victim.IsDead() || a_Victim.IsDown() || IsHeroKnockedOut())
                return;
            GEU32 const u32Now = ::GetTickCount();
            GEInt const iSlot = FindBrawlSlot(a_Victim.GetName(), GETrue);
            if (g_arrBrawlLastBlow[iSlot] != 0 &&
                static_cast<GEInt>(u32Now - g_arrBrawlLastBlow[iSlot]) < static_cast<GEInt>(BLOW_DEBOUNCE_MS))
                return; // the same blow, seen by another hook
            // THE WINDOW OF THE SECOND BLOW: no second blow within BRAWL_WINDOW_MS (10 s) of the first one - the counter is reset
            // and the creature cools down (this blow is the FIRST one again).
            if (g_arrBrawlStart[iSlot] == 0 &&
                (g_arrBrawlLastBlow[iSlot] == 0 || static_cast<GEInt>(u32Now - g_arrBrawlLastBlow[iSlot]) >
                                                       static_cast<GEInt>(BRAWL_WINDOW_MS)))
            {
                g_arrBrawlCount[iSlot] = 0;
                g_arrBrawlReacted[iSlot] = GEFalse;
            }
            // The burning / freezing after ONE cast are no second blow: the second blow is a NEW cast of the hero.
            if (g_arrBrawlStart[iSlot] == 0 && g_arrBrawlCount[iSlot] == 1 && g_arrBrawlCastSeq[iSlot] == g_u32HeroCastSeq)
                return;
            if (g_arrBrawlCount[iSlot] == 0)
                g_arrBrawlCastSeq[iSlot] = g_u32HeroCastSeq;
            g_arrBrawlLastBlow[iSlot] = u32Now | 1;
            ++g_arrBrawlCount[iSlot];
            StartFleeWatch(a_Victim); // the watch makes the reaction to the first blow and keeps the safety net
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Blow %d of the hero on '%s' (party type %d).\n",
                                         static_cast<int>(g_arrBrawlCount[iSlot]), a_Victim.GetName().GetText(),
                                         static_cast<int>(a_Victim.Party.PartyMemberType));
            if (g_arrBrawlCount[iSlot] < 2 || g_arrBrawlStart[iSlot] != 0)
                return;
            // THE SECOND BLOW: the creature accepts the fight and attacks the hero.
            g_arrBrawlStart[iSlot] = u32Now | 1;
            Entity Hero = Entity::GetPlayer();
            a_Victim.Routine.FullStop();
            a_Victim.NPC.SetCurrentTarget(Hero);
            a_Victim.NPC.AttackReason = gEAttackReason_Angry;
            a_Victim.Routine.SetTask("ZS_Attack");
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The second blow: '%s' accepts the fight and attacks the hero "
                                         "('%s'). The rest of the army stays neutral.\n",
                                         a_Victim.GetName().GetText(), Hero.GetName().GetText());
        }

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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The arena quest is not running any more: %d waiting companion(s) of the arena "
                                         "session follow the hero again (ZS_FollowPlayer), %d other waiter(s) are left as they are.\n",
                                         static_cast<int>(iReleased), static_cast<int>(iKept));
        }

        void PollBrawl()
        {
            PollArenaQuestEnd();
            GEBool bAny = GEFalse;
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
                if (g_arrBrawlStart[i] != 0 || g_arrBrawlLastBlow[i] != 0)
                    bAny = GETrue;
            if (!bAny)
                return;
            static GEU32 s_u32Last = 0;
            GEU32 const u32Now = ::GetTickCount();
            if (s_u32Last != 0 && static_cast<GEInt>(u32Now - s_u32Last) < 250)
                return;
            s_u32Last = u32Now;
            GEBool const bHeroDown = IsHeroKnockedOut();
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
            {
                // THE COOL-DOWN: the first blow was borne and no second blow came within BRAWL_WINDOW_MS (10 s) - the counter
                // is reset, the combat state of the creature is cleared and it goes back to following the hero.
                if (g_arrBrawlStart[i] == 0 && g_arrBrawlLastBlow[i] != 0 &&
                    (bHeroDown || static_cast<GEInt>(u32Now - g_arrBrawlLastBlow[i]) > static_cast<GEInt>(BRAWL_WINDOW_MS)))
                {
                    for (GEInt k = 0; k < arrNPCs.GetCount(); ++k)
                    {
                        Entity Npc = arrNPCs.GetAt(k);
                        if (!(Npc.GetName() == g_arrBrawlNames[i]))
                            continue;
                        if (!Npc.IsDead() && !Npc.IsDown())
                            EndBrawlOf(Npc, bHeroDown ? "the master is knocked out" : "no second blow within 10 s: cooled down");
                        break;
                    }
                    g_arrBrawlCount[i] = 0;
                    g_arrBrawlLastBlow[i] = 0;
                    g_arrBrawlReacted[i] = GEFalse;
                    continue;
                }
                if (g_arrBrawlStart[i] == 0)
                    continue;
                GEBool const bTimeOver =
                    static_cast<GEInt>(u32Now - g_arrBrawlStart[i]) > static_cast<GEInt>(BRAWL_MAX_MS);
                GEBool bFound = GEFalse;
                for (GEInt k = 0; k < arrNPCs.GetCount(); ++k)
                {
                    Entity Npc = arrNPCs.GetAt(k);
                    if (!(Npc.GetName() == g_arrBrawlNames[i]))
                        continue;
                    bFound = GETrue;
                    GEBool const bOut = Npc.IsDead() || Npc.IsDown();
                    if (bHeroDown || bTimeOver || bOut)
                    {
                        if (!bOut)
                            EndBrawlOf(Npc, bHeroDown ? "the master is knocked out" : "time is over");
                        bFound = GEFalse; // the slot is closed below
                    }
                    break;
                }
                if (!bFound)
                {
                    g_arrBrawlStart[i] = 0;
                    g_arrBrawlCount[i] = 0;
                    g_arrBrawlLastBlow[i] = 0;
                    g_arrBrawlReacted[i] = GEFalse;
                }
            }
        }

        // Called by the blow watch (PollFleeWatch) after the stumble release; a_iStage 0 is +300 ms, 1 is +1000 ms,
        // 2 is +2000 ms, 3 is +4000 ms after the last blow of the hero.
        void BrawlStageAction(Entity &a_Npc, GEInt a_iStage)
        {
            if (!IsHeroControlled(a_Npc) || a_Npc.IsDead() || a_Npc.IsDown() || IsHeroKnockedOut())
                return;
            GEInt const iSlot = FindBrawlSlot(a_Npc.GetName(), GEFalse);
            if (iSlot < 0 || g_arrBrawlStart[iSlot] != 0)
                return;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return;
            if (a_iStage <= 1 && !g_arrBrawlReacted[iSlot] && g_arrBrawlCount[iSlot] == 1 &&
                !HasTask(a_Npc, "ZS_Stumble") && !HasTask(a_Npc, "ZS_Attack"))
            {
                // THE FIRST BLOW: the reaction of an ordinary green NPC (the weapon, the stance, the manoeuvring in front of
                // the hero, one on one). A stumble in progress is let finish (stage 0) or was released (stage 1).
                g_arrBrawlReacted[iSlot] = GETrue;
                if (!HasTask(a_Npc, "ZS_ReactToDamage"))
                {
                    a_Npc.Routine.FullStop();
                    a_Npc.NPC.SetCurrentTarget(Hero);
                    a_Npc.NPC.AttackReason = gEAttackReason_ReactToDamage;
                    a_Npc.Routine.SetTask("ZS_ReactToDamage");
                }
                GetDefaultLogger().LogFormat("[GenomeLogicFix] The first blow: '%s' reacts as a green NPC (ZS_ReactToDamage "
                                             "in front of the hero, one on one).\n",
                                             a_Npc.GetName().GetText());
            }
            else if (a_iStage == 3 && !HasTask(a_Npc, "ZS_Attack") && !HasTask(a_Npc, "ZS_ReactToDamage") &&
                     !HasTask(a_Npc, "ZS_FollowPlayer") && !a_Npc.Party.Waiting && a_Npc.GetDistanceTo(Hero) > 900.0f)
            {
                // THE ENDLESS RETREAT is stopped: he is far from the hero and still does not follow him.
                GetDefaultLogger().LogFormat("[GenomeLogicFix] '%s' retreated too far (%.0f) after the blow: back to the "
                                             "hero.\n",
                                             a_Npc.GetName().GetText(), static_cast<double>(a_Npc.GetDistanceTo(Hero)));
                a_Npc.Routine.FullStop();
                a_Npc.NPC.SetCurrentTarget(None);
                a_Npc.NPC.AttackReason = gEAttackReason_None;
                a_Npc.Routine.ContinueRoutine();
            }
        }

        // The engine stored a_Attacker as the attacker of a_Victim: a real blow.
        void RecordCombat(Entity &a_Victim, Entity &a_Attacker)
        {
            if (a_Victim == None || a_Attacker == None || a_Victim == a_Attacker)
                return;
            // THE HERO (any form) HIT A MEMBER OF HIS OWN ARMY: the manual two-stage counter of the brawl counts the blow (the
            // first one is borne, the second one within BRAWL_WINDOW_MS starts the fight). Both are of "the hero's side", so
            // this comes before the same-side exit below.
            if (IsHeroSide(a_Attacker) && IsHeroControlled(a_Victim))
            {
                OnHeroBlowOnArmy(a_Victim);
                return;
            }
            GEBool const bVictimSide = IsHeroSide(a_Victim) || IsHeroControlled(a_Victim);
            GEBool const bAttackerSide = IsHeroSide(a_Attacker) || IsHeroControlled(a_Attacker);
            if (bVictimSide == bAttackerSide)
                return;
            Entity &Outsider = bVictimSide ? a_Attacker : a_Victim;
            if (IsDuelTaboo(Outsider))
                return; // the duel opponent never becomes an enemy of the army
            if (Outsider.IsNPC())
                NoteName(g_arrCombatNames, g_arrCombatTick, Outsider.GetName(), COMBAT_MS);
        }

        // The attack reasons that do not need a record: the command of the player, the arena, the duel, the revolution, prey.
        GEBool IsOrderedAttackReason(GEInt a_iReason)
        {
            return a_iReason == gEAttackReason_PlayerCommand || a_iReason == gEAttackReason_Arena ||
                   a_iReason == gEAttackReason_Duel || a_iReason == gEAttackReason_Revolution ||
                   a_iReason == gEAttackReason_Prey;
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
        // THE FIGHT HAS STARTED. An outsider (not the hero, not of the army) and a creature of the army (or the hero) are in
        // a real fight when the engine has written one down as the attacker of the other, or when the combat record of the
        // outsider exists (SetCurrentAttacker / SetLastAttacker recorded a real blow).
        GEBool IsInRealFight(Entity &a_Self, Entity &a_Other)
        {
            if (a_Self == None || a_Other == None)
                return GEFalse;
            Entity *pArmy = nullptr;
            Entity *pOutsider = nullptr;
            if (IsHeroControlled(a_Self) && a_Other.IsNPC() && !IsHeroSide(a_Other) && !IsHeroControlled(a_Other))
            {
                pArmy = &a_Self;
                pOutsider = &a_Other;
            }
            else if (IsHeroControlled(a_Other) && a_Self.IsNPC() && !IsHeroSide(a_Self) && !IsHeroControlled(a_Self))
            {
                pArmy = &a_Other;
                pOutsider = &a_Self;
            }
            if (!pArmy || !pOutsider)
                return GEFalse;
            return HasCombatRecord(*pOutsider) || pArmy->NPC.GetCurrentAttacker() == *pOutsider ||
                           pArmy->NPC.GetLastAttacker() == *pOutsider || pOutsider->NPC.GetCurrentAttacker() == *pArmy ||
                           pOutsider->NPC.GetLastAttacker() == *pArmy
                       ? GETrue
                       : GEFalse;
        }
        // A creature of the army must not take an outsider for a target while there is no real fight with him.
        GEBool ArmyMustNotAttack(Entity &a_Self, Entity &a_Other, GEInt a_iReason)
        {
            if (a_Self == None || a_Other == None || !a_Other.IsNPC() || !IsHeroControlled(a_Self))
                return GEFalse;
            if (IsDuelTaboo(a_Other))
                return GETrue; // the duel opponent is invisible for the army, whatever the reason
            if (IsHeroSide(a_Other) || IsHeroControlled(a_Other) || IsOrderedAttackReason(a_iReason))
                return GEFalse;
            if (HasCombatRecord(a_Other) || a_Self.NPC.GetCurrentAttacker() == a_Other || a_Self.NPC.GetLastAttacker() == a_Other)
                return GEFalse;
            return GETrue;
        }

    } // namespace

    namespace
    {
        // v1.0.34.0: a blocked blow leaves NO trace in the health bar. The hit points are taken off by the native script
        // AssessHit (it writes PSDamageReceiver::DamageAmount and calls AddHitPoints / SetHitPoints), and every blocked path
        // returns BEFORE the original script runs. The pending DamageAmount is also reset to 0, so a stale amount of an
        // earlier hit can never be applied to the companion by anything that reads it later.
        void ZeroPendingDamage(Entity &a_Victim)
        {
            if (a_Victim != None && a_Victim.DamageReceiver.IsValid())
                a_Victim.DamageReceiver.DamageAmount = 0;
        }
    } // namespace

    // The one hook for the arena magic and the city assaults: DoLogicalDamage is the choke point of every hit
    // (projectiles, burning ticks, ice). self = victim, other = the damaging object.
    // In the arena only two fighters exist: fire / ice / lightning damage to the opponent can only come from the hero,
    // unless the damaging object is explicitly owned by the opponent himself. So no fragile "who created the projectile"
    // chain is needed there. Freezing / igniting is also detected by the state of the opponent (IsFrozen / IsBurning)
    // before and after the original, because the status is applied inside AssessHit.
    ME_DEFINE_AND_HOOK_SCRIPT(DoLogicalDamage)
    {
        Entity Victim = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Source = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());

        static GEBool s_bAliveLogged = GEFalse;
        if (!s_bAliveLogged)
        {
            s_bAliveLogged = GETrue;
            GetDefaultLogger().LogFormat("[GenomeLogicFix] DoLogicalDamage hook is alive.\n");
        }

        // Diagnostics only: every blow on a member of the army, before anything is decided.
        LogArmyHit("DoLogicalDamage (script, every melee / missile / spell hit)", Victim, Source,
                   static_cast<GEInt>(GetHitDamageType(Source, Victim)));

        // No friendly fire inside the army of the hero: the blow is not processed at all (no damage, no reaction).
        {
            Entity FriendlyAttacker = GetDamageOwnerNpc(Source);
            if (IsFriendlyFire(FriendlyAttacker, Victim, GEFalse, &Source))
            {
                ZeroPendingDamage(Victim); // no damage at all: the health bar of the companion does not move
                return 0;
            }
            // The fallen master is never hit again by his own party (no finishing, and no damage from the inertia of an
            // animation that was already running): the damage is equal to zero, the creature is released.
            if (Victim != None && IsHeroSide(Victim) && IsHeroKnockedOut())
            {
                Entity Hitter = FriendlyAttacker;
                if (Hitter == None && Victim.IsNPC())
                    Hitter = Victim.NPC.GetCurrentAttacker(); // the blow came without an owner: the last attacker counts
                if (Hitter != None && IsHeroControlled(Hitter))
                {
                    StandDownFromMaster(Hitter, "DoLogicalDamage");
                    return 0;
                }
            }
        }

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
                GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] arena hit on '%s': source='%s' (own type=%d, "
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
        else if (IsUnprovokedAssault(Victim, Source))
            OnCityAssault(Victim); // two-stage reaction of the witnesses: attention first, aggro on the second blow

        GEInt const iResult = DoLogicalDamage_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);

        // v1.0.35.0: the blow knocked a resident of a friendly city out: the crime is declared at once.
        if (Victim != None && Victim.IsNPC() && !bArenaVictim && Source != None && IsCreatedByHero(Victim, Source))
            OnCityKnockout(Victim, "DoLogicalDamage");

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
        return iResult;
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] %s hook is alive.\n", a_strWhere);
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
                    GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] arena: hero hurt '%s' (%s): hero task='%s' "
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
            else if (IsUnprovokedAssault(Victim, Hero))
                OnCityAssault(Victim); // two-stage reaction of the witnesses: attention first, aggro on the second blow
            s_bBusy = GEFalse;
        }

        // Diagnostics: the attacker stored by PSNpc::SetCurrentAttacker / SetLastAttacker on a member of the army. The
        // damage type is the one stored on the victim (it can still hold the previous blow here, see AssessHit).
        void LogAttackerSet(GELPCChar a_strFunction, PSNpc *a_pThis, Entity const *a_pAttacker)
        {
            if (!a_pThis || !a_pAttacker || !a_pThis->m_pEngineEntityPropertySet || a_pAttacker->GetInstance() == nullptr)
                return;
            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (!pOwner)
                return;
            Entity Victim(pOwner);
            Entity Attacker(*a_pAttacker);
            if (!Victim.IsNPC() || Victim == Attacker)
                return;
            GEInt const iType = Victim.DamageReceiver.IsValid() ? static_cast<GEInt>(Victim.DamageReceiver.DamageType) : 0;
            RecordCombat(Victim, Attacker); // a real blow: the native rules of the army open
            LogArmyHit(a_strFunction, Victim, Attacker, iType);
        }

        // ONE FOR ALL: an outsider became the current attacker of a member of the army (or of the hero in any form).
        void OnArmyAttacked(PSNpc *a_pThis, Entity const *a_pAttacker)
        {
            if (!a_pThis || !a_pAttacker || !a_pThis->m_pEngineEntityPropertySet || a_pAttacker->GetInstance() == nullptr)
                return;
            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (!pOwner)
                return;
            Entity Victim(pOwner);
            Entity Attacker(*a_pAttacker);
            if (!Victim.IsNPC() || Victim.IsDead())
                return;
            DefendAllies(Victim, Attacker);
        }

        mCFunctionHook Hook_NpcSetCurrentAttacker;
        void GE_STDCALL NpcSetCurrentAttacker(PSNpc *a_pThis, Entity const *a_pAttacker)
        {
            using mFOriginal = void(GE_STDCALL *)(PSNpc *, Entity const *);
            Hook_NpcSetCurrentAttacker.GetOriginalFunction<mFOriginal>()(a_pThis, a_pAttacker);
            OnHeroAttackerSet(a_pThis, a_pAttacker, "PSNpc::SetCurrentAttacker");
            LogAttackerSet("PSNpc::SetCurrentAttacker", a_pThis, a_pAttacker);
        }

        mCFunctionHook Hook_NpcSetLastAttacker;
        void GE_STDCALL NpcSetLastAttacker(PSNpc *a_pThis, Entity const *a_pAttacker)
        {
            using mFOriginal = void(GE_STDCALL *)(PSNpc *, Entity const *);
            Hook_NpcSetLastAttacker.GetOriginalFunction<mFOriginal>()(a_pThis, a_pAttacker);
            OnHeroAttackerSet(a_pThis, a_pAttacker, "PSNpc::SetLastAttacker");
            LogAttackerSet("PSNpc::SetLastAttacker", a_pThis, a_pAttacker);
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
            using mFOriginal = void(GE_STDCALL *)(PSNpc *, GEInt, GEBool);
            GEBool const bWatched = a_pThis && a_bEnable && (a_iMask & 3) != 0 && a_pThis->m_pEngineEntityPropertySet;
            GEBool const bWasBurning = bWatched && (a_iMask & 1) ? a_pThis->IsBurning() : GEFalse;
            GEBool const bWasFrozen = bWatched && (a_iMask & 2) ? a_pThis->IsFrozen() : GEFalse;

            Hook_NpcEnableStatusEffects.GetOriginalFunction<mFOriginal>()(a_pThis, a_iMask, a_bEnable);

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
                    GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] arena: %s started on '%s' by an enchanted melee weapon "
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] arena: %s started on '%s' (last attacker '%s', "
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

        // PSNpc::SetLastCast(): remembers when the hero cast a spell.
        mCFunctionHook Hook_NpcSetLastCast;
        void GE_STDCALL NpcSetLastCast(PSNpc *a_pThis)
        {
            using mFOriginal = void(GE_STDCALL *)(PSNpc *);
            Hook_NpcSetLastCast.GetOriginalFunction<mFOriginal>()(a_pThis);
            if (!a_pThis || !a_pThis->m_pEngineEntityPropertySet)
                return;
            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (pOwner && Entity(pOwner) == Entity::GetPlayer())
            {
                g_u32HeroCastTick = ::GetTickCount();
                if (g_u32HeroCastTick == 0)
                    g_u32HeroCastTick = 1;
                ++g_u32HeroCastSeq;
                static GEInt s_iCastLines = 0;
                if (s_iCastLines < 30)
                {
                    ++s_iCastLines;
                    bCString const strSpell = Entity::GetPlayer().NPC.LastSpell;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] hero cast '%s'\n", strSpell.GetText());
                }
            }
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] the opponent '%s' is told to end the fight: task='%s' "
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The hero is knocked out after a violation in the arena (opponent "
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
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Hero = Entity::GetPlayer();
        if (Self == Hero && g_bSpareArena)
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
            g_bSpareActive = GEFalse; // a new knock-out: the fall point is taken anew
            BeginHeroImmunity();      // v1.0.57.0: from now until he stands on his feet nobody may kill the hero
            RememberHeroFall(GEFalse); // mercy: the place where the hero fell (the arena loss is handled right below)
        }

        // The OPPONENT of the duel is knocked out by somebody who is not the hero (a witness, a companion, a summon): the
        // quest must not become a victory - the fight is lost. The one who dealt the blow is the current attacker of the
        // knocked-out NPC (the last attacker when it is empty); nobody written down (a burning tick of the hero's fire) is
        // not a third party.
        GEBool bThirdParty = GEFalse;
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
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] The arena opponent '%s' is knocked out by '%s', not by "
                                                 "the hero: the duel is lost.\n",
                                                 Self.GetName().GetText(), Who.GetName().GetText());
                }
            }
        }

        Entity Opponent;
        GEBool bForcedLoss = GEFalse;
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] Defeat of the hero: duel quest running=%d, "
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The hero is knocked out in the arena fight (opponent '%s', "
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

        GEInt const iResult = Defeat_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);

        // v1.0.35.0: a resident of a friendly city is knocked out by the hero: the full crime at once, no timer.
        if (Self != None && !(Self == Hero) && Self.IsNPC())
            OnCityKnockout(Self, "Defeat");

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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] The duel quest is still running after Defeat: "
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
        if (Self == Hero && g_bSpareArena && !bForcedLoss)
            EndArenaDuelAfterKnockout();
        return iResult;
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
    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_WatchFight_Loop)
    {
        Entity Self(a_pSPU->GetSelfEntity());
        CheckArenaCompanions(); // the spectators of the arena run this loop: the ring is closed to companions / summons
        TrackDuelForCompanions();
        if (Self != None && ReleaseWatcherIfOver(Self))
            return GETrue;

        GEBool const bResult = ZS_WatchFight_Loop_Original(a_rRunTimeStack, a_pSPU);

        if (Self != None && g_enuCityPhase == mECityPhase_Watching && IsWatcher(Self))
        {
            Entity Hero = Entity::GetPlayer();
            Self.SetAlignmentTarget(Hero);
        }
        return bResult;
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
        CheckArenaCompanions();
        TrackDuelForCompanions();
        {
            Entity Gladiator(a_pSPU->GetSelfEntity());
            // v1.0.47.0: BEFORE the native loop sends him to the hero: the hero is in the ring with the magic in his hand ->
            // the stance of the warning at once, the order to come closer is never carried out.
            if (Gladiator != None && (PreemptMagicWarning(Gladiator) || HoldOpponentInWarning(Gladiator)))
                return GETrue; // the magic in the hand of the hero: the stance, not the arena routine
        }
        return ZS_Arena_Loop_Original(a_rRunTimeStack, a_pSPU);
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
        // True: the NPC was handled (the attack was dropped), the native loop must not run in this tick.
        GEBool FixStaleHeroTarget(Entity &a_Npc)
        {
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
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] ZS_Attack_Loop: '%s' had the removed body of the hero as the target - "
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] ZS_Attack_Loop: '%s' has no body of the hero to attack - the attack is "
                                             "dropped.\n",
                                             a_Npc.GetName().GetText());
            }
            return GETrue;
        }

        // No C++ object with a destructor in this function (a structured exception handler needs it that way). The guard counter tells
        // the crash reporter (CrashDiag) that an access violation inside the call is handled here and is not the end of the game.
        GEBool CallAttackLoopGuarded(gFScriptAIState a_pfnOriginal, bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack,
                                     gCScriptProcessingUnit *a_pSPU, GEBool *o_pFaulted)
        {
            GEBool bResult = GETrue;
            CrashDiag::GuardEnter();
            __try
            {
                __try
                {
                    bResult = a_pfnOriginal(a_rRunTimeStack, a_pSPU);
                }
                __finally
                {
                    CrashDiag::GuardLeave();
                }
            }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            {
                *o_pFaulted = GETrue;
                bResult = GETrue;
            }
            return bResult;
        }
    } // namespace

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Attack_Loop)
    {
        // v1.0.66.0: the stale target (the removed body of the hero after a transformation) is repaired before anything reads it.
        {
            Entity Fighter(a_pSPU->GetSelfEntity());
            if (FixStaleHeroTarget(Fighter))
                return GETrue;
        }
        // The master is knocked out: a creature of his party that was fighting him stops at once (no finishing blow).
        {
            Entity Fighter(a_pSPU->GetSelfEntity());
            if (StandDownFromMaster(Fighter, "ZS_Attack_Loop"))
                return GETrue;
            // v1.0.41.0: the opponent in the warning stance (the magic in the hand of the hero) is put back into the stance.
            // v1.0.47.0: and he is put there BEFORE the first blow when the magic is already in the hand.
            if (Fighter != None && (PreemptMagicWarning(Fighter) || HoldOpponentInWarning(Fighter)))
                return GETrue;
        }
        // The event "the hero stands again" is noticed here too: after the hero got up nobody asks about killing him.
        SpareHousekeeping();
        CheckArenaCompanions(); // the ring is closed to companions / summons while the duel quest runs
        TrackDuelForCompanions(); // the companions wait while the duel quest runs and come back after it
        {
            Entity Gladiator(a_pSPU->GetSelfEntity());
            Entity GTarget = Gladiator != None ? Gladiator.NPC.GetCurrentTarget() : Entity();
            if (Gladiator != None && GTarget != None && IsHeroControlled(GTarget) && IsDuelTaboo(Gladiator))
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] The duel opponent '%s' leaves '%s' (party of the hero) alone.\n",
                                             Gladiator.GetName().GetText(), GTarget.GetName().GetText());
                Gladiator.Routine.FullStop();
                Gladiator.NPC.SetCurrentTarget(None);
                Gladiator.NPC.SetCurrentAttacker(None);
                Gladiator.NPC.AttackReason = gEAttackReason_None;
                Gladiator.Routine.ContinueRoutine();
                return GETrue;
            }
        }
        if (g_bAggroRelease)
        {
            Entity Self(a_pSPU->GetSelfEntity());
            Entity Hero = Entity::GetPlayer();
            GEInt const iIndex = Self != None ? FindAggro(Self.GetName()) : -1;
            if (iIndex >= 0 && Self.NPC.GetCurrentTarget() == Hero && !IsHeroKnockedOut())
            {
                g_arrAggroNames[iIndex] = g_arrAggroNames[g_iAggroCount - 1];
                g_arrAggroNames[g_iAggroCount - 1] = bCString();
                --g_iAggroCount;
                if (g_iAggroCount == 0)
                    g_bAggroRelease = GEFalse;
                Self.Routine.FullStop();
                Self.NPC.SetCurrentTarget(None);
                Self.NPC.AttackReason = gEAttackReason_None;
                Self.Routine.ContinueRoutine();
                GetDefaultLogger().LogFormat("[GenomeLogicFix] '%s' calms down: the hero is on his feet again (%d left).\n",
                                             Self.GetName().GetText(), static_cast<int>(g_iAggroCount));
                return GETrue;
            }
        }
        GEBool bFaulted = GEFalse;
        GEBool const bResult = CallAttackLoopGuarded(ZS_Attack_Loop_Original, a_rRunTimeStack, a_pSPU, &bFaulted);
        if (bFaulted)
        {
            // v1.0.66.0: the native loop faulted (a target that is not there any more): the attack is dropped, the game goes on.
            Entity Faulty(a_pSPU->GetSelfEntity());
            static GEInt s_iFaultLines = 0;
            if (s_iFaultLines < 30)
            {
                ++s_iFaultLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] ZS_Attack_Loop: the native loop of '%s' faulted (access violation) - the "
                                             "attack is dropped, the NPC goes back to his routine.\n",
                                             Faulty != None ? Faulty.GetName().GetText() : "?");
            }
            if (Faulty != None && Faulty.IsNPC())
            {
                Faulty.Routine.FullStop();
                Faulty.NPC.SetCurrentTarget(None);
                Faulty.NPC.SetCurrentAttacker(None);
                Faulty.NPC.AttackReason = gEAttackReason_None;
                Faulty.Routine.ContinueRoutine();
            }
        }
        return bResult;
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
        GEInt const WTYPE_MAX = 48;
        bCString g_arrUserWait[WTYPE_MAX];
        GEInt g_iUserWait = 0;
        bCString g_arrAutoWait[WTYPE_MAX];
        GEInt g_iAutoWait = 0;
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

        GEFloat DistanceFromSessionStart(bCVector const &a_vPosition)
        {
            GEFloat const fDx = a_vPosition.m_fX - g_vArenaSessionStart.m_fX;
            GEFloat const fDy = a_vPosition.m_fY - g_vArenaSessionStart.m_fY;
            GEFloat const fDz = a_vPosition.m_fZ - g_vArenaSessionStart.m_fZ;
            return std::sqrt(fDx * fDx + fDy * fDy + fDz * fDz);
        }

        mEWaiterType GetWaiterType(Entity &a_Npc)
        {
            bCString const strName = a_Npc.GetName();
            if (FindNameIn(g_arrUserWait, g_iUserWait, strName) >= 0)
                return mEWaiterType_User;
            if (FindWaiter(strName) >= 0 || FindNameIn(g_arrAutoWait, g_iAutoWait, strName) >= 0)
                return mEWaiterType_Auto;
            return mEWaiterType_None;
        }

        GELPCChar WaiterTypeText(mEWaiterType a_enuType)
        {
            return a_enuType == mEWaiterType_User ? "USER" : (a_enuType == mEWaiterType_Auto ? "AUTO" : "none");
        }

        void ClearWaiterMarks(Entity &a_Npc)
        {
            bCString const strName = a_Npc.GetName();
            RemoveNameFrom(g_arrUserWait, g_iUserWait, strName);
            RemoveNameFrom(g_arrAutoWait, g_iAutoWait, strName);
            RemoveNameFrom(g_arrWaiters, g_iWaiterCount, strName);
        }

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
        {
            ClearWaiterMarks(a_Npc);
            AddNameTo(g_arrUserWait, g_iUserWait, WTYPE_MAX, a_Npc.GetName());
        }

        // The plugin / the game ordered the wait: the next teleport carries him.
        void MarkWaiterAuto(Entity &a_Npc)
        {
            bCString const strName = a_Npc.GetName();
            RemoveNameFrom(g_arrUserWait, g_iUserWait, strName);
            AddNameTo(g_arrAutoWait, g_iAutoWait, WTYPE_MAX, strName);
        }

        void ResetWaiterMarks();
        void MarkWaiterAutoPublic(Entity &a_Npc) { MarkWaiterAuto(a_Npc); }
        void MarkWaiterUserPublic(Entity &a_Npc) { MarkWaiterUser(a_Npc); }
        void ClearWaiterMarksPublic(Entity &a_Npc) { ClearWaiterMarks(a_Npc); }

        void ResetWaiterMarks()
        {
            for (GEInt i = 0; i < WTYPE_MAX; ++i)
            {
                g_arrUserWait[i] = bCString();
                g_arrAutoWait[i] = bCString();
            }
            g_iUserWait = 0;
            g_iAutoWait = 0;
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
                RemoveNameFrom(g_arrUserWait, g_iUserWait, strName); // v1.0.68.0: a stale USER mark of an earlier wait
                RemoveNameFrom(g_arrAutoWait, g_iAutoWait, strName);
                g_arrWaiters[g_iWaiterCount++] = strName;           // v1.0.68.0: WaiterType = AUTO, the waiter of THIS arena session
                Npc.Party.Waiting = GETrue;
                Npc.Routine.FullStop();
                Npc.Routine.SetTask("ZS_WaitForPlayer");
                PrintCompanionWaitMessage(Npc);
                GetDefaultLogger().LogFormat("[GenomeLogicFix] '%s' (party type %d) waits: the duel quest runs.\n",
                                             strName.GetText(), static_cast<int>(Npc.Party.PartyMemberType));
                ++iNew;
            }
            if (a_bDuelStart || iNew > 0)
                GetDefaultLogger().LogFormat("[GenomeLogicFix] The duel quest runs: %d companion(s) of the hero wait now "
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena: the quest is Running again (Lost -> Running) after a lost / cancelled duel "
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
                    g_vArenaSessionStart = SessionHero.GetPosition();
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The duel is over (%s): %d companion(s) follow the hero "
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena session ended (%s): %d AUTO waiter(s) of the session, release mode %d.\n",
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
            if (FindNameIn(g_arrUserWait, g_iUserWait, strName) >= 0)
                return GEFalse; // the player has said "wait here" since the load: his order stays
            // Only a companion that waits NEAR the hero (on the tribune of the arena) belongs to the arena session. A companion that waits
            // far away (another city: the order of the player, or a stone) keeps his wait - the running arena quest has nothing to do with him.
            if (a_Npc.GetDistanceTo(a_Hero) > ARENA_ADOPT_RADIUS)
                return GEFalse;
            if (g_iWaiterCount >= WAITER_MAX)
                return GEFalse;
            RemoveNameFrom(g_arrAutoWait, g_iAutoWait, strName);
            g_arrWaiters[g_iWaiterCount++] = strName; // WaiterType = AUTO, the waiter of the arena session
            char strLine[300] = {};
            _snprintf_s(strLine, sizeof(strLine), _TRUNCATE,
                        "[GenomeLogicFix][teleport] save game | '%s' | party type %d | Waiting=1 | distance %.0f | WaiterType=AUTO | ADOPTED: "
                        "the arena quest is natively Running in the loaded save, so his wait is the arena session's, not the player's",
                        strName.GetText(), static_cast<int>(enuType), static_cast<double>(a_Npc.GetDistanceTo(a_Hero)));
            GetDefaultLogger().LogFormat("%s\n", strLine);
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
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] Save game: no arena quest is Running - the waiting companions keep the "
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Save game: the arena quest is Running, %d waiting companion(s) adopted as AUTO waiters "
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
            if (g_bArenaSessionHold)
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] The duel is over: the opponent '%s' keeps away from the army "
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The duel is lost: the opponent was not beaten by the hero.\n");
        }

        void ResetDuelState()
        {
            for (GEInt i = 0; i < WAITER_MAX; ++i)
                g_arrWaiters[i] = bCString();
            g_iWaiterCount = 0;
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
        GEBool IsHeroTransformedNow()
        {
            // v1.0.34.0: the body of a form ("Transform_*") answers IsNPC() == false (the log shows faction -1 / party type -1
            // for it), so IsNPC() must NOT be required here, otherwise a demon was taken for the human form.
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return GEFalse;
            if (Hero.IsNPC() && Hero.NPC.IsTransformed())
                return GETrue;
            bCString const strName = Hero.GetName();
            if (strName.GetText() == nullptr || strName.GetText()[0] == 0)
                return GEFalse;
            return _stricmp(strName.GetText(), "PC_Hero") != 0 ? GETrue : GEFalse;
        }

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

        // The BODY OF THE FORM: the log of the demon test shows that the hits of the hero come from an entity named
        // "Transform_Demon_Potion" while GetPlayer() never returned it (no beast-form diagnostics were ever written, they are
        // all guarded by "the player is not PC_Hero"). So the transformation spawns a separate creature (the template
        // "Transform_<form>[_Potion]") for the hero, and the NPCs of the world see THAT creature, not the hero entity. The
        // world judges it by the faction / species rules of a monster (a non-humanoid with the faction None -> every
        // humanoid, a slave included, takes it for an enemy). It is told by the name of its template.
        GEBool IsHeroFormEntity(Entity &a_Npc)
        {
            // v1.0.34.0: no IsNPC() here - the body of the form is not an NPC for the wrapper.
            if (a_Npc == None || a_Npc == Entity::GetPlayer())
                return GEFalse;
            bCString const strName = a_Npc.GetName();
            return _strnicmp(strName.GetText(), "Transform_", 10) == 0 ? GETrue : GEFalse;
        }

        GEBool IsHeroControlled(Entity &a_Npc)
        {
            Entity Hero = Entity::GetPlayer();
            if (a_Npc == None || a_Npc == Hero || !a_Npc.IsNPC())
                return GEFalse;
            Entity Leader = a_Npc.Party.GetPartyLeader();
            GEBool bLeaderIsHero = Leader == Hero;
            // The beast form: the transformation (MagicTransform -> StartTransform) makes the spawned creature the player
            // (GetPlayer() returns it), while the party of the hero still belongs to his own body "PC_Hero", which is
            // hidden meanwhile. The army follows the master in any form, so that body counts as the master too.
            if (!bLeaderIsHero && Leader != None && IsHeroTransformedNow())
            {
                Entity Body = Entity::GetEntity(bCString("PC_Hero"));
                if (Body != None)
                {
                    if (a_Npc == Body)
                        return GEFalse;
                    bLeaderIsHero = Leader == Body;
                }
            }
            if (!bLeaderIsHero)
                return GEFalse;
            return a_Npc.Party.PartyMemberType != gEPartyMemberType_None ? GETrue : GEFalse;
        }

        GEBool IsRedForHero(Entity &a_Npc)
        {
            gEAttitude const enuAttitude = AI_Decision::GetAttitudeToPlayer(a_Npc);
            return enuAttitude == gEAttitude_Hostile || enuAttitude == gEAttitude_Predator;
        }

        // The NPC who made the damage: the damaging object itself (melee: the attacker) or the owner up its chain.
        Entity GetDamageOwnerNpc(Entity &a_Source)
        {
            Entity Current = a_Source;
            for (GEInt i = 0; i < 4 && Current != None; ++i)
            {
                // v1.0.34.0: the hero in a transformed body is not an NPC for the wrapper, but he is the attacker all the same.
                if (Current.IsNPC() || IsHeroSide(Current))
                    return Current;
                if (!Current.Interaction.IsValid())
                    break;
                Entity Owner = Current.Interaction.GetOwner();
                if (Owner == None || Owner == Current)
                    break;
                Current = Owner;
            }
            return Entity();
        }

        // True: the controlled creature must not kill this victim (any NPC / beast / monster that is not red for the hero).
        GEBool FollowerMustSpare(Entity &a_Attacker, Entity &a_Victim, GELPCChar a_strWhere)
        {
            if (a_Attacker == None || a_Victim == None || a_Attacker == a_Victim || !a_Victim.IsNPC())
                return GEFalse;
            if (!IsHeroControlled(a_Attacker))
                return GEFalse;
            if (a_Victim == Entity::GetPlayer() || IsHeroControlled(a_Victim))
                return GEFalse;
            // The ORIGINAL rule of the game stays for every attacker that is not a humanoid (animals, monsters, ...): the
            // native IsDeadlyDamage asks IsHumanoid of BOTH sides and, when one of them is not, the blow is deadly at once.
            // Only a humanoid attacker (a humanoid companion / summon) fighting a humanoid can leave the other knocked
            // down - and only there the hero's "not a red enemy" rule below makes sense. So a wolf, a snapper or a monster
            // of the party kills everybody it brawls with, as it always did.
            if (!AI_Decision::IsHumanoid(a_Attacker))
                return GEFalse;
            // Only a creature that CAN be knocked down is spared. The game itself tells them apart: IsDeadlyDamage /
            // HasAttackReasonToKill ask IsHumanoid of the victim (an animal, a monster, ... has no knock-down animation and
            // always dies), so those are never spared, whatever their attitude to the hero is.
            if (!AI_Decision::IsHumanoid(a_Victim))
                return GEFalse;
            if (IsRedForHero(a_Victim))
                return GEFalse; // a fight to the death against a real enemy of the hero
            static GEInt s_iLines = 0;
            if (s_iLines < 80)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] %s: '%s' (controlled by the hero, party type %d) must not "
                                             "kill '%s': he is not red for the hero (attitude %d).\n",
                                             a_strWhere, a_Attacker.GetName().GetText(),
                                             static_cast<int>(a_Attacker.Party.PartyMemberType),
                                             a_Victim.GetName().GetText(),
                                             static_cast<int>(AI_Decision::GetAttitudeToPlayer(a_Victim)));
            }
            return GETrue;
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
        void OnHeroSummonCast(GELPCChar a_strScript, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity,
                              GELPVoid a_pOtherEntity)
        {
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] arena: the hero casts a summon (%s, self='%s', "
                                             "other='%s', last spell='%s') -> the crowd reacts at once\n",
                                             a_strScript, Self != None ? Self.GetName().GetText() : "-",
                                             Other != None ? Other.GetName().GetText() : "-",
                                             bCString(Hero.NPC.LastSpell).GetText());
            }
            s_bBusy = GETrue;
            s_bBusy = GEFalse;
        }
    } // namespace

#define GLF_SUMMON_HOOK(NAME)                                                                                          \
    ME_DEFINE_AND_HOOK_SCRIPT(NAME)                                                                                    \
    {                                                                                                                  \
        GEInt const iResult = NAME##_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);                         \
        OnHeroSummonCast(#NAME, a_pSPU, a_pSelfEntity, a_pOtherEntity);                                                \
        return iResult;                                                                                                \
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
        GEBool IsHeroHealSpell(bCString const &a_strSpell, GEInt &o_iCastType, bCString &o_strHitFunc)
        {
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] arena: the hero casts %s (spell '%s', cast type=%d, "
                                         "hit script '%s') -> %s\n",
                                         a_strScript, strSpell.GetText(), static_cast<int>(iCastType),
                                         strHitFunc.GetText(),
                                         bHeal ? "a heal / cure like a consumable" : "left alone (fast variant / unknown)");
            if (bHeal)
                OnHeroConsumeStarted();
        }
    } // namespace

#define GLF_HEAL_HOOK(NAME)                                                                                            \
    ME_DEFINE_AND_HOOK_SCRIPT(NAME)                                                                                    \
    {                                                                                                                  \
        GEInt const iResult = NAME##_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);                         \
        OnHeroHealCast(#NAME, a_pSPU, a_pSelfEntity);                                                                  \
        return iResult;                                                                                                \
    }

    GLF_HEAL_HOOK(MagicHeal)
    GLF_HEAL_HOOK(MagicCurePoison)
    GLF_HEAL_HOOK(MagicCureDisease)

    namespace
    {
        // The hero (any entity of his side) in a TRANSFORMED body: the body of the form ("Transform_*"), the player entity of
        // the form or his hidden human body while the form lasts.
        GEBool IsHeroAttackerInForm(Entity &a_Attacker)
        {
            return IsHeroFormEntity(a_Attacker) || (IsHeroSide(a_Attacker) && IsHeroTransformedNow());
        }

        // A SINGLE-TARGET MAGIC PROJECTILE of the hero (a fireball, an ice lance, a lightning bolt): the damaging object is
        // not a creature (a blow of a weapon comes with the attacker himself as the object, an arrow is not elemental) and
        // carries an elemental damage type of its own (the type stored on the victim is not used, it belongs to the previous
        // hit). An area spell never comes here: it reaches Entity::DoDamage (a_bAreaDamage = true).
        GEBool IsPointMagicSource(Entity *a_pSource)
        {
            if (!a_pSource || *a_pSource == None || a_pSource->IsNPC() || IsHeroSide(*a_pSource))
                return GEFalse;
            // v1.0.34.0: a PHYSICAL projectile is never magic: an arrow / a bolt carries the damage type Missile (a blade /
            // impact / physics object likewise), whatever the hero cast before.
            if (a_pSource->Damage.IsValid())
            {
                gEDamageType const enuOwn = a_pSource->Damage.DamageType;
                if (enuOwn == gEDamageType_Missile || enuOwn == gEDamageType_Blade)
                    return GEFalse;
            }
            return GetSourceElementalType(*a_pSource) != gEDamageType_None ? GETrue : GEFalse;
        }

        // FRIENDLY FIRE (v1.0.34.0, rules of 1.0.33.0). The hero damages a member of his army (party types 1..5):
        //  1. The hero in ANY TRANSFORMED FORM (a demon, a wolf, a bloodfly, a gorge ...): ALWAYS blocked, everything.
        //  2. The hero in the HUMAN FORM (PC_Hero):
        //     - any melee blow, any shot of a bow / crossbow, any area spell (a_bAreaDamage = true: meteor, fire rain, frost
        //       wave ...): ALWAYS blocked (IsFriendlyFire = true);
        //     - the ONLY exception: a single-target magic projectile of the hero shot at a creature of his party (a fireball,
        //       an ice lance, a lightning bolt): passes (IsFriendlyFire = false); it opens the manual counter of the brawl.
        //  3. Both the attacker and the victim are of the army: always blocked.
        GEBool IsFriendlyFire(Entity &a_Attacker, Entity &a_Victim, GEBool a_bAreaDamage, Entity *a_pSource)
        {
            if (a_Attacker == None || a_Victim == None || a_Attacker == a_Victim)
                return GEFalse;
            if (!IsHeroControlled(a_Victim))
                return GEFalse;
            GEBool const bHeroSide = IsHeroSide(a_Attacker);
            if (!bHeroSide && !IsHeroControlled(a_Attacker))
                return GEFalse;
            if (bHeroSide)
            {
                GEBool const bForm = IsHeroAttackerInForm(a_Attacker);
                if (!bForm && !a_bAreaDamage && IsPointMagicSource(a_pSource))
                    return GEFalse; // the one exception: a point magic projectile of the human form
                static GEInt s_iHeroLines = 0;
                if (s_iHeroLines < 60)
                {
                    ++s_iHeroLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] Damage of the hero ('%s', %s%s) on '%s' (party type %d) is "
                                                 "blocked.\n",
                                                 a_Attacker.GetName().GetText(), bForm ? "transformed" : "human form",
                                                 a_bAreaDamage ? ", area" : "", a_Victim.GetName().GetText(),
                                                 static_cast<int>(a_Victim.Party.PartyMemberType));
                }
                return GETrue;
            }
            static GEInt s_iLines = 0;
            if (s_iLines < 60)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Friendly fire inside the army of the hero is blocked: '%s' "
                                             "(party type %d) -> '%s' (party type %d).\n",
                                             a_Attacker.GetName().GetText(),
                                             static_cast<int>(a_Attacker.Party.PartyMemberType),
                                             a_Victim.GetName().GetText(),
                                             static_cast<int>(a_Victim.Party.PartyMemberType));
            }
            return GETrue;
        }
        GEBool IsHeroInBeastForm()
        {
            return IsHeroTransformedNow();
        }
    } // namespace

    //
    // PRIMARY AGGRESSION OF THE FACTION NONE AGAINST A SUMMON. A wild animal, a slave, a neutral (political alignment
    // None) must not be the first to attack, or to choose as its target, a creature summoned by the hero (party type 5:
    // skeleton, ogre, demon, ...). It ignores the summon until the summon hurts it first (the summon is then stored as
    // the current / last attacker of the NPC and the rule lets go). The other factions keep their native rules.
    //
    namespace
    {
        GEBool IsPrimaryAggressionOnSummon(Entity &a_Self, Entity &a_Other)
        {
            if (a_Self == None || a_Other == None || !a_Self.IsNPC() || !a_Other.IsNPC())
                return GEFalse;
            if (a_Self == Entity::GetPlayer() || a_Self == a_Other || IsHeroControlled(a_Self))
                return GEFalse;
            if (a_Self.NPC.PoliticalAlignment != gEPoliticalAlignment_None)
                return GEFalse;
            if (a_Other.Party.PartyMemberType != gEPartyMemberType_Summoned || !IsHeroControlled(a_Other))
                return GEFalse;
            // The summon has already hurt him: the fight is legal.
            if (a_Self.NPC.GetCurrentAttacker() == a_Other || a_Self.NPC.GetLastAttacker() == a_Other)
                return GEFalse;
            return GETrue;
        }

        // ONE FOR ALL, ALL FOR ONE: the outsiders who attack a member of the army, remembered by name for a while.
        GEInt const DEFEND_MAX = 16;
        GEU32 const DEFEND_MS = 20000;
        GEFloat const DEFEND_RADIUS = 2500.0f;
        bCString g_arrDefendNames[DEFEND_MAX];
        GEU32 g_arrDefendTick[DEFEND_MAX] = {};

        GEBool IsDefendedTarget(bCString const &a_strName)
        {
            GEU32 const u32Now = ::GetTickCount();
            for (GEInt i = 0; i < DEFEND_MAX; ++i)
                if (g_arrDefendTick[i] != 0 && g_arrDefendNames[i] == a_strName &&
                    static_cast<GEInt>(u32Now - g_arrDefendTick[i]) < static_cast<GEInt>(DEFEND_MS))
                    return GETrue;
            return GEFalse;
        }

        void ClearDefendedTargets()
        {
            // The full reset of the fight records (v1.0.30.0: they used to be wiped on EVERY attitude query, inside
            // IsDefendedTarget, which also erased the timer "the hero struck" and the records of the brawl).
            ClearFocusRecords();
            ClearCombatRecords();
            ClearBrawls();
            for (GEInt i = 0; i < DEFEND_MAX; ++i)
            {
                g_arrDefendNames[i] = bCString();
                g_arrDefendTick[i] = 0;
            }
        }

        void RememberDefendedTarget(bCString const &a_strName)
        {
            GEU32 const u32Now = ::GetTickCount();
            GEInt iFree = -1;
            for (GEInt i = 0; i < DEFEND_MAX; ++i)
            {
                if (g_arrDefendTick[i] != 0 && g_arrDefendNames[i] == a_strName)
                {
                    g_arrDefendTick[i] = u32Now;
                    return;
                }
                if (iFree < 0 && (g_arrDefendTick[i] == 0 ||
                                  static_cast<GEInt>(u32Now - g_arrDefendTick[i]) >= static_cast<GEInt>(DEFEND_MS)))
                    iFree = i;
            }
            if (iFree < 0)
                iFree = 0;
            g_arrDefendNames[iFree] = a_strName;
            g_arrDefendTick[iFree] = u32Now;
        }
    } // namespace
    namespace
    {
        // FOCUS RECORDS: who (an outsider) took whom (a member of the army) for a target in AssessTarget, and when.
        GEInt const FOCUS_MAX = 24;
        GEU32 const FOCUS_MS = 15000;
        bCString g_arrFocusEnemy[FOCUS_MAX];
        bCString g_arrFocusVictim[FOCUS_MAX];
        GEU32 g_arrFocusTick[FOCUS_MAX] = {};

        void NoteFocusRecord(bCString const &a_strEnemy, bCString const &a_strVictim)
        {
            GEU32 const u32Now = ::GetTickCount();
            GEInt iFree = -1;
            for (GEInt i = 0; i < FOCUS_MAX; ++i)
            {
                if (g_arrFocusTick[i] != 0 && g_arrFocusEnemy[i] == a_strEnemy && g_arrFocusVictim[i] == a_strVictim)
                {
                    g_arrFocusTick[i] = u32Now;
                    return;
                }
                if (iFree < 0 && (g_arrFocusTick[i] == 0 ||
                                  static_cast<GEInt>(u32Now - g_arrFocusTick[i]) >= static_cast<GEInt>(FOCUS_MS)))
                    iFree = i;
            }
            if (iFree < 0)
                iFree = 0;
            g_arrFocusEnemy[iFree] = a_strEnemy;
            g_arrFocusVictim[iFree] = a_strVictim;
            g_arrFocusTick[iFree] = u32Now;
        }

        GEBool HasFocusRecord(bCString const &a_strEnemy, bCString const &a_strVictim)
        {
            GEU32 const u32Now = ::GetTickCount();
            for (GEInt i = 0; i < FOCUS_MAX; ++i)
                if (g_arrFocusTick[i] != 0 && g_arrFocusEnemy[i] == a_strEnemy && g_arrFocusVictim[i] == a_strVictim &&
                    static_cast<GEInt>(u32Now - g_arrFocusTick[i]) < static_cast<GEInt>(FOCUS_MS))
                    return GETrue;
            return GEFalse;
        }

        void ClearFocusRecords()
        {
            for (GEInt i = 0; i < FOCUS_MAX; ++i)
            {
                g_arrFocusEnemy[i] = bCString();
                g_arrFocusVictim[i] = bCString();
                g_arrFocusTick[i] = 0;
            }
        }

        // a_Victim: the member of the army (or the hero) who is attacked / taken into focus, a_Attacker: the outsider.
        // Every free (or waiting) creature of the army in sight takes the outsider for an enemy and attacks him.
        void DefendAllies(Entity &a_Victim, Entity &a_Attacker)
        {
            static GEBool s_bBusy = GEFalse;
            if (s_bBusy || a_Victim == None || a_Attacker == None || a_Victim == a_Attacker)
                return;
            Entity Hero = Entity::GetPlayer();
            if (!a_Attacker.IsNPC() || a_Attacker.IsDead() || IsHeroSide(a_Attacker))
                return;
            // The hero (any form) hitting a member of his army is an internal brawl: nobody defends, nobody attacks.
            if (IsHeroSide(a_Attacker) || IsDuelTaboo(a_Attacker))
                return;
            if (!IsHeroControlled(a_Victim))
                return;
            if (IsHeroControlled(a_Attacker))
                return; // inside the army nobody is an enemy
            Entity Opponent;
            if (FindArenaOpponent(Opponent) && Opponent == a_Attacker)
                return; // the duel opponent is the business of the arena rules
            s_bBusy = GETrue;
            RememberDefendedTarget(a_Attacker.GetName());
            // the waiting ones stay in the tribune until the squad is released (the quest runs, or the release is pending)
            GEBool const bQuest = HasRunningArenaQuest() || g_bDuelWasActive;
            GEInt iHelpers = 0;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Ally = arrNPCs.GetAt(i);
                if (Ally == a_Victim || Ally == Hero || Ally == a_Attacker || Ally.IsDead() || Ally.IsDown())
                    continue;
                if (!IsHeroControlled(Ally) || Ally.NPC.GetCurrentTarget() == a_Attacker)
                    continue;
                if (Ally.GetDistanceTo(a_Attacker) > DEFEND_RADIUS || !Ally.IsFreeLOS(a_Attacker, GEFalse))
                    continue;
                // Free = fights nobody; a waiting one is called as well, but not out of the ring of a running duel.
                GEBool const bWaiting = Ally.Party.Waiting;
                if (!bWaiting && !IsPeaceful(Ally))
                    continue;
                if (bWaiting && bQuest && IsInsideArenaZone(Ally))
                    continue;
                if (bWaiting)
                    Ally.Party.Waiting = GEFalse;
                Ally.Routine.FullStop();
                AI_Perception::AssessTarget(Ally, a_Attacker, gEAttackReason_Enemy);
                if (!(Ally.NPC.GetCurrentTarget() == a_Attacker) || !HasTask(Ally, "ZS_Attack"))
                {
                    Ally.NPC.SetCurrentTarget(a_Attacker);
                    Ally.NPC.AttackReason = gEAttackReason_Enemy;
                    Ally.Routine.SetTask("ZS_Attack");
                }
                ++iHelpers;
            }
            s_bBusy = GEFalse;
            static GEInt s_iLines = 0;
            if (iHelpers > 0 && s_iLines < 60)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Mutual defence: '%s' attacks '%s' (party type %d) -> %d "
                                             "member(s) of the army attack him.\n",
                                             a_Attacker.GetName().GetText(), a_Victim.GetName().GetText(),
                                             static_cast<int>(a_Victim.Party.PartyMemberType),
                                             static_cast<int>(iHelpers));
            }
        }
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
        // v1.0.38.0: the Wanderers / Druids are known by their NATIVE political status gEPoliticalAlignment_Mid (the template
        // of Arakos in g3dit, PSNpc.PoliticalAlignment). The old parsing of the names (Ranger_*, druid, wanderer) is gone.
        GEBool IsWandererNpc(Entity &a_Npc)
        {
            if (a_Npc == None || !a_Npc.IsNPC())
                return GEFalse;
            return a_Npc.NPC.PoliticalAlignment == gEPoliticalAlignment_Mid ? GETrue : GEFalse;
        }

        // The target is the hero in a transformed body (the player entity of the form or the body of the form).
        GEBool IsTransformedHeroTarget(Entity &a_Other)
        {
            if (a_Other == None)
                return GEFalse;
            if (IsHeroFormEntity(a_Other))
                return GETrue;
            return a_Other == Entity::GetPlayer() && IsHeroTransformedNow() ? GETrue : GEFalse;
        }

        // The hero (any form) or his creature has already hurt this NPC: the fight is legal, no filter.
        GEBool HasBeenHurtBy(Entity &a_Self, Entity &a_Other)
        {
            Entity Cur = a_Self.NPC.GetCurrentAttacker();
            Entity Last = a_Self.NPC.GetLastAttacker();
            if (Cur != None && (IsHeroSide(Cur) || IsHeroControlled(Cur) || Cur == a_Other))
                return GETrue;
            if (Last != None && (IsHeroSide(Last) || IsHeroControlled(Last) || Last == a_Other))
                return GETrue;
            return GEFalse;
        }

        // The alignment of the target for the rule 5. The body of a form is no NPC for the wrapper: the alignment of the
        // hero is then read from his own hidden body PC_Hero.
        // v1.0.38.0: the alignment is read STRICTLY from the entity that is the target right now - the active body of the
        // form (Transform_*, the current player entity) or the creature of the party (types 1..5). The human PC_Hero is never
        // read. The body of a form is no NPC for the wrapper (IsNPC() == false), so the property set itself is asked.
        GEBool IsSlaveTarget(Entity &a_Other)
        {
            if (a_Other == None || (IsHeroSide(a_Other) && !IsTransformedHeroTarget(a_Other)))
                return GEFalse; // the human body of the hero (PC_Hero / the player in his own body): excluded
            if (_stricmp(a_Other.GetName().GetText(), "PC_Hero") == 0)
                return GEFalse;
            if (!a_Other.NPC.IsValid())
                return GEFalse;
            return a_Other.NPC.PoliticalAlignment == gEPoliticalAlignment_Slave ? GETrue : GEFalse;
        }

        // Rule 5. a_Self: the one who would become aggressive, a_Other: the target.
        GEBool IsSlaveVsNoneBlocked(Entity &a_Self, Entity &a_Other)
        {
            if (a_Self == None || a_Other == None || !a_Self.IsNPC() || a_Self == a_Other)
                return GEFalse;
            if (a_Self == Entity::GetPlayer() || IsHeroControlled(a_Self) || IsHeroSide(a_Self))
                return GEFalse;
            if (a_Self.IsDead() || !AI_Decision::IsHumanoid(a_Self) || a_Self.NPC.PoliticalAlignment != gEPoliticalAlignment_None)
                return GEFalse;
            if (!IsPeaceful(a_Self) || HasBeenHurtBy(a_Self, a_Other))
                return GEFalse;
            if (IsSentAgainstHero(a_Self)) // v1.0.67.0: the crowd of the crime fights the form like the man
                return GEFalse;
            if (!IsSlaveTarget(a_Other))
                return GEFalse;
            GEBool const bForm = IsTransformedHeroTarget(a_Other);
            GEBool const bParty = a_Other.IsNPC() && IsHeroControlled(a_Other); // party types 1..5
            return bForm || bParty ? GETrue : GEFalse;
        }

        // Rule 6.
        GEBool IsWandererImmune(Entity &a_Self, Entity &a_Other)
        {
            if (!IsWandererNpc(a_Self) || a_Self.IsDead() || a_Self == a_Other)
                return GEFalse;
            if (!IsTransformedHeroTarget(a_Other))
                return GEFalse;
            if (IsSentAgainstHero(a_Self)) // v1.0.67.0: sent against the hero by the plugin
                return GEFalse;
            // The hero attacked a Wanderer first: the native rules (and the aggression) apply.
            return HasBeenHurtBy(a_Self, a_Other) ? GEFalse : GETrue;
        }

        void LogPoliticalFilter(GELPCChar a_strWhere, GELPCChar a_strRule, Entity &a_Self, Entity &a_Other)
        {
            static GEInt s_iLines = 0;
            if (s_iLines >= 60)
                return;
            ++s_iLines;
            GetDefaultLogger().LogFormat("[GenomeLogicFix] %s: %s - '%s' keeps calm towards '%s'.\n", a_strWhere, a_strRule,
                                         a_Self.GetName().GetText(), a_Other.GetName().GetText());
        }
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
        GEBool IsFormWeaponBlocked(Entity &a_Self, Entity &a_Other)
        {
            if (a_Self == None || a_Other == None || !a_Self.IsNPC() || a_Self.IsDead())
                return GEFalse;
            if (a_Self == a_Other || IsHeroSide(a_Self) || IsHeroControlled(a_Self))
                return GEFalse;
            if (!IsTransformedHeroTarget(a_Other))
                return GEFalse;
            // v1.0.67.0: sent against the hero by the plugin (the crowd of the crime, the duel opponent): the form is the same enemy for
            // him as the human body was - the claws are a weapon for him.
            if (IsSentAgainstHero(a_Self))
                return GEFalse;
            // The hero already hit him: the fight is legal.
            if (HasBeenHurtBy(a_Self, a_Other))
                return GEFalse;
            // He already fights the hero, or takes him for an enemy.
            if (a_Self.NPC.GetCurrentTarget() == a_Other && a_Self.NPC.AttackReason != gEAttackReason_None)
                return GEFalse;
            gEAttitude const enuAttitude = AI_Decision::GetAttitudeToPlayer(a_Self);
            if (enuAttitude == gEAttitude_Angry || enuAttitude == gEAttitude_Hostile || enuAttitude == gEAttitude_Predator)
                return GEFalse;
            return GETrue;
        }

        void LogFormWeapon(GELPCChar a_strWhere, Entity &a_Self, Entity &a_Other)
        {
            static GEInt s_iLines = 0;
            if (s_iLines >= 40)
                return;
            ++s_iLines;
            GetDefaultLogger().LogFormat("[GenomeLogicFix] %s: the built-in weapon of the form '%s' is no threat for '%s'.\n",
                                         a_strWhere, a_Other.GetName().GetText(), a_Self.GetName().GetText());
        }
    } // namespace

    ME_DEFINE_AND_HOOK_SCRIPT(AssessPlayerWeapon)
    {
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (IsFormWeaponBlocked(Self, Other))
        {
            LogFormWeapon("AssessPlayerWeapon", Self, Other);
            return 0;
        }
        return AssessPlayerWeapon_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    ME_DEFINE_AND_HOOK_SCRIPT(AssessWarn)
    {
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (IsFormWeaponBlocked(Self, Other))
        {
            LogFormWeapon("AssessWarn", Self, Other);
            return 0;
        }
        return AssessWarn_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    ME_DEFINE_AND_HOOK_SCRIPT(CanThreaten)
    {
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (IsFormWeaponBlocked(Self, Other))
            return 0;
        return CanThreaten_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // The choice of a target (the script that switches an NPC to ZS_Attack).
    //  1. A creature under the control of the hero (party types 1..5) never takes the hero in ANY form (demon, wolf,
    //     ...), the body of the form or another creature of his army for a target.
    //  2. An NPC of the faction None never chooses a summon of the hero as a target before the summon hurts him first.
    //  3. When an outsider takes a member of the army for a target, the free members of the army defend him.
    ME_DEFINE_AND_HOOK_SCRIPT(AssessTarget)
    {
        PollFleeWatch(); // diagnostics only
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (Self == None || Other == None)
            return AssessTarget_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        Entity Hero = Entity::GetPlayer();
        // v1.0.41.0: the opponent in the warning stance (the magic in the hand of the hero) does not take him for a target.
        // v1.0.47.0: the very first order of the native code (the choice of the target) is blocked and replaced by the stance.
        if ((Other == Hero || IsHeroSide(Other)) && (IsWarnedOpponent(Self) || PreemptMagicWarning(Self)))
            return 0;
        // v1.0.36.0: the political filters (rule 5: faction None vs a Slave target, rule 6: the Wanderers vs the hero in a
        // transformed body). The automatic aggression is not started at all.
        if (IsWandererImmune(Self, Other))
        {
            LogPoliticalFilter("AssessTarget", "a Wanderer sees a human in the beast form", Self, Other);
            return 0;
        }
        if (IsSlaveVsNoneBlocked(Self, Other))
        {
            LogPoliticalFilter("AssessTarget", "faction None vs a Slave target (form / party creature)", Self, Other);
            return 0;
        }
        // An OUTSIDER that has hit the army (or the hero) is in a real fight: from the first blow on the AI of the squad has
        // full native freedom (the stumble, the retreat after a thrust, the answer). None of the filters below is applied -
        // the target is never reset in the middle of a native micro-stumble. Only the duel taboo stays.
        if (!IsDuelTaboo(Other) && !IsDuelTaboo(Self) && IsInRealFight(Self, Other))
            return AssessTarget_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if (IsHeroControlled(Self))
        {
            if (IsDuelTaboo(Other))
            {
                static GEInt s_iTabooLines = 0;
                if (s_iTabooLines < 40)
                {
                    ++s_iTabooLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] AssessTarget: '%s' (party type %d) does not see the duel "
                                                 "opponent '%s'.\n",
                                                 Self.GetName().GetText(), static_cast<int>(Self.Party.PartyMemberType),
                                                 Other.GetName().GetText());
                }
                return 0;
            }
            // The hidden human body of the hero counts as the master as well.
            GEBool const bMaster = Other == Hero || IsHeroFormEntity(Other) ||
                                   _stricmp(Other.GetName().GetText(), "PC_Hero") == 0;
            // THE MASTER BLOCK: until the SECOND blow of the hero (the counter of the brawl) the creature never takes him for a
            // target, the answer is a hard 0. The second blow opens the brawl slot, and only then the native answer passes.
            if (bMaster && !IsBrawlFighting(Self))
            {
                static GEInt s_iHeroLines = 0;
                if (s_iHeroLines < 40)
                {
                    ++s_iHeroLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] AssessTarget: '%s' (controlled by the hero) does not "
                                                 "take his master '%s' for a target (transformed=%d).\n",
                                                 Self.GetName().GetText(), Other.GetName().GetText(),
                                                 static_cast<int>(IsHeroInBeastForm()));
                }
                return 0;
            }
            if (IsFriendlyFire(Self, Other))
                return 0;
            // The native rule of the army: no fight with an outsider before a REAL blow (he hurt the hero / the army, or
            // the hero / the army hurt him). A mob that has only decided to attack the hero and runs at him is left alone.
            if (ArmyMustNotAttack(Self, Other, a_iArgs))
            {
                static GEInt s_iNoLines = 0;
                if (s_iNoLines < 60)
                {
                    ++s_iNoLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] AssessTarget: '%s' (party type %d) does not attack '%s': "
                                                 "no real blow yet (attack reason %d).\n",
                                                 Self.GetName().GetText(), static_cast<int>(Self.Party.PartyMemberType),
                                                 Other.GetName().GetText(), static_cast<int>(a_iArgs));
                }
                return 0;
            }
            return AssessTarget_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
        if (IsHeroControlled(Other) && IsDuelTaboo(Self))
            return 0; // the duel opponent attacks nobody of the party of the hero
        if (IsPrimaryAggressionOnSummon(Self, Other))
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] AssessTarget: '%s' (no faction) ignores the summon '%s' of "
                                             "the hero until it hurts him first.\n",
                                             Self.GetName().GetText(), Other.GetName().GetText());
            }
            return 0;
        }
        GEInt const iResult = AssessTarget_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        // Native perception result: the outsider took a member of the army for his target = a recorded focus.
        if (Self.IsNPC() && !Self.IsDead() && !IsHeroSide(Self) && IsHeroControlled(Other) &&
            Self.NPC.GetCurrentTarget() == Other)
            NoteFocusRecord(Self.GetName(), Other.GetName());
        return iResult;
    }

    namespace
    {
        // The hero in a transformed body (the player is the spawned creature, or the body "Transform_*"): the hidden
        // human body "PC_Hero" is the entity that the native fight scripts were written for. Fills a_Body with it.
        GEBool FindHumanBodyFor(Entity &a_Attacker, Entity &a_Body)
        {
            if (a_Attacker == None || !a_Attacker.IsNPC())
                return GEFalse;
            GEBool const bForm = IsHeroFormEntity(a_Attacker) ||
                                 (a_Attacker == Entity::GetPlayer() && IsHeroTransformedNow());
            if (!bForm)
                return GEFalse;
            Entity Body = Entity::GetEntity(bCString("PC_Hero"));
            if (Body == None || Body == a_Attacker || !Body.IsNPC() || Body.IsDead())
                return GEFalse;
            a_Body = Body;
            return GETrue;
        }
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
        Entity Victim = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Source = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        Entity Attacker = GetDamageOwnerNpc(Source);
        if (Attacker == None && Victim != None && Victim.IsNPC())
            Attacker = Victim.NPC.GetCurrentAttacker(); // OnDamage may pass no damaging object
        GEInt iResult = 0;
        // THE FRIENDLY-FIRE FILTER ON THE REACTION TOO: OnDamage calls AssessHit directly (not only through DoLogicalDamage),
        // so a blocked blow (a transformed hero, a sword, an arrow, an area spell) must not stagger the creature, open the
        // brawl or be counted as a blow of the manual counter. Only a point magic projectile of the human form goes on.
        if (Victim != None && Attacker != None && Victim.IsNPC() && IsHeroSide(Attacker) && IsHeroControlled(Victim) &&
            IsFriendlyFire(Attacker, Victim, GEFalse, &Source))
        {
            ZeroPendingDamage(Victim);
            return 0;
        }
        Entity Body;
        if (Victim != None && Victim.IsNPC() && Source != None && Source == Attacker && IsHeroControlled(Victim) &&
            FindHumanBodyFor(Attacker, Body))
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] AssessHit: the blow of '%s' (%p) on '%s' is judged by the "
                                             "engine in the name of '%s' (distance between the bodies %.0f).\n",
                                             Attacker.GetName().GetText(), static_cast<void *>(Attacker.GetInstance()),
                                             Victim.GetName().GetText(), Body.GetName().GetText(),
                                             static_cast<double>(Attacker.GetDistanceTo(Body)));
            }
            Entity BodyArg = Body;
            iResult = AssessHit_Original(a_pSPU, a_pSelfEntity, &BodyArg, a_iArgs);
        }
        else
            iResult = AssessHit_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        // v1.0.35.0: AssessHit is where the native code knocks the victim out (ZS_Unconscious): a green resident who falls
        // here at the hands of the hero is the second blow at once (no 10 s window).
        if (Victim != None && Attacker != None && Victim.IsNPC() && IsHeroSide(Attacker) && !IsHeroControlled(Victim))
            OnCityKnockout(Victim, "AssessHit");
        // A blow of the hero (any form) on a creature of his army: the engine has just judged it natively. In his direct
        // focus the blow opens the native brawl (the first blow is borne, the second one starts the fight, handled in the
        // name of PC_Hero); outside the focus it is a chance blow and nothing is opened. The rest of the army stays out of it.
        if (Victim != None && Attacker != None && Victim.IsNPC() && IsHeroSide(Attacker) && IsHeroControlled(Victim))
        {
            GEBool const bFocus = GETrue; // v1.0.28.0: every blow of the hero that came through opens the native brawl
            if (bFocus)
                RecordCombat(Victim, Attacker);
            static GEInt s_iBrawlLines = 0;
            if (s_iBrawlLines < 60)
            {
                ++s_iBrawlLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] AssessHit: the hero ('%s') hit '%s' (party type %d), "
                                             "damage passed=%d -> %s (task '%s', result %d)\n",
                                             Attacker.GetName().GetText(), Victim.GetName().GetText(),
                                             static_cast<int>(Victim.Party.PartyMemberType), static_cast<int>(bFocus),
                                             bFocus ? "native brawl reaction allowed" : "nothing opened",
                                             Victim.Routine.GetCurrentTask().GetText(), static_cast<int>(iResult));
            }
        }
        if (Victim != None && Attacker != None && Victim.IsNPC() && !Victim.IsDead() && IsHeroControlled(Victim) &&
            !IsHeroSide(Attacker) && !IsHeroControlled(Attacker))
        {
            // An OUTSIDER (an orc, a monster, a bandit) hit a member of the army: v1.0.27.0 - nothing of the plugin is
            // applied to it. The engine alone judges the blow (ZS_Stumble, the answer of the victim and of the others).
            static GEInt s_iOutsiderLines = 0;
            if (s_iOutsiderLines < 60)
            {
                ++s_iOutsiderLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] AssessHit: the outsider '%s' hit '%s' (party type %d): "
                                             "native reaction, the plugin does not interfere (task '%s').\n",
                                             Attacker.GetName().GetText(), Victim.GetName().GetText(),
                                             static_cast<int>(Victim.Party.PartyMemberType),
                                             Victim.Routine.GetCurrentTask().GetText());
            }
        }
        return iResult;
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
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (Self != None && Other != None && Self.IsNPC() && !Self.IsDead() && IsHeroControlled(Self) && IsHeroSide(Other))
        {
            if (IsHeroKnockedOut())
            {
                StandDownFromMaster(Self, "AssessAgressor");
                return 1;
            }
            GEInt const iSlot = FindBrawlSlot(Self.GetName(), GEFalse);
            GEBool const bFight = iSlot >= 0 && g_arrBrawlStart[iSlot] != 0;
            if (!bFight)
            {
                // THE FIRST BLOW: the reaction of a green NPC, not the fight.
                Entity Hero = Entity::GetPlayer();
                if (iSlot >= 0)
                    g_arrBrawlReacted[iSlot] = GETrue;
                Self.NPC.SetCurrentTarget(Hero);
                Self.NPC.AttackReason = gEAttackReason_ReactToDamage;
                Self.Routine.SetTask("ZS_ReactToDamage");
                static GEInt s_iLines = 0;
                if (s_iLines < 60)
                {
                    ++s_iLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] AssessAgressor: the first blow of the hero on '%s' (party "
                                                 "type %d): ZS_ReactToDamage instead of the immediate attack.\n",
                                                 Self.GetName().GetText(), static_cast<int>(Self.Party.PartyMemberType));
                }
                return 1;
            }
        }
        GEInt const iAgressorResult = AssessAgressor_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        // v1.0.35.0: the stumble states call AssessAgressor after every blow, also the one that knocked the victim out.
        if (Self != None && Other != None && Self.IsNPC() && IsHeroSide(Other) && !IsHeroControlled(Self))
            OnCityKnockout(Self, "AssessAgressor");
        return iAgressorResult;
    }

    // ZS_Stumble: the native stumble of a creature that was hit. It takes its attacker from the creature itself
    // (GetCurrentAttacker) and the stumble animation is made relative to him. When the attacker is the transformed hero,
    // the engine gets his human body PC_Hero for the time of the call, exactly as for the standard hero.
    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Stumble)
    {
        Entity Self(a_pSPU->GetSelfEntity());
        if (Self != None && Self.IsNPC() && IsHeroControlled(Self))
        {
            Entity Attacker = Self.NPC.GetCurrentAttacker();
            Entity Body;
            if (FindHumanBodyFor(Attacker, Body))
            {
                static GEInt s_iLines = 0;
                if (s_iLines < 40)
                {
                    ++s_iLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] ZS_Stumble: '%s' stumbles from '%s' as from '%s'.\n",
                                                 Self.GetName().GetText(), Attacker.GetName().GetText(),
                                                 Body.GetName().GetText());
                }
                Self.NPC.SetCurrentAttacker(Body);
                GEBool const bResult = ZS_Stumble_Original(a_rRunTimeStack, a_pSPU);
                Self.NPC.SetCurrentAttacker(Attacker);
                return bResult;
            }
        }
        return ZS_Stumble_Original(a_rRunTimeStack, a_pSPU);
    }

    // NO LOOTING AND NO FINISHING OF THE MASTER. When the hero is down, a creature of his own party (types 1..5) does
    // not search his pockets and does not strike the final blow: WantToRansack / HasAttackReasonToKill answer "no"
    // (see the hooks below), and the two routines themselves are left at once if they were started anyway.
    namespace
    {
        GEBool IsMasterDownVictim(Entity &a_Self, Entity &a_Other)
        {
            return a_Self != None && a_Other != None && a_Self.IsNPC() && IsHeroControlled(a_Self) && IsHeroSide(a_Other);
        }

        GEBool LeaveMasterAlone(gCScriptProcessingUnit *a_pSPU, GELPCChar a_strWhat)
        {
            Entity Self(a_pSPU->GetSelfEntity());
            if (Self == None || !Self.IsNPC())
                return GEFalse;
            Entity Target(a_pSPU->GetTargetEntity());
            Entity Current = Self.NPC.GetCurrentTarget();
            if (!IsMasterDownVictim(Self, Target) && !IsMasterDownVictim(Self, Current))
                return GEFalse;
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] %s: '%s' (party type %d) does not touch his master.\n",
                                             a_strWhat, Self.GetName().GetText(),
                                             static_cast<int>(Self.Party.PartyMemberType));
            }
            Self.Routine.FullStop();
            Self.NPC.SetCurrentTarget(None);
            Self.NPC.AttackReason = gEAttackReason_None;
            Self.Routine.ContinueRoutine();
            return GETrue;
        }
    } // namespace

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_RansackBody)
    {
        if (LeaveMasterAlone(a_pSPU, "ZS_RansackBody"))
            return GETrue;
        return ZS_RansackBody_Original(a_rRunTimeStack, a_pSPU);
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Attack_FinishingAttack)
    {
        if (LeaveMasterAlone(a_pSPU, "ZS_Attack_FinishingAttack"))
            return GETrue;
        return ZS_Attack_FinishingAttack_Original(a_rRunTimeStack, a_pSPU);
    }
    // AssessDefeat(self, other): the perception of a defeated fighter (it starts the looting / finishing reaction).
    // A creature of the party does not take its own fallen master for a victim.
    ME_DEFINE_AND_HOOK_SCRIPT(AssessDefeat)
    {
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (IsMasterDownVictim(Self, Other))
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] AssessDefeat: '%s' (party type %d) ignores the fall of his master.\n",
                                             Self.GetName().GetText(), static_cast<int>(Self.Party.PartyMemberType));
            }
            return 0;
        }
        return AssessDefeat_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    ME_DEFINE_AND_HOOK_SCRIPT(IsDeadlyDamage)
    {
        GEInt const iResult = IsDeadlyDamage_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if (!iResult)
            return iResult;
        Entity Victim = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Source = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        Entity Attacker = GetDamageOwnerNpc(Source);
        // v1.0.57.0: THE IRON IMMUNITY: no blow (a one-hand thrust, a stab, a control effect) is deadly for the hero who is down or
        // only rising - whoever deals it and whatever the mercy says.
        if (Victim != None && Victim == Entity::GetPlayer() && UpdateHeroImmunity())
        {
            static GEInt s_iImmuneLines = 0;
            if (s_iImmuneLines < 40)
            {
                ++s_iImmuneLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] IsDeadlyDamage: the blow of '%s' on the hero who is down / rising is "
                                             "not deadly.\n",
                                             Attacker != None ? Attacker.GetName().GetText() : "-");
            }
            return 0;
        }
        // A creature of the party never kills the hero (a brawl inside the party ends with a knock-out, not with death).
        // FollowerMustSpare leaves the hero out on purpose, and a non-humanoid creature (a skeleton) is deadly natively.
        if (Attacker != None && Victim != None && IsHeroSide(Victim) && IsHeroControlled(Attacker))
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] IsDeadlyDamage: the blow of '%s' (party type %d) on his master "
                                             "is not deadly.\n",
                                             Attacker.GetName().GetText(), static_cast<int>(Attacker.Party.PartyMemberType));
            }
            return 0;
        }
        return FollowerMustSpare(Attacker, Victim, "IsDeadlyDamage") ? 0 : iResult;
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
        GEInt const iResult = GetAttitudeToPlayer_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if (iResult != gEAttitude_Angry && iResult != gEAttitude_Hostile && iResult != gEAttitude_Predator &&
            iResult != gEAttitude_Panic)
            return iResult;
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (Self == None || !Self.IsNPC())
            return iResult;
        if (IsHeroControlled(Self) && !IsBrawlFighting(Self))
            return gEAttitude_Friendly;
        // v1.0.36.0: the target of this query is the player (the entity of the form while the hero is transformed).
        {
            Entity PlayerTarget = Entity::GetPlayer();
            if (IsWandererImmune(Self, PlayerTarget))
            {
                LogPoliticalFilter("GetAttitudeToPlayer", "a Wanderer sees a human in the beast form", Self, PlayerTarget);
                return gEAttitude_Neutral;
            }
            if (IsSlaveVsNoneBlocked(Self, PlayerTarget))
            {
                LogPoliticalFilter("GetAttitudeToPlayer", "faction None vs a Slave target (form)", Self, PlayerTarget);
                return gEAttitude_Neutral;
            }
        }
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (Other != None && IsPrimaryAggressionOnSummon(Self, Other))
            return gEAttitude_Neutral;
        return iResult;
    }

    // GetAttitude(self, other): who is friend / enemy of whom.
    //  - Inside the army, and towards the master in ANY form of transformation: Friendly (always, not only when the
    //    native answer was aggressive - the name of a companion stays blue).
    //  - A creature of the army takes an enemy who attacks one of the army (see DefendAlly) for Hostile.
    //  - Faction None never takes a summon of the hero for an enemy before the summon hurts it first.
    ME_DEFINE_AND_HOOK_SCRIPT(GetAttitude)
    {
        PollFleeWatch(); // diagnostics only
        GEInt const iResult = GetAttitude_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (Self == None || Other == None || !Self.IsNPC() || Self == Other)
            return iResult;
        GEBool const bAggressive = iResult == gEAttitude_Angry || iResult == gEAttitude_Hostile ||
                                   iResult == gEAttitude_Predator;
        // v1.0.36.0: rule 6 (the Wanderers do not take the hero in a beast form for an enemy) and rule 5 (faction None vs a
        // Slave target). Only an aggressive native answer is replaced, by Neutral.
        // v1.0.38.0: the Wanderers (gEPoliticalAlignment_Mid) also never PANIC at the hero in a form.
        if ((bAggressive || iResult == gEAttitude_Panic) && IsWandererImmune(Self, Other))
        {
            LogPoliticalFilter("GetAttitude", "a Wanderer (Mid) sees a human in the beast form", Self, Other);
            return gEAttitude_Neutral;
        }
        if (bAggressive)
        {
            if (IsSlaveVsNoneBlocked(Self, Other))
            {
                LogPoliticalFilter("GetAttitude", "faction None vs a Slave target (form / party creature)", Self, Other);
                return gEAttitude_Neutral;
            }
        }
        // The fight has started: an outsider is in a real fight with the hero / the army (he hit one of them, or one of them
        // hit him) - the plugin touches nothing, the native attitude and the native answer of the squad decide.
        if (!IsDuelTaboo(Other) && !IsDuelTaboo(Self) && IsInRealFight(Self, Other))
            return iResult;
        if (!IsHeroControlled(Self))
        {
            if (!bAggressive || (!IsPrimaryAggressionOnSummon(Self, Other) && !(IsHeroControlled(Other) && IsDuelTaboo(Self))))
                return iResult;
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] GetAttitude: '%s' (no faction) ignores the summon '%s' of "
                                             "the hero until it hurts him first (was %d).\n",
                                             Self.GetName().GetText(), Other.GetName().GetText(),
                                             static_cast<int>(iResult));
            }
            return gEAttitude_Neutral;
        }
        Entity Hero = Entity::GetPlayer();
        GEBool const bOtherIsHero = Other == Hero || IsHeroFormEntity(Other);
        if (bAggressive && IsDuelTaboo(Other))
            return gEAttitude_Neutral; // the opponent of the duel is invisible for the army
        if (bOtherIsHero && IsBrawlFighting(Self))
            return iResult; // the second blow opened the brawl: the native attitude to the hero is not overridden
        if (!bOtherIsHero && !IsHeroControlled(Other) && IsDefendedTarget(Other.GetName()))
            return gEAttitude_Hostile; // somebody attacks a member of the army: the army sees an enemy in him
        if (iResult == gEAttitude_Friendly)
            return iResult;
        if (bOtherIsHero || IsHeroControlled(Other))
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix] GetAttitude: '%s' (controlled by the hero) stays friendly "
                                             "to '%s' (was %d).\n",
                                             Self.GetName().GetText(), Other.GetName().GetText(),
                                             static_cast<int>(iResult));
            }
            return gEAttitude_Friendly;
        }
        return iResult;
    }

    // The colour of the name under the cursor. A creature of the army is blue (Friendly) in any form of the hero: the
    // native script compares the party leader with the player, which is the transformed body, not PC_Hero.
    ME_DEFINE_AND_HOOK_SCRIPT(GetFocusNameAttitude)
    {
        GEInt const iResult = GetFocusNameAttitude_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (Self != None && Self.IsNPC() && IsHeroControlled(Self))
            return gEAttitude_Friendly;
        return iResult;
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
        GEBool IsCityBrawlVictimAlive()
        {
            if (g_strCityVictim.IsEmpty())
                return GEFalse;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc.GetName() == g_strCityVictim)
                    return Npc.IsDead() ? GEFalse : GETrue;
            }
            return GEFalse; // unknown: nothing is changed
        }
    } // namespace

    ME_DEFINE_AND_HOOK_SCRIPT(StartSayAttackReason)
    {
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        gEAttackReason enuOriginal = gEAttackReason_None;
        GEBool bSwapped = GEFalse;
        if (g_enuCityPhase == mECityPhase_Aggro && Self != None && Self.IsNPC() && !(Self == Entity::GetPlayer()))
        {
            enuOriginal = Self.NPC.AttackReason;
            if ((enuOriginal == gEAttackReason_Murder || enuOriginal == gEAttackReason_Enemy) &&
                FindAggro(Self.GetName()) >= 0 && IsCityBrawlVictimAlive())
            {
                Self.NPC.AttackReason = gEAttackReason_StopFight;
                bSwapped = GETrue;
                static GEInt s_iLines = 0;
                if (s_iLines < 40)
                {
                    ++s_iLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] '%s' shouts about the brawl, not about a murder (attack "
                                                 "reason %d -> %d for the voice line); nobody died.\n",
                                                 Self.GetName().GetText(), static_cast<int>(enuOriginal),
                                                 static_cast<int>(gEAttackReason_StopFight));
                }
            }
        }
        GEInt const iResult = StartSayAttackReason_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if (bSwapped)
            Self.NPC.AttackReason = enuOriginal;
        return iResult;
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] ZS_ReactToDamage: the code differs (opcode %02X): the line of the "
                                             "stance is left as it is.\n",
                                             pPush[0]);
                return;
            }
            GEU8 *const *ppOperand = reinterpret_cast<GEU8 *const *>(pPush + 1);
            GEU8 const *pString = *ppOperand;
            GEU8 const *pEnd = pBase + 0x139000; // the image of the module (SizeOfImage 0x139000)
            if (pString < pBase || pString + 16 > pEnd || strcmp(reinterpret_cast<char const *>(pString), "WhatAreYouDoing") != 0)
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] ZS_ReactToDamage: the operand does not point at \"WhatAreYouDoing\": "
                                             "the line of the stance is left as it is.\n");
                return;
            }
            DWORD dwOld = 0;
            if (!::VirtualProtect(pPush + 1, 4, PAGE_EXECUTE_READWRITE, &dwOld))
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] ZS_ReactToDamage: cannot unlock the code for the line patch.\n");
                return;
            }
            *reinterpret_cast<GEU8 const **>(pPush + 1) = reinterpret_cast<GEU8 const *>(g_arrReactSvm);
            DWORD dwDummy = 0;
            ::VirtualProtect(pPush + 1, 4, dwOld, &dwDummy);
            ::FlushInstructionCache(::GetCurrentProcess(), pPush + 1, 4);
            g_bReactSvmPatched = GETrue;
            GetDefaultLogger().LogFormat("[GenomeLogicFix] ZS_ReactToDamage: the line of the stance is replaceable (patched).\n");
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, stage 1: the hero is in the magic mode, but the "
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, stage 1: the hero holds a forbidden weapon (%s) - the opponent '%s' "
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena: the forbidden weapon is put away - the opponent '%s' %sgoes back "
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, transformation (%s): the duel is cancelled (%s), the quest is open "
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
            if (bInRing && g_bSpareArmed)
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, stage 2: %s - the crime is declared, the duel quest is closed.\n",
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, stage 2: the opponent '%s' stays in the fight (ZS_Attack against the "
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

        // v1.0.61.0: the hero fell (stage 2): the opponent stops the fight and goes away, and says the line over the hero that lies on the
        // sand (the voice and the silence of a lying opponent are decided by ScheduleOverLine / SayArenaLine; no text on the screen).
        void ReleaseStage2Hold(GELPCChar a_strWhy)
        {
            if (g_strStage2Hold.IsEmpty())
                return;
            bCString const strName = g_strStage2Hold;
            g_strStage2Hold = bCString();
            Entity Opponent = Entity::GetEntity(strName);
            if (Opponent == None || !Opponent.IsNPC() || Opponent.IsDead())
                return;
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, stage 2: the hero is down (%s) - the opponent '%s' stops the fight "
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
            if (g_strStage2Hold.IsEmpty())
                return;
            GEU32 const u32Now = ::GetTickCount();
            if (g_u32Stage2HoldTick != 0 && static_cast<GEInt>(u32Now - g_u32Stage2HoldTick) < 400)
                return;
            g_u32Stage2HoldTick = u32Now | 1;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None || !g_bSpareArmed)
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, stage 2: the opponent '%s' is put back into the attack on the "
                                             "hero (the engine took him out of it).\n",
                                             Opponent.GetName().GetText());
            }
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, transformation: the opponent '%s' and the crowd (%d) stay in the fight - "
                                         "they are aimed at the body of the form when it appears.\n",
                                         g_strFormOpponent.IsEmpty() ? "-" : g_strFormOpponent.GetText(),
                                         static_cast<int>(g_iAggroCount));
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
            if (Hero == None || !g_bSpareArmed)
            {
                ResetFormAggro();
                return;
            }
            if (!IsHeroTransformedNow())
            {
                if (g_bFormSeen)
                {
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, transformation: the form is gone (the hero is a man again) - the "
                                                 "pursuit of the form is over.\n");
                    ResetFormAggro();
                }
                else if (static_cast<GEInt>(u32Now - g_u32FormPendingSince) >= static_cast<GEInt>(FORM_WAIT_MS))
                {
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, transformation: no form appeared (the cast was interrupted).\n");
                    ResetFormAggro();
                }
                return;
            }
            g_bFormSeen = GETrue;
            if (Hero.IsDead())
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, transformation: the body of the form is dead - the pursuit is over.\n");
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
                if (!bOpponent && FindAggro(strName) < 0)
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena, transformation: %d NPC(s) of the crowd / the opponent are ordered to "
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Arena: %s - the duel is cancelled, the opponent leaves the ring.\n",
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
        GEInt const TELEPORT_FOLLOWERS_MAX = 16;
        GEFloat const TELEPORT_FOLLOWER_MIN_DISTANCE = 300.0f; // beside the hero already: nothing to move (a failed cast)
        GEFloat const TELEPORT_HERO_MIN_MOVE = 1000.0f;        // the hero really went somewhere (a stone, not a step)
        GEU32 const TELEPORT_PENDING_MS = 4000;                // a technical limit of the late move below (not a game timing)
        bCString g_arrTeleportFollowers[TELEPORT_FOLLOWERS_MAX];
        GEInt g_iTeleportFollowers = 0;
        bCVector g_vTeleportHeroBefore;
        GEBool g_bTeleportPending = GEFalse; // the native script returned, the hero has not changed the place yet
        GEU32 g_u32TeleportPendingTick = 0;
        GEBool g_bTeleportScriptRuns = GEFalse; // inside the native MagicTeleport: the script WaitPlayerParty may be asked

        // v1.0.50.0: the companion was collected for the move (the native script must not put him to wait nor print the plaque).
        GEBool IsTeleportFollower(Entity &a_Npc)
        {
            if (a_Npc == None)
                return GEFalse;
            bCString const strName = a_Npc.GetName();
            for (GEInt i = 0; i < g_iTeleportFollowers; ++i)
                if (g_arrTeleportFollowers[i] == strName)
                    return GETrue;
            return GEFalse;
        }

        // The companion was NOT carried to the hero (the move failed / the hero did not leave the place): what the native script
        // would have done is done now - he waits where he is, and the plaque "<name> is waiting" is shown.
        void RestoreNativeWait(Entity &a_Npc)
        {
            a_Npc.Party.Waiting = GETrue;
            a_Npc.Routine.FullStop();
            a_Npc.Routine.SetTask("ZS_WaitForPlayer");
            PrintCompanionWaitMessage(a_Npc);
        }

        GEFloat DistanceBetween(bCVector const &a_vFirst, bCVector const &a_vSecond)
        {
            GEFloat const fDx = a_vFirst.m_fX - a_vSecond.m_fX;
            GEFloat const fDy = a_vFirst.m_fY - a_vSecond.m_fY;
            GEFloat const fDz = a_vFirst.m_fZ - a_vSecond.m_fZ;
            return std::sqrt(fDx * fDx + fDy * fDy + fDz * fDz);
        }

        // v1.0.68.0: ONE LINE PER COMPANION PER TELEPORT STONE: the name, the party type, the AI task, Waiting, the distance to the hero, the
        // WaiterType and the exact reason why he was carried / skipped / stopped. The line goes to the plugin log and to the ring of the
        // crash report (CrashDiag::Note), so a crash right after a stone shows who was doing what.
        void LogTeleportRecord(GELPCChar a_strStage, Entity &a_Npc, Entity &a_Hero, GELPCChar a_strDecision, GELPCChar a_strReason)
        {
            bCString const strTask = a_Npc.Routine.GetCurrentTask();
            gEPartyMemberType const enuType = a_Npc.Party.PartyMemberType;
            char strLine[420] = {};
            _snprintf_s(strLine, sizeof(strLine), _TRUNCATE,
                        "[GenomeLogicFix][teleport] %s | '%s' | party type %d | task '%s' | Waiting=%d | distance %.0f | WaiterType=%s | %s: %s",
                        a_strStage, a_Npc.GetName().GetText(), static_cast<int>(enuType), strTask.GetText() ? strTask.GetText() : "?",
                        static_cast<int>(a_Npc.Party.Waiting ? 1 : 0),
                        a_Hero != None ? static_cast<double>(a_Npc.GetDistanceTo(a_Hero)) : -1.0, WaiterTypeText(GetWaiterType(a_Npc)),
                        a_strDecision, a_strReason);
            GetDefaultLogger().LogFormat("%s\n", strLine);
            CrashDiag::Note(strLine);
        }

        GEInt const TELEPORT_SEEN_MAX = 48;
        bCString g_arrTeleportSeen[TELEPORT_SEEN_MAX];
        GEInt g_iTeleportSeen = 0;

        // v1.0.69.0: ONE candidate for the teleport. a_bPartyMember: he stands in the member list of the party of the hero - the very list the
        // native script MagicTeleport walks through (WaitPlayerParty is run for every member of the types 1..3). Such a member is a
        // companion even when the check "the leader of his own party is the hero" fails (IsHeroControlled): that was the reason why
        // companions were put to wait and left behind - they were not collected, but the native script found them all the same.
        // Every skip is written down, none is silent any more.
        void ConsiderTeleportCandidate(Entity &Npc, Entity &Hero, GEBool a_bPartyMember)
        {
            if (Npc == None || Npc == Hero || !Npc.IsNPC())
                return;
            gEPartyMemberType const enuType = Npc.Party.PartyMemberType;
            GEBool const bPermanent = enuType == gEPartyMemberType_Party || enuType == gEPartyMemberType_Mob ||
                                      enuType == gEPartyMemberType_Slave;
            GEBool const bControlled = IsHeroControlled(Npc);
            if (!bControlled && !(a_bPartyMember && bPermanent))
                return; // not a companion of the hero at all: not recorded
            if (g_iTeleportFollowers >= TELEPORT_FOLLOWERS_MAX)
            {
                LogTeleportRecord("collect", Npc, Hero, "SKIPPED", "the list of the carried companions is full");
                return;
            }
            if (Npc.IsDead())
            {
                LogTeleportRecord("collect", Npc, Hero, "SKIPPED", "he is dead");
                return;
            }
            // v1.0.69.0: a companion who is knocked down is NOT dropped any more (before, he was skipped in silence here, the native script
            // put him to wait, and he stayed behind). He is carried too; only his position is changed, his AI is left as it is.
            {
                // each companion is judged (and written down) once per stone, whichever list shows him first
                bCString const strSeen = Npc.GetName();
                for (GEInt i = 0; i < g_iTeleportSeen; ++i)
                    if (g_arrTeleportSeen[i] == strSeen)
                        return;
                if (g_iTeleportSeen < TELEPORT_SEEN_MAX)
                    g_arrTeleportSeen[g_iTeleportSeen++] = strSeen;
            }
            if (!bControlled)
                LogTeleportRecord("collect", Npc, Hero, "NOTE",
                                  "a member of the party of the hero, but the leader of his own party is NOT the hero: taken by the member list");
            {
                if (!bPermanent)
                {
                    LogTeleportRecord("collect", Npc, Hero, "SKIPPED", "party type 4 / 5 (a tamed / summoned creature): left to the native script");
                    return;
                }
                // v1.0.51.0: THE BLACK LIST (IgnoreTeleportNames of the INI), checked FIRST, by the native name. Such a companion
                // is never collected, so he is never carried: the native script WaitPlayerParty runs for him as in the pure game
                // (he stays at the old place, "waits", the plaque is shown), whether he followed the hero or already waited.
                if (IsTeleportNameIgnored(Npc.GetName().GetText()))
                {
                    LogTeleportRecord("collect", Npc, Hero, "SKIPPED", "the name is in IgnoreTeleportNames: stays and waits natively");
                    return;
                }
                // v1.0.68.0: THE WAITER TYPE decides, the native flag Waiting alone does not. A wait that the PLAYER ordered (USER, or an
                // old wait that no mark describes) is never broken: the companion stays. A wait that the plugin / the game ordered (AUTO:
                // the start of an arena duel, a failed carry, a stone that put him to wait) is ignored: he is carried like a follower.
                if (Npc.Party.Waiting)
                {
                    mEWaiterType const enuWait = GetWaiterType(Npc);
                    if (enuWait != mEWaiterType_Auto)
                    {
                        LogTeleportRecord("collect", Npc, Hero, "SKIPPED",
                                          enuWait == mEWaiterType_User ? "Waiting by the order of the PLAYER (USER): he stays"
                                                                       : "Waiting with no mark (an older order of the player): he stays");
                        return;
                    }
                    g_arrTeleportFollowers[g_iTeleportFollowers++] = Npc.GetName();
                    LogTeleportRecord("collect", Npc, Hero, "CARRIED",
                                      "Waiting by an AUTO order (the plugin / the game): the native flag is ignored, he is carried");
                    return;
                }
                ClearWaiterMarks(Npc); // he follows: an old mark of a wait that is over is dropped
                g_arrTeleportFollowers[g_iTeleportFollowers++] = Npc.GetName();
                LogTeleportRecord("collect", Npc, Hero, "CARRIED", "follows the hero (Waiting=0)");
            }
        }

        // BEFORE the native MagicTeleport: the companions (types 1..3) that follow the hero, and the place of the hero.
        // v1.0.69.0: two sources. (1) every NPC of the world whose party leader is the hero (the old way); (2) the member list of the party
        // of the hero - the list the native script itself walks. A companion that is in (2) but not in (1) was silently dropped before, put to
        // wait by the native script and left behind (the log showed "native wait" with no "collect" line at all).
        void CollectTeleportFollowers()
        {
            g_iTeleportFollowers = 0;
            g_iTeleportSeen = 0;
            g_bTeleportPending = GEFalse;
            Entity Hero = Entity::GetPlayer();
            g_vTeleportHeroBefore = Hero.GetPosition();
            // v1.0.68.0: a teleport ends the arena session of the plugin: the hero is leaving the arena; whoever waited by the order of
            // the session (AUTO) is carried below like any other AUTO waiter.
            if (g_bArenaSessionHold)
                EndArenaSession("a teleport", GEFalse, 0);
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                ConsiderTeleportCandidate(Npc, Hero, GEFalse);
            }
            GEInt iMembers = 0;
            {
                bTValArray<eCEntity *> arrMembers = Hero.Party.GetMembers(GEFalse);
                for (GEInt i = 0; i < arrMembers.GetCount(); ++i)
                {
                    eCEntity *pMember = arrMembers.GetAt(i);
                    if (!pMember)
                        continue;
                    ++iMembers;
                    Entity Member(static_cast<eCEntity const *>(pMember));
                    ConsiderTeleportCandidate(Member, Hero, GETrue);
                }
            }
            char strLine[240] = {};
            _snprintf_s(strLine, sizeof(strLine), _TRUNCATE,
                        "[GenomeLogicFix][teleport] stone: %d NPC(s) in the world, %d member(s) in the party of the hero, %d companion(s) carried",
                        static_cast<int>(arrNPCs.GetCount()), static_cast<int>(iMembers), static_cast<int>(g_iTeleportFollowers));
            GetDefaultLogger().LogFormat("%s\n", strLine);
            CrashDiag::Note(strLine);
        }

        // v1.0.69.0: THE NET. The native script asks WaitPlayerParty for a member of the party that the collection did not take (for whatever
        // reason). Instead of letting the script leave him behind waiting, he is collected NOW - the hero has not moved yet, so the carry
        // after the cast takes him along. Never for a wait of the player, never for the black list, never for the types 4 / 5.
        GEBool TryLateCollect(Entity &a_Member)
        {
            if (a_Member == None || !a_Member.IsNPC() || a_Member.IsDead() || g_iTeleportFollowers >= TELEPORT_FOLLOWERS_MAX)
                return GEFalse;
            gEPartyMemberType const enuType = a_Member.Party.PartyMemberType;
            if (enuType != gEPartyMemberType_Party && enuType != gEPartyMemberType_Mob && enuType != gEPartyMemberType_Slave)
                return GEFalse;
            if (IsTeleportNameIgnored(a_Member.GetName().GetText()))
                return GEFalse;
            if (a_Member.Party.Waiting && GetWaiterType(a_Member) != mEWaiterType_Auto)
                return GEFalse; // the player's wait stays
            Entity Hero = Entity::GetPlayer();
            g_arrTeleportFollowers[g_iTeleportFollowers++] = a_Member.GetName();
            LogTeleportRecord("late collect", a_Member, Hero, "CARRIED",
                              "the native script asked for him but the collection missed him: taken now, no wait, no plaque");
            return GETrue;
        }

        // v1.0.68.0: THE SAFEGUARD AGAINST THE RUN ACROSS THE MAP. After a teleport a companion of the types 1..3 who is farther than
        // FAR_COMPANION_DISTANCE from the hero (he stayed in another city) and whose wait is not the player's gets the order to stay where he
        // is (FullStop + ZS_WaitForPlayer, AUTO, no plaque): the native follow would send him across the whole map. A USER wait is never
        // touched. A companion who already waits (AUTO) is left as he is. The next teleport stone carries every AUTO waiter to the hero.
        void SecureFarCompanions(GELPCChar a_strWhy)
        {
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc == None || Npc == Hero || !Npc.IsNPC() || Npc.IsDead() || !IsHeroControlled(Npc))
                    continue;
                gEPartyMemberType const enuType = Npc.Party.PartyMemberType;
                if (enuType != gEPartyMemberType_Party && enuType != gEPartyMemberType_Mob && enuType != gEPartyMemberType_Slave)
                    continue;
                if (Npc.GetDistanceTo(Hero) <= FAR_COMPANION_DISTANCE)
                    continue;
                mEWaiterType const enuWait = GetWaiterType(Npc);
                if (Npc.Party.Waiting)
                {
                    LogTeleportRecord(a_strWhy, Npc, Hero, "FAR, LEFT",
                                      enuWait == mEWaiterType_Auto ? "already waits by an AUTO order: stays where he is, no run"
                                                                   : "waits by the order of the player: never touched");
                    continue;
                }
                Npc.Routine.FullStop();
                Npc.Party.Waiting = GETrue;
                Npc.Routine.SetTask("ZS_WaitForPlayer");
                MarkWaiterAuto(Npc);
                LogTeleportRecord(a_strWhy, Npc, Hero, "FAR, STOPPED",
                                  "more than 10000 away and not carried: FullStop + wait (AUTO) so he does not run across the map");
            }
        }

        // AFTER the native MagicTeleport: the hero stands at the new place; the collected companions are put to him by the info
        // command MoveTo and go on following him.
        void MoveTeleportFollowers(GELPCChar a_strWhy)
        {
            Entity Hero = Entity::GetPlayer();
            GEInt iMoved = 0;
            for (GEInt i = 0; i < g_iTeleportFollowers; ++i)
            {
                Entity Npc = Entity::GetEntity(g_arrTeleportFollowers[i]);
                if (Hero == None)
                    continue;
                if (Npc == None || !Npc.IsNPC() || Npc.IsDead())
                {
                    char strGone[200] = {};
                    _snprintf_s(strGone, sizeof(strGone), _TRUNCATE, "[GenomeLogicFix][teleport] moved | '%s' | SKIPPED: the entity is gone or dead",
                                g_arrTeleportFollowers[i].GetText());
                    GetDefaultLogger().LogFormat("%s\n", strGone);
                    CrashDiag::Note(strGone);
                    continue;
                }
                gEPartyMemberType const enuType = Npc.Party.PartyMemberType;
                if (enuType == gEPartyMemberType_None || enuType == gEPartyMemberType_Controlled ||
                    enuType == gEPartyMemberType_Summoned)
                {
                    LogTeleportRecord("moved", Npc, Hero, "SKIPPED", "left the party meanwhile, or is not a permanent companion");
                    continue;
                }
                if (Npc.IsDown())
                {
                    // knocked down: only the place is changed, his AI (the knock-down task) is not touched
                    GEFloat const fDownBefore = Npc.GetDistanceTo(Hero);
                    if (fDownBefore > TELEPORT_FOLLOWER_MIN_DISTANCE)
                    {
                        Npc.MoveTo(Hero);
                        if (Npc.GetDistanceTo(Hero) > TELEPORT_FOLLOWER_MIN_DISTANCE)
                            Npc.Teleport(Hero);
                        ++iMoved;
                    }
                    char strDown[220] = {};
                    _snprintf_s(strDown, sizeof(strDown), _TRUNCATE,
                                "[GenomeLogicFix][teleport] moved | '%s' | knocked down | distance %.0f -> %.0f | CARRIED: only the place is changed",
                                Npc.GetName().GetText(), static_cast<double>(fDownBefore), static_cast<double>(Npc.GetDistanceTo(Hero)));
                    GetDefaultLogger().LogFormat("%s\n", strDown);
                    CrashDiag::Note(strDown);
                    continue;
                }
                GEFloat const fBefore = Npc.GetDistanceTo(Hero);
                GEBool bCarried = GETrue; // beside the hero already counts as carried
                if (fBefore > TELEPORT_FOLLOWER_MIN_DISTANCE)
                {
                    Npc.MoveTo(Hero); // the info command "MoveTo": silent, instant
                    GEFloat fAfter = Npc.GetDistanceTo(Hero);
                    if (fAfter > TELEPORT_FOLLOWER_MIN_DISTANCE)
                    {
                        Npc.Teleport(Hero); // the second way of the game to put a character to another one
                        fAfter = Npc.GetDistanceTo(Hero);
                    }
                    bCarried = fAfter <= TELEPORT_FOLLOWER_MIN_DISTANCE;
                    if (bCarried)
                        ++iMoved;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] Teleport (%s): the companion '%s' (party type %d) follows the "
                                                 "hero: distance %.0f -> %.0f (%s).\n",
                                                 a_strWhy, Npc.GetName().GetText(), static_cast<int>(enuType),
                                                 static_cast<double>(fBefore), static_cast<double>(fAfter),
                                                 bCarried ? "carried, no plaque" : "NOT carried, he waits");
                }
                if (bCarried)
                {
                    // The native script was not allowed to put him to wait (WaitPlayerParty is held back): he just goes on following
                    // the hero, with no plaque on the screen.
                    // v1.0.68.0: an AUTO waiter (the arena order, a failed carry) is carried too: the wait is over, no mark is left.
                    ClearWaiterMarks(Npc);
                    Npc.Party.Waiting = GEFalse;
                    Npc.Routine.FullStop();
                    Npc.Routine.SetTask("ZS_FollowPlayer");
                    LogTeleportRecord("moved", Npc, Hero, "CARRIED", "he stands at the hero and follows him (ZS_FollowPlayer)");
                }
                else
                {
                    RestoreNativeWait(Npc); // not carried: the native behaviour and the plaque "<name> is waiting"
                    MarkWaiterAuto(Npc);    // v1.0.68.0: this wait is the plugin's, not the player's: the next stone carries him
                    LogTeleportRecord("moved", Npc, Hero, "NOT CARRIED", "the move failed: he waits where he is (AUTO), the next stone carries him");
                }
            }
            if (iMoved == 0)
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Teleport (%s): nobody had to be moved.\n", a_strWhy);
            SecureFarCompanions(a_strWhy);
            g_iTeleportFollowers = 0;
            g_bTeleportPending = GEFalse;
            for (GEInt i = 0; i < TELEPORT_FOLLOWERS_MAX; ++i)
                g_arrTeleportFollowers[i] = bCString();
        }

        // AFTER the native MagicTeleport script: when the hero is already at the new place the followers are moved at once; when
        // his coordinates have not changed yet (the move can be finished by the engine a moment later), the followers stay
        // collected and PollTeleportFollowers (every tick of every NPC) moves them in the frame the hero has moved.
        void FinishTeleportFollowers(GELPCChar a_strWhy)
        {
            GEFloat const fMoved = DistanceBetween(Entity::GetPlayer().GetPosition(), g_vTeleportHeroBefore);
            if (g_iTeleportFollowers == 0)
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Teleport (%s): nobody is carried (see the records above, one per companion).\n",
                                             a_strWhy);
                if (fMoved >= TELEPORT_HERO_MIN_MOVE)
                    SecureFarCompanions(a_strWhy); // v1.0.68.0: nobody is carried, but a far companion must not run across the map
                return;
            }
            if (fMoved >= TELEPORT_HERO_MIN_MOVE)
            {
                MoveTeleportFollowers(a_strWhy);
                return;
            }
            g_bTeleportPending = GETrue;
            g_u32TeleportPendingTick = ::GetTickCount() | 1;
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Teleport (%s): the hero has moved only %.0f so far - the companions wait "
                                         "for his arrival.\n",
                                         a_strWhy, static_cast<double>(fMoved));
        }

        void PollTeleportFollowers()
        {
            if (!g_bTeleportPending)
                return;
            if (static_cast<GEInt>(::GetTickCount() - g_u32TeleportPendingTick) > static_cast<GEInt>(TELEPORT_PENDING_MS))
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Teleport: the hero did not leave the place, the companions stay "
                                             "and wait (the native plaque).\n");
                g_bTeleportPending = GEFalse;
                for (GEInt i = 0; i < g_iTeleportFollowers; ++i)
                {
                    Entity Npc = Entity::GetEntity(g_arrTeleportFollowers[i]);
                    if (Npc != None && Npc.IsNPC() && !Npc.IsDead() && !Npc.IsDown())
                    {
                        RestoreNativeWait(Npc);
                        MarkWaiterAuto(Npc); // v1.0.68.0: the plugin's wait, the next stone carries him
                        Entity TimeoutHero = Entity::GetPlayer();
                        LogTeleportRecord("timeout", Npc, TimeoutHero, "NOT CARRIED", "the hero did not leave the place: he waits (AUTO)");
                    }
                }
                g_iTeleportFollowers = 0;
                return;
            }
            if (DistanceBetween(Entity::GetPlayer().GetPosition(), g_vTeleportHeroBefore) >= TELEPORT_HERO_MIN_MOVE)
                MoveTeleportFollowers("the hero has arrived");
        }
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

    // v1.0.40.0: THE STATES OF THE HERO (RequirementsCheck hooks SetCurrentTask / AISetTask / SetCurrentState / AISetState of the
    // routine of the hero and reports every change here, the names are the ones of the log: PS_Magic, PS_Magic_Reload,
    // PS_Magic_Loop, PS_Magic_QuickCast, ...).
    //  - the task PS_Magic (a rune / a scroll taken in the hand) with the Running arena quest and the hero in the ring ->
    //    stage 1 (the opponent takes the stance of the first blow and warns once);
    //  - the task leaves PS_Magic (PS_Normal / PS_Melee / ...) -> the opponent goes back to the melee duel;
    //  - the substate of the cast itself (PS_Magic_QuickCast, PS_Magic_Cast, PS_Magic_PowerCast, PS_Magic_Summon) -> stage 2.
    //  A health potion is PS_MagicPotion / PS_QuickUse, not PS_Magic: it is ignored here.
    void OnHeroRoutineChange(GELPCChar a_strName, GEBool a_bIsTask)
    {
        if (!a_strName || !a_strName[0])
            return;
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
    ME_DEFINE_AND_HOOK_SCRIPT(MagicTeleport)
    {
        Entity Caster = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (Caster == None || !IsHeroSide(Caster))
            return MagicTeleport_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);

        // Before the native script: who of the types 1..3 follows the hero (the native script puts them to wait and prints
        // "<name> is waiting"). The types 4 / 5 are left to the native script completely (v1.0.50.0: no tracker, no messages).
        CollectTeleportFollowers();
        g_bTeleportScriptRuns = GETrue; // the plaque "<name> is waiting" of a collected companion is held back meanwhile
        GEInt const iResult = MagicTeleport_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        g_bTeleportScriptRuns = GEFalse;
        FinishTeleportFollowers("MagicTeleport");
        return iResult;
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
        if (g_bTeleportScriptRuns)
        {
            Entity Member = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
            if (Member != None && IsTeleportFollower(Member))
                return 1; // he is carried to the hero: neither the plaque nor the waiting
            if (Member != None && Member != Entity::GetPlayer() && TryLateCollect(Member))
                return 1; // v1.0.69.0: the net - the collection missed him, he is carried all the same
            // v1.0.68.0: the native script puts the rest of the party to wait during the cast: that wait is the GAME's (AUTO), not the
            // player's - unless he already waited before the cast (the order of the player, kept as it is).
            if (Member != None && !Member.Party.Waiting)
            {
                GEInt const iResult = WaitPlayerParty_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
                if (Member.Party.Waiting)
                {
                    MarkWaiterAutoPublic(Member);
                    Entity NativeWaitHero = Entity::GetPlayer();
                    LogTeleportRecord("native wait", Member, NativeWaitHero, "AUTO",
                                      "the native script WaitPlayerParty put him to wait during the cast; the next stone carries him");
                }
                return iResult;
            }
        }
        return WaitPlayerParty_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    // The native OnTick of every NPC runs unchanged (the distance, the dismissal of the summons, the return home of the
    // tamed): the plugin does not look at the creatures under control any more.
    ME_DEFINE_AND_HOOK_SCRIPT(OnTick)
    {
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (Self == None || !Self.IsNPC() || Self == Entity::GetPlayer())
            return OnTick_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);

        PollTeleportFollowers(); // v1.0.49.0: the late move of the companions after a teleport (one flag read when idle)
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

        return OnTick_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    ME_DEFINE_AND_HOOK_SCRIPT(HasAttackReasonToKill)
    {
        GEInt const iResult = HasAttackReasonToKill_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if (!iResult)
            return iResult;

        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        Entity Hero = Entity::GetPlayer();
        // The creatures of the party never strike the final blow at their fallen master.
        if (IsMasterDownVictim(Self, Other))
            return 0;
        // A summon / companion of the hero does not finish off an NPC who is not red for the hero.
        if (FollowerMustSpare(Self, Other, "HasAttackReasonToKill"))
            return 0;
        if (Self == None || Self == Hero || !(Other == Hero) || !Self.IsNPC())
            return iResult;

        // v1.0.57.0: THE IRON IMMUNITY. The hero is down, in the animation of the fall or only rising: the answer is NO for
        // everybody, wherever the NPC stands and whatever the mercy / the arena / the attack reason are. It is lifted only by
        // UpdateHeroImmunity (the hero is up and in the routine PS_Normal_Loop / PS_Melee_Loop ...).
        if (UpdateHeroImmunity())
        {
            static GEInt s_iImmuneLines = 0;
            if (s_iImmuneLines < 60)
            {
                ++s_iImmuneLines;
                GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] '%s' asks whether to kill the hero: attack reason=%d, the hero "
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
            GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] '%s' asks whether to kill the hero: attack reason=%d, "
                                         "mercy armed=%d, distance from the fall point=%.0f, arena fight=%d -> %s\n",
                                         Self.GetName().GetText(), static_cast<int>(Self.NPC.AttackReason),
                                         static_cast<int>(g_bSpareArmed), static_cast<double>(fDistance),
                                         static_cast<int>(bArenaFight),
                                         (bMercy || bArenaFight) ? "the plugin: NO" : "the plugin: leaves yes");
        }
        if (!bMercy && !bArenaFight)
            return iResult;

        GetDefaultLogger().LogFormat("[GenomeLogicFix] '%s' does not finish the hero off (attack reason %d): he takes "
                                     "the weapon and the gold instead.\n",
                                     Self.GetName().GetText(), static_cast<int>(Self.NPC.AttackReason));
        return 0;
    }

    //
    // WantToRansack(npc, hero): the NPC takes the weapon and the gold of the knocked-out hero. The native script refuses
    // for a class Mage / Paladin, but the hero was robbed anyway after the mercy of the plugin. So the exception is
    // enforced here: a Mage / Paladin (the hero or the NPC himself) is never ransacked while the mercy is applied.
    //
    ME_DEFINE_AND_HOOK_SCRIPT(WantToRansack)
    {
        GEInt const iResult = WantToRansack_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        Entity Hero = Entity::GetPlayer();
        // The creatures of the party never rob their fallen master.
        if (IsMasterDownVictim(Self, Other))
            return 0;
        if (Self == None || Self == Hero || !(Other == Hero) || !Self.IsNPC())
            return iResult;

        gEClass const enuHeroClass = Hero.NPC.Class;
        gEClass const enuNpcClass = Self.NPC.Class;
        GEBool const bExempt = enuHeroClass == gEClass_Mage || enuHeroClass == gEClass_Paladin ||
                               enuNpcClass == gEClass_Mage || enuNpcClass == gEClass_Paladin;
        static GEInt s_iRansackLines = 0;
        if (s_iRansackLines < 120)
        {
            ++s_iRansackLines;
            GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] '%s' (class %d) wants to ransack the hero (class %d): "
                                         "native=%d -> %s\n",
                                         Self.GetName().GetText(), static_cast<int>(enuNpcClass),
                                         static_cast<int>(enuHeroClass), static_cast<int>(iResult),
                                         (iResult && bExempt) ? "the plugin: NO (Mage / Paladin)" : "unchanged");
        }
        return (iResult && bExempt) ? 0 : iResult;
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
            if (g_Config.EnableHardDifficultyAttackSpeed || a_fScale <= 1.0f || !a_pOwner)
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
            using mFOriginal = void(GE_STDCALL *)(GUI2 *, bCUnicodeString const *, gEGameMessageType);
            if (a_enuType == gEGameMessageType_Failure && g_u32PlaqueBlockUntil != 0 &&
                static_cast<GEInt>(::GetTickCount() - g_u32PlaqueBlockUntil) < 0)
            {
                static GEInt s_iLines = 0;
                if (s_iLines < 20)
                {
                    ++s_iLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix] The native plaque of the end of the arena (Failure) is blocked: "
                                                 "only the voice is heard.\n");
                }
                return;
            }
            Hook_GuiPrintGameMessage.GetOriginalFunction<mFOriginal>()(a_pThis, a_pText, a_enuType);
        }

        void ClearStaleSessionHero(gCSession *a_pSession); // v1.0.67.0: defined below, next to SessionStop

        mCFunctionHook Hook_SessionStart;
        void GE_STDCALL SessionStart(gCSession *a_pThis, gESession_StartMode a_enuMode)
        {
            using mFOriginal = void(GE_STDCALL *)(gCSession *, gESession_StartMode);
            // v1.0.55.0: the three marks tell, from the log alone, WHERE a hang of the loading is: before the reset (the plugin is not
            // reached at all), inside the original gCSession::Start (the game itself / another plugin), or after it.
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Session start (mode %d): entering.\n", static_cast<int>(a_enuMode));
            ResetCityState(a_enuMode == gESession_StartMode_LoadGame ? "a save game is loaded" : "a new game starts");
            ClearStaleSessionHero(a_pThis); // v1.0.67.0: a hero pointer left by a session that did not pass through Stop
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Session start: the original gCSession::Start is called.\n");
            Hook_SessionStart.GetOriginalFunction<mFOriginal>()(a_pThis, a_enuMode);
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Session start: the original returned, the world is up.\n");
            // v1.1.1.0: after a LOAD the waiting companions of a duel that was Running in the save are looked up once the quests are there
            // (PollSessionAdopt, from TrackDuelForCompanions). A new game has no such state. Only a flag is set here: nothing of the game is called.
            if (a_enuMode == gESession_StartMode_LoadGame)
                ArmArenaAdoptAfterLoad();
        }

        //
        // v1.0.67.0: THE CRASH ON THE RELOAD OF A SAVE GAME AFTER A TRANSFORMATION (ge3log8 / 9 / 12 + the CrashDiag report: always the same
        // stack and the same dead pointer 0xEC1A7C10). gCSession::GetHero() (Game.dll 1.8.7) is "return [session+0x24] ? [session+0x24] :
        // [session+0x1C]". The transformation puts the real body of the hero (PC_Hero) into +0x24 and the form into +0x1C (the player).
        // gCSession::Stop zeroes +0x1C and +0x20 but NOT +0x24, and the new session (Start -> CreatePlayerEntity) writes only +0x1C. So after
        // the reload GetHero() keeps returning the removed PC_Hero of the previous world - a destroyed eCEntity (its vtable is already the
        // base-class one: SharedBase.dll+0x8D194, the slot 0xA0 is empty). Script_QuickLoot.dll builds an Entity from GetHero() in the tick
        // of every NPC: Entity(eCEntity const *) -> AttachTo -> call [vtable+0xA0] = a jump to address 0 (EIP=0 in the crash log).
        // The plugin is not what reads the dead pointer; it clears it where the engine forgot to: right after Stop, and only when the layout
        // is the one that was verified (the machine code of GetHero is compared byte by byte).
        //
        GEBool VerifyGetHeroLayout()
        {
            static GEInt s_iState = 0; // 0: not checked, 1: the layout is the verified one, -1: it is not (nothing is touched then)
            if (s_iState != 0)
                return s_iState > 0 ? GETrue : GEFalse;
            static GEU8 const arrExpected[] = {0x8B, 0x41, 0x24, 0x85, 0xC0, 0x75, 0x03, 0x8B, 0x41, 0x1C, 0xC3};
            HMODULE const hGame = ::GetModuleHandleA("Game.dll");
            GEU8 const *pCode = hGame ? reinterpret_cast<GEU8 const *>(
                                            ::GetProcAddress(hGame, "?GetHero@gCSession@@QAEPAVeCDynamicEntity@@XZ"))
                                      : nullptr;
            s_iState = pCode && memcmp(pCode, arrExpected, sizeof(arrExpected)) == 0 ? 1 : -1;
            GetDefaultLogger().LogFormat("[GenomeLogicFix] gCSession::GetHero layout check: %s.\n",
                                         s_iState > 0 ? "as verified (the stale hero pointer of the stopped session can be cleared)"
                                                      : "DIFFERENT - the stale hero pointer of the stopped session is not touched");
            return s_iState > 0 ? GETrue : GEFalse;
        }

        void ClearStaleSessionHero(gCSession *a_pSession)
        {
            if (!a_pSession || !VerifyGetHeroLayout())
                return;
            GEU32 volatile *pPlayer = reinterpret_cast<GEU32 volatile *>(reinterpret_cast<GEU8 *>(a_pSession) + 0x1C);
            GEU32 volatile *pHero = reinterpret_cast<GEU32 volatile *>(reinterpret_cast<GEU8 *>(a_pSession) + 0x24);
            // Stop has cleared the player (+0x1C); a hero pointer that is still there belongs to the world that was just destroyed.
            if (*pPlayer != 0 || *pHero == 0)
                return;
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Session stop: the session still points to the hero body 0x%08X of the destroyed "
                                         "world (a transformation was active) - the pointer is cleared.\n",
                                         static_cast<unsigned>(*pHero));
            *pHero = 0;
        }

        mCFunctionHook Hook_SessionStop;
        void GE_STDCALL SessionStop(gCSession *a_pThis)
        {
            using mFOriginal = void(GE_STDCALL *)(gCSession *);
            ResetCityState("the session stops");
            Hook_SessionStop.GetOriginalFunction<mFOriginal>()(a_pThis);
            ClearStaleSessionHero(a_pThis);
        }

        mCFunctionHook Hook_EntitySetTimeScale;
        void GE_STDCALL EntitySetTimeScale(Entity *a_pThis, GEFloat a_fScale)
        {
            using mFOriginal = void(GE_STDCALL *)(Entity *, GEFloat);
            Hook_EntitySetTimeScale.GetOriginalFunction<mFOriginal>()(
                a_pThis, ClampOpponentTimeScale(a_pThis ? a_pThis->GetInstance() : nullptr, a_fScale));
        }

        mCFunctionHook Hook_RoutineSetTimeScale;
        void GE_STDCALL RoutineSetTimeScale(PSRoutine *a_pThis, GEFloat a_fScale)
        {
            using mFOriginal = void(GE_STDCALL *)(PSRoutine *, GEFloat);
            eCEntity *pOwner = a_pThis ? static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity() : nullptr;
            Hook_RoutineSetTimeScale.GetOriginalFunction<mFOriginal>()(a_pThis, ClampOpponentTimeScale(pOwner, a_fScale));
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
            using mFOriginal = Entity *(GE_STDCALL *)(PSParty *, Entity *);
            Entity *pResult = Hook_PartyGetLeader.GetOriginalFunction<mFOriginal>()(a_pThis, a_pRet);
            if (!a_pRet || !a_pThis || !a_pThis->m_pEngineEntityPropertySet || a_pRet->GetInstance() == nullptr)
                return pResult;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None || *a_pRet == Hero)
                return pResult;
            if (_stricmp(a_pRet->GetName().GetText(), "PC_Hero") != 0)
                return pResult; // the leader is somebody else: nothing to fix
            if (_stricmp(Hero.GetName().GetText(), "PC_Hero") == 0)
                return pResult; // the hero is in his own body
            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (!pOwner)
                return pResult;
            Entity Owner(pOwner);
            if (Owner == Hero || _stricmp(Owner.GetName().GetText(), "PC_Hero") == 0 || !Owner.IsNPC() ||
                Owner.Party.PartyMemberType == gEPartyMemberType_None)
                return pResult;
            *a_pRet = Hero;
            return a_pRet;
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
            using mFOriginal = GEBool(GE_STDCALL *)(PSNpc *, GEU32);
            eCEntity *pOwner = a_pThis && a_pThis->m_pEngineEntityPropertySet
                                   ? static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity()
                                   : nullptr;
            // v1.0.59.0: DisableMinHitDurationOnHard=true: the native "no pause between the blows" of the arena opponent returns.
            if (pOwner && !g_Config.DisableMinHitDurationOnHard)
            {
                Entity Owner(pOwner);
                if (Owner.IsNPC() && Owner.NPC.AttackReason == gEAttackReason_Arena)
                {
                    Owner.NPC.AttackReason = gEAttackReason_Duel;
                    GEBool const bResult = Hook_NpcCanHit.GetOriginalFunction<mFOriginal>()(a_pThis, a_u32MinDuration);
                    Owner.NPC.AttackReason = gEAttackReason_Arena;
                    return bResult;
                }
            }
            return Hook_NpcCanHit.GetOriginalFunction<mFOriginal>()(a_pThis, a_u32MinDuration);
        }

        // gCQuest_PS::SetStatus(gEQuestStatus const &) (Game.dll, called directly by OnNPCDefeated and the dialogue
        // scripts). When the opponent of the duel was knocked out by somebody who is not the hero, the quest of the arena
        // must not become Success / Won: it becomes Lost (the status that also switches the arena off). Every other
        // quest and every other status pass through untouched. This is the hook that worked in the first versions.
        mCFunctionHook Hook_QuestSetStatus;
        GEBool GE_STDCALL QuestSetStatus(gCQuest_PS *a_pThis, gEQuestStatus const *a_penuStatus)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCQuest_PS *, gEQuestStatus const *);
            // v1.0.43.0 diagnostics: every status change of an arena quest, with the state of the hero (who restarts the duel
            // quest after the ransack shows up here).
            if (a_pThis && a_penuStatus && a_pThis->GetType() == gEQuestType_Arena)
            {
                static GEInt s_iStatusLines = 0;
                if (s_iStatusLines < 80)
                {
                    ++s_iStatusLines;
                    GetDefaultLogger().LogFormat("[GenomeLogicFix][diag] arena quest '%s': SetStatus(%d), now %d, hero down=%d, "
                                                 "mercy armed=%d active=%d, caller %p\n",
                                                 a_pThis->GetName().GetText(), static_cast<int>(*a_penuStatus),
                                                 static_cast<int>(a_pThis->GetStatus()), static_cast<int>(IsHeroKnockedOut()),
                                                 static_cast<int>(g_bSpareArmed), static_cast<int>(g_bSpareActive),
                                                 _ReturnAddress());
                }
            }
            if (a_pThis && a_penuStatus && g_bLostToThirdParty &&
                (*a_penuStatus == gEQuestStatus_Success || *a_penuStatus == gEQuestStatus_Won) &&
                a_pThis->GetType() == gEQuestType_Arena)
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] duel quest '%s': status %d replaced with Lost (the opponent "
                                             "was not beaten by the hero).\n",
                                             a_pThis->GetName().GetText(), static_cast<int>(*a_penuStatus));
                gEQuestStatus const enuLost = gEQuestStatus_Lost;
                GEBool const bResult = Hook_QuestSetStatus.GetOriginalFunction<mFOriginal>()(a_pThis, &enuLost);
                AnnounceLossToThirdParty();
                return bResult;
            }
            GEBool const bWasLost = a_pThis && a_pThis->GetStatus() == gEQuestStatus_Lost ? GETrue : GEFalse; // v1.0.68.0
            GEBool const bSetResult = Hook_QuestSetStatus.GetOriginalFunction<mFOriginal>()(a_pThis, a_penuStatus);
            if (a_pThis && a_penuStatus && *a_penuStatus == gEQuestStatus_Running &&
                a_pThis->GetType() == gEQuestType_Arena)
                OnArenaQuestStarted(bWasLost); // the dialogue with the master of the arena: the companions wait NOW
            return bSetResult;
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
            using mFOriginal = void(GE_STDCALL *)(Entity *, Entity const *, GEU32, gEDamageType);
            if (a_pThis && a_pSource && a_pThis->GetInstance() && a_pSource->GetInstance())
            {
                Entity Victim(*a_pThis);
                Entity Source(*a_pSource);
                Entity Attacker = GetDamageOwnerNpc(Source);
                LogArmyHit("Entity::DoDamage (area / scripted damage)", Victim, Source, static_cast<GEInt>(a_enuType));
                if (IsFriendlyFire(Attacker, Victim, GETrue))
                {
                    ZeroPendingDamage(Victim);
                    return; // no friendly fire inside the army of the hero (also area spells): the damage is 0
                }
                // While the hero is knocked out nothing of his party hurts him (the damage is equal to zero).
                if (Attacker != None && IsHeroSide(Victim) && IsHeroControlled(Attacker) && IsHeroKnockedOut())
                    return;
            }
            Hook_EntityDoDamage.GetOriginalFunction<mFOriginal>()(a_pThis, a_pSource, a_u32Amount, a_enuType);
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Script_Game.dll is not loaded: the arena speed-up of the "
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
                GetDefaultLogger().LogFormat("[GenomeLogicFix] The code of Script_Game.dll differs from the expected one "
                                             "(bytes %02X %02X .. %02X %02X %02X %02X %02X): the arena speed-up of the "
                                             "hard difficulty is left as it is.\n",
                                             pPlace[0], pPlace[1], pPlace[6], pPlace[7], pPlace[8], pPlace[9], pPlace[10]);
                return;
            }
            DWORD dwOld = 0;
            GEU8 *pJump = pBase + 0x4DC1;
            if (!::VirtualProtect(pJump, 1, PAGE_EXECUTE_READWRITE, &dwOld))
            {
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Cannot unlock the code for the arena speed-up patch.\n");
                return;
            }
            *pJump = 0xEB; // jne -> jmp short
            DWORD dwDummy = 0;
            ::VirtualProtect(pJump, 1, dwOld, &dwDummy);
            ::FlushInstructionCache(::GetCurrentProcess(), pJump, 1);
            GetDefaultLogger().LogFormat("[GenomeLogicFix] The x1.25 attack speed of the arena opponent on the hard "
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
        GEInt const INFO_COND_JOIN = 14;
        GEInt const INFO_COND_DISMISS = 15;    // native "You are free." (PartyDismiss), owner PC_Hero
        GEInt const INFO_COND_MOBDISMISS = 33; // native second "You are free." (CmdDismiss / MobDismiss), owner PC_Hero
        GEInt const INFO_COND_WAIT = 34;

        GEInt GetInfoCondType(gCInfo_PS const *a_pInfo)
        {
            return *reinterpret_cast<GEInt const *>(reinterpret_cast<GEU8 const *>(a_pInfo) + 0x28);
        }

        GEBool IsHeroInfo(gCInfo_PS const *a_pInfo)
        {
            return _stricmp(a_pInfo->GetOwner().GetText(), "PC_Hero") == 0 ? GETrue : GEFalse;
        }

        // A tamed animal or a dominated person / orc of the hero (party member type 4), alive and on his feet.
        GEBool IsTamedOrDominated(Entity &a_Npc)
        {
            if (a_Npc == None || !a_Npc.IsNPC() || a_Npc.IsDead() || a_Npc.IsDown())
                return GEFalse;
            if (!IsHeroControlled(a_Npc))
                return GEFalse;
            return a_Npc.Party.PartyMemberType == gEPartyMemberType_Controlled ? GETrue : GEFalse;
        }

        // The partner of the running dialogue is such a creature.
        GEBool GetDialogPartner(gCInfo_PS const *a_pInfo, Entity &o_Npc)
        {
            gCInfoManager_PS *pManager = a_pInfo->m_pInfoManager ? a_pInfo->m_pInfoManager
                                                                  : gCInfoManager_PS::GetInstance();
            if (!pManager)
                return GEFalse;
            gCEntity *pPartner = pManager->GetDialogPartnerNPC();
            if (!pPartner)
                return GEFalse;
            o_Npc = Entity(static_cast<eCEntity const *>(pPartner));
            if (!IsTamedOrDominated(o_Npc))
                return GEFalse;
            return GETrue;
        }

        void OrderWait(Entity &a_Npc)
        {
            EndBrawlOf(a_Npc, "dialogue: wait here"); // the combat state is wiped
            MarkWaiterUserPublic(a_Npc);              // v1.0.68.0: the player's order
            a_Npc.Party.Waiting = GETrue;
            a_Npc.Routine.FullStop();
            a_Npc.Routine.SetTask("ZS_WaitForPlayer");
            PrintCompanionWaitMessage(a_Npc);
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Dialogue: '%s' (party type %d) is told to wait here.\n",
                                         a_Npc.GetName().GetText(), static_cast<int>(a_Npc.Party.PartyMemberType));
        }

        void OrderFollow(Entity &a_Npc)
        {
            ClearWaiterMarksPublic(a_Npc); // v1.0.68.0
            a_Npc.Party.Waiting = GEFalse;
            EndBrawlOf(a_Npc, "dialogue: follow me"); // wipes the combat state, sets ZS_FollowPlayer + ContinueRoutine
            a_Npc.Party.Waiting = GEFalse;
            a_Npc.Routine.SetTask("ZS_FollowPlayer");
            a_Npc.Routine.ContinueRoutine();
            GetDefaultLogger().LogFormat("[GenomeLogicFix] Dialogue: '%s' (party type %d) is told to follow the hero.\n",
                                         a_Npc.GetName().GetText(), static_cast<int>(a_Npc.Party.PartyMemberType));
        }

        // gCInfo_PS::AreConditionsFulfilled() const (Game.dll): the line is offered for such a partner whatever the native
        // rules (no talking to monsters / slaves) say. "Wait here" only while he does not wait yet.
        mCFunctionHook Hook_InfoConditions;

        // v1.0.39.0: the text of the dismissal line: "Ты свободен." (any ending of . / ! / spaces).
        GEBool IsFreeLineText(bCUnicodeString const &a_strText)
        {
            static wchar_t const arrFree[] = L"\x0422\x044B \x0441\x0432\x043E\x0431\x043E\x0434\x0435\x043D"; // "Ты свободен"
            GELPCUnicodeChar pText = a_strText.GetText();
            size_t const uLen = wcslen(arrFree);
            if (!pText || wcsncmp(pText, arrFree, uLen) != 0)
                return GEFalse;
            for (pText += uLen; *pText; ++pText)
                if (*pText != L'.' && *pText != L'!' && *pText != L' ')
                    return GEFalse;
            return GETrue;
        }

        GEBool GE_STDCALL InfoConditions(gCInfo_PS *a_pThis)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCInfo_PS *);
            // v1.0.39.0: FILTER BY TEXT. While a dialogue with a tamed / dominated creature (type 4) runs, the lines whose text
            // is "Ты свободен." are counted: exactly ONE of them stays - the first info of that text (in the list of the
            // manager) that belongs to the hero or to the partner himself; every other one is hidden, whatever its condition.
            if (a_pThis)
            {
                gCInfoManager_PS *pManager = a_pThis->m_pInfoManager ? a_pThis->m_pInfoManager
                                                                      : gCInfoManager_PS::GetInstance();
                GEBool const bRunning = pManager && *(reinterpret_cast<GEU8 const *>(pManager) + 0x14) != 0;
                Entity Partner;
                if (bRunning && GetDialogPartner(a_pThis, Partner) && IsFreeLineText(a_pThis->GetDialogDescription()))
                {
                    bCString const strPartner = Partner.GetName();
                    // The keeper: the info of the native PartyDismiss (cond 15, whose script handles type 4) when it carries
                    // the text, otherwise the first info of that text.
                    gCInfo_PS *pKeeper = nullptr;
                    gCInfo_PS *pFirst = nullptr;
                    bTPtrArray<gCInfo_PS *> const &arrInfos = pManager->GetInfos();
                    for (GEInt i = 0; i < arrInfos.GetCount(); ++i)
                    {
                        gCInfo_PS *pInfo = arrInfos.GetAt(i);
                        if (!pInfo)
                            continue;
                        GEBool const bHeroOrPartner = IsHeroInfo(pInfo) || pInfo->GetOwner() == strPartner;
                        if (!bHeroOrPartner || !IsFreeLineText(pInfo->GetDialogDescription()))
                            continue;
                        if (!pFirst)
                            pFirst = pInfo;
                        if (!pKeeper && GetInfoCondType(pInfo) == INFO_COND_DISMISS)
                            pKeeper = pInfo;
                    }
                    if (!pKeeper)
                        pKeeper = pFirst;
                    return pKeeper == a_pThis ? GETrue : GEFalse;
                }
            }
            if (a_pThis && IsHeroInfo(a_pThis))
            {
                GEInt const iType = GetInfoCondType(a_pThis);
                Entity Npc;
                if ((iType == INFO_COND_WAIT || iType == INFO_COND_JOIN || iType == INFO_COND_DISMISS ||
                     iType == INFO_COND_MOBDISMISS) &&
                    GetDialogPartner(a_pThis, Npc))
                {
                    // v1.0.37.0: MUTUALLY EXCLUSIVE lines by the flag Party.Waiting: a creature that follows the hero offers
                    // ONLY "Wait here", a creature that waits offers ONLY "Follow me".
                    GEBool const bWaiting = Npc.Party.Waiting;
                    if (iType == INFO_COND_WAIT)
                        return bWaiting ? GEFalse : GETrue;
                    if (iType == INFO_COND_JOIN)
                        return bWaiting ? GETrue : GEFalse;
                    // v1.0.37.0: EXACTLY ONE "You are free." Two native infos carry the line (PartyDismiss, cond 15, and
                    // MobDismiss, cond 33): the first stays (always offered), the duplicate is hidden.
                    return iType == INFO_COND_DISMISS ? GETrue : GEFalse;
                }
            }
            return Hook_InfoConditions.GetOriginalFunction<mFOriginal>()(a_pThis);
        }

        // gCInfo_PS::Execute(gCInfoManager_PS *) (Game.dll): the chosen line. The order is given at once, the native
        // script is skipped and the dialogue is closed.
        mCFunctionHook Hook_InfoExecute;
        GEBool GE_STDCALL InfoExecute(gCInfo_PS *a_pThis, gCInfoManager_PS *a_pManager)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCInfo_PS *, gCInfoManager_PS *);
            if (a_pThis && IsHeroInfo(a_pThis))
            {
                GEInt const iType = GetInfoCondType(a_pThis);
                Entity Npc;
                if ((iType == INFO_COND_WAIT || iType == INFO_COND_JOIN) && GetDialogPartner(a_pThis, Npc))
                {
                    if (iType == INFO_COND_WAIT)
                        OrderWait(Npc);
                    else
                        OrderFollow(Npc);
                    gCInfoManager_PS *pManager = a_pManager ? a_pManager : a_pThis->m_pInfoManager;
                    if (pManager)
                        pManager->EndInfoManager(nullptr);
                    *(reinterpret_cast<GEU8 *>(a_pThis) + 0x18) = 0; // the line can be chosen again and again
                    return GETrue;
                }
                // v1.0.68.0: THE ORDER OF THE PLAYER FOR AN ORDINARY COMPANION (party type 1..3). The native script runs unchanged; here only
                // the source of the order is written down: "Wait here" -> WaiterType USER (never carried by a teleport, never released by the
                // plugin), "Follow me" -> the mark is dropped.
                if (iType == INFO_COND_WAIT || iType == INFO_COND_JOIN)
                {
                    gCInfoManager_PS *pPartnerManager = a_pManager ? a_pManager : (a_pThis->m_pInfoManager ? a_pThis->m_pInfoManager
                                                                                                           : gCInfoManager_PS::GetInstance());
                    gCEntity *pPartner = pPartnerManager ? pPartnerManager->GetDialogPartnerNPC() : nullptr;
                    if (pPartner)
                    {
                        Entity Companion(static_cast<eCEntity const *>(pPartner));
                        if (Companion != None && Companion.IsNPC() && !Companion.IsDead() && IsHeroControlled(Companion))
                        {
                            if (iType == INFO_COND_WAIT)
                            {
                                MarkWaiterUserPublic(Companion);
                                char strLine[200] = {};
                                _snprintf_s(strLine, sizeof(strLine), _TRUNCATE,
                                            "[GenomeLogicFix][teleport] dialogue | '%s' | party type %d | told to WAIT by the player: WaiterType=USER",
                                            Companion.GetName().GetText(), static_cast<int>(Companion.Party.PartyMemberType));
                                GetDefaultLogger().LogFormat("%s\n", strLine);
                                CrashDiag::Note(strLine);
                            }
                            else
                                ClearWaiterMarksPublic(Companion);
                        }
                    }
                }
            }
            return Hook_InfoExecute.GetOriginalFunction<mFOriginal>()(a_pThis, a_pManager);
        }

        // gCInfo_PS::GetDialogDescription() const: the text of the line (hidden return buffer).
        mCFunctionHook Hook_InfoDescription;
        bCUnicodeString *GE_STDCALL InfoDescription(gCInfo_PS *a_pThis, bCUnicodeString *a_pRet)
        {
            using mFOriginal = bCUnicodeString *(GE_STDCALL *)(gCInfo_PS *, bCUnicodeString *);
            bCUnicodeString *pResult = Hook_InfoDescription.GetOriginalFunction<mFOriginal>()(a_pThis, a_pRet);
            if (a_pThis && a_pRet && IsHeroInfo(a_pThis))
            {
                GEInt const iType = GetInfoCondType(a_pThis);
                Entity Npc;
                if ((iType == INFO_COND_WAIT || iType == INFO_COND_JOIN) && GetDialogPartner(a_pThis, Npc))
                {
                    if (iType == INFO_COND_WAIT)
                        *a_pRet = L"\x0416\x0434\x0438 \x0437\x0434\x0435\x0441\x044C"; // "Жди здесь"
                    else
                        *a_pRet = L"\x0418\x0434\x0438 \x0437\x0430 \x043C\x043D\x043E\x0439"; // "Иди за мной"
                }
            }
            return pResult;
        }

        // Entity::HasInfo(other) const (Script.dll): "the hero has something to say to this NPC". The perception scripts
        // (AssessTalk, the focus) ask it before the creature can be spoken to. True for such a creature.
        mCFunctionHook Hook_EntityHasInfo;
        GEBool GE_STDCALL EntityHasInfo(Entity *a_pThis, Entity const *a_pOther)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(Entity *, Entity const *);
            GEBool const bResult = Hook_EntityHasInfo.GetOriginalFunction<mFOriginal>()(a_pThis, a_pOther);
            if (bResult || !a_pThis || !a_pOther || a_pThis->GetInstance() == nullptr || a_pOther->GetInstance() == nullptr)
                return bResult;
            Entity Hero = Entity::GetPlayer();
            Entity First(*a_pThis);
            Entity Second(*a_pOther);
            if (First == Hero && IsTamedOrDominated(Second))
                return GETrue;
            if (Second == Hero && IsTamedOrDominated(First))
                return GETrue;
            return bResult;
        }
    } // namespace



    void InstallArenaHooks()
    {
        // The default logger of the SDK is stdout, which the game does not have: write the log of the plugin to
        // <Documents>\Gothic3\logs\Script_GenomeLogicFix.log.
        bCString const strLogPath = GetAndCreateLogFilePath("logs", "Script_GenomeLogicFix.log");
        if (!strLogPath.IsEmpty())
        {
            // The logger opens the file for writing, i.e. the log of the previous run is lost - exactly the log of a run
            // that ended with a crash. It is kept as Script_GenomeLogicFix.prev.log (one generation).
            char strPrev[MAX_PATH] = {};
            strncpy_s(strPrev, strLogPath.GetText(), _TRUNCATE);
            size_t const uLen = strlen(strPrev);
            if (uLen > 4 && _stricmp(strPrev + uLen - 4, ".log") == 0 && uLen + 5 < MAX_PATH)
            {
                strcpy_s(strPrev + uLen - 4, MAX_PATH - (uLen - 4), ".prev.log");
                ::MoveFileExA(strLogPath.GetText(), strPrev, MOVEFILE_REPLACE_EXISTING);
            }
            SetDefaultLogger(mCLoggingAdmin<mCFileLogger>::GetLogger(strLogPath.GetText()));
        }

        LoadConfig();

        // v1.0.66.0: the crash reporter goes first (before any hook of the plugin can be hit by a fault).
        if (g_Config.CrashDiagnostics && !strLogPath.IsEmpty())
            CrashDiag::Install(strLogPath.GetText());

        auto const InstallOne = [](mCFunctionHook &a_Hook, GELPCChar a_strName, GELPVoid a_pNew,
                                   GELPCChar a_strModule = "Script.dll")
        {
            HMODULE hModule = ::GetModuleHandleA(a_strModule);
            GELPVoid pOriginal = hModule ? reinterpret_cast<GELPVoid>(::GetProcAddress(hModule, a_strName)) : nullptr;
            if (!pOriginal || !a_Hook.Prepare(pOriginal, a_pNew).ThisCall().Hook())
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Failed to hook %s\n", a_strName);
            else
                GetDefaultLogger().LogFormat("[GenomeLogicFix] Hooked %s\n", a_strName);
        };
        InstallOne(Hook_GuiPrintGameMessage, "?PrintGameMessage@GUI2@@QAEXABVbCUnicodeString@@W4gEGameMessageType@@@Z",
                   reinterpret_cast<GELPVoid>(&GuiPrintGameMessage));
        InstallOne(Hook_SessionStart, "?Start@gCSession@@QAEXW4gESession_StartMode@@@Z",
                   reinterpret_cast<GELPVoid>(&SessionStart), "Game.dll");
        InstallOne(Hook_SessionStop, "?Stop@gCSession@@QAEXXZ", reinterpret_cast<GELPVoid>(&SessionStop), "Game.dll");
        InstallOne(Hook_NpcSetCurrentAttacker, "?SetCurrentAttacker@PSNpc@@QAEXABVEntity@@@Z",
                   reinterpret_cast<GELPVoid>(&NpcSetCurrentAttacker));
        InstallOne(Hook_NpcSetLastAttacker, "?SetLastAttacker@PSNpc@@QAEXABVEntity@@@Z",
                   reinterpret_cast<GELPVoid>(&NpcSetLastAttacker));
        InstallOne(Hook_NpcSetLastCast, "?SetLastCast@PSNpc@@QAEXXZ", reinterpret_cast<GELPVoid>(&NpcSetLastCast));
        InstallOne(Hook_NpcEnableStatusEffects, "?EnableStatusEffects@PSNpc@@QAEXH_N@Z",
                   reinterpret_cast<GELPVoid>(&NpcEnableStatusEffects));
        InstallOne(Hook_EntitySetTimeScale, "?SetTimeScale@Entity@@QAEXM@Z",
                   reinterpret_cast<GELPVoid>(&EntitySetTimeScale));
        InstallOne(Hook_RoutineSetTimeScale, "?SetTimeScale@PSRoutine@@QAEXM@Z",
                   reinterpret_cast<GELPVoid>(&RoutineSetTimeScale));
        InstallOne(Hook_NpcCanHit, "?CanHit@PSNpc@@QAE_NK@Z", reinterpret_cast<GELPVoid>(&NpcCanHit));
        InstallOne(Hook_QuestSetStatus, "?SetStatus@gCQuest_PS@@QAE_NABW4gEQuestStatus@@@Z",
                   reinterpret_cast<GELPVoid>(&QuestSetStatus), "Game.dll");
        InstallOne(Hook_EntityDoDamage, "?DoDamage@Entity@@QAEXABV1@KW4gEDamageType@@@Z",
                   reinterpret_cast<GELPVoid>(&EntityDoDamage));
        InstallOne(Hook_PartyGetLeader, "?GetPartyLeader@PSParty@@QBE?AVEntity@@XZ",
                   reinterpret_cast<GELPVoid>(&PartyGetLeader));
        // v1.0.35.0: the dialogue lines "wait here" / "follow me" for tamed / dominated creatures.
        InstallOne(Hook_InfoConditions, "?AreConditionsFulfilled@gCInfo_PS@@IBE_NXZ",
                   reinterpret_cast<GELPVoid>(&InfoConditions), "Game.dll");
        InstallOne(Hook_InfoExecute, "?Execute@gCInfo_PS@@QAE_NPAVgCInfoManager_PS@@@Z",
                   reinterpret_cast<GELPVoid>(&InfoExecute), "Game.dll");
        InstallOne(Hook_InfoDescription, "?GetDialogDescription@gCInfo_PS@@QBE?AVbCUnicodeString@@XZ",
                   reinterpret_cast<GELPVoid>(&InfoDescription), "Game.dll");
        InstallOne(Hook_EntityHasInfo, "?HasInfo@Entity@@QBE_NABV1@@Z", reinterpret_cast<GELPVoid>(&EntityHasInfo));
        // v1.0.39.0: the low-level teleport of the hero (both overloads), before any third-party auto-teleport plugin.

        // v1.0.59.0: the x1.25 speed-up of the hard difficulty is removed only while EnableHardDifficultyAttackSpeed is false.
        if (!g_Config.EnableHardDifficultyAttackSpeed)
            RemoveArenaHardSpeedUp();
        else
            GetDefaultLogger().LogFormat("[GenomeLogicFix] EnableHardDifficultyAttackSpeed=true: the native speed-up of the arena "
                                         "opponent on the hard difficulty is kept.\n");

        // v1.0.59.0: the line of the stance of ZS_ReactToDamage becomes replaceable (see the hook of ZS_ReactToDamage).
        PatchReactToDamageSvm();

        // Script hooks (Stage 3) are registered by the static blocks above; apply them now.
        GetScriptHookRegistry().Hook();

        // v1.0.53.0: FreezeFix (FreezeFix.cpp): the combat memory survives ZS_Freeze / ZS_Freeze_Loop.
        InitFreezeFixHooks();
    }
} // namespace RequirementsCheck

mCScriptHookRegistry &GetScriptHookRegistry()
{
    static mCScriptHookRegistry s_Registry;
    return s_Registry;
}
