#include "FactionLogic.h"
#include "DebugLog.h"
#include "GroupLogic.h"
#include "GroupBrawl.h"
#include "PluginRuntime.h"

#include <g3sdk/Script.h>
#include <g3sdk/util/ScriptUtil.h>
#include <g3sdk/util/ScriptInterface.h>
#include <g3sdk/util/Hook.h>
#include <algorithm>
#include <cstring>
#include <intrin.h>
#include <vector>

namespace FactionLogic
{
    namespace
    {
        mCFunctionHook enclaveStatusHook;
        Entity *impactVictim = nullptr;
        Entity *impactAttacker = nullptr;
        struct LocalFight
        {
            bCPropertyID attacker;
            bCPropertyID victim;
            GEU32 expires;
        };
        constexpr GEU32 localFightMemoryMs = 10000;
        constexpr GEU32 heroFightMemoryMs = 2500;
        constexpr GEFloat closePursuitCombatDistance = 1000.0f;
        std::vector<LocalFight> localFights;
        std::vector<LocalFight> preparationHits;
        std::vector<LocalFight> lowReputationHits;
        bCPropertyID heroFightOpponent;
        GEU32 heroFightUntil = 0;
        constexpr GEU32 citizenStumbleTimeoutMs = 1000;
        constexpr GEU32 workReleaseTimeoutMs = 750;
        constexpr GEU32 fleeStillTimeMs = 1000;
        constexpr GEU32 hitConfirmationMs = 250;
        constexpr GEFloat witnessRadius = 2500.0f;
        constexpr GEFloat enclaveAggroRadius = 2500.0f;
        bool shareEnclaveAggro = true;
        bool sharingHostileTarget = false;
        struct PursuitMember
        {
            bCPropertyID id;
            bCVector waitPosition = bCVector();
            GEU32 waitSince = 0;
        };
        struct SharedPursuit
        {
            bCPropertyID hero;
            std::vector<PursuitMember> members;
            bool ending = false;
            bCPropertyID recentOpponent;
            GEU32 closeCombatUntil = 0;
        };
        constexpr GEU32 pursuitCheckIntervalMs = 100;
        constexpr GEU32 pursuitWaitIntervalMs = 2000;
        constexpr GEFloat pursuitMovementTolerance = 30.0f;
        constexpr GEU32 cancelAttackRva = 0x1A4D0;
        constexpr GEU32 attackRangeCancelCallRva = 0x1A1F6;
        constexpr GEU32 sprintRangeCancelCallRva = 0x1A457;
        mCFunctionHook cancelAttackHook;
        GEU8 const *scriptGameBase = nullptr;
        bool pursuitHookInstalled = false;
        std::vector<SharedPursuit> sharedPursuits;
        GEU32 nextPursuitCheck = 0;
        constexpr GEFloat fleeProgressDistance = 20.0f;
        constexpr char citizenWaitTask[] = "ZS_GLF_CitizenWait";
        void ReturnFleeingCitizensToRoutine(Entity &enclave);
        void LoadFlights();
        void RecordAuthorityHit(Entity &victim, Entity &attacker);
        bool CanSeeBattleAndFlee(Entity &citizen);
        bool HasBattleOrder(Entity &npc);
        bool CitizenSeesOtherHeroFight(Entity &npc);

        bool IsEnclaveAuthority(Entity &npc, Entity &enclave)
        {
            auto const alignment = static_cast<gEPoliticalAlignment>(npc.NPC.PoliticalAlignment);
            return alignment != gEPoliticalAlignment_Citizen && alignment != gEPoliticalAlignment_Partisan &&
                   (alignment == enclave.Enclave.PoliticalAlignment ||
                    AI_Decision::GetPoliticalAttitude(npc, enclave) == gEAttitude_Friendly);
        }

        bool IsEnclaveBattle(Entity &enclave)
        {
            if (enclave == None || !enclave.Enclave.IsValid())
                return false;
            auto const status = static_cast<gEEnclaveStatus>(enclave.Enclave.Status);
            return status == gEEnclaveStatus_Attack || status == gEEnclaveStatus_Flee;
        }

        bool IsRevolutionBattle(Entity &enclave)
        {
            return IsEnclaveBattle(enclave) && enclave.Enclave.Revolution;
        }

        bool IsCivilian(Entity &npc)
        {
            return npc != None && npc.NPC.IsValid() && npc != Entity::GetPlayer() &&
                npc.NPC.PoliticalAlignment == gEPoliticalAlignment_Citizen &&
                (npc.NPC.Species == gESpecies_Human || npc.NPC.Species == gESpecies_Orc);
        }

        bool IsAuthorityAidDuringPreparation(Entity &witness, Entity &victim)
        {
            auto const alignment = static_cast<gEPoliticalAlignment>(witness.NPC.PoliticalAlignment);
            if (alignment != gEPoliticalAlignment_Citizen && alignment != gEPoliticalAlignment_Partisan)
                return false;
            Entity enclave = witness.NPC.GetEnclave();
            return enclave != None && enclave.Enclave.IsValid() && enclave.Enclave.Revolution &&
                enclave.Enclave.Status == gEEnclaveStatus_Routine && victim.NPC.GetEnclave() == enclave &&
                IsEnclaveAuthority(victim, enclave);
        }

        bool IsDefendingAuthorityDuringPreparation(Entity &witness, Entity &target)
        {
            if (target == None || target != Entity::GetPlayer() || !target.NPC.IsValid() ||
                GroupLogic::IsHeroControlled(witness) || witness.NPC.GetCurrentAttacker() == target ||
                witness.NPC.GetLastAttacker() == target)
                return false;
            Entity victim = impactAttacker && *impactAttacker == target ?
                *impactVictim : target.NPC.GetCurrentTarget();
            if (victim == witness)
                return false;
            if (victim != None && victim.NPC.IsValid() && IsAuthorityAidDuringPreparation(witness, victim))
                return true;
            GEU32 const now = ::GetTickCount();
            return std::any_of(preparationHits.begin(), preparationHits.end(), [&](LocalFight const &hit) {
                if (static_cast<GEInt>(now - hit.expires) >= 0)
                    return false;
                Entity authority(hit.victim);
                return authority != None && authority.NPC.IsValid() &&
                    IsAuthorityAidDuringPreparation(witness, authority);
            });
        }

        GEInt GE_STDCALL ChangeEnclaveStatus(gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args)
        {
            auto original = enclaveStatusHook.GetOriginalFunction<gFScript>();
            if (!spu || (args != gEEnclaveStatus_Flee && args != gEEnclaveStatus_Routine))
                return DebugLog::TraceEnclaveStatus(original, spu, self, other, args);

            Entity subject = other ? *static_cast<Entity *>(other) : Entity(spu->GetOtherEntity());
            Entity enclave = subject == None || subject.Enclave.IsValid() ? subject : subject.GetEnclave();
            if (!IsEnclaveBattle(enclave))
                return DebugLog::TraceEnclaveStatus(original, spu, self, other, args);

            LoadFlights();
            ReturnFleeingCitizensToRoutine(enclave);
            if (args == gEEnclaveStatus_Flee)
            {
                GEInt const result = DebugLog::TraceEnclaveStatus(original, spu, self, other, args);
                Update();
                return result;
            }

            std::vector<bCPropertyID> citizens;
            auto const members = enclave.Enclave.GetMembers();
            for (GEInt i = 0; i < members.GetCount(); ++i)
            {
                auto const *member = members.GetAt(i);
                Entity npc(member);
                if (npc != None && npc.NPC.IsValid() && npc.NPC.PoliticalAlignment == gEPoliticalAlignment_Citizen &&
                    npc.NPC.GetEnclave() == enclave)
                    citizens.push_back(member->GetID());
            }

            GEInt const result = DebugLog::TraceEnclaveStatus(original, spu, self, other, args);
            if (enclave != None && enclave.Enclave.IsValid() && enclave.Enclave.Status == gEEnclaveStatus_Routine)
                for (auto const &id : citizens)
                {
                    Entity npc(id);
                    if (npc == None || !npc.NPC.IsValid() || npc.NPC.GetEnclave() != enclave ||
                        npc.NPC.PoliticalAlignment == gEPoliticalAlignment_Citizen)
                        continue;
                    npc.NPC.PoliticalAlignment = gEPoliticalAlignment_Citizen;
                    DebugLog::GetLogger().LogFormat("[FactionLogic] Citizen retained enclave='%s' npc='%s'\n",
                                                   enclave.GetName().GetText(), npc.GetName().GetText());
                }
            Update();
            return result;
        }

        struct Flight
        {
            bCPropertyID citizen;
            bCPropertyID threat;
            bool pending;
            bCPropertyID issuedThreat;
            bool directThreat = false;
            GEU32 stumbleSince = 0;
            GEU32 workReleaseSince = 0;
            GEU32 lastFleeProgress = 0;
            bool stopLogged = false;
            bCVector progressPosition;
            bCVector departurePosition;
        };
        std::vector<Flight> flights;
        constexpr char flightEventPrefix[] = "GLF_FlightV1|";
        bool flightsLoaded = false;

        bCString FlightEvent(bCPropertyID const &id)
        {
            return bCString(flightEventPrefix) + id.GetText();
        }

        void SetFlightEvent(bCPropertyID const &id, bool active)
        {
            Entity memory = GroupLogic::Brawl::BrawlMemoryOwner();
            if (memory == None || !memory.PlayerMemory.IsValid())
                return;
            if (active)
                memory.PlayerMemory.SetGameEvent(FlightEvent(id));
            else
                memory.PlayerMemory.ClearGameEvent(FlightEvent(id));
        }

