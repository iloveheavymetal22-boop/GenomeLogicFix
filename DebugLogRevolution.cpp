#include "DebugLog.h"
#include "PluginRuntime.h"
#include "GroupLogic.h"

#include <g3sdk/Script.h>
#include <g3sdk/util/Hook.h>
#include <cstring>
#include <intrin.h>
#include <algorithm>
#include <g3sdk/util/ScriptInterface.h>
#include <vector>

namespace DebugLog
{
    namespace
    {
        mCFunctionHook notifyHook;
        mCFunctionHook flagHook;
        mCFunctionHook scriptTargetHook;
        mCFunctionHook engineTargetHook;
        thread_local void *scriptTargetCaller = nullptr;
        mCCallHook comparisonHook;
        gFScript originalComparison = nullptr;
        thread_local unsigned int activeNotification = 0;
        struct FactionCreatureSample
        {
            bCPropertyID npc;
            bCPropertyID target;
            bCString phase;
            GEInt reason;
            GEInt nativeAttitude;
            GEInt finalAttitude;
            GEU32 tick;
        };
        std::vector<FactionCreatureSample> factionCreatureSamples;
        thread_local bool samplingFactionCreature = false;

        Entity Resolve(GELPVoid entity, eCEntity *fallback)
        {
            return entity ? *static_cast<Entity *>(entity) : Entity(fallback);
        }

        Entity AsEnclave(Entity &entity)
        {
            if (entity == None || entity.Enclave.IsValid())
                return entity;
            return entity.GetEnclave();
        }

        struct Member
        {
            bCPropertyID id;
            gEPoliticalAlignment alignment;
            gENPCType type;
        };

        void LogAttitudeSnapshot(Entity &enclave, char const *phase)
        {
            if (enclave == None || !enclave.Enclave.IsValid())
                return;
            Entity hero = Entity::GetPlayer();
            if (hero == None)
                return;

            auto const members = enclave.Enclave.GetMembers();
            bCString const enclaveName = enclave.GetName();
            auto const tick = static_cast<unsigned int>(::GetTickCount());
            int const status = enclave.Enclave.Status;
            bool const revolution = enclave.Enclave.Revolution;
            bool const raid = enclave.Enclave.Raid;
            bool const switched = enclave.Enclave.Switched;
            GetLogger().LogFormat(
                "[Revolution] attitude snapshot tick=%u phase=%s enclave='%s' status=%d flag=%d raid=%d switched=%d members=%d\n",
                tick, phase, enclaveName.GetText(), status, revolution, raid, switched, members.GetCount());
            for (GEInt i = 0; i < members.GetCount(); ++i)
            {
                Entity npc(members.GetAt(i));
                if (npc == None || npc == hero || !npc.NPC.IsValid())
                    continue;
                Entity home = npc.NPC.GetEnclave();
                bCString const homeName = home != None ? home.GetName() : bCString("-");
                int const political = AI_Decision::GetPoliticalAttitude(npc, enclave);
                int const revolutionAttitude = AI_Decision::GetRevolutionAttitude(npc, hero);
                int const focus = AI_Decision::GetFocusNameAttitude(npc, hero);
                GetLogger().LogFormat(
                    "[Revolution] attitude tick=%u phase=%s enclave='%s' npc='%s' instance=%p home='%s' "
                    "pal=%d type=%d class=%d political=%d storedPlayer=%d revolutionAttitude=%d focus=%d\n",
                    tick, phase, enclaveName.GetText(), npc.GetName().GetText(), static_cast<eCEntity *>(npc),
                    homeName.GetText(), static_cast<int>(npc.NPC.PoliticalAlignment), static_cast<int>(npc.NPC.Type),
                    static_cast<int>(npc.NPC.Class), political, static_cast<int>(npc.NPC.AttitudeToPlayer2),
                    revolutionAttitude, focus);
            }
        }

        void LogContext()
        {
            auto *manager = gCInfoManager_PS::GetInstance();
            auto *info = manager && manager->GetIsRunning() ? manager->GetCurrentInfo() : nullptr;
            if (info)
                GetLogger().LogFormat("[Revolution] dialogue='%s' quest='%s'\n",
                                      info->GetName().GetText(), info->GetQuest().GetText());
        }

