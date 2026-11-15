#include "SocialLogic.h"
#include "FactionLogic.h"
#include "PluginRuntime.h"
#include "GroupLogic.h"
#include "GroupBrawl.h"
#include "DebugLog.h"
#include <g3sdk/util/ScriptUtil.h>
#include <g3sdk/util/ScriptInterface.h>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdlib>
#include <cstring>

namespace SocialLogic
{
    using GroupLogic::IsHeroControlled;
    using Host::IsHeroSide;
    using Host::FindArenaOpponent;
    using Host::FindRememberedOpponent;
    using Host::HasRunningArenaQuest;
    using Host::IsHeroInsideArenaZone;
    using Host::HasTask;
    using Host::HasReacted;
    using Host::IsAttackingHero;
    using Host::ForceReaction;
    using Host::NoteAggro;
    using Host::GetWitnessVerdict;
    using Host::ForceAttack;
    namespace { GEFloat const WITNESS_RADIUS = 2500.0f; }
        //
        // Assault of a hero with a high reputation. Script_Game.dll 1.8.7 (GetAttitudeToPlayer / GetZoneAttitudeToPlayer)
        // compares the political fame and the city (enclave) fame of the hero with 0x4B = 75: from 75 up (the maximum is
        // 100) every resident is "Friendly" to him, and the perception scripts (AssessHit / AssessMurder) then ignore
        // the beating of a fellow citizen. Below 75 the game reacts by itself, so the plugin only steps in from 75.
        //

    GEI32 const REPUTATION_MIN = 75;
    GEU32 const REACTION_MS = 10000;
    enum class Cause { Assault = 1, Duel, NativeProvocation, SelfDefence };
    struct Incident
    {
        std::string victim, group, enclave, event;
        GEInt alignment = 0;
        Cause cause = Cause::Assault;
        GEInt stage = 1;
        GEU32 quietAfter = 0;
        GEBool knockoutSeen = GEFalse;
        GEBool weaknessPending = GEFalse;
        std::string projectile;
    };
    static std::vector<Incident> g_incidents;
    static std::vector<std::string> g_impacts;
    struct PendingImpact
    {
        Entity victim;
        GEI32 hitPointsBefore;
        GEU32 expires;
    };
    static std::vector<PendingImpact> g_pendingImpacts;
    static GEBool g_loaded = GEFalse;
    static GEU32 g_pollTick = 0;
    static bCString g_socialVictimID;
    GELPCChar const EVENT_PREFIX = "GLF_SocialV1|";