        void LoadFlights()
        {
            if (flightsLoaded)
                return;
            Entity memory = GroupLogic::Brawl::BrawlMemoryOwner();
            if (memory == None || !memory.PlayerMemory.IsValid())
                return;
            flightsLoaded = true;
            auto const &events = static_cast<gCPlayerMemory_PS const *>(
                memory.PlayerMemory.m_pEngineEntityPropertySet)->GetPlayerKnows();
            for (GEInt i = 0; i < events.GetCount(); ++i)
            {
                auto const text = events.GetAt(i).GetText();
                if (std::strncmp(text, flightEventPrefix, sizeof(flightEventPrefix) - 1) != 0)
                    continue;
                bCPropertyID id;
                id.SetText(bCString(text + sizeof(flightEventPrefix) - 1));
                if (!id.IsValid() || std::any_of(flights.begin(), flights.end(),
                        [&](Flight const &flight) { return flight.citizen == id; }))
                    continue;
                Flight flight{};
                flight.citizen = id;
                Entity npc(id);
                if (npc != None && npc.NPC.IsValid())
                {
                    Entity threat = npc.NPC.GetCurrentTarget();
                    if (threat != None && threat.GetInstance())
                        flight.threat = flight.issuedThreat = threat.GetInstance()->GetID();
                }
                flights.push_back(flight);
            }
        }

        bool HasTrackedFlight(Entity &npc)
        {
            return npc != None && npc.GetInstance() && std::any_of(flights.begin(), flights.end(),
                [&](Flight const &flight) { return flight.citizen == npc.GetInstance()->GetID(); });
        }

        bool HasTask(Entity &npc, char const *task)
        {
            return npc.Routine.IsValid() && npc.Routine.GetCurrentTask() == task;
        }

        enum class AggroGroup { None, Humanoid, Undead, Monster };

        AggroGroup GetAggroGroup(Entity &npc)
        {
            switch (static_cast<gESpecies>(npc.NPC.Species))
            {
            case gESpecies_Human:
            case gESpecies_Orc:
                return AggroGroup::Humanoid;
            case gESpecies_Skeleton:
            case gESpecies_Zombie:
            case gESpecies_Demon:
            case gESpecies_Gargoyle:
                return AggroGroup::Undead;
            case gESpecies_Ogre:
            case gESpecies_Troll:
            case gESpecies_Trex:
            case gESpecies_Golem:
            case gESpecies_FireGolem:
            case gESpecies_IceGolem:
            case gESpecies_Stalker:
                return AggroGroup::Monster;
            default:
                return AggroGroup::None;
            }
        }

        bool IsAggroHostile(GEInt attitude, AggroGroup group)
        {
            return attitude == gEAttitude_Hostile ||
                (group != AggroGroup::Humanoid && attitude == gEAttitude_Predator);
        }

        bool HasEnclaveConflict(Entity &enclave)
        {
            return enclave != None && enclave.Enclave.IsValid() &&
                (enclave.Enclave.Revolution || IsEnclaveBattle(enclave));
        }

        bool IsStumbling(Entity &npc)
        {
            auto const task = npc.Routine.GetCurrentTask();
            return task == "ZS_Stumble" || task == "ZS_QuickStumble" || task == "ZS_PierceStumble";
        }

        bool Incapacitated(Entity &npc)
        {
            return IsDown(npc) || IsStumbling(npc) || HasTask(npc, "ZS_Freeze") || HasTask(npc, "ZS_MagicSleep");
        }
    }

    void LoadConfig(char const *iniPath)
    {
        char value[16] = {};
        ::GetPrivateProfileStringA("Main", "ShareEnclaveAggro", "true", value, sizeof(value), iniPath);
        shareEnclaveAggro = _stricmp(value, "true") == 0 || std::strcmp(value, "1") == 0;
    }

    bool IsPursuing(Entity &npc, Entity &target)
    {
        return npc.NPC.IsValid() && npc.NPC.GetCurrentTarget() == target && HasTask(npc, "ZS_Attack");
    }

    namespace
    {
        SharedPursuit *FindSharedPursuit(bCPropertyID const &id)
        {
            for (auto &pursuit : sharedPursuits)
                if (std::any_of(pursuit.members.begin(), pursuit.members.end(),
                    [&](auto const &member) { return member.id == id; }))
                    return &pursuit;
            return nullptr;
        }

        bool IsPursuitPaused(Entity &npc)
        {
            return IsStumbling(npc) || HasTask(npc, "ZS_HeavyParadeStumble") ||
                HasTask(npc, "ZS_Fall") || HasTask(npc, "ZS_ReactToDamage");
        }

        bool IsSharedPursuer(Entity &npc, Entity &hero)
        {
            if (npc == None || !npc.NPC.IsValid() || !npc.Routine.IsValid() || npc.IsDead() ||
                (IsDown(npc) && !IsPursuitPaused(npc)) ||
                GroupLogic::IsHeroControlled(npc) || GroupLogic::Host::IsHeroSide(npc))
                return false;
            Entity target = npc.NPC.GetCurrentTarget();
            if (target != hero && (target != None || !IsPursuitPaused(npc)))
                return false;
            Entity enclave = npc.NPC.GetEnclave();
            return !HasEnclaveConflict(enclave) && (IsPursuing(npc, hero) || IsPursuitPaused(npc));
        }

        bool IsSharedPursuitAttack(Entity &npc)
        {
            auto const reason = npc.NPC.AttackReason;
            return reason == gEAttackReason_Enemy || reason == gEAttackReason_ReactToDamage;
        }

        bool PursuitDiagnostics()
        {
            return DebugLog::IsEnabled() && PluginRuntime::GetConfig().FactionDiagnostics;
        }

        GEInt GE_STDCALL CancelAttack(gCScriptProcessingUnit *spu, Entity *self, Entity *other, GEInt args)
        {
            using Original = GEInt (GE_STDCALL *)(gCScriptProcessingUnit *, Entity *, Entity *, GEInt);
            auto original = cancelAttackHook.GetOriginalFunction<Original>();
            auto const *caller = static_cast<GEU8 const *>(_ReturnAddress());
            if ((caller != scriptGameBase + attackRangeCancelCallRva + 5 &&
                 caller != scriptGameBase + sprintRangeCancelCallRva + 5) ||
                sharedPursuits.empty() || !spu)
                return original(spu, self, other, args);

            Entity npc = self ? *self : Entity(spu->GetSelfEntity());
            Entity hero = Entity::GetPlayer();
            if (hero == None || !IsSharedPursuer(npc, hero) ||
                !IsSharedPursuitAttack(npc) || IsDown(hero))
                return original(spu, self, other, args);

            auto *pursuit = FindSharedPursuit(npc.GetInstance()->GetID());
            if (!pursuit || pursuit->ending || pursuit->hero != hero.GetInstance()->GetID())
                return original(spu, self, other, args);

            auto const distance = npc.GetDistanceTo(hero);
            if (pursuit->closeCombatUntil != 0 &&
                static_cast<GEI32>(::GetTickCount() - pursuit->closeCombatUntil) < 0)
            {
                Entity opponent(pursuit->recentOpponent);
                if (IsSharedPursuer(opponent, hero) && opponent.GetDistanceTo(hero) <= closePursuitCombatDistance)
                    return 1;
            }

            for (auto const &entry : pursuit->members)
            {
                Entity member(entry.id);
                if (member != npc && IsSharedPursuer(member, hero) &&
                    member.GetDistanceTo(hero) < distance)
                    return 1;
            }

            pursuit->ending = true;
            nextPursuitCheck = ::GetTickCount();
            if (PursuitDiagnostics())
                DebugLog::GetLogger().LogFormat(
                    "[FactionLogic][aggro] pursuit retreat leader='%s' distanceToHero=%.0f members=%u\n",
                    npc.GetName().GetText(), static_cast<double>(distance),
                    static_cast<unsigned>(pursuit->members.size()));
            return original(spu, self, other, args);
        }

        bool IsCancelAttackCall(GEU32 rva)
        {
            auto const *call = scriptGameBase + rva;
            GEI32 displacement = 0;
            std::memcpy(&displacement, call + 1, sizeof(displacement));
            return call[0] == 0xE8 && rva + 5 + displacement == cancelAttackRva;
        }

        void ResumeWaitingPursuer(PursuitMember &member, Entity &npc, Entity &hero, GEU32 now)
        {
            auto const state = npc.Routine.GetCurrentState();
            if (!IsPursuing(npc, hero) || !IsSharedPursuitAttack(npc) || Incapacitated(npc) ||
                (state != "ZS_Attack_Wait" && state != "ZS_Attack_Loop"))
            {
                member.waitSince = 0;
                return;
            }

            auto const distance = npc.GetDistanceTo(hero);
            auto const attackRange = AI_Commands::GetAttackRange(npc, hero);
            if (attackRange <= 0 || distance <= attackRange)
            {
                member.waitSince = 0;
                return;
            }

            auto const position = npc.GetPosition();
            if (member.waitSince == 0 || (position - member.waitPosition).GetSquareMagnitude() >
                pursuitMovementTolerance * pursuitMovementTolerance)
            {
                member.waitPosition = position;
                member.waitSince = now | 1u;
                return;
            }
            if (static_cast<GEI32>(now - member.waitSince) < static_cast<GEI32>(pursuitWaitIntervalMs))
                return;

            member.waitPosition = position;
            member.waitSince = now | 1u;
            npc.Routine.SetState("ZS_Attack_Goto");
            if (PursuitDiagnostics())
                DebugLog::GetLogger().LogFormat(
                    "[FactionLogic][aggro] pursuit resumed approach npc='%s' state='%s' distanceToHero=%.0f attackRange=%d\n",
                    npc.GetName().GetText(), state.GetText(), static_cast<double>(distance), attackRange);
        }

