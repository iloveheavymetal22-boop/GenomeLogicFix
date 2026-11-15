#pragma once

#include <g3sdk/Game.h>

class Entity;

namespace FactionLogic
{
    void LoadConfig(char const *iniPath);
    bool IsPursuing(Entity &npc, Entity &target);
    void ShareHostileTarget(Entity &initiator, Entity &hero);
    void InstallSharedPursuitHook();
    bool InstallEnclaveStatusHook();
    bool AllowsCityWitness(Entity &witness, Entity &victim);
    bool AdjustNpcAttitude(Entity &npc, Entity &other, GEInt &adjusted);
    bool AdjustAttitudeToPlayer(Entity &npc, GEInt nativeAttitude, GEInt &adjusted);
    bool SuppressWitnessAid(Entity &witness, Entity &opponent);
    bool IsJoiningLocalNoneFight(Entity &fighter, Entity &target);
    bool StopCitizenWatching(Entity &citizen);
    bool IgnoresDrawnWeapon(Entity &npc);
    class WitnessImpactScope
    {
        Entity *previousVictim;
        Entity *previousAttacker;
    public:
        WitnessImpactScope(Entity &victim, Entity &attacker);
        ~WitnessImpactScope();
        WitnessImpactScope(WitnessImpactScope const &) = delete;
        WitnessImpactScope &operator=(WitnessImpactScope const &) = delete;
    };
    bool IsNpcInRevolution(Entity &npc);
    bool IsProtectedCitizen(Entity &npc);
    bool SuppressCombatTarget(Entity &npc, Entity &target);
    bool SuppressCitizenIntervention(Entity &witness, Entity &opponent, GEInt reason);
    bool IsPeacefulSettlementCreature(Entity &npc, Entity &creature);
    bool ReleaseCombatTarget(Entity &npc);
    bool IsDown(Entity &npc);
    void RecordImpact(Entity &victim, Entity &attacker);
    void Update();
    void Reset();
}