    std::string Identity(Entity const &npc)
    {
        return npc != None && npc.GetInstance() ? npc.GetInstance()->GetID().GetText().GetText() : "";
    }
    GEBool SourceIsHero(Entity &source)
    {
        Entity owner = GroupLogic::Combat::GetDamageOwnerNpc(source);
        if (owner != None) return IsHeroSide(owner);
        std::vector<Entity> pending{source};
        for (size_t i = 0; i < pending.size() && i < 8; ++i)
        {
            Entity current = pending[i];
            if (current == None) continue;
            if (IsHeroSide(current)) return GETrue;
            if (!current.Interaction.IsValid()) continue;
            Entity user = current.Interaction.GetUser();
            if (user != None && IsHeroSide(user)) return GETrue;
            pending.push_back(current.Interaction.GetOwner());
            pending.push_back(current.Interaction.GetSpell());
        }
        return GEFalse;
    }
    GEBool IsAvailable(Entity &npc)
    {
        return npc != None && npc.GetInstance()->IsCachedIn() && npc.GetInstance()->HasProcessingRangeEntered() &&
            !npc.GetInstance()->IsProcessingDisabled();
    }
    Entity SocialMemoryOwner()
    {
        return GroupLogic::Brawl::BrawlMemoryOwner();
    }
    Entity ResolveIdentity(std::string const &id, bTObjArray<Entity> const &npcs)
    {
        for (GEInt i = 0; i < npcs.GetCount(); ++i)
            if (Identity(npcs.GetAt(i)) == id) return npcs.GetAt(i);
        return None;
    }
    GEInt FindIncident(std::string const &id)
    {
        for (size_t i = 0; i < g_incidents.size(); ++i)
            if (g_incidents[i].victim == id) return static_cast<GEInt>(i);
        return -1;
    }
    void SaveIncident(Incident &record)
    {
        Entity memory = SocialMemoryOwner();
        if (memory == None || !memory.PlayerMemory.IsValid()) return;
        std::string key = std::string(EVENT_PREFIX) + record.victim + "|" + record.group + "|" + record.enclave + "|" +
            std::to_string(record.alignment) + "|" + std::to_string(static_cast<GEInt>(record.cause)) + "|" + std::to_string(record.stage);
        if (record.weaknessPending) key += "|W";
        if (key == record.event) return;
        if (!memory.PlayerMemory.SetGameEvent(bCString(key.c_str())))
        {
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] could not store event for '%s'.\n", record.victim.c_str());
            return;
        }
        if (!record.event.empty()) memory.PlayerMemory.ClearGameEvent(bCString(record.event.c_str()));
        record.event = key;
    }
    void EraseIncident(size_t index)
    {
        Entity memory = SocialMemoryOwner();
        if (memory != None && memory.PlayerMemory.IsValid() && !g_incidents[index].event.empty())
            memory.PlayerMemory.ClearGameEvent(bCString(g_incidents[index].event.c_str()));
        g_incidents.erase(g_incidents.begin() + index);
    }
    void LoadIncidents()
    {
        if (g_loaded) return;
        Entity memory = SocialMemoryOwner();
        if (memory == None || !memory.PlayerMemory.IsValid()) return;
        auto const &events = static_cast<gCPlayerMemory_PS const *>(memory.PlayerMemory.m_pEngineEntityPropertySet)->GetPlayerKnows();
        for (GEInt i = 0; i < events.GetCount(); ++i)
        {
            std::string key = events.GetAt(i).GetText();
            if (key.compare(0, strlen(EVENT_PREFIX), EVENT_PREFIX) != 0) continue;
            std::vector<std::string> fields;
            size_t start = strlen(EVENT_PREFIX), end;
            do { end = key.find('|', start); fields.push_back(key.substr(start, end - start)); start = end + 1; }
            while (end != std::string::npos);
            if ((fields.size() != 6 && (fields.size() != 7 || fields[6] != "W")) || fields[0].empty() || fields[1].empty() ||
                fields[3].size() > 2 || fields[4].size() != 1 || fields[5].size() != 1) continue;
            GEInt alignment = atoi(fields[3].c_str()), cause = atoi(fields[4].c_str()), stage = atoi(fields[5].c_str());
            if (alignment < 0 || alignment > 32 || cause < 1 || cause > 4 || stage < 0 || stage > 2 ||
                FindIncident(fields[0]) >= 0) continue;
            Incident record;
            record.victim = fields[0]; record.group = fields[1]; record.enclave = fields[2]; record.alignment = alignment;
            if (alignment == gEPoliticalAlignment_None)
                record.group = "none:" + record.victim;
            record.cause = static_cast<Cause>(cause); record.stage = stage; record.event = key;
            record.quietAfter = ::GetTickCount() + REACTION_MS;
            record.weaknessPending = fields.size() == 7;
            g_incidents.push_back(record);
        }
        g_loaded = GETrue;
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] restored %u conflict record(s) from native game events.\n",
            static_cast<unsigned>(g_incidents.size()));
    }
    GEBool HasHighReputation(Entity &npc, GEI32 &enclaveFame, GEI32 &politicalFame)
    {
        enclaveFame = politicalFame = -1;
        Entity city = npc.NPC.GetEnclave();
        if (city != None && city.Enclave.IsValid()) enclaveFame = city.Enclave.PlayerFame;
        Entity hero = SocialMemoryOwner();
        if (hero != None && hero.PlayerMemory.IsValid()) politicalFame = hero.PlayerMemory.GetPoliticalFame(npc.NPC.PoliticalAlignment);
        return enclaveFame >= REPUTATION_MIN || politicalFame >= REPUTATION_MIN;
    }
    GEBool IsSocialCandidate(Entity &npc)
    {
        if (npc == None || !npc.IsNPC() || npc.IsDead() || IsHeroSide(npc) || IsHeroControlled(npc) || !AI_Decision::IsHumanoid(npc))
            return GEFalse;
        if (FactionLogic::IsNpcInRevolution(npc)) return GEFalse;
        GEI32 enclave, political;
        return HasHighReputation(npc, enclave, political);
    }
    GEBool ArenaExempt(Entity &npc)
    {
        Entity opponent;
        return (FindArenaOpponent(opponent) && npc == opponent) ||
            (HasRunningArenaQuest() && (IsHeroInsideArenaZone() || (FindRememberedOpponent(opponent) && npc == opponent)));
    }
    GEBool IsDownOrRising(Entity &npc)
    {
        return npc.IsDown() || npc.Routine.AIMode == gEAIMode_Down || HasTask(npc, "ZS_Unconscious") ||
            HasTask(npc, "ZS_LieKnockDown") || HasTask(npc, "ZS_PiercedKO");
    }
    GEBool HasThreat(Entity &npc)
    {
        Entity hero = Entity::GetPlayer();
        Entity target = npc.NPC.GetCurrentTarget();
        bCString const state = npc.Routine.GetCurrentState();
        return IsDownOrRising(npc) || npc.NPC.IsBurning() || npc.NPC.IsFrozen() || npc.NPC.IsPoisoned() || npc.NPC.IsDiseased() ||
            npc.Routine.AIMode == gEAIMode_Flee || HasTask(npc, "ZS_Flee") || HasTask(npc, "ZS_MagicSleep") || HasTask(npc, "ZS_Freeze") ||
            _stricmp(state.GetText(), "ZS_ReactToDamage") == 0 || _stricmp(state.GetText(), "ZS_MagicSleep") == 0 ||
            ((target == hero || (target != None && IsHeroSide(target))) &&
                (npc.NPC.AttackReason != gEAttackReason_None || npc.NPC.LastFightAgainstPlayer == gELastFightAgainstPlayer_Running)) ||
            (hero != None && hero.NPC.GetCurrentTarget() == npc &&
                (_strnicmp(hero.Routine.GetCurrentState().GetText(), "PS_Attack", 9) == 0 ||
                 _strnicmp(hero.Routine.GetCurrentState().GetText(), "PS_Magic", 8) == 0));
    }
    Cause ClassifyCause(Entity &npc)
    {
        auto const reason = static_cast<gEAttackReason>(npc.NPC.AttackReason);
        if (ArenaExempt(npc) || reason == gEAttackReason_Duel || reason == gEAttackReason_Arena) return Cause::Duel;
        switch (reason)
        {
            case gEAttackReason_ReactToWeapon: case gEAttackReason_StopThief: case gEAttackReason_Theft:
            case gEAttackReason_LeaveZone: case gEAttackReason_GateGuard: case gEAttackReason_Intruder:
            case gEAttackReason_UseInteractObject: return Cause::NativeProvocation;
            default: break;
        }
        if ((reason != gEAttackReason_None && reason != gEAttackReason_ReactToDamage) ||
            npc.NPC.LastFightAgainstPlayer == gELastFightAgainstPlayer_Running) return Cause::SelfDefence;
        gEAttitude const attitude = AI_Decision::GetAttitudeToPlayer(npc);
        return attitude == gEAttitude_Friendly || attitude == gEAttitude_Neutral ? Cause::Assault : Cause::SelfDefence;
    }
    void LogImpactDecision(Entity &npc, char const *direction, char const *decision, GEInt index)
    {
        if (!DebugLog::IsEnabled())
            return;
        GEInt const cause = index >= 0 ? static_cast<GEInt>(g_incidents[index].cause) : 0;
        GEInt const stage = index >= 0 ? g_incidents[index].stage : -1;
        GEI32 enclaveFame, politicalFame;
        HasHighReputation(npc, enclaveFame, politicalFame);
        DebugLog::GetLogger().LogFormat(
            "[GenomeLogicFix][social-impact] tick=%u npc='%s' direction=%s decision=%s cause=%d stage=%d "
            "attitude=%d reason=%d lastFight=%d enclaveFame=%d politicalFame=%d\n",
            static_cast<unsigned int>(::GetTickCount()), npc.GetName().GetText(), direction, decision, cause, stage,
            static_cast<int>(AI_Decision::GetAttitudeToPlayer(npc)), static_cast<int>(npc.NPC.AttackReason),
            static_cast<int>(npc.NPC.LastFightAgainstPlayer), enclaveFame, politicalFame);
    }

    GEInt AddIncident(Entity &npc, Cause cause)
    {
        Incident record;
        record.victim = record.group = Identity(npc);
        record.enclave = Identity(npc.NPC.GetEnclave()); record.alignment = npc.NPC.PoliticalAlignment;
        if (record.alignment == gEPoliticalAlignment_None)
            record.group = "none:" + record.victim;
        record.cause = cause; record.stage = 0;
        record.quietAfter = ::GetTickCount() + REACTION_MS;
        g_incidents.push_back(record);
        if (cause != Cause::Assault) SaveIncident(g_incidents.back());
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] '%s' id='%s': conflict cause=%d, reason=%d, stage=%d.\n",
            npc.GetName().GetText(), record.victim.c_str(), static_cast<int>(cause), static_cast<int>(npc.NPC.AttackReason), record.stage);
        return static_cast<GEInt>(g_incidents.size() - 1);
    }
    GEBool IsUnprovokedAssault(Entity &victim, Entity &source)
    {
        if (!IsSocialCandidate(victim) || source == None || !SourceIsHero(source) || ArenaExempt(victim)) return GEFalse;
        LoadIncidents();
        GEInt index = FindIncident(Identity(victim));
        if (index >= 0 && g_incidents[index].cause != Cause::Assault && g_incidents[index].cause != Cause::NativeProvocation && !HasThreat(victim))
        {
            EraseIncident(index);
            index = -1;
        }
        if (index < 0) index = AddIncident(victim, ClassifyCause(victim));
        if (g_incidents[index].cause == Cause::NativeProvocation)
        {
            g_incidents[index].cause = Cause::Assault;
            g_incidents[index].stage = 0;
        }
        return g_incidents[index].cause == Cause::Assault;
    }

    void ConfirmPendingImpacts()
    {
        if (g_pendingImpacts.empty() || !g_impacts.empty())
            return;
        GEU32 const now = ::GetTickCount();
        for (size_t i = 0; i < g_pendingImpacts.size();)
        {
            auto const pending = g_pendingImpacts[i];
            Entity victim = pending.victim;
            auto const index = FindIncident(Identity(victim));
            bool const valid = index >= 0 && g_incidents[index].cause == Cause::Assault &&
                !g_incidents[index].knockoutSeen && IsSocialCandidate(victim) &&
                !ArenaExempt(victim) && victim.DamageReceiver.IsValid();
            GEI32 const after = valid ? static_cast<GEI32>(victim.DamageReceiver.HitPoints) : -1;
            bool const expired = static_cast<GEInt>(now - pending.expires) >= 0;
            if (valid && after >= pending.hitPointsBefore && !expired)
            {
                ++i;
                continue;
            }

            g_pendingImpacts.erase(g_pendingImpacts.begin() + i);
            bool counted = false;
            if (valid && !expired && after < pending.hitPointsBefore)
            {
                Entity attacker = victim.NPC.GetCurrentAttacker();
                if (attacker == None)
                    attacker = victim.NPC.GetLastAttacker();
                counted = attacker != None && SourceIsHero(attacker);
            }
            if (DebugLog::IsEnabled())
                DebugLog::GetLogger().LogFormat(
                    "[GenomeLogicFix][social-impact] npc='%s' deferred hpBefore=%d hpAfter=%d counted=%d expired=%d\n",
                    victim != None ? victim.GetName().GetText() : "-", pending.hitPointsBefore, after,
                    static_cast<int>(counted), static_cast<int>(expired));
            if (counted)
            {
                g_impacts.push_back(Identity(victim));
                OnCityAssault(victim);
                g_impacts.pop_back();
            }
            else if (index >= 0 && g_incidents[index].cause == Cause::Assault && g_incidents[index].stage == 0)
                EraseIncident(index);
        }
    }

    void PollIncidents(GEBool force = GEFalse)
    {
        if (!g_impacts.empty()) return;
        ConfirmPendingImpacts();
        LoadIncidents();
        GEU32 const now = ::GetTickCount();
        if (!force && g_pollTick != 0 && static_cast<GEInt>(now - g_pollTick) < 500) return;
        g_pollTick = now;
        if (g_incidents.empty()) return;
        auto const npcs = Entity::GetNPCs();
        std::unordered_map<std::string, GEInt> entities;
        entities.reserve(npcs.GetCount());
        for (GEInt i = 0; i < npcs.GetCount(); ++i) entities.emplace(Identity(npcs.GetAt(i)), i);
        auto resolve = [&](std::string const &id) -> Entity {
            auto const found = entities.find(id);
            return found != entities.end() ? npcs.GetAt(found->second) : Entity();
        };
        for (size_t i = g_incidents.size(); i-- > 0;)
        {
            Entity npc = resolve(g_incidents[i].victim);
            if (!IsAvailable(npc)) continue;
            if (npc.IsDead()) { EraseIncident(i); continue; }
            if (!IsDownOrRising(npc)) g_incidents[i].knockoutSeen = GEFalse;
        }
        std::vector<std::string> reconciled;
        for (auto const &record : g_incidents)
        {
            if (std::find(reconciled.begin(), reconciled.end(), record.group) != reconciled.end()) continue;
            GEBool ready = GETrue;
            for (auto const &member : g_incidents)
            {
                if (member.group != record.group) continue;
                Entity npc = resolve(member.victim);
                if (!IsAvailable(npc) || HasThreat(npc) || static_cast<GEInt>(now - member.quietAfter) < 0 ||
                    (member.cause == Cause::Assault && (!npc.DamageReceiver.IsValid() ||
                        static_cast<GEInt>(npc.DamageReceiver.HitPointsMax) <= 0 ||
                        static_cast<GEInt>(npc.DamageReceiver.HitPoints) < static_cast<GEInt>(npc.DamageReceiver.HitPointsMax))))
                { ready = GEFalse; break; }
            }
            if (ready) reconciled.push_back(record.group);
        }
        for (size_t i = g_incidents.size(); i-- > 0;)
            if (std::find(reconciled.begin(), reconciled.end(), g_incidents[i].group) != reconciled.end())
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] '%s': reconciled after recovery and end of threat.\n", g_incidents[i].victim.c_str());
                EraseIncident(i);
            }
    }
    bool IsAwaitingSecondAssault(Entity &npc, Entity &target)
    {
        if (npc == None || target == None || !npc.NPC.IsValid() || !IsHeroSide(target) ||
            FactionLogic::IsNpcInRevolution(npc) || ArenaExempt(npc) || npc.IsDead() || IsDownOrRising(npc))
            return false;
        auto const id = Identity(npc);
        auto const index = FindIncident(id);
        if (index < 0)
            return false;
        auto const &incident = g_incidents[index];
        return incident.cause == Cause::Assault && !incident.weaknessPending &&
               (incident.stage == 1 || (incident.stage == 0 &&
                (std::find(g_impacts.begin(), g_impacts.end(), id) != g_impacts.end() ||
                 std::any_of(g_pendingImpacts.begin(), g_pendingImpacts.end(),
                    [&](PendingImpact const &pending) { return pending.victim == npc; }))));
    }

    bool HoldFirstAssaultWarning(Entity &npc)
    {
        if (npc == None || !npc.NPC.IsValid() || !npc.Routine.IsValid() || !HasTask(npc, "ZS_Attack"))
            return false;
        Entity target = npc.NPC.GetCurrentTarget();
        if (!IsAwaitingSecondAssault(npc, target))
            return false;

        npc.Routine.FullStop();
        npc.NPC.SetCurrentTarget(target);
        npc.NPC.AttackReason = gEAttackReason_None;
        npc.NPC.CombatState = 0;
        npc.Routine.SetTask("ZS_ReactToDamage");
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] '%s': first assault returned to warning.\n",
                                       npc.GetName().GetText());
        return true;
    }

    void ObserveTarget(Entity &npc, Entity &target, GEInt reason)
    {
        if (!IsSocialCandidate(npc) || !IsHeroSide(target)) return;
        LoadIncidents();
        if (FindIncident(Identity(npc)) >= 0 || reason == gEAttackReason_ReactToDamage ||
            reason == gEAttackReason_None || reason == gEAttackReason_StopFight) return;
        Cause cause = Cause::SelfDefence;
        if (reason == gEAttackReason_Duel || reason == gEAttackReason_Arena) cause = Cause::Duel;
        else if (reason == gEAttackReason_ReactToWeapon || reason == gEAttackReason_StopThief || reason == gEAttackReason_Theft ||
            reason == gEAttackReason_LeaveZone || reason == gEAttackReason_GateGuard || reason == gEAttackReason_Intruder ||
            reason == gEAttackReason_UseInteractObject) cause = Cause::NativeProvocation;
        AddIncident(npc, cause);
    }

        // Presentation state only; saved incidents control escalation and reconciliation.
        enum mECityPhase
        {
            mECityPhase_Idle = 0,
            mECityPhase_Watching = 1,
            mECityPhase_Aggro = 2,
        };

        GEU32 const WATCH_MS = 10000;  // spectator duration, not warning expiry
        GEInt const WATCH_MAX = 64;
        static mECityPhase g_enuCityPhase = mECityPhase_Idle;
        static GEU32 g_u32AggroUntil = 0;
        static GEU32 g_u32LastAggroPass = 0;
        static bCString g_arrWatchNames[WATCH_MAX];
        static GEU32 g_arrWatchUntil[WATCH_MAX] = {};
        static GEInt g_iWatchCount = 0;

    static std::unordered_map<std::string, std::string> g_punishmentByNpc;
    static std::string g_punishedGroup;
    static std::unordered_map<std::string, std::string> g_watchAttackRetries;
    static GEBool g_resumedSpare = GEFalse;
    constexpr GEU32 WitnessTraceDurationMs = 3000;
    struct WitnessTrace
    {
        bCString task, state, target;
        GEInt reason = -1;
        GEInt mode = -1;
        GEInt combat = -1;
        GEBool overlay = GEFalse;
    };
    static std::unordered_map<std::string, WitnessTrace> g_witnessTrace;
    static GEU32 g_witnessTraceStart = 0;

    void SampleWitnessTrace(Entity &npc, std::string const &id, WitnessTrace &trace, GEU32 elapsed, GELPCChar phase)
    {
        bCString const task = npc.Routine.GetCurrentTask();
        bCString const state = npc.Routine.GetCurrentState();
        Entity target = npc.NPC.GetCurrentTarget();
        std::string const targetId = Identity(target);
        bCString const targetKey(targetId.c_str());
        GEInt const reason = static_cast<GEInt>(npc.NPC.AttackReason);
        GEInt const mode = static_cast<GEInt>(npc.Routine.AIMode);
        GEInt const combat = static_cast<GEInt>(npc.NPC.CombatState);
        auto const *animation = npc.Animation.IsValid()
            ? static_cast<eCVisualAnimation_PS const *>(npc.Animation.m_pEngineEntityPropertySet) : nullptr;
        auto const *actor = animation ? animation->GetActor() : nullptr;
        GEBool const overlay = actor && actor->IsActorPlaying(static_cast<eCWrapper_emfx2Actor::eEMotionType>(4));
        if (!phase && trace.task == task && trace.state == state && trace.target == targetKey &&
            trace.reason == reason && trace.mode == mode && trace.combat == combat && trace.overlay == overlay) return;

        trace = {task, state, targetKey, reason, mode, combat, overlay};
        DebugLog::GetLogger().LogFormat(
            "[GenomeLogicFix][witness-trace] +%u ms %s npc='%s' id='%s' task='%s' state='%s' target='%s' targetId='%s' reason=%d mode=%d combat=%d overlay=%d.\n",
            static_cast<unsigned>(elapsed), phase ? phase : "changed", npc.GetName().GetText(), id.c_str(),
            task.GetText(), state.GetText(), target != None ? target.GetName().GetText() : "-", targetId.c_str(), reason, mode, combat, static_cast<int>(overlay));
    }

    void ObserveWitnessTrace(Entity &npc)
    {
        if (g_witnessTrace.empty()) return;
        GEU32 const elapsed = ::GetTickCount() - g_witnessTraceStart;
        if (elapsed > WitnessTraceDurationMs + 1000)
        {
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][witness-trace] expired with %u witness(es) awaiting a final tick.\n",
                static_cast<unsigned>(g_witnessTrace.size()));
            g_witnessTrace.clear();
            return;
        }
        std::string const id = Identity(npc);
        auto const found = g_witnessTrace.find(id);
        if (found == g_witnessTrace.end()) return;
        GEBool const finished = elapsed >= WitnessTraceDurationMs;
        SampleWitnessTrace(npc, id, found->second, elapsed, finished ? "final" : nullptr);
        if (finished) g_witnessTrace.erase(found);
    }

    void ObserveNpc(Entity &npc)
    {
        if (npc == None || !npc.IsNPC()) return;
        ObserveWitnessTrace(npc);
        PollIncidents();
        if (std::none_of(g_incidents.begin(), g_incidents.end(), [](Incident const &record) {
            return record.cause == Cause::Assault && record.stage == 2;
        })) return;
        Entity hero = Entity::GetPlayer();
        if (hero == None || npc.GetDistanceTo(hero) > WITNESS_RADIUS || !Host::IsAttackingHero(npc, hero)) return;
        auto const npcs = Entity::GetNPCs();
        for (auto const &record : g_incidents)
        {
            if (record.cause != Cause::Assault || record.stage != 2) continue;
            Entity victim = ResolveIdentity(record.victim, npcs);
            if (!IsAvailable(victim) || victim.GetDistanceTo(hero) > WITNESS_RADIUS) continue;
            if (npc != victim && (npc.NPC.PoliticalAlignment == gEPoliticalAlignment_None ||
                record.alignment == gEPoliticalAlignment_None)) continue;
            if (!g_resumedSpare)
            {
                Host::ArmSocialSpare("saved social punishment is still active");
                g_resumedSpare = GETrue;
            }
            if (g_enuCityPhase != mECityPhase_Aggro) g_u32AggroUntil = ::GetTickCount() + 30000;
            g_enuCityPhase = mECityPhase_Aggro;
            g_socialVictimID = bCString(record.victim.c_str());
            NoteAggro(npc);
            g_punishmentByNpc.emplace(Identity(npc), record.group);
            return;
        }
    }


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
            return FindWatcher(bCString(Identity(a_Npc).c_str())) >= 0;
        }

        void AddWatcher(Entity &a_Npc)
        {
            bCString const strName(Identity(a_Npc).c_str());
            if (FindWatcher(strName) >= 0 || g_iWatchCount >= WATCH_MAX)
                return;
            g_arrWatchNames[g_iWatchCount] = strName;
            g_arrWatchUntil[g_iWatchCount++] = ::GetTickCount() + WATCH_MS;
        }

        void RemoveWatcher(GEInt a_iIndex)
        {
            if (a_iIndex < 0 || a_iIndex >= g_iWatchCount)
                return;
            g_arrWatchNames[a_iIndex] = g_arrWatchNames[g_iWatchCount - 1];
            g_arrWatchUntil[a_iIndex] = g_arrWatchUntil[g_iWatchCount - 1];
            --g_iWatchCount;
        }

        // An NPC that does his own business: he fights nobody and has no reason to fight.
        GEBool IsPeaceful(Entity &a_Npc)
        {
            return a_Npc.NPC.AttackReason == gEAttackReason_None && a_Npc.NPC.CombatState == 0;
        }

        GEBool IsSlaveSpectator(Entity &npc)
        {
            if (npc == None || !npc.IsNPC() || npc.IsDead() || npc.IsDown() ||
                IsHeroSide(npc) || IsHeroControlled(npc) || !AI_Decision::IsSlave(npc))
                return GEFalse;
            // Native IsSlave already handles a switched/fleeing enclave. An ongoing
            // revolution and direct self-defence must retain their native reactions.
            Entity enclave = npc.NPC.GetEnclave();
            if ((enclave != None && enclave.Enclave.IsValid() && enclave.Enclave.Revolution) ||
                npc.NPC.AttackReason == gEAttackReason_Revolution)
                return GEFalse;
            Entity hero = Entity::GetPlayer();
            return !HasBeenHurtBy(npc, hero);
        }

        GEBool ObserveSlaveWitness(Entity &npc, Entity &hero, Entity &victim)
        {
            if (npc == victim || !IsSlaveSpectator(npc)) return GEFalse;
            // Do not interrupt an unrelated native fight or an existing flight.
            if (npc.Routine.AIMode == gEAIMode_Flee ||
                (npc.NPC.GetCurrentTarget() != None && npc.NPC.GetCurrentTarget() != hero && !IsPeaceful(npc)))
                return GETrue;
            if (!HasTask(npc, "ZS_WatchFight"))
            {
                npc.Routine.FullStop();
                npc.NPC.SetCurrentTarget(hero);
                npc.NPC.SetAlternativeTarget(victim);
                npc.NPC.AttackReason = gEAttackReason_None;
                npc.NPC.CombatState = 0;
                npc.Routine.SetTask("ZS_WatchFight");
                npc.SetAlignmentTarget(hero);
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] '%s': native slave remains a spectator of another NPC's conflict.\n", npc.GetName().GetText());
            }
            AddWatcher(npc);
            return GETrue;
        }

        GEBool HandleSlaveWitnessTarget(Entity &npc, Entity &target, GEInt reason)
        {
            // Restrict intervention in another fight, not theft, prey, scripted orders,
            // reactions to a direct hit, or the revolution itself.
            if (!IsHeroSide(target) ||
                (reason != gEAttackReason_StopFight && reason != gEAttackReason_Enemy && reason != gEAttackReason_Murder))
                return GEFalse;
            return ObserveSlaveWitness(npc, target, None);
        }

        // Hard attack order: the NPC drops what he does and attacks the hero (what AssessTarget does for a fighter).
        // Stage 1: the witnesses in sight stop what they do, turn to the hero and watch.
        void LogWitnessDecision(Entity &witness, Entity &victim, int stage, GELPCChar reason)
        {
            if (!DebugLog::IsEnabled() || witness == None || !witness.NPC.IsValid())
                return;
            DebugLog::GetLogger().LogFormat(
                "[GenomeLogicFix][social-witness] tick=%u npc='%s' victim='%s' stage=%d decision='%s' watching=%d\n",
                static_cast<unsigned int>(::GetTickCount()), witness.GetName().GetText(), victim.GetName().GetText(),
                stage, reason, static_cast<int>(HasTask(witness, "ZS_WatchFight")));
        }

        void StartWatching(Entity &a_Hero, Entity &a_Victim)
        {
            if (FactionLogic::IsNpcInRevolution(a_Victim)) return;
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
                GEInt const iKnown = FindWatcher(bCString(Identity(Known).c_str()));
                if (iKnown >= 0 && !HasTask(Known, "ZS_WatchFight"))
                    RemoveWatcher(iKnown);
            }
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Witness = arrNPCs.GetAt(i);
                if (!FactionLogic::AllowsCityWitness(Witness, a_Victim))
                {
                    if (DebugLog::IsEnabled() && Witness != None && Witness.GetDistanceTo(a_Hero) <= WITNESS_RADIUS)
                        LogWitnessDecision(Witness, a_Victim, 1, "enclave or faction witness rules");
                    continue;
                }
                GELPCChar reason = nullptr;
                mEWitnessVerdict const enuVerdict = GetWitnessVerdict(Witness, a_Hero, a_Victim, &reason);
                LogWitnessDecision(Witness, a_Victim, 1, reason);
                if (enuVerdict == mEWitnessVerdict_TooFar)
                    ++iTooFar;
                else if (enuVerdict == mEWitnessVerdict_NoLineOfSight)
                    ++iNoLos;
                if (enuVerdict != mEWitnessVerdict_Ok)
                    continue;

                bool const watching = HasTask(Witness, "ZS_WatchFight");
                if (watching && Witness.NPC.GetCurrentTarget() == a_Hero &&
                    Witness.NPC.GetAlternativeTarget() == a_Victim)
                {
                    AddWatcher(Witness);
                    ++iAlready;
                    continue;
                }
                if (!watching && (!IsPeaceful(Witness) || HasReacted(Witness, a_Hero)))
                {
                    LogWitnessDecision(Witness, a_Victim, 1, "busy or already reacted");
                    ++iBusy;
                    continue;
                }
                if (watching && DebugLog::IsEnabled())
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social-witness] npc='%s' switched to victim='%s'\n",
                                                   Witness.GetName().GetText(), a_Victim.GetName().GetText());

                Witness.Routine.FullStop();
                Witness.NPC.SetCurrentTarget(a_Hero);
                Witness.NPC.SetAlternativeTarget(a_Victim);
                Witness.Routine.SetTask("ZS_WatchFight");
                Witness.SetAlignmentTarget(a_Hero); // turn to the hero at once, the loop keeps him turned to the fight
                if (HasTask(Witness, "ZS_WatchFight"))
                {
                    if (watching)
                    {
                        GEInt const previousWatch = FindWatcher(bCString(Identity(Witness).c_str()));
                        if (previousWatch >= 0)
                            RemoveWatcher(previousWatch);
                    }
                    AddWatcher(Witness);
                    ++iNew;
                }
                else
                    ++iFailed;
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Assault, stage 1 (attention): '%s' was hit, %d witness(es) "
                                         "watch now (%d already watched), %d busy, %d could not switch, %d too far, "
                                         "%d without line of sight; window %u ms.\n",
                                         a_Victim.GetName().GetText(), static_cast<int>(iNew),
                                         static_cast<int>(iAlready), static_cast<int>(iBusy),
                                         static_cast<int>(iFailed), static_cast<int>(iTooFar), static_cast<int>(iNoLos),
                                         static_cast<unsigned>(WATCH_MS));
        }

        std::vector<Entity> OtherAssaultVictims(std::string const &group, Entity &victim,
                                               bTObjArray<Entity> const &npcs)
        {
            std::vector<Entity> victims;
            auto const victimId = Identity(victim);
            for (auto const &record : g_incidents)
            {
                if (record.group != group || record.victim == victimId || record.cause != Cause::Assault)
                    continue;
                Entity other = ResolveIdentity(record.victim, npcs);
                if (other != None && !FactionLogic::IsNpcInRevolution(other))
                    victims.push_back(other);
            }
            return victims;
        }

        bool AllowsGroupWitness(Entity &witness, Entity &victim, std::vector<Entity> &otherVictims)
        {
            return FactionLogic::AllowsCityWitness(witness, victim) ||
                   std::any_of(otherVictims.begin(), otherVictims.end(), [&](Entity &other) {
                       return witness != other && FactionLogic::AllowsCityWitness(witness, other);
                   });
        }

        // Both repeated aggression and a one-hit KO use the same current witness filter.
        void StartTotalAggro(Entity &a_Hero, Entity &a_Victim, GEBool a_bFirstTime,
                             GEBool directWitnessAttack = GEFalse)
        {
            if (FactionLogic::IsNpcInRevolution(a_Victim)) return;
            GEInt iForced = 0;
            GEInt iReact = 0;
            GEInt iTooFar = 0;
            GEInt iNoLos = 0;

            GEBool const traceWitnesses = directWitnessAttack && PluginRuntime::GetConfig().Diagnostics;
            if (traceWitnesses)
            {
                g_witnessTrace.clear();
                g_witnessTraceStart = ::GetTickCount();
            }

            GEInt const victimIndex = FindIncident(Identity(a_Victim));
            if (victimIndex < 0) return;
            std::string const group = g_incidents[victimIndex].group;
            auto rememberParticipant = [&](Entity &npc) {
                g_punishmentByNpc[Identity(npc)] = group;
            };
            rememberParticipant(a_Victim);

            // The second blow: from now on nobody within SPARE_RADIUS of the place where the hero falls may kill him.
            Host::ArmSocialSpare("the second blow in a city (total aggro)");

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
            auto otherVictims = OtherAssaultVictims(group, a_Victim, arrNPCs);
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Witness = arrNPCs.GetAt(i);

                if (FactionLogic::IsNpcInRevolution(Witness)) continue;
                if (std::find(otherVictims.begin(), otherVictims.end(), Witness) != otherVictims.end() &&
                    Witness.GetDistanceTo(a_Hero) <= WITNESS_RADIUS)
                {
                    rememberParticipant(Witness);
                    if (!Witness.IsDead() && !IsDownOrRising(Witness) && !IsAttackingHero(Witness, a_Hero))
                    {
                        ForceAttack(Witness, a_Hero, gEAttackReason_Murder);
                        ++iForced;
                    }
                    continue;
                }
                if (!AllowsGroupWitness(Witness, a_Victim, otherVictims))
                {
                    if (DebugLog::IsEnabled() && Witness != None && Witness.GetDistanceTo(a_Hero) <= WITNESS_RADIUS)
                        LogWitnessDecision(Witness, a_Victim, 2, "enclave or faction witness rules");
                    continue;
                }
                GELPCChar reason = nullptr;
                mEWitnessVerdict const enuVerdict = GetWitnessVerdict(Witness, a_Hero, a_Victim, &reason);
                LogWitnessDecision(Witness, a_Victim, 2, reason);
                if (enuVerdict == mEWitnessVerdict_NotAWitness)
                    continue;
                GEBool const bWatcher = IsWatcher(Witness);
                // A former watcher must still be close and have line of sight now.
                if (enuVerdict != mEWitnessVerdict_Ok)
                {
                    if (enuVerdict == mEWitnessVerdict_TooFar)
                        ++iTooFar;
                    else
                        ++iNoLos;
                    continue;
                }
                // The current and previous direct victims keep their own defence.
                // Apply normal witness distance/visibility checks before observing.
                if (ObserveSlaveWitness(Witness, a_Hero, a_Victim))
                    continue;
                if (IsAttackingHero(Witness, a_Hero))
                {
                    g_punishmentByNpc.emplace(Identity(Witness), group);
                    continue;
                }
                if (HasTask(Witness, "ZS_Attack") ||
                    (!HasTask(Witness, "ZS_WatchFight") && !IsPeaceful(Witness)))
                {
                    LogWitnessDecision(Witness, a_Victim, 2, "busy");
                    continue;
                }
                rememberParticipant(Witness);

                AI_Memory::MemorizePlayerCrime(Witness, a_Hero, gEPlayerCrime_Attack);
                if (directWitnessAttack || bWatcher || AI_Decision::IsGuard(Witness))
                {
                    ForceAttack(Witness, a_Hero, gEAttackReason_Enemy);
                    if (directWitnessAttack)
                    {
                        g_watchAttackRetries[Identity(Witness)] = group;
                        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] weakness witness '%s': direct attack order, task='%s'.\n",
                            Witness.GetName().GetText(), Witness.Routine.GetCurrentTask().GetText());
                    }
                    if (traceWitnesses)
                    {
                        std::string const id = Identity(Witness);
                        SampleWitnessTrace(Witness, id, g_witnessTrace[id], ::GetTickCount() - g_witnessTraceStart, "ordered");
                    }
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

            // Fighters leave the watcher list; passive slaves need the native watch
            // loop to finish normally when this incident ends.
            for (GEInt i = g_iWatchCount - 1; i >= 0; --i)
            {
                Entity watcher = ResolveIdentity(g_arrWatchNames[i].GetText(), arrNPCs);
                if (!IsSlaveSpectator(watcher) || !HasTask(watcher, "ZS_WatchFight")) RemoveWatcher(i);
            }
            g_enuCityPhase = mECityPhase_Aggro;
            g_u32AggroUntil = ::GetTickCount() + 30000;
            if (g_u32AggroUntil == 0)
                g_u32AggroUntil = 1;
            g_socialVictimID = bCString(Identity(a_Victim).c_str());
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Assault, stage 2 (%s): '%s' - %d ordered to ZS_Attack, "
                                         "%d reacted by themselves, %d too far, %d without line of sight.\n",
                                         a_bFirstTime ? "total aggro" : "aggro goes on", a_Victim.GetName().GetText(),
                                         static_cast<int>(iForced), static_cast<int>(iReact), static_cast<int>(iTooFar),
                                         static_cast<int>(iNoLos));
        }

        void ResetCityState(GELPCChar why)
        {
            g_witnessTrace.clear();
            g_watchAttackRetries.clear();
            g_punishmentByNpc.clear(); g_punishedGroup.clear();
            g_incidents.clear(); g_impacts.clear(); g_pendingImpacts.clear();
            g_loaded = GEFalse; g_pollTick = 0; g_resumedSpare = GEFalse;
            g_socialVictimID = bCString();
            g_enuCityPhase = mECityPhase_Idle;
            g_u32AggroUntil = g_u32LastAggroPass = 0;
            g_iWatchCount = 0;
            Host::ResetDeclareThrottle();
            Host::ResetSharedSessionState();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] runtime cache reset (%s); saved events retained.\n", why);
        }

        void OnCityAssault(Entity &victim, GEBool force, GEBool directWitnessAttack)
        {
            Entity hero = Entity::GetPlayer();
            GEInt const index = FindIncident(Identity(victim));
            if (index < 0 || g_incidents[index].cause != Cause::Assault) return;
            auto const npcs = Entity::GetNPCs();
            GEInt previous = -1;
            auto const &current = g_incidents[index];
            Entity enclave = victim.NPC.GetEnclave();
            if (current.alignment != gEPoliticalAlignment_None && current.stage < 2 &&
                victim.GetDistanceTo(hero) <= WITNESS_RADIUS)
            {
                for (size_t i = 0; i < g_incidents.size(); ++i)
                {
                    auto const &candidate = g_incidents[i];
                    if (static_cast<GEInt>(i) == index || candidate.cause != Cause::Assault || candidate.stage == 0 ||
                        candidate.alignment == gEPoliticalAlignment_None)
                        continue;

                    bool eligible = enclave != None && !current.enclave.empty() && current.enclave == candidate.enclave;
                    if (!eligible && current.enclave.empty() && current.alignment != gEPoliticalAlignment_None &&
                        current.alignment == candidate.alignment)
                    {
                        Entity other = ResolveIdentity(candidate.victim, npcs);
                        eligible = other != None && !FactionLogic::IsNpcInRevolution(other) &&
                                   other.GetDistanceTo(hero) <= WITNESS_RADIUS;
                    }
                    if (eligible && (previous < 0 || candidate.stage > g_incidents[previous].stage ||
                        (candidate.stage == g_incidents[previous].stage &&
                         static_cast<GEInt>(candidate.quietAfter - g_incidents[previous].quietAfter) >= 0)))
                        previous = static_cast<GEInt>(i);
                }
            }

            bool const first = current.stage == 0;
            std::string const group = previous >= 0 ? g_incidents[previous].group : current.group;
            bool const wasAggro = current.stage == 2 || (previous >= 0 && g_incidents[previous].stage == 2);
            bool const aggro = force || !first || previous >= 0;
            g_incidents[index].group = group;
            g_incidents[index].stage = aggro ? 2 : 1;
            g_incidents[index].quietAfter = ::GetTickCount() + REACTION_MS;
            if (aggro)
            {
                for (auto &record : g_incidents)
                {
                    if (record.group != group)
                        continue;
                    record.stage = 2;
                    record.weaknessPending = GEFalse;
                    SaveIncident(record);
                }
            }
            else SaveIncident(g_incidents[index]);
            g_socialVictimID = bCString(Identity(victim).c_str());
            if (aggro)
            {
                GEU32 const now = ::GetTickCount();
                if (force || !wasAggro || previous >= 0 || g_u32LastAggroPass == 0 || static_cast<GEInt>(now - g_u32LastAggroPass) >= 700 ||
                    g_enuCityPhase != mECityPhase_Aggro)
                {
                    g_u32LastAggroPass = now;
                    StartTotalAggro(hero, victim, !wasAggro, directWitnessAttack);
                }
            }
            else
            {
                g_enuCityPhase = mECityPhase_Watching;
                StartWatching(hero, victim);
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] '%s': direct aggression stage=%d, group='%s'.\n",
                victim.GetName().GetText(), aggro ? 2 : 1, group.c_str());
        }

        void OnCityKnockout(Entity &victim, GELPCChar where, Entity *source)
        {
            if (!IsSocialCandidate(victim) || !IsDownOrRising(victim) || ArenaExempt(victim)) return;
            if (HasTask(victim, "ZS_MagicSleep") || _stricmp(victim.Routine.GetCurrentState().GetText(), "ZS_MagicSleep") == 0) return;
            LoadIncidents();
            GEInt const index = FindIncident(Identity(victim));
            if (index < 0 || g_incidents[index].cause != Cause::Assault || g_incidents[index].knockoutSeen) return;
            Entity attacker = source ? *source : victim.NPC.GetCurrentAttacker();
            if (attacker == None && !source) attacker = victim.NPC.GetLastAttacker();
            if (attacker == None || !SourceIsHero(attacker)) return;
            GEBool const weakness = g_incidents[index].weaknessPending;
            if (weakness)
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] weakness impact: hero hit '%s' and knocked them out.\n",
                    victim.GetName().GetText());
            g_incidents[index].knockoutSeen = GETrue;
            OnCityAssault(victim, GETrue, weakness);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] '%s': unprovoked knockout confirmed (%s).\n", victim.GetName().GetText(), where);
        }
        ImpactScope::ImpactScope(Entity &target, Entity &source, GEBool notifyBeforeNative) : victim(target)
        {
            if (victim != None && IsHeroSide(victim))
            {
                Entity attacker = GroupLogic::Combat::GetDamageOwnerNpc(source);
                if (IsSocialCandidate(attacker))
                {
                    PollIncidents(GETrue);
                    LoadIncidents();
                    if (FindIncident(Identity(attacker)) < 0)
                    {
                        Cause cause = ClassifyCause(attacker);
                        if (cause == Cause::Assault) cause = Cause::SelfDefence;
                        AddIncident(attacker, cause);
                    }
                    GEInt const index = FindIncident(Identity(attacker));
                    LogImpactDecision(attacker, "npc-to-hero", "retaliation", index);
                    if (index >= 0 && g_incidents[index].cause == Cause::Assault &&
                        g_incidents[index].weaknessPending &&
                        std::find(g_impacts.begin(), g_impacts.end(), Identity(attacker)) == g_impacts.end())
                    {
                        victim = attacker;
                        retaliating = GETrue;
                        outer = GETrue;
                        g_impacts.push_back(Identity(victim));
                    }
                }
                return;
            }
            if (!IsSocialCandidate(victim) || source == None || !SourceIsHero(source)) return;
            std::string const id = Identity(victim);
            if (std::find(g_impacts.begin(), g_impacts.end(), id) != g_impacts.end()) return;
            PollIncidents(GETrue);
            LoadIncidents();
            auto index = FindIncident(id);
            std::string const enclave = Identity(victim.NPC.GetEnclave());
            bool const hitWatchingWitness = victim.NPC.PoliticalAlignment != gEPoliticalAlignment_None &&
                HasTask(victim, "ZS_WatchFight") && !enclave.empty() &&
                PluginRuntime::Conflict::IsArmed() && !PluginRuntime::Conflict::IsArenaPunishment() &&
                std::any_of(g_incidents.begin(), g_incidents.end(), [&](Incident const &record) {
                    return record.alignment != gEPoliticalAlignment_None &&
                           record.cause == Cause::Assault && record.stage == 2 &&
                           record.enclave == enclave && record.victim != id;
                });
            if (hitWatchingWitness && !ArenaExempt(victim))
            {
                if (index < 0)
                    index = AddIncident(victim, Cause::Assault);
                else
                    g_incidents[index].cause = Cause::Assault;
                LogImpactDecision(victim, "hero-to-npc", "watching-witness-hit", index);
            }
            else if (!IsUnprovokedAssault(victim, source))
            {
                LogImpactDecision(victim, "hero-to-npc", "not-assault", FindIncident(id));
                return;
            }
            index = FindIncident(id);
            LogImpactDecision(victim, "hero-to-npc", "assault-candidate", index);
            if (source.Projectile.IsValid())
            {
                std::string const projectile = Identity(source);
                if (!projectile.empty() && g_incidents[index].projectile == projectile) return;
                g_incidents[index].projectile = projectile;
            }
            if (victim.DamageReceiver.IsValid())
                hitPointsBefore = victim.DamageReceiver.HitPoints;
            g_impacts.push_back(id); outer = GETrue;
            notified = notifyBeforeNative;
            if (notified) OnCityAssault(victim);
        }
        ImpactScope::~ImpactScope()
        {
            if (!outer) return;
            Entity hero = Entity::GetPlayer();
            std::string const id = Identity(victim);
            GEInt const index = FindIncident(id);
            if (index >= 0 && !victim.IsDead() && !g_incidents[index].knockoutSeen)
            {
                if (!retaliating && IsDownOrRising(victim))
                    PluginRuntime::OnCityKnockout(victim, "impact completed", &hero);
                GEInt const after = FindIncident(id);
                if (!notified && after >= 0 && !g_incidents[after].knockoutSeen)
                {
                    GEBool const weakness = retaliating || g_incidents[after].weaknessPending;
                    if (weakness)
                        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] weakness impact: %s, NPC='%s'.\n",
                            retaliating ? "NPC hit hero" : "hero hit NPC", victim.GetName().GetText());
                    bool const freshDamage = hitPointsBefore < 0 || !victim.DamageReceiver.IsValid() ||
                        victim.DamageReceiver.HitPoints < hitPointsBefore;
                    if (DebugLog::IsEnabled())
                        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social-impact] npc='%s' hpBefore=%d hpAfter=%d counted=%d\n",
                            victim.GetName().GetText(), hitPointsBefore,
                            victim.DamageReceiver.IsValid() ? static_cast<int>(victim.DamageReceiver.HitPoints) : -1,
                            static_cast<int>(weakness || freshDamage));
                    auto const pending = std::find_if(g_pendingImpacts.begin(), g_pendingImpacts.end(),
                        [&](PendingImpact const &impact) { return impact.victim == victim; });
                    if (weakness || freshDamage)
                    {
                        if (pending != g_pendingImpacts.end())
                            g_pendingImpacts.erase(pending);
                        OnCityAssault(victim, weakness, weakness);
                    }
                    else if (pending == g_pendingImpacts.end())
                        g_pendingImpacts.push_back({victim, hitPointsBefore, ::GetTickCount() + 250});
                }
            }
            auto const it = std::find(g_impacts.begin(), g_impacts.end(), id);
            if (it != g_impacts.end()) g_impacts.erase(it);
        }

        // Called by the hook of ZS_WatchFight_Loop for the NPC that runs it. True: the watcher was released and has
        // already got his routine back (the state must not go on).
        GEBool RestoreWitnessAttack(Entity &npc)
        {
            if (!HasTask(npc, "ZS_WatchFight") || g_watchAttackRetries.empty()) return GEFalse;
            std::string const id = Identity(npc);
            auto const retry = g_watchAttackRetries.find(id);
            if (retry == g_watchAttackRetries.end()) return GEFalse;
            std::string const group = retry->second;
            g_watchAttackRetries.erase(retry);

            if (!PluginRuntime::Conflict::IsArmed() || PluginRuntime::Conflict::IsArenaPunishment() ||
                PluginRuntime::HeroState::IsHeroDownOrRising() || npc.IsDead() || IsDownOrRising(npc) ||
                IsSlaveSpectator(npc) || npc.NPC.AttackReason != gEAttackReason_Enemy)
                return GEFalse;

            auto const participant = g_punishmentByNpc.find(id);
            GEInt const active = FindIncident(g_socialVictimID.GetText());
            if (participant == g_punishmentByNpc.end() || participant->second != group || active < 0 ||
                g_incidents[active].stage != 2 || g_incidents[active].group != group)
                return GEFalse;

            Entity hero = Entity::GetPlayer();
            auto const npcs = Entity::GetNPCs();
            Entity victim = ResolveIdentity(g_incidents[active].victim, npcs);
            if (hero == None || victim == None || FactionLogic::IsNpcInRevolution(victim) ||
                GetWitnessVerdict(npc, hero, victim) != mEWitnessVerdict_Ok)
                return GEFalse;
            auto otherVictims = OtherAssaultVictims(group, victim, npcs);
            if (!AllowsGroupWitness(npc, victim, otherVictims))
                return GEFalse;

            GEInt const watcher = FindWatcher(bCString(id.c_str()));
            if (watcher >= 0) RemoveWatcher(watcher);
            ForceAttack(npc, hero, gEAttackReason_Enemy);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] '%s' id='%s': restored attack after return to watching, task='%s'.\n",
                npc.GetName().GetText(), id.c_str(), npc.Routine.GetCurrentTask().GetText());
            return HasTask(npc, "ZS_Attack");
        }

        GEBool ReleaseWatcherIfOver(Entity &a_Self)
        {
            if (RestoreWitnessAttack(a_Self)) return GETrue;
            GEInt const iIndex = FindWatcher(bCString(Identity(a_Self).c_str()));
            if (iIndex < 0)
                return GEFalse;
            GEU32 const u32Now = ::GetTickCount();
            if (!TickReached(u32Now, g_arrWatchUntil[iIndex]))
                return GEFalse;

            if (IsSlaveSpectator(a_Self) && HasTask(a_Self, "ZS_WatchFight"))
            {
                if (g_enuCityPhase == mECityPhase_Aggro && !TickReached(u32Now, g_u32AggroUntil) && IsCityBrawlVictimAlive())
                    return GEFalse;
                // A native witness call may have no 75%-reputation incident record.
                // Bound that observation by the existing ten-second watch duration.
                if (g_enuCityPhase == mECityPhase_Idle && a_Self.Routine.GetTaskTime() < WATCH_MS / 1000.0f)
                    return GEFalse;
            }

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
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Assault: observation ended after %u ms (warning memory retained), '%s' goes back to "
                                             "his routine (%d watcher(s) left).\n",
                                             static_cast<unsigned>(WATCH_MS), a_Self.GetName().GetText(),
                                             static_cast<int>(g_iWatchCount));
            }
            return GETrue;
        }



        // v1.0.51.0: THE CLEAN SLATE (called by the event "Mercy ended"). Every memory of the blows of the hero is dropped for
        // EVERY NPC of the location: the phase / victim / timers of the city beating (the FIRST / SECOND blow of the residents),
        // the watchers, the memory of the assault, the counters of the brawl of the army (blow count, started fight, reaction),
        // the blow watch, the records of the fights. The NPCs that still watch the old beating (ZS_WatchFight) get their
        // routine back first - the native loop never ends by itself. The list of the NPCs sent against the hero (g_arrAggro*)
        // and the release flag are NOT touched: the same event calms those down in ZS_Attack_Loop. The next blow is the first one.
        void CapturePunishedConflict(Entity &winner)
        {
            if (winner == None) return;
            auto const found = g_punishmentByNpc.find(Identity(winner));
            g_punishedGroup = found == g_punishmentByNpc.end() ? std::string() : found->second;
        }

        void ClearPunishedConflict()
        {
            g_punishedGroup.clear();
        }

        GEInt ResetBlowHistory()
        {
            g_watchAttackRetries.clear();
            g_pendingImpacts.clear();
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

            LoadIncidents();
            g_resumedSpare = GEFalse;
            for (size_t i = g_incidents.size(); i-- > 0;)
                if (g_incidents[i].cause == Cause::Assault && g_incidents[i].stage == 2 &&
                    g_incidents[i].group == g_punishedGroup) EraseIncident(i);
            for (auto it = g_punishmentByNpc.begin(); it != g_punishmentByNpc.end();)
                if (it->second == g_punishedGroup) it = g_punishmentByNpc.erase(it);
                else ++it;
            ClearPunishedConflict();
            g_enuCityPhase = mECityPhase_Idle;
            g_socialVictimID = bCString();
            g_u32AggroUntil = g_u32LastAggroPass = 0;
            Host::ResetDeclareThrottle();
            g_iWatchCount = 0;

            return iReleased;
        }

        GEBool IsCityBrawlVictimAlive()
        {
            auto const npcs = Entity::GetNPCs();
            Entity victim = ResolveIdentity(g_socialVictimID.GetText(), npcs);
            return victim != None && !victim.IsDead();
        }

    GEBool IsWatching(Entity &npc) { return g_enuCityPhase == mECityPhase_Watching && IsWatcher(npc); }
    GEBool IsAggro() { return g_enuCityPhase == mECityPhase_Aggro; }
}

