#pragma once
#include <g3sdk/Script.h>
#include <g3sdk/util/Hook.h>

// Shared startup and session lifecycle inside the existing DLL.
namespace PluginRuntime
{
    // Genome Logic Fix configuration, file <Gothic 3>\Ini\GenomeLogicFix.ini (section [Main]).
    // The file is created with the defaults when it does not exist.
    struct Config
    {
        // true (default): using a consumable (food, meat, potion) during a PHYSICALLY started arena fight is a defeat
        // ("Выход за пределы арены"). false: the rule is off, healing in the arena is allowed.
        // INI line: PunishArenaPotions=true
        GEBool PunishArenaPotions = GETrue;

        // v1.0.51.0: the black list of the teleport: the names (Entity::GetName(), the native name, NOT the name over the head),
        // separated by commas / semicolons, any case. A companion of the party types 1..3 whose name is in the list is NEVER
        // carried to the hero through a teleport stone / rune: he stays at the old place and waits natively (WaitPlayerParty),
        // whether he was following the hero or not. The two names are the example of the format.
        // INI line: IgnoreTeleportNames=Nefarius,Orknarok
        char IgnoreTeleportNames[512] = "Nefarius,Orknarok";

        // v1.0.52.0: true (default): burning / freezing of the arena opponent that comes from an ENCHANTED MELEE WEAPON of the hero
        // (a fire / ice sword: the hero is not casting, did not cast and did not shoot just now) is NOT a violation - the duel goes
        // on as the normal melee duel. The spells themselves (the cast states) and the bow / crossbow stay forbidden. false: every
        // burning / freezing of the opponent is the crime of the arena, as before.
        // INI line: AllowArenaEnchantedWeapons=true
        GEBool AllowArenaEnchantedWeapons = GETrue;

        // v1.0.59.0: false (default): the native x1.25 speed-up of the animations (attacks, parades, stumbles) of the arena opponent on the
        // HARD difficulty is removed (the code patch of Script_Game.dll and the clamp of SetTimeScale). true: the native speed-up returns.
        // The change of this key is read at the start of the game.
        // INI line: EnableHardDifficultyAttackSpeed=false
        GEBool EnableHardDifficultyAttackSpeed = GEFalse;

        // v1.0.59.0: false (default): the game may NOT skip the pause between two blows (MinHitDuration of ge3.ini) of the arena
        // opponent (the native CanHit skips it for the attack reason Arena): the opponent strikes with the honest pauses of ge3.ini and
        // of the templates of the NPCs. true: the native machine-gun spam of blows returns.
        // INI line: DisableMinHitDurationOnHard=false
        GEBool DisableMinHitDurationOnHard = GEFalse;

        GEBool Diagnostics = GEFalse;
        GEBool NpcCombatDiagnostics = GEFalse;
        GEBool FactionDiagnostics = GEFalse;
    };

    Config &GetConfig();
    // Reads the INI (writes it with the defaults if missing or if the key is absent).
    void LoadConfig();
    // Writes the current configuration.
    GEBool SaveConfig();
    // v1.0.51.0: GETrue, if the native name of an NPC is in Config::IgnoreTeleportNames (any case, spaces around are cut).
    GEBool IsTeleportNameIgnored(GELPCChar a_strName);

    void OnHeroConsumeStarted();

    struct NativeHookBinding
    {
        mCFunctionHook *hook;
        GELPCChar symbol;
        GELPVoid replacement;
        GELPCChar module;
    };
    void OnHeroRoutineChange(GELPCChar name, GEBool isTask);
    void OnNpcTick(Entity &npc);
    void BeginKnockout(Entity &victim);
    void EndKnockout(Entity &victim);
    void ObserveKnockout(Entity &victim);
    void OnCityKnockout(Entity &victim, GELPCChar where, Entity *source = nullptr);
    void InstallSharedHooks();
    extern mCFunctionHook Hook_SessionStart;
    extern mCFunctionHook Hook_SessionStop;
    extern mCFunctionHook Hook_SessionProcess;
    void GE_STDCALL SessionStart(gCSession *session, gESession_StartMode mode);
    void GE_STDCALL SessionStop(gCSession *session);
    void GE_STDCALL SessionProcess(gCSession *session);
    void ClearStaleSessionHero(gCSession *session);
    void ResetBlowHistory(GELPCChar why);
    void ResetSharedSessionState();

