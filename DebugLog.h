#pragma once

#include <g3sdk/util/Logging.h>
#include <g3sdk/Game.h>

class Entity;

// Shared logging entry point; existing message formatting is preserved.
namespace DebugLog
{
    bCString Initialize();
    GEBool IsEnabled();
    bool IsNpcCombatEnabled();
    void FlushNpcCombatDiagnostics(bool gameRunning);
    mCLoggerBase &GetLogger();
    void InstallLootDiagnostics();
    bool LoadFactionDiagnostics(char const *iniPath);
    void InstallRevolutionDiagnostics(bool statusReady);
    GEInt TraceEnclaveStatus(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args);
    void ResetRevolutionMonitoring();
    void PollRevolutionMonitoring();
    void LogRevolutionAttack(Entity &npc, Entity &target, GEInt reason);
    void LogFactionCreatureTarget(Entity &npc, Entity &target, GELPCChar phase,
                                  GEInt reason, GEInt nativeAttitude = -1, GEInt finalAttitude = -1);
    void LogNpcProtection(Entity &attacker, Entity &victim, Entity &source, GEInt difficulty,
                          bool optionEnabled, bool substituted, GEInt protection, GEInt levelUsed);
}