namespace SocialLogic
{
    using GroupLogic::IsHeroFormEntity;
    using GroupLogic::IsHeroTransformedNow;
    using Host::IsSentAgainstHero;
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

        // A Slave-aligned hero form is protected across factions. Party creatures retain the None restriction.
        GEBool IsSlaveAlignedTargetProtected(Entity &a_Self, Entity &a_Other)
        {
            if (a_Self == None || a_Other == None || !a_Self.IsNPC() || a_Self == a_Other)
                return GEFalse;
            if (a_Self == Entity::GetPlayer() || IsHeroControlled(a_Self) || IsHeroSide(a_Self))
                return GEFalse;
            if (a_Self.IsDead() || !AI_Decision::IsHumanoid(a_Self))
                return GEFalse;
            if (!IsPeaceful(a_Self) || HasBeenHurtBy(a_Self, a_Other))
                return GEFalse;
            if (IsSentAgainstHero(a_Self)) // v1.0.67.0: the crowd of the crime fights the form like the man
                return GEFalse;
            if (!IsSlaveTarget(a_Other))
                return GEFalse;
            GEBool const bForm = IsTransformedHeroTarget(a_Other);
            GEBool const bParty = a_Self.NPC.PoliticalAlignment == gEPoliticalAlignment_None &&
                                  a_Other.IsNPC() && IsHeroControlled(a_Other);
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
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] %s: %s - '%s' keeps calm towards '%s'.\n", a_strWhere, a_strRule,
                                         a_Self.GetName().GetText(), a_Other.GetName().GetText());
        }
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
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] %s: the built-in weapon of the form '%s' is no threat for '%s'.\n",
                                         a_strWhere, a_Other.GetName().GetText(), a_Self.GetName().GetText());
        }
}