        GEInt TraceEnclave(gFScript original, bool notification, gCScriptProcessingUnit *spu,
                           GELPVoid self, GELPVoid other, GEInt args)
        {
            if (!spu)
                return original(spu, self, other, args);

            Entity actor = Resolve(self, spu->GetSelfEntity());
            Entity subject = Resolve(other, spu->GetOtherEntity());
            Entity enclave = AsEnclave(subject);
            if (enclave == None || !enclave.Enclave.IsValid() ||
                (!activeNotification && !enclave.Enclave.Revolution && !(notification && args == 2)))
                return original(spu, self, other, args);

            bCString const name = enclave.GetName();
            bCString const actorName = actor != None ? actor.GetName() : bCString("-");
            int const beforeStatus = enclave.Enclave.Status;
            auto const members = enclave.Enclave.GetMembers();
            std::vector<Member> before;
            before.reserve(members.GetCount());
            for (GEInt i = 0; i < members.GetCount(); ++i)
            {
                auto *instance = members.GetAt(i);
                Entity npc(instance);
                if (npc != None && npc.NPC.IsValid())
                    before.push_back({instance->GetID(), npc.NPC.PoliticalAlignment, npc.NPC.Type});
            }

            GetLogger().LogFormat("[Revolution] %s enclave='%s' actor='%s' args=%d status=%d flag=%d members=%u\n",
                                  notification ? "NotifyEnclave" : "ChangeEnclaveStatus", name.GetText(),
                                  actorName.GetText(), args, beforeStatus, static_cast<int>(enclave.Enclave.Revolution),
                                  static_cast<unsigned int>(before.size()));
            if (notification)
            {
                LogContext();
                ++activeNotification;
            }
            GEInt const result = original(spu, self, other, args);
            if (notification)
                --activeNotification;

            if (enclave != None && enclave.Enclave.IsValid())
            {
                GetLogger().LogFormat("[Revolution] %s result enclave='%s' returned=%d status=%d->%d flag=%d\n",
                                      notification ? "NotifyEnclave" : "ChangeEnclaveStatus", name.GetText(), result,
                                      beforeStatus, static_cast<int>(enclave.Enclave.Status),
                                      static_cast<int>(enclave.Enclave.Revolution));
                if (!notification && beforeStatus == gEEnclaveStatus_Routine &&
                    enclave.Enclave.Status == gEEnclaveStatus_Attack && enclave.Enclave.Revolution)
                    LogAttitudeSnapshot(enclave, "combat-start");
                for (auto const &member : before)
                {
                    Entity npc(member.id);
                    if (npc == None || !npc.NPC.IsValid())
                        continue;
                    int const alignment = npc.NPC.PoliticalAlignment;
                    int const type = npc.NPC.Type;
                    if (alignment != member.alignment || type != member.type)
                        GetLogger().LogFormat("[Revolution] member changed enclave='%s' npc='%s' instance=%p pal=%d->%d type=%d->%d\n",
                                              name.GetText(), npc.GetName().GetText(), npc.GetGameEntity(),
                                              member.alignment, alignment, member.type, type);
                }
            }
            return result;
        }

        GEInt GE_STDCALL Notify(gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args)
        {
            return TraceEnclave(notifyHook.GetOriginalFunction<gFScript>(), true, spu, self, other, args);
        }

        GEInt GE_STDCALL CompareMember(gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args)
        {
            GEInt const attitude = originalComparison(spu, self, other, args);
            if (activeNotification && self && other)
            {
                auto &npc = *static_cast<Entity *>(self);
                auto &enclave = *static_cast<Entity *>(other);
                if (npc != None && npc.NPC.IsValid() && enclave != None)
                    GetLogger().LogFormat("[Revolution] member evaluated enclave='%s' npc='%s' instance=%p pal=%d type=%d class=%d attitude=%d inRange=%d\n",
                                          enclave.GetName().GetText(), npc.GetName().GetText(), npc.GetGameEntity(),
                                          static_cast<int>(npc.NPC.PoliticalAlignment), static_cast<int>(npc.NPC.Type),
                                          static_cast<int>(npc.NPC.Class), attitude,
                                          npc.Navigation.IsValid() && npc.Navigation.IsInProcessingRange());
            }
            return attitude;
        }

