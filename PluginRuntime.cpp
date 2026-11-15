#include "RequirementsCheck.h"
#include "PluginRuntime.h"
#include "NpcHealth.h"
#include "ArenaJudge.h"
#include "SocialLogic.h"
#include "FactionLogic.h"
#include "GroupBrawl.h"
#include "GroupLogic.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include "DebugLog.h"
#include "CrashDiag.h"
#include "FreezeFix.h"

#include <g3sdk/Script.h>
#include <g3sdk/util/ScriptUtil.h>

// Central DLL entry points. Preserve startup order and leave hook registration in place.
extern "C" __declspec(dllexport) gSScriptInit const *GE_STDCALL ScriptInit(void)
{
    // Ensure that Script_Game.dll is loaded.
    GetScriptAdmin().LoadScriptDLL("Script_Game.dll");

    PluginRuntime::LoadConfig();
    DebugLog::Initialize();

    if (!RequirementsCheck::Install())
        DebugLog::GetLogger().LogFormat("[RequirementsCheck] Some hooks could not be installed, see messages above.\n");
    PluginRuntime::InstallSharedHooks();

    return &GetScriptInit();
}

//
// Entry Point
//

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH: ::DisableThreadLibraryCalls(hModule); break;
        case DLL_PROCESS_DETACH: break;
    }
    return TRUE;
}

namespace PluginRuntime
{
    void OnHeroRoutineChange(GELPCChar a_strName, GEBool a_bIsTask)
    {
        using GroupLogic::Brawl::LogDialogueRecovery;
        using GroupLogic::Brawl::LogMagicAim;

        if (!a_strName || !a_strName[0])
            return;
        if (_strnicmp(a_strName, "PS_Talk", 7) == 0)
        {
            Entity hero = Entity::GetPlayer();
            Entity target = hero != None && hero.NPC.IsValid() ? hero.NPC.GetCurrentTarget() : Entity();
            LogDialogueRecovery(a_strName, hero.GetInstance(), target.GetInstance());
        }
        if (_strnicmp(a_strName, "PS_Magic", 8) == 0 && _strnicmp(a_strName, "PS_MagicPotion", 14) != 0)
            LogMagicAim(a_strName); // Incoming state; the routine setter may not have applied it yet.
        Host::OnArenaRoutineChange(a_strName, a_bIsTask);
    }
    static void PollSocialRecovery()
    {
        if (!Conflict::IsArenaPunishment() &&
            (Conflict::IsFallActive() || (Conflict::IsArmed() && HeroState::IsHeroKnockedOut())))
            Conflict::SpareHousekeeping();
    }
    // Called once for each eligible NPC event, before the native OnTick.
    void OnNpcTick(Entity &Self)
    {
        using GroupLogic::Brawl::UpdatePersonalRecovery;
        using GroupLogic::Brawl::ObserveRecovery;
        using GroupLogic::PollTeleportFollowers;
        GroupLogic::Lifecycle::DismissSummonsOnHeroLoss();
        PluginRuntime::ObserveKnockout(Self);
        UpdatePersonalRecovery(Self);
        ObserveRecovery(Self, "OnTick");

        PollTeleportFollowers(); // v1.0.49.0: the late move of the companions after a teleport (one flag read when idle)
        ArenaJudge::Punishment::OnArenaNpcTick(Self);
        PollSocialRecovery();
        if (!Conflict::IsArenaPunishment()) SocialLogic::ObserveNpc(Self);
    }