namespace SocialLogic
{
    GEBool IgnorePerceivedTarget(Entity &Self, Entity &Other)
    {
        if (IsWandererImmune(Self, Other))
        {
            LogPoliticalFilter("AssessTarget", "a Wanderer sees a human in the beast form", Self, Other);
            return GETrue;
        }
        if (IsSlaveAlignedTargetProtected(Self, Other))
        {
            LogPoliticalFilter("AssessTarget", IsTransformedHeroTarget(Other) ? "Slave-aligned hero form" : "faction None vs a Slave party creature", Self, Other);
            return GETrue;
        }
        return GEFalse;
    }
    GEBool AdjustPoliticalAttitude(Entity &Self, Entity &Other, GEInt iResult, GEInt &adjusted)
    {
        GEBool const bAggressive = iResult == gEAttitude_Angry || iResult == gEAttitude_Hostile ||
                                   iResult == gEAttitude_Predator;
        // Wanderers and protected Slave-aligned targets suppress automatic aggression.
        // v1.0.38.0: the Wanderers (gEPoliticalAlignment_Mid) also never PANIC at the hero in a form.
        if ((bAggressive || iResult == gEAttitude_Panic) && IsWandererImmune(Self, Other))
        {
            LogPoliticalFilter("GetAttitude", "a Wanderer (Mid) sees a human in the beast form", Self, Other);
            { adjusted = gEAttitude_Neutral; return GETrue; }
        }
        if (bAggressive)
        {
            if (IsSlaveAlignedTargetProtected(Self, Other))
            {
                LogPoliticalFilter("GetAttitude", IsTransformedHeroTarget(Other) ? "Slave-aligned hero form" : "faction None vs a Slave party creature", Self, Other);
                { adjusted = gEAttitude_Neutral; return GETrue; }
            }
        }
        return GEFalse;
    }
    GEBool AdjustPlayerPoliticalAttitude(Entity &Self, GEInt &adjusted)
    {
        // v1.0.36.0: the target of this query is the player (the entity of the form while the hero is transformed).
        {
            Entity PlayerTarget = Entity::GetPlayer();
            if (IsWandererImmune(Self, PlayerTarget))
            {
                LogPoliticalFilter("GetAttitudeToPlayer", "a Wanderer sees a human in the beast form", Self, PlayerTarget);
                { adjusted = gEAttitude_Neutral; return GETrue; }
            }
            if (IsSlaveAlignedTargetProtected(Self, PlayerTarget))
            {
                LogPoliticalFilter("GetAttitudeToPlayer", "Slave-aligned hero form", Self, PlayerTarget);
                { adjusted = gEAttitude_Neutral; return GETrue; }
            }
        }
        return GEFalse;
    }
}

