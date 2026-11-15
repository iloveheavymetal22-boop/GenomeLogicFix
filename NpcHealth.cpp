#include "NpcHealth.h"
#include "DebugLog.h"
#include "GroupLogic.h"

#include <g3sdk/Script.h>
#include <g3sdk/util/Hook.h>
#include <g3sdk/util/ScriptUtil.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace NpcHealth
{
    namespace
    {
        bool scaleNpcHealth = false;
        bool scaleHealthByDifficulty = true;
        bool useHardProtectionForNpcCombat = false;
        bool useMaximumNpcLevel = true;
        long healthPerLevel = 20;
        long baseNpcHealth = 200;
        long difficultyHealthPercent = 10;
        GEU32 lastHealthUpdate = 0;

        struct CaseInsensitiveName
    {
            bool operator()(std::string const &left, std::string const &right) const
            {
                return _stricmp(left.c_str(), right.c_str()) < 0;
            }
        };
        using HealthEntries = std::map<std::string, long, CaseInsensitiveName>;
        HealthEntries healthExclusions;
        HealthEntries customNpcHealth;

        long ReadHealthNumber(char const *value, long fallback, long minimum = 1)
    {
            char *end = nullptr;
            errno = 0;
            long const number = std::strtol(value, &end, 10);
            if (end == value || errno == ERANGE || number < minimum) return fallback;
            end += std::strspn(end, " \t");
            return *end == '\0' || *end == ';' ? number : fallback;
        }

        void ReadHealthEntries(char const *path, char const *section, HealthEntries &entries, bool exclusions,
                               bool templatesOnly = false)
    {
            std::vector<char> buffer(4096);
            while (::GetPrivateProfileSectionA(section, buffer.data(), static_cast<DWORD>(buffer.size()), path)
                   >= buffer.size() - 2)
                buffer.resize(buffer.size() * 2);
            for (char const *entry = buffer.data(); *entry; entry += std::strlen(entry) + 1)
            {
                std::string const line(entry);
                auto const separator = line.find('=');
                if (separator == std::string::npos) continue;
                auto const end = line.find_last_not_of(" \t", separator - 1);
                if (separator == 0 || end == std::string::npos) continue;
                auto const start = line.find_first_not_of(" \t");
                char const *value = entry + separator + 1;
                value += std::strspn(value, " \t");
                auto const key = line.substr(start, end - start + 1);
                if (exclusions && _stricmp(key.c_str(), "Templates") == 0)
                {
                    std::string const names(value, std::strcspn(value, ";"));
                    for (size_t offset = 0; offset < names.size();)
                    {
                        auto const comma = names.find(',', offset);
                        auto const name = names.substr(offset, comma - offset);
                        auto const first = name.find_first_not_of(" \t");
                        if (first != std::string::npos)
                            entries.emplace(name.substr(first, name.find_last_not_of(" \t") - first + 1), 1);
                        if (comma == std::string::npos) break;
                        offset = comma + 1;
                    }
                    continue;
                }
                if (templatesOnly) continue;
                long const hp = exclusions ? (_stricmp(value, "true") == 0 || std::strcmp(value, "1") == 0)
                                           : ReadHealthNumber(value, 0);
                if (hp > 0) entries.emplace(key, hp);
            }
        }

    }

    void LoadConfig(char const *path)
    {
        struct DefaultSetting
        {
            char const *section;
            char const *key;
            char const *value;
        };
        DefaultSetting const defaults[] = {
            {"NPCHealth", "ScaleByLevel", "false"},
            {"NPCHealth", "BaseHP", "200"},
            {"NPCHealth", "LevelSource", "LevelMax"},
            {"NPCHealth", "HPPerLevel", "20"},
            {"NPCHealth", "ScaleByDifficulty", "true"},
            {"NPCHealth", "DifficultyHPPercent", "10"},
            {"NPCHealth", "UseHardProtectionForNpcCombat", "false"},
        };
        for (auto const &setting : defaults)
        {
            char existing[32] = {};
            ::GetPrivateProfileStringA(setting.section, setting.key, "<<missing>>",
                                       existing, sizeof(existing), path);
            if (std::strcmp(existing, "<<missing>>") == 0)
                ::WritePrivateProfileStringA(setting.section, setting.key, setting.value, path);
        }
        char templates[32] = {};
        ::GetPrivateProfileStringA("NPCHealth", "Templates", "<<missing>>", templates, sizeof(templates), path);
        bool useLegacyExclusions = std::strcmp(templates, "<<missing>>") == 0;
        char exclusions[4] = {};
        if (useLegacyExclusions &&
            ::GetPrivateProfileSectionA("NPCHealth.Exclusions", exclusions, sizeof(exclusions), path) == 0)
        {
            ::WritePrivateProfileStringA("NPCHealth", "Templates",
                                         "Duck, Chicken, Lizard, Snake, Rabbit, Turtle", path);
            useLegacyExclusions = false;
        }

        char value[64] = {};
        ::GetPrivateProfileStringA("NPCHealth", "ScaleByLevel", "false", value, sizeof(value), path);
        scaleNpcHealth = _stricmp(value, "true") == 0 || std::strcmp(value, "1") == 0;
        if (!scaleNpcHealth) return;
        ::GetPrivateProfileStringA("NPCHealth", "UseHardProtectionForNpcCombat", "false", value, sizeof(value), path);
        useHardProtectionForNpcCombat = _stricmp(value, "true") == 0 || std::strcmp(value, "1") == 0;
        ::GetPrivateProfileStringA("NPCHealth", "ScaleByDifficulty", "true", value, sizeof(value), path);
        scaleHealthByDifficulty = _stricmp(value, "true") == 0 || std::strcmp(value, "1") == 0;
        ::GetPrivateProfileStringA("NPCHealth", "LevelSource", "LevelMax", value, sizeof(value), path);
        useMaximumNpcLevel = _stricmp(value, "LevelMax") == 0;
        auto readSetting = [&](char const *key, char const *defaultText, long fallback, long minimum) {
            auto const length = ::GetPrivateProfileStringA("NPCHealth", key, defaultText, value, sizeof(value), path);
            return length < sizeof(value) - 1 ? ReadHealthNumber(value, fallback, minimum) : fallback;
        };
        healthPerLevel = readSetting("HPPerLevel", "20", 20, 1);
        baseNpcHealth = readSetting("BaseHP", "200", 200, 0);
        difficultyHealthPercent = readSetting("DifficultyHPPercent", "10", 10, 0);
        if (difficultyHealthPercent > 100) difficultyHealthPercent = 10;

        healthExclusions.clear();
        customNpcHealth.clear();
        if (useLegacyExclusions)
            ReadHealthEntries(path, "NPCHealth.Exclusions", healthExclusions, true);
        else
            ReadHealthEntries(path, "NPCHealth", healthExclusions, true, true);
        std::string const iniPath(path);
        std::string const customPath = iniPath.substr(0, iniPath.find_last_of("\\/") + 1) + "customHealth.ini";
        ReadHealthEntries(customPath.c_str(), "Health", customNpcHealth, false);
    }

    namespace
    {
        long GetConfiguredNpcHealth(Entity &npc)
        {
            auto *instance = npc.GetGameEntity();
            if (!instance || npc == Entity::GetPlayer() || npc.PlayerMemory.IsValid() ||
                !npc.NPC.IsValid() || npc.IsDead() || instance->IsKilled())
                return 0;
            auto const *source = instance->GetCreator();
            if (!source) return 0;
            auto const *templateNpc = static_cast<gCNPC_PS const *>(source->GetPropertySet(eEPropertySetType_NPC));
            if (!templateNpc) return 0;
            auto const level = templateNpc->GetLevel();
            auto const levelMax = templateNpc->GetLevelMax();
            char const *templateName = source->GetName().GetText();
            if (healthExclusions.count(templateName)) return 0;

            auto manual = customNpcHealth.find(npc.GetName().GetText());
            if (manual == customNpcHealth.end()) manual = customNpcHealth.find(templateName);
            if (manual != customNpcHealth.end()) return manual->second;

            long percent = 100;
            if (scaleHealthByDifficulty)
            {
                auto const difficulty = Entity::GetCurrentDifficulty();
                if (difficulty == EDifficulty_Easy) percent -= difficultyHealthPercent;
                if (difficulty == EDifficulty_Hard) percent += difficultyHealthPercent;
            }
            auto const selectedLevel = useMaximumNpcLevel ? levelMax : level;
            auto const base = baseNpcHealth + static_cast<unsigned long long>(selectedLevel) * healthPerLevel;
            auto const scaled = (base / 100) * percent + (base % 100) * percent / 100;
            return static_cast<long>(std::clamp(scaled, 1ULL,
                static_cast<unsigned long long>((std::numeric_limits<long>::max)())));
        }

        mCFunctionHook maxHealthScriptHook;
        bool maxHealthScriptHooked = false;
        mCFunctionHook protectionScriptHook;
        mCFunctionHook levelMaxScriptHook;
        bool protectionHooksAttempted = false;
        struct ProtectionScope
        {
            static thread_local ProtectionScope *current;
            ProtectionScope *previous = current;
            eCEntity const *target;
            bool substituted = false;
            bool useHardLevel;
            GEInt levelUsed = -1;

            ProtectionScope(eCEntity const *entity, bool substitute) : target(entity), useHardLevel(substitute)
            { current = this; }
            ~ProtectionScope() { current = previous; }
        };
        thread_local ProtectionScope *ProtectionScope::current = nullptr;

        GEInt GE_STDCALL ReadCombatProtection(gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args)
        {
            Entity victim = self ? *static_cast<Entity *>(self) : Entity(spu ? spu->GetSelfEntity() : nullptr);
            Entity source = other ? *static_cast<Entity *>(other) : Entity(spu ? spu->GetOtherEntity() : nullptr);
            Entity attacker = GroupLogic::Host::GetDamageOwnerNpc(source);
            bool const npcCombat = victim != None && attacker != None && victim != attacker &&
                victim.IsNPC() && attacker.IsNPC() && !GroupLogic::IsHeroControlled(victim) &&
                !GroupLogic::IsHeroControlled(attacker);
            auto const difficulty = Entity::GetCurrentDifficulty();
            bool const useHardLevel = npcCombat && useHardProtectionForNpcCombat && difficulty != EDifficulty_Hard;
            ProtectionScope scope(npcCombat ? victim.GetGameEntity() : nullptr, useHardLevel);
            GEInt const protection = protectionScriptHook.GetOriginalFunction<gFScript>()(spu, self, other, args);
            if (npcCombat && DebugLog::IsNpcCombatEnabled())
                DebugLog::LogNpcProtection(attacker, victim, source, difficulty,
                                          useHardProtectionForNpcCombat, scope.substituted, protection, scope.levelUsed);
            return protection;
        }

        GEInt GE_STDCALL ReadProtectionLevelMax(gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args)
        {
            auto *scope = ProtectionScope::current;
            if (scope && scope->target)
            {
                Entity npc = self ? *static_cast<Entity *>(self) : Entity(spu ? spu->GetSelfEntity() : nullptr);
                if (npc.GetGameEntity() == scope->target)
                {
                    auto const *properties = static_cast<gCNPC_PS const *>(
                        scope->target->GetPropertySet(eEPropertySetType_NPC));
                    if (properties && scope->useHardLevel)
                    {
                        scope->substituted = true;
                        // Stock GetLevelMax adds ten to the live NPC property on Hard.
                        scope->levelUsed = static_cast<GEInt>(std::clamp(static_cast<long long>(properties->GetLevelMax()) + 10,
                            1LL, static_cast<long long>((std::numeric_limits<GEInt>::max)())));
                        return scope->levelUsed;
                    }
                    scope->levelUsed = levelMaxScriptHook.GetOriginalFunction<gFScript>()(spu, self, other, args);
                    return scope->levelUsed;
                }
            }
            return levelMaxScriptHook.GetOriginalFunction<gFScript>()(spu, self, other, args);
        }

        GEInt GE_STDCALL ReadNpcMaxHealth(gCScriptProcessingUnit *spu, GELPVoid self, GELPVoid other, GEInt args)
        {
            Entity npc = self ? *static_cast<Entity *>(self) : Entity(spu ? spu->GetSelfEntity() : nullptr);
            if (GetConfiguredNpcHealth(npc) > 0 && npc.DamageReceiver.IsValid())
            {
                GEInt const maximum = npc.DamageReceiver.HitPointsMax;
                if (maximum > 0) return maximum;
            }
            return maxHealthScriptHook.GetOriginalFunction<gFScript>()(spu, self, other, args);
        }
    }

    void InstallHook()
    {
        if (!scaleNpcHealth) return;
        if ((useHardProtectionForNpcCombat || DebugLog::IsNpcCombatEnabled()) && !protectionHooksAttempted)
        {
            protectionHooksAttempted = true;
            auto const *protection = GetScriptAdminExt().GetScript("GetProtection");
            auto const *levelMax = GetScriptAdminExt().GetScript("GetLevelMax");
            bool const levelHookReady = levelMax && levelMax->m_funcScript &&
                levelMaxScriptHook.Prepare(levelMax->m_funcScript, &ReadProtectionLevelMax).Hook();
            bool const installed = protection && protection->m_funcScript && levelHookReady &&
                protectionScriptHook.Prepare(protection->m_funcScript, &ReadCombatProtection).Hook();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][npc-health] NPC combat protection hooks: %s.\n",
                installed ? "installed" : "failed");
        }
        if (maxHealthScriptHooked) return;
        auto const *script = GetScriptAdminExt().GetScript("GetHitPointsMax");
        maxHealthScriptHooked = script && script->m_funcScript &&
            maxHealthScriptHook.Prepare(script->m_funcScript, &ReadNpcMaxHealth).Hook();
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][npc-health] GetHitPointsMax hook: %s.\n",
            maxHealthScriptHooked ? "installed" : "failed");
    }

    void Update(gCSession &session)
    {
        if (!scaleNpcHealth) return;
        GEU32 const now = ::GetTickCount();
        if (lastHealthUpdate && now - lastHealthUpdate < 200) return;
        lastHealthUpdate = now;

        auto const *hero = session.GetHero();
        auto npcs = Entity::GetNPCs();
        for (GEInt i = 0; i < npcs.GetCount(); ++i)
        {
            Entity npc = npcs.GetAt(i);
            auto *instance = npc.GetGameEntity();
            if (!instance || instance == hero) continue;
            long const maximum = GetConfiguredNpcHealth(npc);
            if (maximum <= 0) continue;

            auto *health = static_cast<gCDamageReceiver_PS *>(instance->GetPropertySet(eEPropertySetType_DamageReceiver));
            if (!health) continue;
            long const previousMax = health->GetHitPointsMax();
            long const previous = health->GetHitPoints();
            if (previousMax <= 0 || previous <= 0 || maximum == previousMax) continue;
            long const current = static_cast<long>(std::clamp(
                (static_cast<long long>(previous) * maximum + previousMax / 2) / previousMax,
                1LL, static_cast<long long>(maximum)));
            health->SetHitPointsMax(maximum);
            health->SetHitPoints(current);
        }
    }
}