        void GE_STDCALL SetScriptTarget(PSNpc *properties, Entity const &target)
        {
            using Original = void (GE_STDCALL *)(PSNpc *, Entity const &);
            void *const previousCaller = scriptTargetCaller;
            scriptTargetCaller = _ReturnAddress();
            scriptTargetHook.GetOriginalFunction<Original>()(properties, target);
            scriptTargetCaller = previousCaller;
        }

        void GE_STDCALL SetEngineTarget(gCNPC_PS *properties, eCEntityProxy const &target)
        {
            using Original = void (GE_STDCALL *)(gCNPC_PS *, eCEntityProxy const &);
            auto original = engineTargetHook.GetOriginalFunction<Original>();
            if (target.IsEntityIDValid() || !properties->GetCurrentTargetEntity().IsEntityIDValid())
            {
                original(properties, target);
                return;
            }

            Entity npc(properties->GetEntity());
            Entity enclave = npc != None && npc.NPC.IsValid() ? npc.NPC.GetEnclave() : Entity();
            if (enclave == None || !enclave.Enclave.IsValid() || !enclave.Enclave.Revolution)
            {
                original(properties, target);
                return;
            }

            Entity previous(properties->GetCurrentTargetEntity().GetEntity());
            auto *caller = scriptTargetCaller ? scriptTargetCaller : _ReturnAddress();
            MEMORY_BASIC_INFORMATION memory = {};
            char modulePath[MAX_PATH] = {};
            char const *moduleName = "-";
            unsigned int offset = 0;
            if (::VirtualQuery(caller, &memory, sizeof(memory)) &&
                ::GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase), modulePath, MAX_PATH) < MAX_PATH &&
                modulePath[0])
            {
                char const *separator = std::strrchr(modulePath, '\\');
                moduleName = separator ? separator + 1 : modulePath;
                offset = static_cast<unsigned int>(reinterpret_cast<UINT_PTR>(caller) -
                                                   reinterpret_cast<UINT_PTR>(memory.AllocationBase));
            }