namespace SocialLogic
{
    GEInt AdjustRansack(Entity &Self, Entity &Hero, GEInt iResult)
    {
        gEClass const enuHeroClass = Hero.NPC.Class;
        gEClass const enuNpcClass = Self.NPC.Class;
        GEBool const bExempt = enuHeroClass == gEClass_Mage || enuHeroClass == gEClass_Paladin ||
                               enuNpcClass == gEClass_Mage || enuNpcClass == gEClass_Paladin;
        static GEInt s_iRansackLines = 0;
        if (s_iRansackLines < 120)
        {
            ++s_iRansackLines;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][diag] '%s' (class %d) wants to ransack the hero (class %d): "
                                         "native=%d -> %s\n",
                                         Self.GetName().GetText(), static_cast<int>(enuNpcClass),
                                         static_cast<int>(enuHeroClass), static_cast<int>(iResult),
                                         (iResult && bExempt) ? "the plugin: NO (Mage / Paladin)" : "unchanged");
        }
        return (iResult && bExempt) ? 0 : iResult;
        }
}

namespace SocialLogic
{
    void AlignWatcherAfterLoop(Entity &Self)
    {
        if (Self != None && SocialLogic::IsWatching(Self))
        {
            Entity Hero = Entity::GetPlayer();
            Self.SetAlignmentTarget(Hero);
        }
    }
}

