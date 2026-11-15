#include "DebugLog.h"
#include "PluginRuntime.h"
#include "GroupLogic.h"
#include <vector>

#include <cstring>
#include <algorithm>

namespace DebugLog
{
    namespace
    {
        class SilentLogger final : public mCLoggerBase
        {
        public:
            void LogString(GELPCChar) override {}
            void LogFormat(GELPCChar, ...) override {}
        };
        SilentLogger &GetSilentLogger()
        {
            static SilentLogger logger;
            return logger;
        }
        GEBool g_Enabled = GEFalse;
        GEBool g_Initialized = GEFalse;
        bCString g_LogPath;
        struct DamageSample
        {
            void const *instance;
            bCPropertyID id;
            bCString attacker;
            bCString source;
            GEInt before;
            GEInt protection;
            GEInt damageType;
            GEInt difficulty;
            unsigned int calculations = 1;
        };
        std::vector<DamageSample> damageSamples;

        gCNPC_PS const *GetNpcProperties(Entity &npc)
        {
            auto const *entity = npc.GetGameEntity();
            return entity ? static_cast<gCNPC_PS const *>(entity->GetPropertySet(eEPropertySetType_NPC)) : nullptr;
        }
    }

    GEBool IsEnabled() { return g_Enabled; }

    bool IsNpcCombatEnabled() { return IsEnabled() && PluginRuntime::GetConfig().NpcCombatDiagnostics; }

    void FlushNpcCombatDiagnostics(bool gameRunning)
    {
        if (damageSamples.empty()) return;
        if (gameRunning)
        {
            auto npcs = Entity::GetNPCs();
            for (GEInt i = 0; i < npcs.GetCount(); ++i)
            {
                Entity victim = npcs.GetAt(i);
                auto const *instance = victim.GetInstance();
                if (!instance || !victim.DamageReceiver.IsValid()) continue;
                auto const sample = std::find_if(damageSamples.begin(), damageSamples.end(),
                    [&](DamageSample const &item) { return item.instance == instance && item.id == instance->GetID(); });
                if (sample == damageSamples.end()) continue;
                GEInt const after = victim.DamageReceiver.HitPoints;
                if (after >= sample->before) continue;
                GetLogger().LogFormat(
                    "[GenomeLogicFix][npc-damage] attacker='%s' victim='%s' source='%s' difficulty=%d "
                    "damage_type=%s(%d) protection=%d hp_before=%d hp_after=%d hp_lost=%d calculations=%u\n",
                    sample->calculations == 1 ? sample->attacker.GetText() : "<multiple>",
                    victim.GetName().GetText(), sample->calculations == 1 ? sample->source.GetText() : "<multiple>",
                    sample->difficulty, sample->calculations == 1 ? GroupLogic::Host::DamageTypeName(sample->damageType) : "?",
                    sample->calculations == 1 ? sample->damageType : -1, sample->calculations == 1 ? sample->protection : -1,
                    sample->before, after, sample->before - (std::max)(0, after), sample->calculations);
            }
        }
        damageSamples.clear();
    }

    void LogNpcProtection(Entity &attacker, Entity &victim, Entity &source, GEInt difficulty,
                          bool optionEnabled, bool substituted, GEInt protection, GEInt levelUsed)
    {
        Entity hero = Entity::GetPlayer();
        auto const *heroNpc = GetNpcProperties(hero);
        auto const *targetNpc = GetNpcProperties(victim);
        auto const *entity = victim.GetGameEntity();
        auto const *creator = entity ? entity->GetCreator() : nullptr;
        auto const *templateNpc = creator ? static_cast<gCNPC_PS const *>(
            creator->GetPropertySet(eEPropertySetType_NPC)) : nullptr;
        auto const damage = source != None && source.Damage.IsValid()
            ? source.Damage.DamageType : gEDamageType_None;
        if (victim.DamageReceiver.IsValid())
        {
            auto const *instance = victim.GetInstance();
            if (instance)
            {
                auto const sample = std::find_if(damageSamples.begin(), damageSamples.end(),
                    [&](DamageSample const &item) { return item.instance == instance && item.id == instance->GetID(); });
                if (sample != damageSamples.end())
                    ++sample->calculations;
                else
                    damageSamples.push_back({instance, instance->GetID(), attacker.GetName(), source.GetName(),
                        victim.DamageReceiver.HitPoints, protection, damage, difficulty});
            }
        }
        char const *attack = "unknown";
        if (damage == gEDamageType_Missile)
            attack = "missile";
        else if (source != None && GroupLogic::Host::IsPointMagicSource(&source))
            attack = "spell";
        else if (damage == gEDamageType_Blade || damage == gEDamageType_Impact)
            attack = "melee";
        GetLogger().LogFormat(
            "[GenomeLogicFix][npc-protection] attacker='%s' victim='%s' source='%s' difficulty=%d "
            "attack=%s damage_type=%s(%d) protection=%d option=%d hard_level_used=%d "
            "hero_level=%d target_level_max=%d template_level_max=%d protection_level_max=%d\n",
            attacker.GetName().GetText(), victim.GetName().GetText(), source.GetName().GetText(),
            difficulty, attack, GroupLogic::Host::DamageTypeName(damage), static_cast<GEInt>(damage),
            protection, optionEnabled ? 1 : 0, substituted ? 1 : 0,
            heroNpc ? static_cast<GEInt>(heroNpc->GetLevel()) : -1,
            targetNpc ? static_cast<GEInt>(targetNpc->GetLevelMax()) : -1,
            templateNpc ? static_cast<GEInt>(templateNpc->GetLevelMax()) : -1, levelUsed);
    }

    bCString Initialize()
    {
        if (g_Initialized)
            return g_LogPath;
        g_Initialized = GETrue;
        g_Enabled = PluginRuntime::GetConfig().Diagnostics;
        SetDefaultLogger(GetSilentLogger());
        if (!g_Enabled)
            return bCString();

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

        g_LogPath = strLogPath;
        return g_LogPath;
    }

    mCLoggerBase &GetLogger()
    {
        return g_Enabled ? GetDefaultLogger() : GetSilentLogger();
    }
}