            bCString const name = npc.GetName();
            bCString const enclaveName = enclave.GetName();
            bCString const previousName = previous != None ? previous.GetName() : bCString("-");
            bCString const task = npc.Routine.IsValid() ? npc.Routine.GetCurrentTask() : bCString("-");
            bCString const state = npc.Routine.IsValid() ? npc.Routine.GetCurrentState() : bCString("-");
            int const mode = previous != None && previous.Routine.IsValid()
                                 ? static_cast<int>(previous.Routine.AIMode) : -1;
            int const reason = npc.NPC.AttackReason;
            original(properties, target);
            GetLogger().LogFormat(
                "[Revolution] target cleared tick=%u enclave='%s' npc='%s' previous='%s' previousMode=%d "
                "reason=%d task='%s' state='%s' caller=%s+0x%X address=%p\n",
                static_cast<unsigned int>(::GetTickCount()), enclaveName.GetText(), name.GetText(),
                previousName.GetText(), mode, reason, task.GetText(), state.GetText(), moduleName, offset, caller);
        }

        void GE_STDCALL SetFlag(gCEnclave_PS *properties, GEBool const &enabled)
        {
            using Original = void (GE_STDCALL *)(gCEnclave_PS *, GEBool const &);
            bool const before = properties->GetRevolution();
            Entity enclave(properties->GetEntity());
            bool const enabling = !before && enabled;
            if (enabling)
                LogAttitudeSnapshot(enclave, "before-flag");
            flagHook.GetOriginalFunction<Original>()(properties, enabled);
            if (enabling && properties->GetRevolution())
                LogAttitudeSnapshot(enclave, "after-flag");
            if (enclave != None)
            {
                GetLogger().LogFormat("[Revolution] flag enclave='%s' %d->%d\n",
                                      enclave.GetName().GetText(), before, properties->GetRevolution());
                LogContext();
            }
        }
    }

    bool LoadFactionDiagnostics(char const *iniPath)
    {
        char value[32] = {};
        ::GetPrivateProfileStringA("Main", "FactionDiagnostics", "", value, sizeof(value), iniPath);
        if (!value[0])
        {
            ::GetPrivateProfileStringA("Main", "RevolutionDiagnostics", "false", value, sizeof(value), iniPath);
            ::WritePrivateProfileStringA("Main", "FactionDiagnostics", value, iniPath);
        }
        return !_stricmp(value, "true") || !_stricmp(value, "yes") || !_stricmp(value, "on") || !strcmp(value, "1");
    }

    GEInt TraceEnclaveStatus(gFScript original, gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args)
    {
        if (!IsEnabled() || !PluginRuntime::GetConfig().FactionDiagnostics)
            return original(spu, self, other, args);
        return TraceEnclave(original, false, spu, self, other, args);
    }

    void InstallRevolutionDiagnostics(bool statusReady)
    {
        if (!IsEnabled() || !PluginRuntime::GetConfig().FactionDiagnostics)
            return;

        auto const *notify = GetScriptAdminExt().GetScript("NotifyEnclave");
        auto const *political = GetScriptAdminExt().GetScript("GetPoliticalAttitude");
        auto const gameScript = reinterpret_cast<GEU32>(::GetModuleHandleA("Script_Game.dll"));
        bool comparisonReady = false;
        // This site is the per-member call inside NotifyEnclave, not its initial checks on the initiator.
        if (gameScript && notify && political &&
            reinterpret_cast<GEU32>(notify->m_funcScript) == gameScript + 0x5EF70 &&
            reinterpret_cast<GEU32>(political->m_funcScript) == gameScript + 0xF620)
        {
            auto const address = gameScript + 0x5F822;
            unsigned char const expected[] = {0xE8, 0xF9, 0xFD, 0xFA, 0xFF, 0x8B, 0x3D};
            if (!std::memcmp(reinterpret_cast<void const *>(address), expected, sizeof(expected)))
            {
                originalComparison = political->m_funcScript;
                comparisonReady = comparisonHook.Prepare(address, &CompareMember).Hook();
            }
        }

        bool const notifyReady = notify && notify->m_funcScript && notifyHook.Prepare(notify->m_funcScript, &Notify).Hook();
        auto const game = ::GetModuleHandleA("Game.dll");
        auto const setter = game ? ::GetProcAddress(game, "?SetRevolution@gCEnclave_PS@@QAEXAB_N@Z") : nullptr;
        bool const flagReady = setter && flagHook.Prepare(setter, &SetFlag).ThisCall().Hook();
        auto const engineTarget = game ? ::GetProcAddress(game, "?SetCurrentTargetEntity@gCNPC_PS@@QAEXABVeCEntityProxy@@@Z") : nullptr;
        bool const engineTargetReady = engineTarget && engineTargetHook.Prepare(engineTarget, &SetEngineTarget).ThisCall().Hook();
        auto const script = ::GetModuleHandleA("Script.dll");
        auto const scriptTarget = script ? ::GetProcAddress(script, "?SetCurrentTarget@PSNpc@@QAEXABVEntity@@@Z") : nullptr;
        bool const scriptTargetReady = engineTargetReady && scriptTarget &&
            scriptTargetHook.Prepare(scriptTarget, &SetScriptTarget).ThisCall().Hook();
        GetLogger().LogFormat("[Revolution] diagnostics NotifyEnclave=%d ChangeEnclaveStatus=%d flag=%d memberComparison=%d observationIntervalMs=250 attitudeSnapshots=1 targetClear=%d targetCaller=%d\n",
                              notifyReady, statusReady, flagReady, notifyReady && comparisonReady, engineTargetReady, scriptTargetReady);
    }

    namespace
    {
        struct CombatObservation
        {
            bCPropertyID id;
            bCPropertyID target;
            bCString task;
            bCString state;
            int focus = -1;
            bool inRange = false;
        };

        struct EnclaveObservation
        {
            bCPropertyID id;
            std::vector<CombatObservation> members;
        };

        std::vector<EnclaveObservation> monitored;
        DWORD nextSample = 0;

        bool InRange(Entity &entity)
        {
            return entity.Navigation.IsValid() && entity.Navigation.IsInProcessingRange();
        }

        void SampleMember(Entity &enclave, Entity &npc, Entity &hero, CombatObservation &previous, DWORD now)
        {
            bool const inRange = InRange(npc);
            int const focus = AI_Decision::GetFocusNameAttitude(npc, hero);
            bCString const task = npc.Routine.IsValid() ? npc.Routine.GetCurrentTask() : bCString();
            bCString const state = npc.Routine.IsValid() ? npc.Routine.GetCurrentState() : bCString();
            Entity target = npc.NPC.GetCurrentTarget();
            auto *instance = target != None ? target.GetGameEntity() : nullptr;
            bCPropertyID const targetId = instance ? instance->GetID() : bCPropertyID();
            bool const initial = previous.focus < 0;
            bool const friendly = focus == gEAttitude_Friendly && focus != previous.focus;
            bool const targetChanged = targetId != previous.target;
            bool const enteredRange = !initial && inRange && !previous.inRange;
            bool const behaviourChanged = inRange && (task != previous.task || (!instance && state != previous.state));
            if (initial || friendly || targetChanged || enteredRange || behaviourChanged)
                GetLogger().LogFormat(
                    "[Revolution] observation tick=%u enclave='%s' npc='%s' instance=%p initial=%d focus=%d->%d "
                    "inRange=%d enteredRange=%d heroDistance=%.1f targetChanged=%d target='%s' targetInstance=%p "
                    "targetInRange=%d targetHeroDistance=%.1f reason=%d task='%s' state='%s'\n",
                    static_cast<unsigned int>(now), enclave.GetName().GetText(), npc.GetName().GetText(),
                    npc.GetGameEntity(), initial, previous.focus, focus, inRange, enteredRange,
                    static_cast<double>(npc.GetDistanceTo(hero)), targetChanged, instance ? target.GetName().GetText() : "-",
                    instance, instance && InRange(target), instance ? static_cast<double>(target.GetDistanceTo(hero)) : -1.0,
                    static_cast<int>(npc.NPC.AttackReason), task.GetText(), state.GetText());
            previous = {previous.id, targetId, task, state, focus, inRange};
        }
    }

    void ResetRevolutionMonitoring()
    {
        factionCreatureSamples.clear();
        monitored.clear();
        nextSample = 0;
    }

    void PollRevolutionMonitoring()
    {
        if (!IsEnabled() || !PluginRuntime::GetConfig().FactionDiagnostics)
            return;
        DWORD const now = ::GetTickCount();
        if (nextSample && static_cast<LONG>(now - nextSample) < 0)
            return;
        nextSample = now + 250;

        Entity hero = Entity::GetPlayer();
        if (hero == None)
            return;

        monitored.erase(std::remove_if(monitored.begin(), monitored.end(), [](auto const &entry)
        {
            Entity enclave(entry.id);
            return enclave == None || !enclave.Enclave.IsValid() || !enclave.Enclave.Revolution;
        }), monitored.end());

        GEInt const count = PSEnclave::GetEnclaveCount();
        for (GEInt i = 0; i < count; ++i)
        {
            Entity enclave = PSEnclave::GetEnclaveAt(i);
            if (enclave == None || !enclave.Enclave.IsValid() || !enclave.Enclave.Revolution)
                continue;
            auto const *instance = static_cast<eCEntity *>(enclave);
            if (!instance)
                continue;
            auto const id = instance->GetID();
            auto entry = std::find_if(monitored.begin(), monitored.end(), [&](auto const &item) { return item.id == id; });
            if (entry == monitored.end())
            {
                monitored.push_back({id, {}});
                entry = monitored.end() - 1;
            }
            auto const members = enclave.Enclave.GetMembers();
            for (GEInt j = 0; j < members.GetCount(); ++j)
            {
                auto const *member = members.GetAt(j);
                Entity npc(member);
                if (npc == None || npc == hero || !npc.NPC.IsValid())
                    continue;
                auto const memberId = member->GetID();
                auto &samples = entry->members;
                auto sample = std::find_if(samples.begin(), samples.end(),
                                           [&](auto const &item) { return item.id == memberId; });
                if (sample == samples.end())
                {
                    samples.push_back({memberId});
                    sample = samples.end() - 1;
                }
                SampleMember(enclave, npc, hero, *sample, now);
            }
        }
    }

    void LogRevolutionAttack(Entity &npc, Entity &target, GEInt reason)
    {
        if (reason != gEAttackReason_Revolution || !IsEnabled() || !PluginRuntime::GetConfig().FactionDiagnostics ||
            npc == None || target == None || !npc.NPC.IsValid())
            return;

        Entity enclave = npc.NPC.GetEnclave();
        bCString const name = enclave != None ? enclave.GetName() : bCString("-");
        bCString const state = npc.Routine.IsValid() ? npc.Routine.GetCurrentState() : bCString("-");
        bCString const task = npc.Routine.IsValid() ? npc.Routine.GetCurrentTask() : bCString("-");
        GetLogger().LogFormat("[Revolution] attack requested enclave='%s' npc='%s' instance=%p target='%s' targetInstance=%p reason=%d task='%s' state='%s'\n",
                              name.GetText(), npc.GetName().GetText(), npc.GetGameEntity(), target.GetName().GetText(),
                              target.GetGameEntity(), reason, task.GetText(), state.GetText());
    }

    void LogFactionCreatureTarget(Entity &npc, Entity &target, GELPCChar phase, GEInt reason,
                                  GEInt nativeAttitude, GEInt finalAttitude)
    {
        if (samplingFactionCreature || !IsEnabled() || !PluginRuntime::GetConfig().FactionDiagnostics ||
            npc == None || target == None || npc == target || !npc.NPC.IsValid() || !target.NPC.IsValid() ||
            target.NPC.PoliticalAlignment == gEPoliticalAlignment_None)
            return;
        Entity hero = Entity::GetPlayer();
        if (hero == None || npc == hero || npc.GetDistanceTo(hero) > 5000.0f ||
            !AI_Decision::IsHumanoid(npc) || AI_Decision::IsHumanoid(target))
            return;

        auto const now = ::GetTickCount();
        auto const npcId = npc.GetInstance()->GetID();
        auto const targetId = target.GetInstance()->GetID();
        factionCreatureSamples.erase(std::remove_if(factionCreatureSamples.begin(), factionCreatureSamples.end(),
            [&](FactionCreatureSample const &sample) { return now - sample.tick >= 2000; }),
            factionCreatureSamples.end());
        if (std::any_of(factionCreatureSamples.begin(), factionCreatureSamples.end(),
                [&](FactionCreatureSample const &sample) {
                    return sample.npc == npcId && sample.target == targetId && sample.phase == phase &&
                        sample.reason == reason && sample.nativeAttitude == nativeAttitude &&
                        sample.finalAttitude == finalAttitude;
                }))
            return;
        factionCreatureSamples.push_back({npcId, targetId, bCString(phase), reason, nativeAttitude, finalAttitude, now});

        samplingFactionCreature = true;
        int const politicalAttitude = AI_Decision::GetPoliticalAttitude(npc, target);
        samplingFactionCreature = false;
        Entity npcEnclave = npc.NPC.GetEnclave();
        Entity targetEnclave = target.NPC.GetEnclave();
        bCString const npcHome = npcEnclave != None ? npcEnclave.GetName() : bCString("-");
        bCString const targetHome = targetEnclave != None ? targetEnclave.GetName() : bCString("-");
        GetLogger().LogFormat(
            "[Faction] creature target tick=%u phase=%s npc='%s' npcPal=%d npcEnclave='%s' party=%d controlled=%d "
            "target='%s' targetPal=%d targetEnclave='%s' targetSpecies=%d targetClass=%d "
            "political=%d native=%d final=%d reason=%d\n",
            static_cast<unsigned int>(now), phase, npc.GetName().GetText(), static_cast<int>(npc.NPC.PoliticalAlignment),
            npcHome.GetText(), npc.Party.IsValid() ? static_cast<int>(npc.Party.PartyMemberType) : -1,
            static_cast<int>(GroupLogic::IsHeroControlled(npc)), target.GetName().GetText(),
            static_cast<int>(target.NPC.PoliticalAlignment), targetHome.GetText(),
            static_cast<int>(target.NPC.Species), static_cast<int>(target.NPC.Class), politicalAttitude,
            nativeAttitude, finalAttitude, reason);
    }
}