namespace SocialLogic::Hooks
{
    using PluginRuntime::Conflict::FindAggro;
    namespace { Entity ResolveEntity(GELPVoid entity, eCEntity *fallback) { return entity ? *static_cast<Entity *>(entity) : Entity(fallback); } }
    GEInt AssessPlayerWeapon(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (FactionLogic::IgnoresDrawnWeapon(Self))
            return 0;
        if (IsFormWeaponBlocked(Self, Other))
        {
            LogFormWeapon("AssessPlayerWeapon", Self, Other);
            return 0;
        }
        return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
    GEInt AssessWarn(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (FactionLogic::IgnoresDrawnWeapon(Self))
            return 0;
        if (IsFormWeaponBlocked(Self, Other))
        {
            LogFormWeapon("AssessWarn", Self, Other);
            return 0;
        }
        return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
    GEInt CanThreaten(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity Other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (IsFormWeaponBlocked(Self, Other))
            return 0;
        return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
    GEInt StartSayAttackReason(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        gEAttackReason enuOriginal = gEAttackReason_None;
        GEBool bSwapped = GEFalse;
        if (SocialLogic::IsAggro() && Self != None && Self.IsNPC() && !(Self == Entity::GetPlayer()))
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
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] '%s' shouts about the brawl, not about a murder (attack "
                                                 "reason %d -> %d for the voice line); nobody died.\n",
                                                 Self.GetName().GetText(), static_cast<int>(enuOriginal),
                                                 static_cast<int>(gEAttackReason_StopFight));
                }
            }
        }
        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        if (bSwapped)
            Self.NPC.AttackReason = enuOriginal;
        return iResult;
        }
}