    namespace
    {
        bCPropertyID arenaKnockoutVictim;
        GEBool arenaKnockoutRemembered = GEFalse;
        GEBool arenaKnockoutInProgress = GEFalse;
        GEBool IsKnockedOut(Entity &npc)
        {
            if (npc == None || !npc.IsNPC()) return GEFalse;
            bCString const task = npc.Routine.GetCurrentTask();
            return npc.IsDown() || npc.Routine.AIMode == gEAIMode_Down ||
                _stricmp(task.GetText(), "ZS_Unconscious") == 0 ||
                _stricmp(task.GetText(), "ZS_LieKnockDown") == 0 || _stricmp(task.GetText(), "ZS_PiercedKO") == 0;
        }
        GEBool MatchesArenaKnockout(Entity &npc)
        {
            return arenaKnockoutRemembered && npc != None &&
                npc.GetInstance()->GetID() == arenaKnockoutVictim;
        }
    }
    void BeginKnockout(Entity &victim)
    {
        if (MatchesArenaKnockout(victim)) arenaKnockoutRemembered = GEFalse;
        if (!Host::IsArenaKnockout(victim)) return;
        arenaKnockoutVictim = victim.GetInstance()->GetID();
        arenaKnockoutRemembered = GETrue;
        arenaKnockoutInProgress = GETrue;
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][knockout] '%s': arena context captured before native Defeat.\n", victim.GetName().GetText());
    }
    void EndKnockout(Entity &victim)
    {
        if (!MatchesArenaKnockout(victim)) return;
        arenaKnockoutInProgress = GEFalse;
        ObserveKnockout(victim);
    }
    void ObserveKnockout(Entity &victim)
    {
        if (MatchesArenaKnockout(victim) && !arenaKnockoutInProgress &&
            (victim.IsDead() || !IsKnockedOut(victim)))
        {
            arenaKnockoutRemembered = GEFalse;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][knockout] '%s': arena fall context released.\n", victim.GetName().GetText());
        }
    }
    void OnCityKnockout(Entity &victim, GELPCChar where, Entity *source)
    {
        ObserveKnockout(victim);
        if (MatchesArenaKnockout(victim)) return;
        SocialLogic::OnCityKnockout(victim, where, source);
    }

    using SocialLogic::ResetCityState;
    using GroupLogic::Brawl::ResetBrawlDiagnostics;
    using GroupLogic::Brawl::ClearBrawls;
    using GroupLogic::Combat::ResetFleeWatch;
    using GroupLogic::Combat::ClearFocusRecords;
    using GroupLogic::Combat::ClearCombatRecords;
    using Host::ArmArenaAdoptAfterLoad;
    mCFunctionHook Hook_SessionStart;
    mCFunctionHook Hook_SessionStop;
    mCFunctionHook Hook_SessionProcess;

    void GE_STDCALL SessionProcess(gCSession *session)
    {
        bool const wasRunning = session->IsGameRunning();
        if (wasRunning) GroupLogic::Lifecycle::CaptureHeroLossSummons();
        Hook_SessionProcess.GetOriginalFunction(&SessionProcess)(session);
        DebugLog::FlushNpcCombatDiagnostics(wasRunning && session->IsGameRunning());
        if (wasRunning) GroupLogic::Lifecycle::DismissSummonsOnHeroLoss();
        if (session->IsGameRunning())
        {
            FactionLogic::Update();
            DebugLog::PollRevolutionMonitoring();
            NpcHealth::Update(*session);
            GroupLogic::Lifecycle::UpdateSecondarySummons();
            GroupLogic::Lifecycle::ReviewSummonTargets();
        }
    }
        void GE_STDCALL SessionStart(gCSession *a_pThis, gESession_StartMode a_enuMode)
        {
            using mFOriginal = void(GE_STDCALL *)(gCSession *, gESession_StartMode);
            // v1.0.55.0: the three marks tell, from the log alone, WHERE a hang of the loading is: before the reset (the plugin is not
            // reached at all), inside the original gCSession::Start (the game itself / another plugin), or after it.
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Session start (mode %d): entering.\n", static_cast<int>(a_enuMode));
            ResetBrawlDiagnostics();
            DebugLog::ResetRevolutionMonitoring();
            FactionLogic::Reset();
            ResetCityState(a_enuMode == gESession_StartMode_LoadGame ? "a save game is loaded" : "a new game starts");
            ClearStaleSessionHero(a_pThis); // v1.0.67.0: a hero pointer left by a session that did not pass through Stop
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Session start: the original gCSession::Start is called.\n");
            Hook_SessionStart.GetOriginalFunction<mFOriginal>()(a_pThis, a_enuMode);
            NpcHealth::InstallHook();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Session start: the original returned, the world is up.\n");
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
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] gCSession::GetHero layout check: %s.\n",
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
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Session stop: the session still points to the hero body 0x%08X of the destroyed "
                                         "world (a transformation was active) - the pointer is cleared.\n",
                                         static_cast<unsigned>(*pHero));
            *pHero = 0;
        }
        void GE_STDCALL SessionStop(gCSession *a_pThis)
        {
            using mFOriginal = void(GE_STDCALL *)(gCSession *);
            ResetBrawlDiagnostics();
            DebugLog::ResetRevolutionMonitoring();
            FactionLogic::Reset();
            ResetCityState("the session stops");
            Hook_SessionStop.GetOriginalFunction<mFOriginal>()(a_pThis);
            ClearStaleSessionHero(a_pThis);
        }

        void ResetBlowHistory(GELPCChar a_strWhy)
        {
            GEInt const iReleased = SocialLogic::ResetBlowHistory();
            ClearBrawls();
            ResetFleeWatch();
            ClearFocusRecords();
            ClearCombatRecords();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Blow history reset (%s): punished social conflicts and party counters reset; "
                                         "unresolved social warnings retained (%d watchers released).\n",
                                         a_strWhy, static_cast<int>(iReleased));
        }
    void ResetSharedSessionState()
    {
        GroupLogic::Lifecycle::ResetHeroLossSummons();
        GroupLogic::Lifecycle::ResetSecondarySummons();
        arenaKnockoutRemembered = GEFalse;
        arenaKnockoutInProgress = GEFalse;
        Conflict::ResetSpare();
        Host::ResetFormAggro();
        HeroState::ResetHeroImmunity();
        Conflict::ClearAggro();
        GroupLogic::Combat::ClearDefendedTargets();
        Host::ResetDuelState();
    }

}

namespace SocialLogic::Host
{
    void ResetSharedSessionState() { PluginRuntime::ResetSharedSessionState(); }
}

gSScriptInit &GetScriptInit()
{
    static gSScriptInit s_ScriptInit;
    return s_ScriptInit;
}

mCScriptHookRegistry &GetScriptHookRegistry()
{
    static mCScriptHookRegistry s_Registry;
    return s_Registry;
}


namespace PluginRuntime
{



    namespace
    {
        mCFunctionHook combatMoveInstructionHook;

        bool CanAttachCombatEntity(eCEntity const *entity)
        {
            if (!entity)
                return true;
            __try
            {
                auto const *vtable = *reinterpret_cast<void const *const *const *>(entity);
                // Entity::AttachTo calls this slot before testing the entity's property sets.
                return vtable && vtable[0xA0 / sizeof(void *)] != nullptr;
            }
            __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
            {
                return false;
            }
        }

        GEBool GE_STDCALL CombatMoveInstruction(GELPVoid args, gCScriptProcessingUnit *spu, GEBool fullStop)
        {
            using Original = GEBool(GE_STDCALL *)(GELPVoid, gCScriptProcessingUnit *, GEBool);
            auto original = combatMoveInstructionHook.GetOriginalFunction<Original>();
            if (!spu || fullStop)
                return original(args, spu, fullStop);

            auto const *move = static_cast<gCScriptProcessingUnit::sAICombatMoveInstr_Args const *>(args);
            auto const *target = move && move->SelfEntity ? move->TargetEntity : spu->GetOtherEntity();
            if (CanAttachCombatEntity(target))
                return original(args, spu, fullStop);

            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][combat] cancelled move with unavailable target=%p.\n", target);
            // The native full-stop path releases the instruction and returns completion.
            return original(nullptr, spu, GETrue);
        }