        void UpdateSharedPursuits(GEU32 now)
        {
            if (sharedPursuits.empty() || static_cast<GEI32>(now - nextPursuitCheck) < 0)
                return;
            nextPursuitCheck = now + pursuitCheckIntervalMs;

            Entity hero = Entity::GetPlayer();
            if (hero == None || PluginRuntime::HeroState::IsHeroKnockedOut() || IsDown(hero))
            {
                sharedPursuits.clear();
                return;
            }

            auto const heroId = hero.GetInstance()->GetID();
            for (auto pursuit = sharedPursuits.begin(); pursuit != sharedPursuits.end();)
            {
                if (pursuit->hero != heroId)
                {
                    pursuit = sharedPursuits.erase(pursuit);
                    continue;
                }

                for (auto member = pursuit->members.begin(); member != pursuit->members.end();)
                {
                    Entity npc(member->id);
                    if (!IsSharedPursuer(npc, hero))
                    {
                        member = pursuit->members.erase(member);
                        continue;
                    }
                    if (IsPursuitPaused(npc))
                    {
                        member->waitSince = 0;
                        ++member;
                        continue;
                    }
                    if (!pursuit->ending)
                    {
                        ResumeWaitingPursuer(*member, npc, hero, now);
                        ++member;
                        continue;
                    }

                    if (IsSharedPursuitAttack(npc))
                    {
                        npc.Routine.FullStop();
                        npc.NPC.SetCurrentTarget(None);
                        npc.NPC.SetAlternativeTarget(None);
                        npc.NPC.AttackReason = gEAttackReason_None;
                        npc.NPC.CombatState = 0;
                        npc.Routine.ContinueRoutine();
                    }
                    member = pursuit->members.erase(member);
                }
                if (pursuit->members.empty())
                    pursuit = sharedPursuits.erase(pursuit);
                else
                    ++pursuit;
            }
        }
    }

    void ShareHostileTarget(Entity &initiator, Entity &hero)
    {
        if (sharingHostileTarget)
            return;

        bool const diagnostics = DebugLog::IsEnabled() && PluginRuntime::GetConfig().FactionDiagnostics;
        auto logInitiator = [&](char const *decision, char const *enclaveName = "-", int attitude = -1)
        {
            if (diagnostics)
                DebugLog::GetLogger().LogFormat(
                    "[FactionLogic][aggro] initiator='%s' enclave='%s' attitude=%d task='%s' decision=%s\n",
                    initiator.GetName().GetText(), enclaveName, attitude,
                    initiator.Routine.IsValid() ? initiator.Routine.GetCurrentTask().GetText() : "-", decision);
        };

        if (!shareEnclaveAggro || !IsPursuing(initiator, hero) ||
            GroupLogic::IsHeroControlled(initiator) || GroupLogic::Host::IsHeroSide(initiator))
        {
            logInitiator(!shareEnclaveAggro ? "disabled" :
                !IsPursuing(initiator, hero) ? "no-pursuit-of-hero" : "hero-party");
            return;
        }

        auto const group = GetAggroGroup(initiator);
        if (group == AggroGroup::None)
            return;

        Entity enclave = initiator.NPC.GetEnclave();
        bCString const enclaveName = enclave != None ? enclave.GetName() : bCString("-");
        if (HasEnclaveConflict(enclave))
        {
            logInitiator("enclave-conflict", enclaveName.GetText());
            return;
        }

        sharingHostileTarget = true;
        // The caller has already confirmed a native Enemy attack targeting the hero.
        auto const initiatorId = initiator.GetInstance()->GetID();
        if (FindSharedPursuit(initiatorId))
        {
            sharingHostileTarget = false;
            return;
        }
        SharedPursuit pursuit{hero.GetInstance()->GetID(), {{initiatorId}}};
        logInitiator("scan", enclaveName.GetText());
        {
            auto const alignment = static_cast<gEPoliticalAlignment>(initiator.NPC.PoliticalAlignment);
            auto npcs = Entity::GetNPCs();
            GEInt nearby = 0;
            GEInt outsideRadius = 0;
            for (GEInt index = 0; index < npcs.GetCount(); ++index)
            {
                Entity ally = npcs.GetAt(index);
                if (ally == None || ally == hero || ally == initiator || !ally.NPC.IsValid() ||
                    GetAggroGroup(ally) != group)
                    continue;

                auto const distance = ally.GetDistanceTo(initiator);
                if (distance > enclaveAggroRadius)
                {
                    ++outsideRadius;
                    continue;
                }

                ++nearby;
                auto const allyAlignment = static_cast<gEPoliticalAlignment>(ally.NPC.PoliticalAlignment);
                int allyAttitude = -1;
                int focusAttitude = -1;
                char const *decision = nullptr;
                Entity allyEnclave = ally.NPC.GetEnclave();
                Entity existingTarget = ally.NPC.GetCurrentTarget();
                auto const combatBefore = static_cast<GEInt>(ally.NPC.CombatState);
                if (ally.IsDead() || !ally.Routine.IsValid() || Incapacitated(ally) || IsPursuitPaused(ally))
                    decision = "incapacitated";
                else if (group == AggroGroup::Humanoid && allyAlignment != alignment)
                    decision = "different-faction";
                else if (HasEnclaveConflict(allyEnclave))
                    decision = "enclave-conflict";
                else if (FindSharedPursuit(ally.GetInstance()->GetID()))
                    decision = "already-in-shared-pursuit";
                else if (GroupLogic::IsHeroControlled(ally) || GroupLogic::Host::IsHeroSide(ally))
                    decision = "hero-party";
                else if (IsPursuing(ally, hero) && IsSharedPursuitAttack(ally))
                {
                    pursuit.members.push_back({ally.GetInstance()->GetID()});
                    decision = "tracked-existing-attack";
                }
                else if (existingTarget != None && (HasTask(ally, "ZS_Attack") || combatBefore == 1))
                    decision = existingTarget == hero ? "other-attack-on-hero" : "fighting-other-target";
                else
                {
                    allyAttitude = AI_Decision::GetAttitude(ally, hero);
                    // Use the game's red-name decision when the pair has no explicit attitude.
                    if (allyAttitude == gEAttitude_None)
                        focusAttitude = AI_Decision::GetFocusNameAttitude(ally, hero);
                    if (!IsAggroHostile(allyAttitude, group) && !IsAggroHostile(focusAttitude, group))
                        decision = "not-hostile";
                }

                if (!decision)
                {
                    AI_Perception::AssessTarget(ally, hero, gEAttackReason_Enemy);
                    bool const joined = IsPursuing(ally, hero);
                    decision = joined ? "joined" : "assess-returned-without-pursuit";
                    if (joined)
                        pursuit.members.push_back({ally.GetInstance()->GetID()});
                    if (joined && DebugLog::IsEnabled())
                        DebugLog::GetLogger().LogFormat(
                            "[FactionLogic] aggro shared initiator='%s' ally='%s'\n",
                            initiator.GetName().GetText(), ally.GetName().GetText());
                }
                if (diagnostics)
                {
                    Entity target = ally.NPC.GetCurrentTarget();
                    DebugLog::GetLogger().LogFormat(
                        "[FactionLogic][aggro] initiator='%s' candidate='%s' group=%d species=%d enclave='%s' "
                        "distanceFromInitiator=%.0f pal=%d initiatorPal=%d combatBefore=%d attitude=%d focusAttitude=%d task='%s' target='%s' decision=%s\n",
                        initiator.GetName().GetText(), ally.GetName().GetText(), static_cast<int>(group),
                        static_cast<int>(ally.NPC.Species), allyEnclave != None ? allyEnclave.GetName().GetText() : "-",
                        static_cast<double>(distance), static_cast<int>(allyAlignment), static_cast<int>(alignment),
                        combatBefore, allyAttitude, focusAttitude,
                        ally.Routine.IsValid() ? ally.Routine.GetCurrentTask().GetText() : "-",
                        target != None ? target.GetName().GetText() : "-", decision);
                }
            }
            if (diagnostics)
                DebugLog::GetLogger().LogFormat(
                    "[FactionLogic][aggro] scan finished initiator='%s' group=%d radius=%.0f worldNpcs=%d candidates=%d outsideRadius=%d\n",
                    initiator.GetName().GetText(), static_cast<int>(group), static_cast<double>(enclaveAggroRadius),
                    npcs.GetCount(), nearby, outsideRadius);
        }
        if (pursuitHookInstalled && pursuit.members.size() > 1)
        {
            if (sharedPursuits.empty())
                nextPursuitCheck = ::GetTickCount();
            sharedPursuits.push_back(std::move(pursuit));
        }
        sharingHostileTarget = false;
    }

    bool IsNpcInRevolution(Entity &npc)
    {
        if (npc == None || !npc.NPC.IsValid())
            return false;
        Entity enclave = npc.NPC.GetEnclave();
        return IsEnclaveBattle(enclave);
    }

    bool IsProtectedCitizen(Entity &npc)
    {
        return IsCivilian(npc) && IsNpcInRevolution(npc);
    }