namespace SocialLogic::Hooks
{
    GEInt HostileMagic(gFScript original, GELPCChar name, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args)
    {
        Entity caster = self ? *static_cast<Entity *>(self) : Entity(spu->GetSelfEntity());
        Entity victim = other ? *static_cast<Entity *>(other) : Entity(spu->GetOtherEntity());
        if (IsSocialCandidate(victim) && caster != None && SourceIsHero(caster))
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][social] hostile effect %s: caster='%s', victim='%s'.\n",
                name, caster.GetName().GetText(), victim.GetName().GetText());
        GEBool const poison = strcmp(name, "MagicPoison") == 0;
        GEBool const weakness = strcmp(name, "MagicDisease") == 0;
        if (!poison && !weakness)
        {
            ImpactScope social(victim, caster, GETrue);
            return original(spu, self, other, args);
        }
        if (!IsSocialCandidate(victim) || !SourceIsHero(caster) || ArenaExempt(victim))
            return original(spu, self, other, args);

        PollIncidents(GETrue);
        LoadIncidents();
        std::string const id = Identity(victim);
        GEInt index = FindIncident(id);
        Cause const currentCause = ClassifyCause(victim);
        if (currentCause == Cause::Duel) return original(spu, self, other, args);
        Cause const cause = index >= 0 && (g_incidents[index].cause == Cause::Assault || HasThreat(victim)) ?
            g_incidents[index].cause : currentCause;
        GEBool const eligible = cause == Cause::Assault || cause == Cause::NativeProvocation;
        if (!eligible) return original(spu, self, other, args);

        GEBool const alreadyAffected = poison ? victim.NPC.IsPoisoned() : victim.NPC.IsDiseased();
        if (std::find(g_impacts.begin(), g_impacts.end(), id) != g_impacts.end())
            return original(spu, self, other, args);
        g_impacts.push_back(id);
        GEInt const result = original(spu, self, other, args);
        g_impacts.pop_back();
        if (victim.IsDead() || !(poison ? victim.NPC.IsPoisoned() : victim.NPC.IsDiseased()) ||
            (alreadyAffected && !result))
            return result;

        index = FindIncident(id);
        if (index < 0) index = AddIncident(victim, Cause::Assault);
        auto &record = g_incidents[index];
        record.cause = Cause::Assault;
        record.quietAfter = ::GetTickCount() + REACTION_MS;
        if (weakness && record.stage < 2) record.weaknessPending = GETrue;
        SaveIncident(record);
        if (poison) OnCityAssault(victim, GETrue);
        return result;
    }
}