    namespace Host
    {
        void OnArenaRoutineChange(GELPCChar name, GEBool isTask);
        NativeHookBinding const *GetNativeHookBindings(GEU32 &count);
        void RemoveArenaHardSpeedUp();
        void PatchReactToDamageSvm();
        void ArmArenaAdoptAfterLoad();
        GEBool IsArenaKnockout(Entity &victim);
        void ResetFormAggro();
        void ResetDuelState();
    }
}

namespace PluginRuntime::Combat
{
    GEInt DoLogicalDamage(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt Defeat(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt AssessHit(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt AssessAgressor(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt AssessTarget(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt IsDeadlyDamage(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt GetAttitude(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt GetAttitudeToPlayer(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt GetFocusNameAttitude(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);

    GEInt AssessDefeat(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt HasAttackReasonToKill(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    GEInt WantToRansack(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
}



namespace PluginRuntime::Loops
{
    GEBool ZS_Attack_Loop(gFScriptAIState original, bTObjStack<gScriptRunTimeSingleState> &stack, gCScriptProcessingUnit *spu);
    GEBool ZS_WatchFight_Loop(gFScriptAIState original, bTObjStack<gScriptRunTimeSingleState> &stack, gCScriptProcessingUnit *spu);
    GEBool ZS_Arena_Loop(gFScriptAIState original, bTObjStack<gScriptRunTimeSingleState> &stack, gCScriptProcessingUnit *spu);
}

namespace PluginRuntime::Conflict
{
    inline constexpr GEFloat SPARE_RADIUS = 2500.0f;
    inline constexpr GEU32 SPARE_FALL_GRACE_MS = 8000;
    inline constexpr GEInt AGGRO_MAX = 96;
    GEBool IsArmed();
    GEBool IsFallActive();
    GEBool IsArenaPunishment();
    void BeginNewFall();
    GEInt FindAggro(bCString const &name);
    void NoteAggro(Entity &npc);
    void ClearAggro();
    void ArmSpare(GELPCChar why, GEBool arena = GEFalse);
    void ResetSpare();
    void SpareHousekeeping();
    void RememberHeroFall(GEBool endDuel = GETrue);
    GEBool SpareApplies(Entity &npc, GEFloat &distance);
    GEBool ReleaseAggroAfterRecovery(gCScriptProcessingUnit *spu);
}

namespace PluginRuntime::Loops
{
    GEInt OnTick(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
}

namespace PluginRuntime::Native
{
    using NpcSetCurrentAttackerOriginal = void(GE_STDCALL *)(PSNpc *, Entity const *);
    void NpcSetCurrentAttacker(PSNpc *a_pThis, Entity const *a_pAttacker, NpcSetCurrentAttackerOriginal original);
    using NpcSetLastAttackerOriginal = void(GE_STDCALL *)(PSNpc *, Entity const *);
    void NpcSetLastAttacker(PSNpc *a_pThis, Entity const *a_pAttacker, NpcSetLastAttackerOriginal original);
    using EntityDoDamageOriginal = void(GE_STDCALL *)(Entity *, Entity const *, GEU32, gEDamageType);
    void EntityDoDamage(Entity *a_pThis, Entity const *a_pSource, GEU32 a_u32Amount,
                                       gEDamageType a_enuType, EntityDoDamageOriginal original);
}

namespace PluginRuntime::HeroState
{
    GEBool IsHeroKnockedOut();
    GEBool IsHeroDownOrRising();
    void ResetHeroImmunity();
    void BeginHeroImmunity();
    GEBool UpdateHeroImmunity();
}