        void InstallCombatMoveGuard()
        {
            auto const script = ::GetModuleHandleA("Script.dll");
            auto const attach = script ? ::GetProcAddress(script, "?AttachTo@Entity@@QAE_NPAVeCEntity@@@Z") : nullptr;
            unsigned char const expected[] = {0x8B, 0x45, 0x00, 0x8B, 0x90, 0xA0, 0x00, 0x00, 0x00, 0x8B, 0xCD, 0xFF, 0xD2};
            auto const game = ::GetModuleHandleA("Game.dll");
            auto const instruction = game ? ::GetProcAddress(game, "?sAICombatMoveInstr@gCScriptProcessingUnit@@SG_NPAXPAV1@_N@Z") : nullptr;
            bool const installed = attach && instruction &&
                !std::memcmp(reinterpret_cast<unsigned char const *>(attach) + 0x20, expected, sizeof(expected)) &&
                combatMoveInstructionHook.Prepare(instruction, &CombatMoveInstruction).Hook();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][combat] combat move target guard=%d.\n", installed);
        }
    }

    void InstallSharedHooks()
    {
        bCString const strLogPath = DebugLog::Initialize();
        GroupLogic::Brawl::InitializeSessionAccess();

        // v1.0.66.0: the crash reporter goes first (before any hook of the plugin can be hit by a fault).
        if (GetConfig().Diagnostics && !strLogPath.IsEmpty())
            CrashDiag::Install(strLogPath.GetText());

        auto const InstallOne = [](mCFunctionHook &a_Hook, GELPCChar a_strName, GELPVoid a_pNew,
                                   GELPCChar a_strModule = "Script.dll")
        {
            HMODULE hModule = ::GetModuleHandleA(a_strModule);
            GELPVoid pOriginal = hModule ? reinterpret_cast<GELPVoid>(::GetProcAddress(hModule, a_strName)) : nullptr;
            if (!pOriginal || !a_Hook.Prepare(pOriginal, a_pNew).ThisCall().Hook())
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Failed to hook %s\n", a_strName);
            else
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Hooked %s\n", a_strName);
        };
        GEU32 count = 0;
        NativeHookBinding const *bindings = Host::GetNativeHookBindings(count);
        for (GEU32 i = 0; i < count; ++i)
            InstallOne(*bindings[i].hook, bindings[i].symbol, bindings[i].replacement, bindings[i].module);
        // v1.0.39.0: the low-level teleport of the hero (both overloads), before any third-party auto-teleport plugin.

        // v1.0.59.0: the x1.25 speed-up of the hard difficulty is removed only while EnableHardDifficultyAttackSpeed is false.
        if (!GetConfig().EnableHardDifficultyAttackSpeed)
            Host::RemoveArenaHardSpeedUp();
        else
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] EnableHardDifficultyAttackSpeed=true: the native speed-up of the arena "
                                         "opponent on the hard difficulty is kept.\n");

        // v1.0.59.0: the line of the stance of ZS_ReactToDamage becomes replaceable (see the hook of ZS_ReactToDamage).
        Host::PatchReactToDamageSvm();

        DebugLog::InstallLootDiagnostics();
        InstallCombatMoveGuard();

        // Script hooks remain registered by static blocks in ArenaJudge.cpp; apply them now.
        GetScriptHookRegistry().Hook();
        DebugLog::InstallRevolutionDiagnostics(FactionLogic::InstallEnclaveStatusHook());
        FactionLogic::InstallSharedPursuitHook();

        // v1.0.53.0: FreezeFix (FreezeFix.cpp): the combat memory survives ZS_Freeze / ZS_Freeze_Loop.
        InitFreezeFixHooks();
    }
}

namespace PluginRuntime::Combat
{
    using namespace PluginRuntime::HeroState;
    using GroupLogic::Combat::IsCreatedByHero;
    using GroupLogic::Combat::GetHitDamageType;
    using namespace GroupLogic::Combat;
    using namespace GroupLogic::Brawl;
    using namespace SocialLogic;
    using namespace ArenaJudge::Combat;
    using namespace ArenaJudge::Punishment;
    using GroupLogic::IsHeroControlled;
    using GroupLogic::IsHeroFormEntity;
    namespace {
        Entity ResolveEntity(GELPVoid entity, eCEntity *fallback)
        { return entity ? *static_cast<Entity *>(entity) : Entity(fallback); }
    }
    GEInt DoLogicalDamage(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Victim = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Source = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());