    void InstallSharedPursuitHook()
    {
        if (!shareEnclaveAggro)
            return;
        auto *base = reinterpret_cast<GEU8 *>(::GetModuleHandleA("Script_Game.dll"));
        scriptGameBase = base;
        if (base && IsCancelAttackCall(attackRangeCancelCallRva) && IsCancelAttackCall(sprintRangeCancelCallRva))
            pursuitHookInstalled = cancelAttackHook.Prepare(base + cancelAttackRva, &CancelAttack).Hook();
        DebugLog::GetLogger().LogFormat("[FactionLogic][aggro] shared retreat hook=%d\n", pursuitHookInstalled);
    }

    bool InstallEnclaveStatusHook()
    {
        auto const *status = GetScriptAdminExt().GetScript("ChangeEnclaveStatus");
        bool const installed = status && status->m_funcScript &&
                               enclaveStatusHook.Prepare(status->m_funcScript, &ChangeEnclaveStatus).Hook();
        DebugLog::GetLogger().LogFormat("[FactionLogic] enclave status hook=%d\n", installed);
        return installed;
    }

    bool AllowsCityWitness(Entity &witness, Entity &victim)
    {
        if (witness == None || victim == None || !witness.NPC.IsValid() || !victim.NPC.IsValid() ||
            IsNpcInRevolution(witness) || IsNpcInRevolution(victim) ||
            witness.NPC.PoliticalAlignment == gEPoliticalAlignment_None ||
            victim.NPC.PoliticalAlignment == gEPoliticalAlignment_None)
            return false;
        if (IsAuthorityAidDuringPreparation(witness, victim))
            return false;

        Entity enclave = victim.NPC.GetEnclave();
        if (enclave != None && enclave.Enclave.IsValid() && witness.NPC.GetEnclave() != enclave)
            return false;

        return witness.NPC.PoliticalAlignment != gEPoliticalAlignment_Citizen ||
               victim.NPC.PoliticalAlignment == gEPoliticalAlignment_Citizen;
    }

    bool AdjustNpcAttitude(Entity &npc, Entity &other, GEInt &adjusted)
    {
        if (npc == None || other == None || npc == other || !npc.NPC.IsValid() || !other.NPC.IsValid())
            return false;
        auto const alignment = static_cast<gEPoliticalAlignment>(npc.NPC.PoliticalAlignment);
        auto const otherAlignment = static_cast<gEPoliticalAlignment>(other.NPC.PoliticalAlignment);
        bool const citizen = alignment == gEPoliticalAlignment_Citizen;
        bool const otherCitizen = otherAlignment == gEPoliticalAlignment_Citizen;
        bool const partisan = alignment == gEPoliticalAlignment_Partisan;
        bool const unaffiliated = alignment == gEPoliticalAlignment_None || otherAlignment == gEPoliticalAlignment_None;
        Entity hero = Entity::GetPlayer();
        if (npc == hero || other == hero)
            return false;

        Entity enclave = npc.NPC.GetEnclave();
        if (enclave == None || !enclave.Enclave.IsValid() || other.NPC.GetEnclave() != enclave)
            return false;

        bool const revolutionBattle = IsRevolutionBattle(enclave);
        if (!revolutionBattle)
        {
            if (enclave.Enclave.Status != gEEnclaveStatus_Routine ||
                GroupLogic::IsHeroControlled(npc) || GroupLogic::IsHeroControlled(other))
                return false;
            adjusted = unaffiliated ? gEAttitude_Neutral : gEAttitude_Friendly;
            return true;
        }
        if (unaffiliated)
            return false;
        if (!citizen && !otherCitizen && !partisan)
            return false;

        Entity &authority = otherCitizen && !citizen ? npc : other;
        if (!IsEnclaveAuthority(authority, enclave))
            return false;

        if (citizen || otherCitizen)
            adjusted = gEAttitude_Neutral;
        else
            adjusted = gEAttitude_Hostile;
        return true;
    }

    bool AdjustAttitudeToPlayer(Entity &npc, GEInt nativeAttitude, GEInt &adjusted)
    {
        if (npc == None || !npc.NPC.IsValid() || npc == Entity::GetPlayer())
            return false;
        auto const alignment = static_cast<gEPoliticalAlignment>(npc.NPC.PoliticalAlignment);
        if (alignment == gEPoliticalAlignment_None)
            return false;
        Entity enclave = npc.NPC.GetEnclave();
        if (enclave == None || !enclave.Enclave.IsValid())
            return false;
        if (alignment == gEPoliticalAlignment_Partisan && enclave.Enclave.Revolution)
        {
            adjusted = gEAttitude_Friendly;
            return true;
        }
        if (IsCivilian(npc) && (enclave.Enclave.Revolution || IsEnclaveBattle(enclave)))
        {
            adjusted = gEAttitude_Neutral;
            return true;
        }
        if (IsRevolutionBattle(enclave))
        {
            if (alignment != gEPoliticalAlignment_Citizen)
                return false;
            adjusted = gEAttitude_Neutral;
            return true;
        }
        if ((nativeAttitude != gEAttitude_Neutral && nativeAttitude != gEAttitude_None) ||
            (alignment != gEPoliticalAlignment_Citizen && GroupLogic::IsHeroTransformedNow()) ||
            enclave.Enclave.Status != gEEnclaveStatus_Routine || enclave.Enclave.PlayerFame <= 75)
            return false;

        adjusted = gEAttitude_Friendly;
        return true;
    }

    WitnessImpactScope::WitnessImpactScope(Entity &victim, Entity &attacker)
        : previousVictim(impactVictim), previousAttacker(impactAttacker)
    {
        if (victim != None && attacker != None)
        {
            impactVictim = &victim;
            impactAttacker = &attacker;
            RecordAuthorityHit(victim, attacker);
            if (victim.NPC.IsValid() && attacker.NPC.IsValid())
            {
                Entity city = victim.NPC.GetEnclave();
                if (city != None && city.Enclave.IsValid() && city.Enclave.Status == gEEnclaveStatus_Routine &&
                    city.Enclave.PlayerFame < 75)
                {
                    GEU32 const now = ::GetTickCount();
                    auto const attackerId = attacker.GetInstance()->GetID();
                    lowReputationHits.erase(std::remove_if(lowReputationHits.begin(), lowReputationHits.end(),
                        [&](LocalFight const &hit) {
                            return hit.attacker == attackerId || static_cast<GEInt>(now - hit.expires) >= 0;
                        }), lowReputationHits.end());
                    lowReputationHits.push_back({attackerId, victim.GetInstance()->GetID(), now + 2500});
                }
            }
            if (victim.NPC.IsValid() && attacker == Entity::GetPlayer())
            {
                Entity enclave = victim.NPC.GetEnclave();
                if (enclave != None && enclave.Enclave.IsValid() && enclave.Enclave.Revolution &&
                    enclave.Enclave.Status == gEEnclaveStatus_Routine && IsEnclaveAuthority(victim, enclave))
                {
                    GEU32 const now = ::GetTickCount();
                    auto const id = victim.GetInstance()->GetID();
                    preparationHits.erase(std::remove_if(preparationHits.begin(), preparationHits.end(),
                        [&](LocalFight const &hit) {
                            return hit.victim == id || static_cast<GEInt>(now - hit.expires) >= 0;
                        }), preparationHits.end());
                    preparationHits.push_back({attacker.GetInstance()->GetID(), id, now + 2500});
                }
            }
        }
    }

    WitnessImpactScope::~WitnessImpactScope()
    {
        impactVictim = previousVictim;
        impactAttacker = previousAttacker;
    }

    bool SuppressWitnessAid(Entity &witness, Entity &opponent)
    {
        if (witness == None || opponent == None || witness == opponent || !witness.NPC.IsValid() ||
            !opponent.NPC.IsValid() || GroupLogic::IsHeroControlled(witness))
            return false;

        Entity hero = Entity::GetPlayer();
        if (hero == None || witness == hero)
            return false;

        Entity enclave = witness.NPC.GetEnclave();
        if (enclave == None || !enclave.Enclave.IsValid() || enclave.Enclave.Status != gEEnclaveStatus_Routine ||
            IsNpcInRevolution(opponent))
            return false;

        if (witness.NPC.GetCurrentAttacker() == opponent || witness.NPC.GetLastAttacker() == opponent)
            return false;
        if (IsDefendingAuthorityDuringPreparation(witness, opponent))
            return true;

        bool const fromImpact = impactAttacker && *impactAttacker == opponent;
        Entity victim = fromImpact ? *impactVictim : opponent.NPC.GetCurrentTarget();
        if (victim == None || victim == witness || !victim.NPC.IsValid() ||
            (!fromImpact && opponent != hero && opponent.NPC.AttackReason == gEAttackReason_None))
            return false;
        if (!fromImpact && opponent == hero && victim.NPC.GetCurrentAttacker() != hero)
            return false;

        auto const witnessAlignment = static_cast<gEPoliticalAlignment>(witness.NPC.PoliticalAlignment);
        bool const localVictim = victim.NPC.GetEnclave() == enclave;
        bool const localOpponent = opponent.NPC.GetEnclave() == enclave;
        bool const noneInFight = (localVictim && victim.NPC.PoliticalAlignment == gEPoliticalAlignment_None) ||
            (localOpponent && opponent.NPC.PoliticalAlignment == gEPoliticalAlignment_None);
        bool const suppress =
            (witnessAlignment == gEPoliticalAlignment_Citizen && victim == hero) ||
            (witnessAlignment == gEPoliticalAlignment_None && (victim == hero || localVictim || localOpponent)) ||
            noneInFight;
        if (suppress)
            DebugLog::GetLogger().LogFormat("[FactionLogic] witness aid suppressed npc='%s' opponent='%s' victim='%s'\n",
                witness.GetName().GetText(), opponent.GetName().GetText(), victim.GetName().GetText());
        return suppress;
    }