        static GEBool s_bAliveLogged = GEFalse;
        if (!s_bAliveLogged)
        {
            s_bAliveLogged = GETrue;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] DoLogicalDamage hook is alive.\n");
        }

        // Diagnostics only: every blow on a member of the army, before anything is decided.
        LogArmyHit("DoLogicalDamage (script, every melee / missile / spell hit)", Victim, Source,
                   static_cast<GEInt>(GetHitDamageType(Source, Victim)));

        // No friendly fire inside the army of the hero: the blow is not processed at all (no damage, no reaction).
        {
            Entity FriendlyAttacker = GetDamageOwnerNpc(Source);
            if (FactionLogic::IsJoiningLocalNoneFight(FriendlyAttacker, Victim))
            {
                DebugLog::GetLogger().LogFormat("[FactionLogic] witness hit blocked npc='%s' target='%s'\n",
                    FriendlyAttacker.GetName().GetText(), Victim.GetName().GetText());
                ZeroPendingDamage(Victim);
                return 0;
            }
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

        auto arena = ArenaJudge::Combat::BeforeDamage(Victim, Source);
        PollSocialRecovery();
        SocialLogic::ImpactScope social(Victim, Source);
        Entity attacker = GetDamageOwnerNpc(Source);
        FactionLogic::WitnessImpactScope witnessImpact(Victim, attacker);
        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);

        ArenaJudge::Combat::AfterDamage(arena, Victim);
        return iResult;
        }
    GEInt Defeat(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        // Native Defeat(winner, defeated): OnNPCDefeated receives Other, not Self.
        // Keep the original native arguments untouched; all checks below concern the defeated entity.
        Entity Self = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        Entity Hero = Entity::GetPlayer();
        Entity Winner = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        PersonalKnockoutScope knockout(Self, Self != None && IsBrawlFighting(Self));
        PluginRuntime::BeginKnockout(Self);
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][defeat] winner='%s', defeated='%s', player='%s', defeatedIsPlayer=%d, playerDown=%d.\n",
            Winner != None ? Winner.GetName().GetText() : "-", Self != None ? Self.GetName().GetText() : "-",
            Hero != None ? Hero.GetName().GetText() : "-", static_cast<int>(Self != None && Self == Hero),
            static_cast<int>(IsHeroKnockedOut()));
        if (Self != None && IsBrawlFighting(Self))
        {
            ObserveRecovery(Self, "Defeat before native", GETrue);
            SetBrawlEvent(Self, "NoReward", GETrue);
        }
        if (Self != None && Self == Hero && !Conflict::IsFallActive() && !Conflict::IsArenaPunishment())
            SocialLogic::CapturePunishedConflict(Winner);
        auto arena = ArenaJudge::Combat::BeforeDefeat(Self, Hero);
        if (Self != None && Self == Hero) GroupLogic::Lifecycle::CaptureHeroLossSummons(true);
        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        GroupLogic::Lifecycle::OnHeroDefeated(Self);
        PluginRuntime::EndKnockout(Self);
        ObserveRecovery(Self, "Defeat after native");

        // v1.0.35.0: a resident of a friendly city is knocked out by the hero: the full crime at once, no timer.
        if (Self != None && !(Self == Hero) && Self.IsNPC())
            OnCityKnockout(Self, "Defeat", &Winner);

        ArenaJudge::Combat::AfterDefeat(arena, Self, Hero);
        return iResult;
        }
    GEInt AssessHit(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Victim = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Source = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        Entity Attacker = GetDamageOwnerNpc(Source);
        if (Attacker == None && Victim != None && Victim.IsNPC())
            Attacker = Victim.NPC.GetCurrentAttacker(); // OnDamage may pass no damaging object
        if (FactionLogic::IsJoiningLocalNoneFight(Attacker, Victim))
        {
            ZeroPendingDamage(Victim);
            return 0;
        }
        FactionLogic::RecordImpact(Victim, Attacker);
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
        // Snapshot before native processing: it may destroy the projectile.
        // Status effects without a projectile cannot count as a new impact.
        GEBool const bBrawlProjectile = Victim != None && Attacker != None && Victim.IsNPC() &&
            IsHeroSide(Attacker) && IsHeroControlled(Victim) && !IsHeroAttackerInForm(Attacker) &&
            Source != None && Source.Projectile.IsValid() && IsPointMagicSource(&Source);
        bCPropertyID const ProjectileID = bBrawlProjectile ? Source.GetInstance()->GetID() : bCPropertyID();
        PollSocialRecovery();
        SocialLogic::ImpactScope social(Victim, Source);
        FactionLogic::WitnessImpactScope witnessImpact(Victim, Attacker);
        PersonalRewardScope personalReward(Victim, bBrawlProjectile ||
            (IsHeroSide(Attacker) && IsBrawlFighting(Victim)));
        Entity Body;
        if (Victim != None && Victim.IsNPC() && Source != None && Source == Attacker && IsHeroControlled(Victim) &&
            FindHumanBodyFor(Attacker, Body))
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AssessHit: the blow of '%s' (%p) on '%s' is judged by the "
                                             "engine in the name of '%s' (distance between the bodies %.0f).\n",
                                             Attacker.GetName().GetText(), static_cast<void *>(Attacker.GetInstance()),
                                             Victim.GetName().GetText(), Body.GetName().GetText(),
                                             static_cast<double>(Attacker.GetDistanceTo(Body)));
            }
            Entity BodyArg = Body;
            iResult = original(a_pSPU, a_pSelfEntity, &BodyArg, a_iArgs);
        }
        else
            iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        // v1.0.35.0: AssessHit is where the native code knocks the victim out (ZS_Unconscious): a green resident who falls
        // here at the hands of the hero is the second blow at once (no 10 s window).
        if (Victim != None && Attacker != None && Victim.IsNPC() && IsHeroSide(Attacker) && !IsHeroControlled(Victim))
            OnCityKnockout(Victim, "AssessHit", &Attacker);
        // Count a direct allowed projectile after native hit processing. The captured identity
        // does not depend on SetLastCast; status ticks and attacker-setter notifications
        // do not advance the counter. The rest of the party stays out of the fight.
        if (Victim != None && Attacker != None && Victim.IsNPC() && IsHeroSide(Attacker) && IsHeroControlled(Victim))
        {
            GEBool const bFocus = bBrawlProjectile; // only a confirmed direct projectile impact counts
            if (bFocus)
                OnHeroBlowOnArmy(Victim, ProjectileID);
            static GEInt s_iBrawlLines = 0;
            if (s_iBrawlLines < 60)
            {
                ++s_iBrawlLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AssessHit: the hero ('%s') hit '%s' (party type %d), "
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
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AssessHit: the outsider '%s' hit '%s' (party type %d): "
                                             "native reaction, the plugin does not interfere (task '%s').\n",
                                             Attacker.GetName().GetText(), Victim.GetName().GetText(),
                                             static_cast<int>(Victim.Party.PartyMemberType),
                                             Victim.Routine.GetCurrentTask().GetText());
            }
        }
        return iResult;
        }
    GEInt AssessAgressor(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (HandlePersonalAggressor(Self, Other)) return 1;
        GEInt const iAgressorResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        // v1.0.35.0: the stumble states call AssessAgressor after every blow, also the one that knocked the victim out.
        if (Self != None && Other != None && Self.IsNPC() && IsHeroSide(Other) && !IsHeroControlled(Self))
            OnCityKnockout(Self, "AssessAgressor", &Other);
        return iAgressorResult;
        }
    GEInt AssessTarget(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        PollFleeWatch(); // diagnostics only
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (DebugLog::IsEnabled() && GetConfig().FactionDiagnostics &&
            Self != None && Self.NPC.IsValid() && Other != None && Other == Entity::GetPlayer() &&
            !FactionLogic::IsPursuing(Self, Other))
        {
            DebugLog::GetLogger().LogFormat("[FactionLogic][aggro] assess npc='%s' reason=%d eligibleReason=%d task='%s'\n",
                Self.GetName().GetText(), a_iArgs,
                a_iArgs == gEAttackReason_Enemy,
                Self.Routine.IsValid() ? Self.Routine.GetCurrentTask().GetText() : "-");
        }
        DebugLog::LogRevolutionAttack(Self, Other, a_iArgs);
        DebugLog::LogFactionCreatureTarget(Self, Other, "assess-target", a_iArgs);
        if (FactionLogic::SuppressCombatTarget(Self, Other) ||
            FactionLogic::SuppressWitnessAid(Self, Other))
            return 0;
        if (a_iArgs == gEAttackReason_ReactToWeapon && FactionLogic::IgnoresDrawnWeapon(Self))
            return 0;
        if (FactionLogic::SuppressCitizenIntervention(Self, Other, a_iArgs))
        {
            if (DebugLog::IsEnabled())
                DebugLog::GetLogger().LogFormat("[FactionLogic] citizen intervention suppressed npc='%s' target='%s' reason=%d\n",
                    Self.GetName().GetText(), Other.GetName().GetText(), a_iArgs);
            return 0;
        }
        if (Self == None || Other == None)
            return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if ((a_iArgs == gEAttackReason_Enemy || a_iArgs == gEAttackReason_Prey) &&
            FactionLogic::IsPeacefulSettlementCreature(Self, Other))
            return 0;
        Entity Hero = Entity::GetPlayer();
        if (IgnoreArenaPunishmentTarget(Self, Other))
        {
            LogArenaPunishmentFilter(Self, Other, "AssessTarget");
            return 0;
        }
        // v1.0.41.0: the opponent in the warning stance (the magic in the hand of the hero) does not take him for a target.
        if (SocialLogic::HandleSlaveWitnessTarget(Self, Other, a_iArgs))
            return 0;
        // v1.0.47.0: the very first order of the native code (the choice of the target) is blocked and replaced by the stance.
        if ((Other == Hero || IsHeroSide(Other)) && (IsWarnedOpponent(Self) || PreemptMagicWarning(Self)))
            return 0;
        // v1.0.36.0: the political filters (rule 5: faction None vs a Slave target, rule 6: the Wanderers vs the hero in a
        // transformed body). The automatic aggression is not started at all.
        if (SocialLogic::IgnorePerceivedTarget(Self, Other)) return 0;
        if (SocialLogic::IsAwaitingSecondAssault(Self, Other)) return 0;
        SocialLogic::ObserveTarget(Self, Other, a_iArgs);
        // An OUTSIDER that has hit the army (or the hero) is in a real fight: from the first blow on the AI of the squad has
        // full native freedom (the stumble, the retreat after a thrust, the answer). None of the filters below is applied -
        // the target is never reset in the middle of a native micro-stumble. Only the duel taboo stays.
        if (!IsDuelTaboo(Other) && !IsDuelTaboo(Self) && IsInRealFight(Self, Other))
            return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        bool const canShare = Hero != None && Other == Hero &&
            a_iArgs == gEAttackReason_Enemy && !FactionLogic::IsPursuing(Self, Hero);
        GEInt const result = GroupLogic::Combat::ResolveTarget(Self, Other, Hero, original, a_pSPU,
            a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if (canShare)
            FactionLogic::ShareHostileTarget(Self, Hero);
        return result;
    }
    GEInt IsDeadlyDamage(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if (!iResult)
            return iResult;
        Entity Victim = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Source = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        Entity Attacker = GetDamageOwnerNpc(Source);
        if (IsPersonalBrawlPair(Attacker, Victim))
        {
            if (PlayerSupportsPersonalKnockout(Victim) && !IsFatalPersonalProjectile(Source, Victim))
                return 0; // A KO-capable player is safe from melee; projectiles and non-KO forms stay native.
            return iResult; // Companion mortality remains fully native.
        }
        // v1.0.57.0: THE IRON IMMUNITY: no blow (a one-hand thrust, a stab, a control effect) is deadly for the hero who is down or
        // only rising - whoever deals it and whatever the mercy says.
        if (Victim != None && Victim == Entity::GetPlayer() && UpdateHeroImmunity())
        {
            static GEInt s_iImmuneLines = 0;
            if (s_iImmuneLines < 40)
            {
                ++s_iImmuneLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] IsDeadlyDamage: the blow of '%s' on the hero who is down / rising is "
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
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] IsDeadlyDamage: the blow of '%s' (party type %d) on his master "
                                             "is not deadly.\n",
                                             Attacker.GetName().GetText(), static_cast<int>(Attacker.Party.PartyMemberType));
            }
            return 0;
        }
        return FollowerMustSpare(Attacker, Victim, "IsDeadlyDamage") ? 0 : iResult;
        }
    GEInt GetAttitude(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        PollFleeWatch(); // diagnostics only
        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (Self == None || Other == None || !Self.IsNPC() || Self == Other)
            return iResult;
        auto const finish = [&](GEInt attitude) {
            DebugLog::LogFactionCreatureTarget(Self, Other, "attitude", -1, iResult, attitude);
            return attitude;
        };
        if (IgnoreArenaPunishmentTarget(Self, Other))
            return finish(gEAttitude_Neutral);
        if (SocialLogic::IsAwaitingSecondAssault(Self, Other)) return finish(gEAttitude_Friendly);
        GEInt adjusted;
        if (FactionLogic::AdjustNpcAttitude(Self, Other, adjusted)) return finish(adjusted);
        if (Other == Entity::GetPlayer() && FactionLogic::AdjustAttitudeToPlayer(Self, iResult, adjusted))
            return finish(adjusted);
        if (SocialLogic::AdjustPoliticalAttitude(Self, Other, iResult, adjusted)) return finish(adjusted);
        if ((iResult == gEAttitude_Hostile || iResult == gEAttitude_Predator) &&
            FactionLogic::IsPeacefulSettlementCreature(Self, Other))
            return finish(gEAttitude_Neutral);
        // The fight has started: an outsider is in a real fight with the hero / the army (he hit one of them, or one of them
        // hit him) - the plugin touches nothing, the native attitude and the native answer of the squad decide.
        if (!IsDuelTaboo(Other) && !IsDuelTaboo(Self) && IsInRealFight(Self, Other))
            return finish(iResult);
        return finish(GroupLogic::Combat::AdjustAttitude(Self, Other, iResult));
    }
    GEInt GetAttitudeToPlayer(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        GEInt adjusted;
        if (FactionLogic::AdjustAttitudeToPlayer(Self, iResult, adjusted))
            return adjusted;
        if (iResult != gEAttitude_Angry && iResult != gEAttitude_Hostile && iResult != gEAttitude_Predator &&
            iResult != gEAttitude_Panic)
            return iResult;
        if (Self == None || !Self.IsNPC())
            return iResult;
        if (IsHeroControlled(Self) && !IsBrawlFighting(Self))
            return gEAttitude_Friendly;
        if (SocialLogic::AdjustPlayerPoliticalAttitude(Self, adjusted)) return adjusted;
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (Other != None && IsPrimaryAggressionOnSummon(Self, Other))
            return gEAttitude_Neutral;
        return iResult;
        }
    GEInt GetFocusNameAttitude(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (Self != None && Self.NPC.IsValid() && Self.NPC.PoliticalAlignment == gEPoliticalAlignment_None)
            return iResult;
        GEInt adjusted;
        if (FactionLogic::AdjustAttitudeToPlayer(Self, iResult, adjusted))
            return adjusted;
        if (Self != None && Self.IsNPC() && IsHeroControlled(Self))
            return gEAttitude_Friendly;
        return iResult;
        }
}

namespace PluginRuntime::Combat
{
    GEInt AssessDefeat(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (FactionLogic::SuppressCombatTarget(Self, Other))
            return 0;
        if (IsHeroSide(Other) && IsArenaPunishmentActor(Self))
        {
            LogArenaPunishmentFilter(Self, Other, "AssessDefeat: no new crime at hero defeat");
            return 0;
        }
        if (IsMasterDownVictim(Self, Other))
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AssessDefeat: '%s' (party type %d) ignores the fall of his master.\n",
                                             Self.GetName().GetText(), static_cast<int>(Self.Party.PartyMemberType));
            }
            return 0;
        }
        return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
    GEInt HasAttackReasonToKill(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if (!iResult)
            return iResult;

        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        Entity Hero = Entity::GetPlayer();
        if (IsPersonalBrawlPair(Self, Other))
            return PlayerSupportsPersonalKnockout(Other) ? 0 : iResult;
        // The creatures of the party never strike the final blow at their fallen master.
        if (IsMasterDownVictim(Self, Other))
            return 0;
        // A summon / companion of the hero does not finish off an NPC who is not red for the hero.
        if (FollowerMustSpare(Self, Other, "HasAttackReasonToKill"))
            return 0;
        if (Self == None || Self == Hero || !(Other == Hero) || !Self.IsNPC())
            return iResult;

        return ArenaJudge::Combat::FinishHeroDecision(Self, iResult);
    }
    GEInt WantToRansack(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        Entity Hero = Entity::GetPlayer();
        // The creatures of the party never rob their fallen master.
        if (IsMasterDownVictim(Self, Other))
            return 0;
        if (Self == None || Self == Hero || !(Other == Hero) || !Self.IsNPC())
            return iResult;

        return SocialLogic::AdjustRansack(Self, Hero, iResult);
    }
}