    namespace
    {
        bool SuppressWitnessPerception(gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other)
        {
            if (!spu)
                return false;
            Entity witness = self ? *static_cast<Entity *>(self) : Entity(spu->GetSelfEntity());
            Entity opponent = other ? *static_cast<Entity *>(other) : Entity(spu->GetOtherEntity());
            return SuppressWitnessAid(witness, opponent);
        }
    }

    ME_DEFINE_AND_HOOK_SCRIPT(AssessAttack)
    {
        if (SuppressWitnessPerception(a_pSPU, a_pSelfEntity, a_pOtherEntity))
            return 0;
        return AssessAttack_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    ME_DEFINE_AND_HOOK_SCRIPT(AssessFightSound)
    {
        Entity witness = a_pSelfEntity ? *static_cast<Entity *>(a_pSelfEntity) :
            Entity(a_pSPU ? a_pSPU->GetSelfEntity() : nullptr);
        if (CanSeeBattleAndFlee(witness) && !HasBattleOrder(witness))
            return 0;
        if (SuppressWitnessPerception(a_pSPU, a_pSelfEntity, a_pOtherEntity))
            return 0;
        return AssessFightSound_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    ME_DEFINE_AND_HOOK_SCRIPT(AssessMurder)
    {
        if (SuppressWitnessPerception(a_pSPU, a_pSelfEntity, a_pOtherEntity))
            return 0;
        return AssessMurder_Original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
    }

    bool SuppressCombatTarget(Entity &npc, Entity &target)
    {
        if (npc == None || npc == Entity::GetPlayer())
            return false;
        return IsProtectedCitizen(npc) || IsProtectedCitizen(target) ||
            (npc.NPC.IsValid() && IsDefendingAuthorityDuringPreparation(npc, target));
    }

    namespace
    {
        bool IsLowReputationCitizen(Entity &npc)
        {
            if (npc == None || !npc.NPC.IsValid() || npc.NPC.PoliticalAlignment != gEPoliticalAlignment_Citizen ||
                GroupLogic::Host::IsHeroSide(npc) || GroupLogic::IsHeroControlled(npc))
                return false;
            Entity city = npc.NPC.GetEnclave();
            return city != None && city.Enclave.IsValid() && city.Enclave.Status == gEEnclaveStatus_Routine &&
                city.Enclave.PlayerFame < 75;
        }
    }

    bool SuppressCitizenIntervention(Entity &witness, Entity &opponent, GEInt reason)
    {
        bool const stopFight = reason == gEAttackReason_StopFight;
        bool const commandAgainstHero = reason == gEAttackReason_PlayerCommand && opponent != None &&
            opponent == Entity::GetPlayer();
        if ((!stopFight && !commandAgainstHero && reason != gEAttackReason_Enemy && reason != gEAttackReason_Murder &&
                reason != gEAttackReason_Angry && reason != gEAttackReason_ReactToDamage && reason != gEAttackReason_None) ||
            !IsLowReputationCitizen(witness) || opponent == None || witness == opponent || !opponent.NPC.IsValid())
            return false;
        Entity city = witness.NPC.GetEnclave();
        Entity victim;
        bool knownImpact = false;
        if (impactAttacker && *impactAttacker == opponent)
        {
            victim = *impactVictim;
            knownImpact = true;
        }
        else
        {
            GEU32 const now = ::GetTickCount();
            auto const opponentId = opponent.GetInstance()->GetID();
            auto const hit = std::find_if(lowReputationHits.begin(), lowReputationHits.end(), [&](LocalFight const &entry) {
                return entry.attacker == opponentId && static_cast<GEInt>(now - entry.expires) < 0;
            });
            knownImpact = hit != lowReputationHits.end();
            victim = knownImpact ? Entity(hit->victim) : opponent.NPC.GetCurrentTarget();
        }
        if (victim == witness)
            return false;
        bool const recentThirdPartyHit = knownImpact && victim != None && victim.NPC.IsValid() &&
            victim.NPC.GetEnclave() == city;
        if ((witness.NPC.GetCurrentAttacker() == opponent || witness.NPC.GetLastAttacker() == opponent) &&
            (!(stopFight || commandAgainstHero) || !recentThirdPartyHit))
            return false;
        if (victim == None || !victim.NPC.IsValid() || victim.NPC.GetEnclave() != city)
            return stopFight || commandAgainstHero;
        return victim.NPC.PoliticalAlignment != gEPoliticalAlignment_Citizen;
    }

    bool IsDown(Entity &npc)
    {
        return npc != None && npc.Routine.IsValid() &&
               (npc.IsDown() || npc.Routine.AIMode == gEAIMode_Down || HasTask(npc, "ZS_Unconscious"));
    }

    namespace
    {
        bool IsUnaffiliatedLocal(Entity &npc, Entity &enclave)
        {
            return npc != None && npc.NPC.IsValid() &&
                npc.NPC.PoliticalAlignment == gEPoliticalAlignment_None &&
                enclave != None && enclave.Enclave.IsValid() && enclave.Enclave.Status == gEEnclaveStatus_Routine &&
                npc.NPC.GetEnclave() == enclave;
        }

    }

    bool IsJoiningLocalNoneFight(Entity &fighter, Entity &target)
    {
        if (fighter == None || target == None || fighter == target || !fighter.NPC.IsValid() || !target.NPC.IsValid())
            return false;
        Entity enclave = fighter.NPC.GetEnclave();
        if (!IsUnaffiliatedLocal(target, enclave) || GroupLogic::IsHeroControlled(fighter) ||
            GroupLogic::Host::IsHeroSide(fighter) ||
            fighter.NPC.GetCurrentAttacker() == target || fighter.NPC.GetLastAttacker() == target)
            return false;

        auto const targetId = target.GetInstance()->GetID();
        auto const fighterId = fighter.GetInstance()->GetID();
        GEU32 const now = ::GetTickCount();
        auto const isRecent = [&](auto const &fight) {
            return fight.attacker == targetId && static_cast<GEInt>(now - fight.expires) < 0;
        };
        if (std::any_of(localFights.begin(), localFights.end(), [&](auto const &fight) {
            return isRecent(fight) && fight.victim == fighterId;
        }))
            return false;

        Entity hero = Entity::GetPlayer();
        Entity opponent = target.NPC.GetCurrentTarget();
        bool const fightingSomeoneElse = opponent != None && opponent != fighter && opponent.NPC.IsValid() &&
            (opponent == hero || opponent.NPC.GetEnclave() == enclave) && target.NPC.AttackReason != gEAttackReason_None;
        return fightingSomeoneElse || std::any_of(localFights.begin(), localFights.end(), isRecent);
    }

    bool IsPeacefulSettlementCreature(Entity &npc, Entity &creature)
    {
        if (npc == None || creature == None || npc == creature || npc == Entity::GetPlayer() ||
            !npc.NPC.IsValid() || !creature.NPC.IsValid() ||
            creature.NPC.PoliticalAlignment == gEPoliticalAlignment_None)
            return false;

        Entity enclave = creature.NPC.GetEnclave();
        if (enclave == None || !enclave.Enclave.IsValid() || enclave.Enclave.Status != gEEnclaveStatus_Routine ||
            enclave.Enclave.Revolution || creature.NPC.PoliticalAlignment != enclave.Enclave.PoliticalAlignment ||
            GroupLogic::IsHeroControlled(creature) || GroupLogic::Host::IsHeroSide(creature) ||
            !AI_Decision::IsHumanoid(npc) || AI_Decision::IsHumanoid(creature))
            return false;

        if (npc.NPC.GetCurrentAttacker() == creature || npc.NPC.GetLastAttacker() == creature ||
            creature.NPC.GetCurrentAttacker() == npc || creature.NPC.GetLastAttacker() == npc)
            return false;

        auto const political = AI_Decision::GetPoliticalAttitude(npc, creature);
        return political == gEAttitude_None || political == gEAttitude_Neutral || political == gEAttitude_Friendly;
    }

    bool ReleaseCombatTarget(Entity &npc)
    {
        if (npc == None || !npc.NPC.IsValid() || !npc.Routine.IsValid() || npc.IsDead() ||
            (!HasTask(npc, "ZS_Attack") && !HasTask(npc, "ZS_RevolutionAttack")))
            return false;
        Entity target = npc.NPC.GetCurrentTarget();
        bool const witnessAid = IsJoiningLocalNoneFight(npc, target);
        bool const preparing = IsDefendingAuthorityDuringPreparation(npc, target);
        bool const weaponWarning = npc.NPC.AttackReason == gEAttackReason_ReactToWeapon && IgnoresDrawnWeapon(npc);
        bool const citizenStopFight = SuppressCitizenIntervention(npc, target,
            static_cast<GEInt>(npc.NPC.AttackReason));
        bool const peacefulCreature = (npc.NPC.AttackReason == gEAttackReason_Enemy ||
            npc.NPC.AttackReason == gEAttackReason_Prey) && IsPeacefulSettlementCreature(npc, target);
        if (!witnessAid && !preparing && !weaponWarning && !citizenStopFight && !peacefulCreature &&
            !IsProtectedCitizen(npc) && !IsProtectedCitizen(target))
            return false;
        npc.Routine.FullStop();
        npc.NPC.SetCurrentTarget(None);
        npc.NPC.SetAlternativeTarget(None);
        npc.NPC.AttackReason = gEAttackReason_None;
        npc.NPC.CombatState = 0;
        if (weaponWarning && npc.Inventory.IsValid())
            npc.Inventory.HoldStacks(-1, -1);
        npc.Routine.ContinueRoutine();
        DebugLog::GetLogger().LogFormat("[FactionLogic] combat cleared npc='%s' target='%s' reason='%s'\n",
            npc.GetName().GetText(), target != None ? target.GetName().GetText() : "-",
            witnessAid ? "bystander joined a local None fight" :
            citizenStopFight ? "citizen only defends local citizens below 75 fame" :
            peacefulCreature ? "settlement creature has not attacked" :
            weaponWarning ? "drawn weapon during assault" :
            preparing ? "authority aid during preparation" : "citizen protected during assault");
        return true;
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Attack)
    {
        Entity fighter(a_pSPU->GetSelfEntity());
        if (fighter != None && fighter.NPC.IsValid())
        {
            Entity target = fighter.NPC.GetCurrentTarget();
            DebugLog::LogFactionCreatureTarget(fighter, target, "attack-entry",
                static_cast<GEInt>(fighter.NPC.AttackReason));
        }
        if (DebugLog::IsEnabled() && IsLowReputationCitizen(fighter))
        {
            Entity target = fighter.NPC.GetCurrentTarget();
            Entity currentAttacker = fighter.NPC.GetCurrentAttacker();
            Entity lastAttacker = fighter.NPC.GetLastAttacker();
            DebugLog::GetLogger().LogFormat(
                "[FactionLogic] citizen attack entry npc='%s' reason=%d target='%s' currentAttacker='%s' lastAttacker='%s'\n",
                fighter.GetName().GetText(), static_cast<int>(fighter.NPC.AttackReason),
                target != None ? target.GetName().GetText() : "-",
                currentAttacker != None ? currentAttacker.GetName().GetText() : "-",
                lastAttacker != None ? lastAttacker.GetName().GetText() : "-");
        }
        if (ReleaseCombatTarget(fighter))
            return GETrue;
        return ZS_Attack_Original(a_rRunTimeStack, a_pSPU);
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Attack_GoBack)
    {
        Entity fighter(a_pSPU->GetSelfEntity());
        if (fighter != None && fighter.NPC.IsValid() &&
            (fighter.NPC.AttackReason == gEAttackReason_StopFight ||
             fighter.NPC.AttackReason == gEAttackReason_PlayerCommand) && ReleaseCombatTarget(fighter))
            return GETrue;
        return ZS_Attack_GoBack_Original(a_rRunTimeStack, a_pSPU);
    }

    namespace
    {
        bool CanStartCitizenFlight(Entity &citizen)
        {
            if (!IsProtectedCitizen(citizen))
                return false;
            Entity enclave = citizen.NPC.GetEnclave();
            return enclave.Enclave.Status == gEEnclaveStatus_Attack;
        }

        bool ReturnFleeingCitizenToRoutine(Entity &citizen)
        {
            if ((!IsProtectedCitizen(citizen) && !HasTrackedFlight(citizen)) || citizen.IsDead() ||
                (!HasTask(citizen, "ZS_Flee") && !HasTask(citizen, citizenWaitTask)) ||
                Incapacitated(citizen))
                return false;

            citizen.Routine.FullStop();
            citizen.NPC.SetCurrentTarget(None);
            citizen.NPC.SetAlternativeTarget(None);
            citizen.NPC.AttackReason = gEAttackReason_None;
            citizen.NPC.CombatState = 0;
            citizen.Routine.ContinueRoutine();
            DebugLog::GetLogger().LogFormat("[FactionLogic] flee cancelled npc='%s'\n", citizen.GetName().GetText());
            return true;
        }

        void ReturnFleeingCitizensToRoutine(Entity &enclave)
        {
            auto const members = enclave.Enclave.GetMembers();
            for (GEInt i = 0; i < members.GetCount(); ++i)
            {
                Entity member(members.GetAt(i));
                if (member != None && member.NPC.IsValid() && member.NPC.GetEnclave() == enclave)
                    ReturnFleeingCitizenToRoutine(member);
            }
        }

        bool PreventCitizenRetreat(gCScriptProcessingUnit *spu)
        {
            if (!spu)
                return false;
            Entity npc(spu->GetSelfEntity());
            if (npc == None || !npc.NPC.IsValid() || (!IsProtectedCitizen(npc) && !HasTrackedFlight(npc)))
                return false;
            Entity enclave = npc.NPC.GetEnclave();
            return enclave != None && enclave.Enclave.IsValid() && enclave.Enclave.Status == gEEnclaveStatus_Flee &&
                ReturnFleeingCitizenToRoutine(npc);
        }

        void CancelCitizenFlight(Entity &citizen, Flight const &flight)
        {
            if (HasTask(citizen, citizenWaitTask))
            {
                citizen.Routine.ContinueRoutine();
                return;
            }
            if (!flight.issuedThreat.IsValid() || !HasTask(citizen, "ZS_Flee") ||
                citizen.NPC.AttackReason != gEAttackReason_None)
                return;

            Entity target = citizen.NPC.GetCurrentTarget();
            auto const *instance = target != None ? static_cast<eCEntity *>(target) : nullptr;
            if (instance && instance->GetID() != flight.issuedThreat)
                return;

            citizen.Routine.FullStop();
            citizen.NPC.SetCurrentTarget(None);
            citizen.NPC.SetAlternativeTarget(None);
            citizen.Routine.ContinueRoutine();
            DebugLog::GetLogger().LogFormat("[FactionLogic] flee cancelled npc='%s'\n", citizen.GetName().GetText());
        }

        bool CanSeeBattleAndFlee(Entity &citizen)
        {
            if (citizen == None || !citizen.NPC.IsValid() || citizen == Entity::GetPlayer() ||
                !citizen.Routine.IsValid() || !AI_Decision::IsHumanoid(citizen) ||
                GroupLogic::IsHeroControlled(citizen) || GroupLogic::Host::IsHeroSide(citizen))
                return false;
            Entity enclave = citizen.NPC.GetEnclave();
            if (enclave == None || !enclave.Enclave.IsValid() || enclave.Enclave.Status != gEEnclaveStatus_Attack)
                return false;
            if (!enclave.Enclave.Revolution && citizen.NPC.Type == gENPCType_Friend)
                return false;
            if (IsCivilian(citizen))
                return true;
            auto const alignment = static_cast<gEPoliticalAlignment>(citizen.NPC.PoliticalAlignment);
            if (enclave.Enclave.Revolution || alignment == gEPoliticalAlignment_Citizen ||
                alignment == enclave.Enclave.PoliticalAlignment)
                return false;
            return alignment == gEPoliticalAlignment_None || alignment == gEPoliticalAlignment_Slave ||
                alignment == gEPoliticalAlignment_Partisan || !IsEnclaveAuthority(citizen, enclave);
        }

        bool HasBattleOrder(Entity &npc)
        {
            auto const reason = static_cast<gEAttackReason>(npc.NPC.AttackReason);
            return reason == gEAttackReason_Revolution ||
                HasTask(npc, "ZS_RevolutionAttack") || HasTask(npc, "ZS_Attack");
        }

        bool CanInterruptForFlight(Entity &npc)
        {
            auto const task = npc.Routine.GetCurrentTask();
            if (task == "ZS_WatchFight" || task == "ZS_Flee" || task == citizenWaitTask || Incapacitated(npc))
                return true;

            // Some NPCs have no active daily routine entry even though their current task is valid.
            // Preserve custom tasks and quest states by only interrupting known everyday activities.
            constexpr GELPCChar everydayTasks[] = {
                "ZS_Alchemy", "ZS_Barbecue", "ZS_Bard", "ZS_Boss", "ZS_CarryStones", "ZS_Cook", "ZS_Digger",
                "ZS_Farmer", "ZS_Grind", "ZS_Guard", "ZS_HangAround", "ZS_Herd", "ZS_Lumberjack", "ZS_OrcParty",
                "ZS_Pray", "ZS_Preach", "ZS_SlaveBabe", "ZS_Sleep", "ZS_Smalltalk", "ZS_Smith", "ZS_Study",
                "ZS_Trader", "ZS_Woodwork",
            };
            for (auto const everydayTask : everydayTasks)
                if (task == everydayTask)
                    return true;
            return false;
        }

        bool IsUsingWorkInteraction(Entity &npc)
        {
            if (npc.Routine.Action == gEAction_Use)
                return true;
            if (!npc.Navigation.IsValid())
                return false;
            auto const *navigation = static_cast<gCNavigation_PS const *>(npc.Navigation.m_pEngineEntityPropertySet);
            Entity object(static_cast<eCEntity const *>(navigation->GetInteractObject()));
            return object != None && object.Interaction.IsValid() && object.Interaction.GetUser() == npc;
        }

        bool ObserveFlightStop(Entity &npc, Flight &flight, GEU32 now)
        {
            auto const position = npc.GetPosition();
            if (!flight.lastFleeProgress)
            {
                if (!HasTask(npc, "ZS_Flee"))
                    return true;
                flight.lastFleeProgress = now;
                flight.progressPosition = flight.departurePosition = position;
                return false;
            }
            if ((position - flight.progressPosition).GetSquareMagnitude() >=
                fleeProgressDistance * fleeProgressDistance)
            {
                flight.lastFleeProgress = now;
                flight.progressPosition = position;
                flight.stopLogged = false;
            }
            if (static_cast<GEInt>(now - flight.lastFleeProgress) < fleeStillTimeMs)
                return false;
            if (!flight.stopLogged && DebugLog::IsEnabled())
            {
                Entity target = npc.NPC.GetCurrentTarget();
                DebugLog::GetLogger().LogFormat(
                    "[FactionLogic] flee stopped npc='%s' task='%s' state='%s' target='%s' displacement=%.1f\n",
                    npc.GetName().GetText(), npc.Routine.GetCurrentTask().GetText(),
                    npc.Routine.GetCurrentState().GetText(), target != None ? target.GetName().GetText() : "-",
                    static_cast<double>((position - flight.departurePosition).GetMagnitude()));
                flight.stopLogged = true;
            }
            return true;
        }

        struct AuthorityHit
        {
            bCPropertyID victim;
            bCPropertyID attacker;
            GEI32 hpBefore;
            GEU32 expires;
        };
        std::vector<AuthorityHit> authorityHits;

        Entity GetAssaultEnclave(Entity &victim, Entity &attacker)
        {
            Entity hero = Entity::GetPlayer();
            if (hero == None || victim == attacker || (victim != hero && attacker != hero))
                return None;
            Entity authority = victim == hero ? attacker : victim;
            if (authority == None || !authority.NPC.IsValid())
                return None;
            Entity enclave = authority.NPC.GetEnclave();
            return enclave != None && enclave.Enclave.IsValid() && enclave.Enclave.Status == gEEnclaveStatus_Attack &&
                IsEnclaveAuthority(authority, enclave) ? enclave : None;
        }

        GEI32 ReadAuthorityHitHealth(Entity &victim)
        {
            if (victim == None)
                return -1;
            if (victim == Entity::GetPlayer() && victim.PlayerMemory.IsValid())
                return victim.PlayerMemory.GetHitPoints();
            return victim.DamageReceiver.IsValid() ? static_cast<GEI32>(victim.DamageReceiver.HitPoints) : -1;
        }

        void RecordAuthorityHit(Entity &victim, Entity &attacker)
        {
            if (victim == None || attacker == None || GetAssaultEnclave(victim, attacker) == None)
                return;
            GEI32 const hp = ReadAuthorityHitHealth(victim);
            if (hp < 0)
                return;

            auto const victimId = victim.GetInstance()->GetID();
            auto const attackerId = attacker.GetInstance()->GetID();
            if (std::any_of(authorityHits.begin(), authorityHits.end(), [&](AuthorityHit const &hit) {
                return hit.victim == victimId && hit.attacker == attackerId && hit.hpBefore == hp;
            }))
                return;
            authorityHits.push_back({victimId, attackerId, hp, ::GetTickCount() + hitConfirmationMs});
        }

        bool IsInFrontOfWitness(Entity &witness, Entity &participant)
        {
            auto const offset = participant.GetPosition() - witness.GetPosition();
            if (offset.GetSquareMagnitude() > witnessRadius * witnessRadius)
                return false;
            auto const &forward = witness.GetInstance()->GetWorldMatrix().GetZAxis();
            return forward.m_fX * offset.m_fX + forward.m_fZ * offset.m_fZ >= 0.0f;
        }

        void SendHitWitnessesFleeing(Entity &victim, Entity &attacker, Entity &enclave)
        {
            auto const members = enclave.Enclave.GetMembers();
            for (GEInt i = 0; i < members.GetCount(); ++i)
            {
                Entity witness(members.GetAt(i));
                if (witness == victim || witness == attacker || !CanSeeBattleAndFlee(witness) ||
                    witness.NPC.GetEnclave() != enclave || witness.IsDead() || HasBattleOrder(witness) ||
                    !CanInterruptForFlight(witness) || Incapacitated(witness) || HasTask(witness, "ZS_Flee") ||
                    !witness.GetInstance()->HasProcessingRangeEntered() ||
                    (!IsInFrontOfWitness(witness, victim) && !IsInFrontOfWitness(witness, attacker)))
                    continue;

                auto const id = witness.GetInstance()->GetID();
                auto flight = std::find_if(flights.begin(), flights.end(),
                    [&](Flight const &entry) { return entry.citizen == id; });
                if (flight == flights.end())
                {
                    flights.push_back({id, attacker.GetInstance()->GetID(), true});
                    SetFlightEvent(id, true);
                }
                else if (!flight->pending)
                {
                    flight->threat = attacker.GetInstance()->GetID();
                    flight->pending = true;
                    flight->directThreat = false;
                }
            }
        }

        void ConfirmAuthorityHits(GEU32 now)
        {
            for (size_t i = 0; i < authorityHits.size();)
            {
                auto const hit = authorityHits[i];
                Entity victim(hit.victim);
                Entity attacker(hit.attacker);
                Entity enclave = GetAssaultEnclave(victim, attacker);
                GEI32 const hpAfter = ReadAuthorityHitHealth(victim);
                bool const eligible = enclave != None && hpAfter >= 0;
                bool const expired = static_cast<GEInt>(now - hit.expires) >= 0;
                bool const confirmed = eligible && !expired && hpAfter < hit.hpBefore;
                if (eligible && !expired && !confirmed)
                {
                    ++i;
                    continue;
                }
                authorityHits.erase(authorityHits.begin() + i);
                if (DebugLog::IsEnabled() && victim != None && attacker != None)
                    DebugLog::GetLogger().LogFormat(
                        "[FactionLogic] authority hit direction=%s attacker='%s' victim='%s' hpBefore=%d hpAfter=%d confirmed=%d\n",
                        victim == Entity::GetPlayer() ? "authority-to-hero" : "hero-to-authority",
                        attacker.GetName().GetText(), victim.GetName().GetText(), hit.hpBefore, hpAfter,
                        static_cast<int>(confirmed));
                if (confirmed)
                    SendHitWitnessesFleeing(victim, attacker, enclave);
            }
        }
    }

    ME_DEFINE_AND_REGISTER_SCRIPT_AI_STATE(ZS_GLF_CitizenWait)
    {
        static_cast<void>(a_rRunTimeStack);
        Entity citizen(a_pSPU->GetSelfEntity());
        LoadFlights();
        if (citizen != None && citizen.Routine.IsValid() && citizen.GetInstance() &&
            CanSeeBattleAndFlee(citizen) &&
            std::any_of(flights.begin(), flights.end(), [&](auto const &entry) {
                return entry.citizen == citizen.GetInstance()->GetID();
            }))
            return GEFalse;
        if (citizen != None && citizen.Routine.IsValid())
            citizen.Routine.ContinueRoutine();
        return GETrue;
    }

    bool StopCitizenWatching(Entity &citizen)
    {
        if (!CanSeeBattleAndFlee(citizen) || !HasTask(citizen, "ZS_WatchFight"))
            return false;
        citizen.Routine.FullStop();
        citizen.NPC.SetCurrentTarget(None);
        citizen.NPC.SetAlternativeTarget(None);
        citizen.Routine.ContinueRoutine();
        return true;
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_WatchFight)
    {
        Entity witness(a_pSPU->GetSelfEntity());
        if (StopCitizenWatching(witness))
            return GETrue;
        return ZS_WatchFight_Original(a_rRunTimeStack, a_pSPU);
    }

    namespace
    {
        bool CitizenSeesOtherHeroFight(Entity &npc)
        {
            if (npc == None || !npc.NPC.IsValid() || npc.NPC.PoliticalAlignment != gEPoliticalAlignment_Citizen ||
                GroupLogic::IsHeroControlled(npc) || GroupLogic::Host::IsHeroSide(npc))
                return false;
            Entity enclave = npc.NPC.GetEnclave();
            Entity hero = Entity::GetPlayer();
            if (hero == None || enclave == None || !enclave.Enclave.IsValid() ||
                enclave.Enclave.Status != gEEnclaveStatus_Routine)
                return false;

            Entity opponent(heroFightOpponent);
            if (impactAttacker && *impactAttacker == hero)
                opponent = *impactVictim;
            else if (impactVictim && *impactVictim == hero)
                opponent = *impactAttacker;
            if (opponent == None || opponent == npc || !opponent.NPC.IsValid() ||
                opponent.NPC.GetEnclave() != enclave ||
                opponent.NPC.PoliticalAlignment == gEPoliticalAlignment_Citizen)
                return false;
            bool const currentImpact = impactAttacker && (*impactAttacker == hero || *impactVictim == hero);
            bool const recentHit = heroFightUntil && static_cast<GEInt>(::GetTickCount() - heroFightUntil) < 0;
            bool const activeFight = !opponent.IsDead() && HasBattleOrder(opponent) &&
                opponent.NPC.GetCurrentTarget() == hero;
            return (currentImpact || recentHit || activeFight) &&
                (npc.GetDistanceTo(hero) <= witnessRadius || npc.GetDistanceTo(opponent) <= witnessRadius);
        }
    }

    bool IgnoresDrawnWeapon(Entity &npc)
    {
        return (CanSeeBattleAndFlee(npc) || CitizenSeesOtherHeroFight(npc)) && !npc.IsDead() &&
            (!HasBattleOrder(npc) || npc.NPC.AttackReason == gEAttackReason_ReactToWeapon);
    }

    namespace
    {
        bool CancelWeaponWarning(gCScriptProcessingUnit *spu)
        {
            Entity npc(spu->GetSelfEntity());
            if (!IgnoresDrawnWeapon(npc) || Incapacitated(npc))
                return false;
            npc.Routine.FullStop();
            npc.NPC.SetCurrentTarget(None);
            npc.NPC.SetAlternativeTarget(None);
            npc.NPC.AttackReason = gEAttackReason_None;
            npc.NPC.CombatState = 0;
            if (npc.Inventory.IsValid())
                npc.Inventory.HoldStacks(-1, -1);
            npc.Routine.ContinueRoutine();
            if (DebugLog::IsEnabled())
                DebugLog::GetLogger().LogFormat("[FactionLogic] weapon warning cancelled npc='%s'\n",
                    npc.GetName().GetText());
            return true;
        }
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_ReactToWeapon)
    {
        if (CancelWeaponWarning(a_pSPU))
            return GETrue;
        return ZS_ReactToWeapon_Original(a_rRunTimeStack, a_pSPU);
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_ReactToWeapon_WaitForSecondWarn)
    {
        if (CancelWeaponWarning(a_pSPU))
            return GETrue;
        return ZS_ReactToWeapon_WaitForSecondWarn_Original(a_rRunTimeStack, a_pSPU);
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_ReactToWeapon_WaitForAttack)
    {
        if (CancelWeaponWarning(a_pSPU))
            return GETrue;
        return ZS_ReactToWeapon_WaitForAttack_Original(a_rRunTimeStack, a_pSPU);
    }

    void RecordImpact(Entity &victim, Entity &attacker)
    {
        Entity hero = Entity::GetPlayer();
        if (hero != None && victim != attacker && (victim == hero || attacker == hero))
        {
            Entity opponent = victim == hero ? attacker : victim;
            if (opponent != None && opponent.NPC.IsValid())
            {
                heroFightOpponent = opponent.GetInstance()->GetID();
                heroFightUntil = ::GetTickCount() + heroFightMemoryMs;
                if (opponent.GetDistanceTo(hero) <= closePursuitCombatDistance)
                {
                    auto *pursuit = FindSharedPursuit(heroFightOpponent);
                    if (pursuit && !pursuit->ending && pursuit->hero == hero.GetInstance()->GetID())
                    {
                        pursuit->recentOpponent = heroFightOpponent;
                        pursuit->closeCombatUntil = heroFightUntil;
                    }
                }
            }
        }
        if (victim != None && victim.NPC.IsValid() && attacker != None && attacker != hero && attacker.NPC.IsValid())
        {
            Entity enclave = attacker.NPC.GetEnclave();
            auto *instance = attacker.GetInstance();
            auto *victimInstance = victim.GetInstance();
            if (instance && victimInstance && IsUnaffiliatedLocal(attacker, enclave) &&
                (victim == hero || victim.NPC.GetEnclave() == enclave))
            {
                GEU32 const now = ::GetTickCount();
                auto const id = instance->GetID();
                auto const victimId = victimInstance->GetID();
                localFights.erase(std::remove_if(localFights.begin(), localFights.end(),
                    [&](auto const &fight) {
                        return (fight.attacker == id && fight.victim == victimId) ||
                            static_cast<GEInt>(now - fight.expires) >= 0;
                    }), localFights.end());
                localFights.push_back({id, victimId, now + localFightMemoryMs});
            }
        }
        if (attacker == None || attacker == victim || (!CanStartCitizenFlight(victim) &&
            (!CanSeeBattleAndFlee(victim) || HasBattleOrder(victim))))
            return;
        auto const *citizen = static_cast<eCEntity *>(victim);
        auto const *threat = static_cast<eCEntity *>(attacker);
        if (!citizen || !threat)
            return;
        auto const id = citizen->GetID();
        auto current = std::find_if(flights.begin(), flights.end(), [&](auto const &entry) { return entry.citizen == id; });
        if (current == flights.end())
        {
            flights.push_back({id, threat->GetID(), true});
            flights.back().directThreat = true;
            SetFlightEvent(id, true);
        }
        else
        {
            current->threat = threat->GetID();
            current->pending = true;
            current->directThreat = true;
        }
    }

    void Update()
    {
        LoadFlights();
        auto const now = ::GetTickCount();
        UpdateSharedPursuits(now);
        ConfirmAuthorityHits(now);
        for (auto it = flights.begin(); it != flights.end();)
        {
            Entity citizen(it->citizen);
            Entity threat(it->threat);
            if (citizen == None)
            {
                ++it;
                continue;
            }
            if (citizen.IsDead() || !citizen.NPC.IsValid() || !citizen.Routine.IsValid())
            {
                SetFlightEvent(it->citizen, false);
                it = flights.erase(it);
                continue;
            }
            bool const nativeBattle = !IsCivilian(citizen) && HasBattleOrder(citizen) &&
                (!it->pending || !it->directThreat || HasTask(citizen, "ZS_RevolutionAttack") ||
                 citizen.NPC.AttackReason == gEAttackReason_Revolution);
            bool const otherTaskTookOver = !IsCivilian(citizen) && !HasBattleOrder(citizen) &&
                !CanInterruptForFlight(citizen) &&
                !(it->pending && it->workReleaseSince && citizen.Routine.GetCurrentTask().IsEmpty());
            if ((!CanStartCitizenFlight(citizen) && !CanSeeBattleAndFlee(citizen)) || nativeBattle || otherTaskTookOver)
            {
                CancelCitizenFlight(citizen, *it);
                SetFlightEvent(it->citizen, false);
                it = flights.erase(it);
                continue;
            }
            if (!it->pending && !Incapacitated(citizen))
                ObserveFlightStop(citizen, *it, now);
            if (it->pending && (threat == None || threat.IsDead()))
            {
                it->pending = false;
                it->workReleaseSince = 0;
            }
            bool const stumbling = IsStumbling(citizen);
            if (!stumbling)
                it->stumbleSince = 0;
            else if (it->pending && !it->stumbleSince)
                it->stumbleSince = now;
            bool const stuckStumbling = it->stumbleSince && !IsDown(citizen) &&
                static_cast<GEInt>(now - it->stumbleSince) >= citizenStumbleTimeoutMs;
            if (it->pending && (!Incapacitated(citizen) || stuckStumbling))
            {
                if (it->directThreat && it->issuedThreat == it->threat && HasTask(citizen, "ZS_Flee") &&
                    citizen.NPC.GetCurrentTarget() == threat)
                {
                    it->pending = false;
                    it->workReleaseSince = 0;
                    ++it;
                    continue;
                }
                if (!it->workReleaseSince)
                {
                    bool const working = IsUsingWorkInteraction(citizen);
                    citizen.Routine.FullStop();
                    if (working)
                    {
                        citizen.ResetInteractAnimation();
                        it->workReleaseSince = now;
                        if (DebugLog::IsEnabled())
                            DebugLog::GetLogger().LogFormat("[FactionLogic] work interaction released before flee npc='%s'\n",
                                citizen.GetName().GetText());
                    }
                }
                if (it->workReleaseSince && IsUsingWorkInteraction(citizen) &&
                    static_cast<GEInt>(now - it->workReleaseSince) < workReleaseTimeoutMs)
                {
                    ++it;
                    continue;
                }
                it->workReleaseSince = 0;
                citizen.NPC.SetCurrentTarget(threat);
                citizen.NPC.SetAlternativeTarget(None);
                citizen.NPC.AttackReason = gEAttackReason_None;
                citizen.NPC.CombatState = 0;
                citizen.Routine.SetTask("ZS_Flee");
                if (stuckStumbling)
                    DebugLog::GetLogger().LogFormat("[FactionLogic] stuck stumble cleared npc='%s'\n", citizen.GetName().GetText());
                DebugLog::GetLogger().LogFormat("[FactionLogic] flee npc='%s' threat='%s'\n",
                                                citizen.GetName().GetText(), threat.GetName().GetText());
                it->pending = false;
                it->issuedThreat = it->threat;
                it->lastFleeProgress = now;
                it->progressPosition = it->departurePosition = citizen.GetPosition();
                it->stopLogged = false;
                if (DebugLog::IsEnabled())
                {
                    Entity currentTarget = citizen.NPC.GetCurrentTarget();
                    DebugLog::GetLogger().LogFormat(
                        "[FactionLogic] flee started npc='%s' task='%s' state='%s' target='%s' trigger=%s\n",
                        citizen.GetName().GetText(), citizen.Routine.GetCurrentTask().GetText(),
                        citizen.Routine.GetCurrentState().GetText(),
                        currentTarget != None ? currentTarget.GetName().GetText() : "-",
                        it->directThreat ? "direct-hit" : "witnessed-authority-hit");
                }
            }
            ++it;
        }
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Flee)
    {
        if (PreventCitizenRetreat(a_pSPU))
            return GETrue;
        return ZS_Flee_Original(a_rRunTimeStack, a_pSPU);
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_Flee_GotoFleePoint)
    {
        if (PreventCitizenRetreat(a_pSPU))
            return GETrue;
        return ZS_Flee_GotoFleePoint_Original(a_rRunTimeStack, a_pSPU);
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_CALLBACK(OnFlee_GotoFleePoint)
    {
        if (PreventCitizenRetreat(a_pSPU))
            return GETrue;
        return OnFlee_GotoFleePoint_Original(a_pSPU);
    }

    ME_DEFINE_AND_REPLACE_SCRIPT_AI_STATE(ZS_RevolutionAttack_Loop)
    {
        Entity npc(a_pSPU->GetSelfEntity());
        if (ReleaseCombatTarget(npc))
            return GETrue;
        return ZS_RevolutionAttack_Loop_Original(a_rRunTimeStack, a_pSPU);
    }

    void Reset()
    {
        sharedPursuits.clear();
        nextPursuitCheck = 0;
        flights.clear();
        flightsLoaded = false;
        localFights.clear();
        preparationHits.clear();
        lowReputationHits.clear();
        heroFightOpponent = bCPropertyID();
        heroFightUntil = 0;
        authorityHits.clear();
    }
}