namespace PluginRuntime::Loops
{
    using namespace GroupLogic::Brawl;
    using GroupLogic::IsHeroControlled;
    using ArenaJudge::Punishment::FinishIfHeroDead;
    using SocialLogic::ReleaseWatcherIfOver;
    namespace {

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
    }
    GEBool ZS_Attack_Loop(gFScriptAIState original, bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU)
    {

        Entity fighter(a_pSPU->GetSelfEntity());
        if (FactionLogic::ReleaseCombatTarget(fighter) || SocialLogic::HoldFirstAssaultWarning(fighter))
            return GETrue;
        if (FinishIfHeroDead()) return GETrue;

        // v1.0.66.0: the stale target (the removed body of the hero after a transformation) is repaired before anything reads it.
        {
            Entity Fighter(a_pSPU->GetSelfEntity());
            if (ArenaJudge::Loops::FixStaleHeroTarget(Fighter))
                return GETrue;
        }
        // The master is knocked out: a creature of his party that was fighting him stops at once (no finishing blow).
        {
            Entity Fighter(a_pSPU->GetSelfEntity());
            if (StandDownFromMaster(Fighter, "ZS_Attack_Loop"))
                return GETrue;
            // v1.0.41.0: the opponent in the warning stance (the magic in the hand of the hero) is put back into the stance.
            // v1.0.47.0: and he is put there BEFORE the first blow when the magic is already in the hand.
            if (Fighter != None && ArenaJudge::Loops::HoldWarning(Fighter))
                return GETrue;
        }
        if (ArenaJudge::Loops::ReleaseInvalidPunishmentTarget(a_pSPU)) return GETrue;
        // The event "the hero stands again" is noticed here too: after the hero got up nobody asks about killing him.
        PluginRuntime::Conflict::SpareHousekeeping();
        ArenaJudge::Loops::PollCompanions();
        if (ArenaJudge::Loops::ReleaseProtectedPartyTarget(a_pSPU)) return GETrue;
        if (PluginRuntime::Conflict::ReleaseAggroAfterRecovery(a_pSPU)) return GETrue;
        GEBool bFaulted = GEFalse;
        GEBool const bResult = CallAttackLoopGuarded(original, a_rRunTimeStack, a_pSPU, &bFaulted);
        if (bFaulted)
        {
            // v1.0.66.0: the native loop faulted (a target that is not there any more): the attack is dropped, the game goes on.
            Entity Faulty(a_pSPU->GetSelfEntity());
            static GEInt s_iFaultLines = 0;
            if (s_iFaultLines < 30)
            {
                ++s_iFaultLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] ZS_Attack_Loop: the native loop of '%s' faulted (access violation) - the "
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
    GEBool ZS_WatchFight_Loop(gFScriptAIState original, bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU)
    {

        Entity Self(a_pSPU->GetSelfEntity());
        ArenaJudge::Loops::PollCompanions();
        if (FactionLogic::StopCitizenWatching(Self))
            return GETrue;
        if (Self != None && ReleaseWatcherIfOver(Self))
            return GETrue;

        GEBool const bResult = original(a_rRunTimeStack, a_pSPU);

        SocialLogic::AlignWatcherAfterLoop(Self);
        return bResult;
        }
    GEBool ZS_Arena_Loop(gFScriptAIState original, bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU)
    {

        ArenaJudge::Loops::PollCompanions();
        {
            Entity Gladiator(a_pSPU->GetSelfEntity());
            // v1.0.47.0: BEFORE the native loop sends him to the hero: the hero is in the ring with the magic in his hand ->
            // the stance of the warning at once, the order to come closer is never carried out.
            if (Gladiator != None && ArenaJudge::Loops::HoldWarning(Gladiator))
                return GETrue; // the magic in the hand of the hero: the stance, not the arena routine
        }
        return original(a_rRunTimeStack, a_pSPU);
        }
}

namespace PluginRuntime::Conflict
{
    using PluginRuntime::HeroState::IsHeroKnockedOut;
    using PluginRuntime::HeroState::UpdateHeroImmunity;
    using PluginRuntime::HeroState::IsHeroDownOrRising;
    using PluginRuntime::HeroState::ResetHeroImmunity;
    using ArenaJudge::Punishment::FinishIfHeroDead;
    namespace
    {
        GEBool g_bSpareArmed = GEFalse;
        GEBool g_bSpareActive = GEFalse; // a protected knock-out is going on: the fall point below is valid
        GEBool g_bSpareHeroSeenDown = GEFalse; // v1.0.54.0: the hero has really lain down since the fall was remembered
        GEU32 g_u32SpareFallTick = 0;          // v1.0.54.0: the moment the fall was remembered
        GEFloat g_fFallX = 0.0f;
        GEFloat g_fFallY = 0.0f;
        GEFloat g_fFallZ = 0.0f;
        GEBool g_bSpareSawEnemy = GEFalse;
        GEBool g_bSpareArena = GEFalse;
        GEU32 g_u32SpareScan = 0; // technical throttle of the scan below (not a game timing)
        bCString g_arrAggroNames[AGGRO_MAX];
        GEInt g_iAggroCount = 0;


        GEBool g_bAggroRelease = GEFalse;
    }
    GEBool IsArmed() { return g_bSpareArmed; }
    GEBool IsFallActive() { return g_bSpareActive; }
    GEBool IsArenaPunishment() { return g_bSpareArena; }
    void BeginNewFall() { g_bSpareActive = GEFalse; }

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
            ArenaJudge::ConflictHooks::ClearWitnesses();
            g_bAggroRelease = GEFalse;
        }





        void ArmSpare(GELPCChar a_strWhy, GEBool a_bArena)
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
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Mercy: the old fall of the hero is over (he stands) - a new mercy starts.\n");
                ResetSpare();
                ResetHeroImmunity();
            }
            if (!g_bSpareArmed)
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Mercy armed: %s.\n", a_strWhy);
            g_bSpareArmed = GETrue;
            if (a_bArena)
                g_bSpareArena = GETrue;
            if (!IsHeroKnockedOut())
                ArenaJudge::ConflictHooks::ResetDuelLoss(); // a new violation: the next knock-out ends the duel again
        }

        void ResetSpare()
        {
            ArenaJudge::ConflictHooks::ResetBeforeMercy();
            g_bSpareArmed = GEFalse;
            g_bSpareActive = GEFalse;
            g_bSpareHeroSeenDown = GEFalse;
            g_u32SpareFallTick = 0;
            g_bSpareSawEnemy = GEFalse;
            g_bSpareArena = GEFalse;
            ArenaJudge::ConflictHooks::ResetDuelLoss();
        }

        // The hero stands again after a protected knock-out: the mercy and the fall point are dropped at once.
        // Armed but not yet used: it ends when somebody fought the hero and now nobody does.
        void SpareHousekeeping()
        {
            if (FinishIfHeroDead()) return;

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
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Mercy ended: the hero is on his feet again; the NPCs sent "
                                             "against him (%d) calm down.\n",
                                             static_cast<int>(g_iAggroCount));
                if (g_bSpareArena) SocialLogic::ClearPunishedConflict();
                ResetSpare();
                g_bAggroRelease = g_iAggroCount > 0;
                ResetBlowHistory("Mercy ended: the hero is on his feet again"); // v1.0.51.0: a new fight starts from scratch
                return;
            }
            // The knock-out of the hero is noticed here as well, not only in Defeat: a knock-out by a witness does not go
            // through the Defeat hook (no log line in the game), and the duel must end whoever knocked the hero out.
            if (g_bSpareArmed && !g_bSpareActive && IsHeroKnockedOut())
            {
                Entity hero = Entity::GetPlayer();
                if (!g_bSpareArena && hero != None)
                {
                    Entity attacker = hero.NPC.GetCurrentAttacker();
                    SocialLogic::CapturePunishedConflict(attacker);
                }
                RememberHeroFall();
                return;
            }
            if (!g_bSpareArmed || g_bSpareActive || IsHeroKnockedOut())
                return;
            // v1.0.67.0: between the cast of the transformation and the appearance of the form body nobody has the NEW body as the target
            // yet (they still aim at the removed PC_Hero): that is not "nobody fights the hero any more", the mercy must not end here.
            if (ArenaJudge::ConflictHooks::IsFormTransitionPending())
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
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Mercy ended: nobody fights the hero any more.\n");
                SocialLogic::ClearPunishedConflict();
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
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The hero fell at (%.0f, %.0f, %.0f): nobody within %.0f may "
                                         "kill him.\n",
                                         static_cast<double>(g_fFallX), static_cast<double>(g_fFallY),
                                         static_cast<double>(g_fFallZ), static_cast<double>(SPARE_RADIUS));
            // A violation of the arena rules (not the city): the duel is over for everybody, the opponent leaves.
            if (a_bEndDuel && g_bSpareArena)
                ArenaJudge::ConflictHooks::EndDuelAfterKnockout();
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
    GEBool ReleaseAggroAfterRecovery(gCScriptProcessingUnit *a_pSPU)
    {
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
                if (Self.NPC.AttackReason == gEAttackReason_ReactToDamage)
                {
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] '%s': discarded old stand-down; new damage reaction continues.\n",
                                                 Self.GetName().GetText());
                    return GEFalse;
                }
                Self.Routine.FullStop();
                Self.NPC.SetCurrentTarget(None);
                Self.NPC.AttackReason = gEAttackReason_None;
                Self.Routine.ContinueRoutine();
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] '%s' calms down: the hero is on his feet again (%d left).\n",
                                             Self.GetName().GetText(), static_cast<int>(g_iAggroCount));
                return GETrue;
            }
        }
        return GEFalse;
    }
}

namespace SocialLogic::Host
{
    void NoteAggro(Entity &npc) { PluginRuntime::Conflict::NoteAggro(npc); }
    void ArmSocialSpare(GELPCChar why) { PluginRuntime::Conflict::ArmSpare(why); }
}

namespace PluginRuntime::Loops
{
    namespace { Entity ResolveEntity(GELPVoid entity, eCEntity *fallback) { return entity ? *static_cast<Entity *>(entity) : Entity(fallback); } }
    GEInt OnTick(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (Self == None || !Self.IsNPC() || Self == Entity::GetPlayer())
            return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        PluginRuntime::OnNpcTick(Self);

        return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
}

namespace PluginRuntime::Native
{
    using namespace GroupLogic::Combat;
    using GroupLogic::IsHeroControlled;
    using PluginRuntime::HeroState::IsHeroKnockedOut;
    void NpcSetCurrentAttacker(PSNpc *a_pThis, Entity const *a_pAttacker, NpcSetCurrentAttackerOriginal original)
    {

            using mFOriginal = void(GE_STDCALL *)(PSNpc *, Entity const *);
            original(a_pThis, a_pAttacker);
            ArenaJudge::Native::OnHeroAttackerSet(a_pThis, a_pAttacker, "PSNpc::SetCurrentAttacker");
            LogAttackerSet("PSNpc::SetCurrentAttacker", a_pThis, a_pAttacker);
            OnArmyAttacked(a_pThis, a_pAttacker);
            }
    void NpcSetLastAttacker(PSNpc *a_pThis, Entity const *a_pAttacker, NpcSetLastAttackerOriginal original)
    {

            using mFOriginal = void(GE_STDCALL *)(PSNpc *, Entity const *);
            original(a_pThis, a_pAttacker);
            ArenaJudge::Native::OnHeroAttackerSet(a_pThis, a_pAttacker, "PSNpc::SetLastAttacker");
            LogAttackerSet("PSNpc::SetLastAttacker", a_pThis, a_pAttacker);
            OnArmyAttacked(a_pThis, a_pAttacker);
            }
    void EntityDoDamage(Entity *a_pThis, Entity const *a_pSource, GEU32 a_u32Amount,
                                       gEDamageType a_enuType, EntityDoDamageOriginal original)
    {

            using mFOriginal = void(GE_STDCALL *)(Entity *, Entity const *, GEU32, gEDamageType);
            if (a_pThis && a_pSource && a_pThis->GetInstance() && a_pSource->GetInstance())
            {
                Entity Victim(*a_pThis);
                Entity Source(*a_pSource);
                Entity Attacker = GetDamageOwnerNpc(Source);
                if (FactionLogic::IsJoiningLocalNoneFight(Attacker, Victim))
                {
                    ZeroPendingDamage(Victim);
                    return;
                }
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
            Entity socialVictim = a_pThis ? *a_pThis : Entity();
            Entity socialSource = a_pSource ? *a_pSource : Entity();
            PollSocialRecovery();
            SocialLogic::ImpactScope social(socialVictim, socialSource);
            Entity attacker = GetDamageOwnerNpc(socialSource);
            FactionLogic::WitnessImpactScope witnessImpact(socialVictim, attacker);
            original(a_pThis, a_pSource, a_u32Amount, a_enuType);
            }
}

namespace PluginRuntime
{
    //
    // Configuration: <Gothic 3 folder>\Ini\GenomeLogicFix.ini, section [Main], line "PunishArenaPotions=true".
    //
    namespace
    {
        GELPCChar const INI_FILE_NAME = "GenomeLogicFix.ini";
        GELPCChar const INI_SECTION = "Main";
        GELPCChar const INI_KEY_PUNISH_POTIONS = "PunishArenaPotions";
        GELPCChar const INI_KEY_IGNORE_TELEPORT = "IgnoreTeleportNames"; // v1.0.51.0
        GELPCChar const INI_MISSING_MARK = "<<missing>>";                // tells "no such key" from "the key is empty"
        GELPCChar const INI_KEY_ENCHANTED = "AllowArenaEnchantedWeapons"; // v1.0.52.0
        GELPCChar const INI_KEY_HARD_SPEED = "EnableHardDifficultyAttackSpeed"; // v1.0.59.0
        GELPCChar const INI_KEY_MIN_HIT = "DisableMinHitDurationOnHard";        // v1.0.59.0
        GELPCChar const INI_KEY_DIAGNOSTICS = "Diagnostics";                 // v1.0.66.0
        GELPCChar const INI_KEY_NPC_COMBAT_DIAGNOSTICS = "NpcCombatDiagnostics";

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
        GEBool const bCrashDiag = ::WritePrivateProfileStringA(INI_SECTION, INI_KEY_DIAGNOSTICS,
                                                               g_Config.Diagnostics ? "true" : "false", strPath)
                                      ? GETrue
                                      : GEFalse;
        GEBool const bNpcCombatDiag = ::WritePrivateProfileStringA(INI_SECTION, INI_KEY_NPC_COMBAT_DIAGNOSTICS,
            g_Config.NpcCombatDiagnostics ? "true" : "false", strPath);
        return bPotions && bNames && bEnchanted && bHardSpeed && bMinHit && bCrashDiag && bNpcCombatDiag ? GETrue : GEFalse;
    }

    void LoadConfig()
    {
        char strPath[MAX_PATH + 64] = {};
        BuildIniPath(strPath, sizeof(strPath));
        NpcHealth::LoadConfig(strPath);
        FactionLogic::LoadConfig(strPath);
        g_Config.FactionDiagnostics = DebugLog::LoadFactionDiagnostics(strPath);

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
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] PunishArenaPotions=%s\n",
                                         g_Config.PunishArenaPotions ? "true" : "false");
        }

        // v1.0.51.0: IgnoreTeleportNames. An empty value is a valid answer ("nobody is blacklisted"), a missing key is not.
        char strNames[sizeof(g_Config.IgnoreTeleportNames)] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_IGNORE_TELEPORT, INI_MISSING_MARK, strNames, sizeof(strNames), strPath);
        if (strcmp(strNames, INI_MISSING_MARK) == 0)
            bWrite = GETrue; // keep the default of the structure and write it
        else
            strncpy_s(g_Config.IgnoreTeleportNames, sizeof(g_Config.IgnoreTeleportNames), strNames, _TRUNCATE);
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix] IgnoreTeleportNames=%s\n", g_Config.IgnoreTeleportNames);

        // v1.0.52.0: AllowArenaEnchantedWeapons (default true; a missing key is written with the default).
        char strEnchanted[32] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_ENCHANTED, "", strEnchanted, sizeof(strEnchanted), strPath);
        if (strEnchanted[0] == 0)
            bWrite = GETrue;
        else
            g_Config.AllowArenaEnchantedWeapons = ParseIniBool(strEnchanted);
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AllowArenaEnchantedWeapons=%s\n",
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
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix] EnableHardDifficultyAttackSpeed=%s, DisableMinHitDurationOnHard=%s\n",
                                     g_Config.EnableHardDifficultyAttackSpeed ? "true" : "false",
                                     g_Config.DisableMinHitDurationOnHard ? "true" : "false");

        // Diagnostics is off unless explicitly enabled.
        char strCrashDiag[32] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_DIAGNOSTICS, "", strCrashDiag, sizeof(strCrashDiag), strPath);
        if (strCrashDiag[0] == 0)
            bWrite = GETrue;
        else
            g_Config.Diagnostics = ParseIniBool(strCrashDiag);
        char strNpcCombatDiag[32] = {};
        ::GetPrivateProfileStringA(INI_SECTION, INI_KEY_NPC_COMBAT_DIAGNOSTICS, "",
                                   strNpcCombatDiag, sizeof(strNpcCombatDiag), strPath);
        if (strNpcCombatDiag[0] == 0)
            bWrite = GETrue;
        else
            g_Config.NpcCombatDiagnostics = ParseIniBool(strNpcCombatDiag);
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Diagnostics=%s\n", g_Config.Diagnostics ? "true" : "false");

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

}

namespace PluginRuntime::HeroState
{
    using PluginRuntime::Conflict::SPARE_FALL_GRACE_MS;


        GEBool IsHeroKnockedOut()
        {
            Entity Hero = Entity::GetPlayer();
            return Hero.IsDown() || Hero.Routine.AIMode == gEAIMode_Down;
        }
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
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The hero is down: nobody can kill him until he has risen.\n");
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
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The hero is down: nobody can kill him until he has risen.\n");
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
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The hero has risen (state '%s'%s): the immunity is over.\n",
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

}

namespace PluginRuntime
{
    void OnHeroConsumeStarted() { ArenaJudge::Combat::OnHeroConsumeStarted(); }
}
