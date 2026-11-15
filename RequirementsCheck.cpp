#include "DebugLog.h"
#include "RequirementsCheck.h"
#include "PluginRuntime.h"

#include <g3sdk/Script.h>
#include <g3sdk/util/Hook.h>
#include <g3sdk/util/Logging.h>
#include <g3sdk/util/Memory.h>
#include <g3sdk/util/Util.h>

#include <intrin.h>

#include <cstdarg>
#include <cstdio>

namespace RequirementsCheck
{
    namespace
    {
        //
        // Exports of Game.dll (decorated names, verified against Game.lib and Game.dll 1.8.7).
        //
        GELPCChar const NAME_OnApplyStats = "?OnApplyStats@gCInventorySlot@@MAEXPAVeCEntity@@0@Z";
        GELPCChar const NAME_OnUnApplyStats = "?OnUnApplyStats@gCInventorySlot@@MAEXPAVeCEntity@@0@Z";
        GELPCChar const NAME_SetModifier = "?SetModifier@gCAttribute@@QAEXABH@Z";
        GELPCChar const NAME_SetBaseValue = "?SetBaseValue@gCAttribute@@QAEXABH@Z";
        GELPCChar const NAME_SetBaseMaximum = "?SetBaseMaximum@gCStat@@QAEXABH@Z";
        GELPCChar const NAME_SetMaximumModifier = "?SetMaximumModifier@gCStat@@QAEXABH@Z";
        GELPCChar const NAME_SetMaximum = "?SetMaximum@gCStat@@UAEXH@Z";
        GELPCChar const NAME_EquipStack = "?EquipStack@gCInventory_PS@@QAE_NH@Z";
        GELPCChar const NAME_EquipStackToSlot = "?EquipStackToSlot@gCInventory_PS@@QAE_NHW4gESlot@@@Z";
        GELPCChar const NAME_UnEquipStack = "?UnEquipStack@gCInventory_PS@@QAE_NH@Z";
        // Other gCInventory_PS functions that link / unlink stacks (they call gCInventorySlot::OnApplyStats /
        // OnUnApplyStats as well, e.g. when a potion or meat is taken into the hand to be consumed).
        GELPCChar const NAME_LinkStackToSlot = "?LinkStackToSlot@gCInventory_PS@@QAE_NHW4gESlot@@_N@Z";
        GELPCChar const NAME_UnlinkStackFromSlot = "?UnlinkStackFromSlot@gCInventory_PS@@QAEHW4gESlot@@@Z";
        GELPCChar const NAME_HoldStacks = "?HoldStacks@gCInventory_PS@@QAE_NHH@Z";
        GELPCChar const NAME_HoldStackLeftHand = "?HoldStackLeftHand@gCInventory_PS@@QAE_NH@Z";
        GELPCChar const NAME_HoldStackRightHand = "?HoldStackRightHand@gCInventory_PS@@QAE_NH@Z";
        GELPCChar const NAME_SetIsConsumingItem = "?SetIsConsumingItem@gCPlayerMemory_PS@@QAEXAB_N@Z";
        // gCPlayerMemory_PS: real mutation paths of the hero attributes.
#define RC_PM_NAME(FN) "?" #FN "@gCPlayerMemory_PS@@QAE_NABVbCString@@H@Z"
#define RC_PM_OP_NAME(FN) "?" #FN "@gCPlayerMemory_PS@@QAE_NABVbCString@@HW4EAttribModOperation@@@Z"
        GELPCChar const NAME_MemoryAddBaseValue = RC_PM_NAME(AddBaseValue);
        GELPCChar const NAME_MemoryAddValue = RC_PM_NAME(AddValue);
        GELPCChar const NAME_MemorySetBaseValue = RC_PM_NAME(SetBaseValue);
        GELPCChar const NAME_MemorySetValue = RC_PM_NAME(SetValue);
        GELPCChar const NAME_MemoryAddModifier = RC_PM_NAME(AddModifier);
        GELPCChar const NAME_MemorySetModifier = RC_PM_NAME(SetModifier);
        GELPCChar const NAME_MemoryAddMaximum = RC_PM_NAME(AddMaximum);
        GELPCChar const NAME_MemorySetMaximum = RC_PM_NAME(SetMaximum);
        GELPCChar const NAME_MemoryApplyMod = RC_PM_OP_NAME(ApplyMod);
        GELPCChar const NAME_MemoryUnapplyMod = RC_PM_OP_NAME(UnapplyMod);
        // Diagnostics only (RC_DIAGNOSTICS): functions that may start the animation of the hero.
        GELPCChar const NAME_DiagSetCurrentState = "?SetCurrentState@gCScriptRoutine_PS@@QAEXABVbCString@@@Z";
        GELPCChar const NAME_DiagSetCurrentTask = "?SetCurrentTask@gCScriptRoutine_PS@@QAEXABVbCString@@@Z";
        GELPCChar const NAME_DiagAISetState = "?AISetState@gCScriptRoutine_PS@@QAEXABVbCString@@@Z";
        GELPCChar const NAME_DiagAISetTask = "?AISetTask@gCScriptRoutine_PS@@QAEXABVbCString@@@Z";
        // Script.dll: the native requirement check of the scripts. PSInventory::PrintMissingRequirements(int stack,
        // bool print) const calls gCInventory_PS::CanEquip and, if print is set, shows the "missing attribute / skill"
        // warning of the HUD (HUD_MissingAttrib / HUD_MissingSkill). It returns GEFalse if the item cannot be used.
        GELPCChar const NAME_ScriptModule = "Script.dll";
        GELPCChar const NAME_PrintMissingRequirements = "?PrintMissingRequirements@PSInventory@@QBE_NH_N@Z";
        GELPCChar const NAME_StatVtable = "??_7gCStat@@6B@";
        GELPCChar const NAME_CurrentSession = "?ms_pCurrentSession@gCSession@@1PAV1@A";

        // Vtable of gCStat. Objects of this class are the dynamic attributes (HP, MP, SP).
        GELPVoid g_pStatVtable = nullptr;

        // Address of gCSession::ms_pCurrentSession. Guards against touching the session before / after the game.
        gCSession **g_ppCurrentSession = nullptr;

        // Nesting depth of running gCInventory_PS equip / unequip operations of the hero.
        GEInt g_iOperationDepth = 0;
        // A trigger fired while an operation was running, the check runs when the outermost operation returns.
        GEBool g_bPending = GEFalse;
        // The check itself is running (its own UnEquipStack calls must not start another check).
        GEBool g_bEnforcing = GEFalse;

        struct EnforceScope
        {
            EnforceScope()
            {
                g_bEnforcing = GETrue;
            }
            ~EnforceScope()
            {
                g_bEnforcing = GEFalse;
            }
        };

        GELPVoid ResolveExport(GELPCChar a_strModule, GELPCChar a_strName)
        {
            HMODULE hModule = ::GetModuleHandleA(a_strModule);
            GELPVoid pAddress = hModule ? reinterpret_cast<GELPVoid>(::GetProcAddress(hModule, a_strName)) : nullptr;
            if (!pAddress)
                DebugLog::GetLogger().LogFormat("[RequirementsCheck] Export not found in %s: %s\n", a_strModule, a_strName);
            return pAddress;
        }

        GELPVoid ResolveGameExport(GELPCChar a_strName)
        {
            return ResolveExport("Game.dll", a_strName);
        }

        //
        // Access to the hero.
        //
        template <typename T>
        T *GetPropertySetOf(eCEntity *a_pEntity, eEPropertySetType a_enuType)
        {
            return a_pEntity ? static_cast<T *>(a_pEntity->GetPropertySet(a_enuType)) : nullptr;
        }

        // Hero = PC_Hero (not the transformed player entity), the owner of the equipment that is validated.
        // Returns nullptr while no session exists (main menu, loading), so no hook touches half-built state.
        eCEntity *GetHeroEntity()
        {
            if (!g_ppCurrentSession || !*g_ppCurrentSession)
                return nullptr;

#if RC_REQUIRE_GAME_RUNNING
            // While a savegame is loaded attributes and equipment are restored step by step, so requirements can
            // look violated for a moment. Do not touch the equipment until the game is running.
            if (!(*g_ppCurrentSession)->IsGameRunning())
            {
                static GEBool s_bLogged = GEFalse;
                if (!s_bLogged)
                {
                    s_bLogged = GETrue;
                    DebugLog::GetLogger().LogFormat("[RequirementsCheck] Check skipped: gCSession::IsGameRunning() is "
                                                 "false (build with RC_REQUIRE_GAME_RUNNING=0 to disable).\n");
                }
                return nullptr;
            }
#endif
            return (*g_ppCurrentSession)->GetHero();
        }

        gCInventory_PS *GetHeroInventory()
        {
            return GetPropertySetOf<gCInventory_PS>(GetHeroEntity(), eEPropertySetType_Inventory);
        }

        gCPlayerMemory_PS *GetHeroMemory()
        {
            return GetPropertySetOf<gCPlayerMemory_PS>(GetHeroEntity(), eEPropertySetType_PlayerMemory);
        }

        // GETrue while the hero is in the middle of consuming an item (potion, meat, elixir, ...). The inventory and
        // the stacks are being modified by the game then, the check must not touch the equipment.
        GEBool IsHeroConsumingItem()
        {
            gCPlayerMemory_PS const *pMemory = GetHeroMemory();
            return pMemory && pMemory->GetIsConsumingItem();
        }

        // GETrue, if the stack with the given index is (or may be) a piece of equipment. Consumables, food, keys, ...
        // never change which requirements are met, so equipping / unequipping them is no reason to run a check.
        // Unknown / invalid indices are treated as equipment (the safe direction: the check runs).
        GEBool IsEquipmentStack(gCInventory_PS const *a_pInventory, GEInt a_iIndex)
        {
            if (!a_pInventory || a_iIndex < 0 || a_iIndex >= a_pInventory->GetStackCount())
                return GETrue;
            gCInventoryStack const *pStack = a_pInventory->GetStack(a_iIndex);
            return !pStack || pStack->IsEquipment();
        }

        //
        // Current attribute values of the hero.
        //
        GEBool ReadFromStats(bCString const &a_strTag, GEBool a_bMax, GEInt &o_iValue)
        {
            gCStatistics_PS *pStatistics =
                GetPropertySetOf<gCStatistics_PS>(GetHeroEntity(), eEPropertySetType_Statistics);
            if (!pStatistics)
                return GEFalse;

            GEInt iIndex = gCStatsAdmin::GetInstance().GetAttribIndex(a_strTag);
            gCStats &Stats = pStatistics->GetStats();
            if (iIndex < 0 || iIndex >= Stats.GetAttribCount())
                return GEFalse;

            gCAttrib *pAttrib = Stats.GetAttrib(iIndex);
            if (!pAttrib)
                return GEFalse;

            o_iValue = a_bMax ? pAttrib->GetMaximum() : pAttrib->GetValue();
            return GETrue;
        }

        GEBool ReadFromPlayerMemory(bCString const &a_strTag, GEBool a_bMax, GEInt &o_iValue)
        {
            gCPlayerMemory_PS *pMemory = GetHeroMemory();
            if (!pMemory || !pMemory->GetAttribute(a_strTag))
                return GEFalse;

            o_iValue = a_bMax ? pMemory->GetMaximum(a_strTag) : pMemory->GetValue(a_strTag);
            return GETrue;
        }

        GEBool ReadAttribute(bCString const &a_strTag, GEBool a_bMax, GEInt &o_iValue)
        {
            // HP / MP / SP (gCStat): the CURRENT value is never read. With the Update Pack, potions, meat and herbs
            // restore it gradually through background regeneration ticks, so gCStats and gCPlayerMemory_PS are in an
            // intermediate state in the middle of a frame and would look "mismatched". A requirement on the current
            // value of a dynamic stat is reported as "not readable", i.e. it is never violated, so the check can never
            // unequip anything because of a tick. Only the (stable) maximum is evaluated, from gCPlayerMemory_PS.
            gCPlayerMemory_PS *pHeroMemory = GetHeroMemory();
            if (IsDynamicStat(pHeroMemory ? pHeroMemory->GetAttribute(a_strTag) : nullptr))
            {
                if (!a_bMax)
                    return GEFalse;

                if (ReadFromPlayerMemory(a_strTag, GETrue, o_iValue))
                    return GETrue;
                return ReadFromStats(a_strTag, GETrue, o_iValue);
            }

            GEInt iStats = 0;
            GEInt iMemory = 0;
            GEBool const bStats = ReadFromStats(a_strTag, a_bMax, iStats);
            GEBool const bMemory = ReadFromPlayerMemory(a_strTag, a_bMax, iMemory);

            // gCStats is the source required by the specification. Both sources should agree. If they do not, the
            // game maintains only one of them. gCPlayerMemory_PS holds the attribute objects that the game really
            // changes (ApplyMod, Add*, Set*), so it wins; otherwise a stale gCStats would unequip everything.
            static GEBool s_bMismatchLogged = GEFalse;
            GEBool const bMismatch = bStats && bMemory && iStats != iMemory;
            if (bMismatch && !s_bMismatchLogged)
            {
                s_bMismatchLogged = GETrue;
                DebugLog::GetLogger().LogFormat("[RequirementsCheck] WARNING: gCStats (%d) and gCPlayerMemory_PS (%d) "
                                             "disagree for attribute '%s' (max=%d), using gCPlayerMemory_PS.\n",
                                             iStats, iMemory, static_cast<GELPCChar>(a_strTag), a_bMax ? 1 : 0);
            }

            if (bMemory && (RC_PREFER_PLAYER_MEMORY || bMismatch || !bStats))
                o_iValue = iMemory;
            else if (bStats)
                o_iValue = iStats;
            return bStats || bMemory;
        }

        //
        // Item data. Offsets are those of gCItem_PS (see GE_ASSERT_PROPERTY in ge_item_ps.h), index n = 0..3 / 0..5
        // for ReqAttrib1..4 / ModAttrib1..6, stride 0x10:
        //   Req: Tag @ 0x74 + 0x10 * n, Op @ 0x7C + 0x10 * n, Value @ 0x80 + 0x10 * n
        //   Mod: Tag @ 0xCC + 0x10 * n, Op @ 0xD4 + 0x10 * n, Value @ 0xD8 + 0x10 * n
        // The Op property is a bTPropertyContainer<enum> (8 bytes: vtable + value), the value sits at +4.
        //
        GEInt const REQ_COUNT = 4;
        GEInt const MOD_COUNT = 6;
        GEU32 const REQ_TAG_OFFSET = 0x74;
        GEU32 const REQ_OP_OFFSET = 0x7C;
        GEU32 const REQ_VALUE_OFFSET = 0x80;
        GEU32 const MOD_TAG_OFFSET = 0xCC;
        GEU32 const MOD_OP_OFFSET = 0xD4;
        GEU32 const MOD_VALUE_OFFSET = 0xD8;
        GEU32 const SLOT_STRIDE = 0x10;

        template <typename T>
        T const &ItemField(gCItem_PS const &a_Item, GEU32 a_u32Base, GEInt a_iIndex)
        {
            return *reinterpret_cast<T const *>(reinterpret_cast<GELPByte>(const_cast<gCItem_PS *>(&a_Item)) +
                                                a_u32Base + SLOT_STRIDE * a_iIndex);
        }

        // Bonus that the item gives to a_strTag with the operation a_enuOp (AddToVal / AddToMax only).
        // The bonus the item REALLY gives the hero in its slot right now (not the bare template value).
        // gCInventorySlot::OnApplyStats (Game.dll 1.8.7, 0x100BE5F0) applies every modifier of the item through
        // gCPlayerMemory_PS::ApplyMod, and when the quality of the STACK has gEItemQuality_Worn (0x100) it applies
        // value / 2 (truncated towards zero) instead of the full value. The quality is read with the native getter
        // gCInventoryStack::GetQuality(), the modifiers with the same rule as the engine.
        // a_pStack == nullptr: the item is not worn (bare template value).
        GEInt GetOwnBonus(gCItem_PS const &a_Item, bCString const &a_strTag, EAttribModOperation a_enuOp,
                          gCInventoryStack const *a_pStack = nullptr)
        {
            GEBool const bWorn = a_pStack && (static_cast<GEU32>(a_pStack->GetQuality()) & gEItemQuality_Worn) != 0;
            GEInt iSum = 0;
            for (GEInt n = 0; n < MOD_COUNT; ++n)
            {
                if (ItemField<bCString>(a_Item, MOD_TAG_OFFSET, n) != a_strTag)
                    continue;
                if (static_cast<EAttribModOperation>(ItemField<GEInt>(a_Item, MOD_OP_OFFSET, n)) != a_enuOp)
                    continue;

                GEInt iValue = ItemField<GEInt>(a_Item, MOD_VALUE_OFFSET, n);
                if (bWorn)
                    iValue /= 2; // the engine's own halving of worn items (C division truncates like cdq/sar)
                iSum += iValue;
            }
            return iSum;
        }

        //
        // Update Pack "Self-forged" (HUD_Quality_Selfmade) items. Verified by disassembling the Update Pack 1.x
        // binaries (Game.dll / scripts\Script_Game.dll):
        //   - gCInventory_PS::CanEquip (Game.dll 0x100B7DC0) builds the quality flags exactly like
        //     gCInventoryStack::GetCombinedQuality() (stack quality | gCItem_PS quality), calls the script function
        //     "OnPlayerGetEquipBonus" with them and ADDS the result to ReqAttrib1..4 Value, clamping each to >= 0.
        //   - OnPlayerGetEquipBonus (Script_Game.dll 0x1006B840) returns -10 when (quality & 0x80), i.e.
        //     gEItemQuality_Forged, and 0 otherwise. The value is hard-coded in the script (no ini / config key).
        // The template of the item keeps the "bare" requirement, so the plugin has to apply the same correction,
        // otherwise a self-forged weapon is unequipped although the game itself lets the hero wield it.
        //
        // Equip bonus of the stack, the C++ twin of the script function OnPlayerGetEquipBonus.
        GEInt GetEquipBonus(gCInventoryStack const *a_pStack)
        {
            if (!a_pStack)
                return 0;
            GEU32 const u32Quality = a_pStack->GetCombinedQuality();
            return (u32Quality & RC_SELF_FORGED_QUALITY_FLAG) == RC_SELF_FORGED_QUALITY_FLAG
                       ? -RC_SELF_FORGED_REQ_REDUCTION
                       : 0;
        }

        // GETrue, if the equip bonus applies to the requirement on a_strTag. The game applies it to every
        // requirement slot; RC_SELF_FORGED_ONLY_STR_DEX=1 restricts it to the "value" requirements of STR / DEX.
        GEBool IsEquipBonusApplicable(bCString const &a_strTag, GEBool a_bMax)
        {
#if RC_SELF_FORGED_ONLY_STR_DEX
            return !a_bMax && (a_strTag == "STR" || a_strTag == "DEX");
#else
            (void)a_strTag;
            (void)a_bMax;
            return GETrue;
#endif
        }

        // The requirement the hero really has to meet: the template value plus the equip bonus (negative for
        // self-forged items), never below 0. This is the "effective requirement" counterpart of ReadAttribute,
        // which reads the hero's side.
        GEInt GetEffectiveRequirement(gCInventoryStack const *a_pStack, bCString const &a_strTag, GEBool a_bMax,
                                      GEInt a_iTemplateRequired)
        {
            if (a_iTemplateRequired <= 0 || !IsEquipBonusApplicable(a_strTag, a_bMax))
                return a_iTemplateRequired;

            GEInt const iEffective = a_iTemplateRequired + GetEquipBonus(a_pStack);
            return iEffective > 0 ? iEffective : 0;
        }

        // Items that the engine takes OFF when a new item is equipped (see CollectReplacedStacks). Their bonuses are part of
        // the current attribute values of the hero, but they leave together with the replaced item, so the requirements of
        // the NEW item have to be compared against the values WITHOUT them.
        struct ReplacedSet
        {
            static constexpr GEInt MAX_COUNT = 8;
            gCInventoryStack const *arrStack[MAX_COUNT] = {};
            GEInt iCount = 0;
        };

        // Sum of the bonuses (operation a_enuOp on a_strTag) of all items of the set. Defined below GetItemPropertySet.
        GEInt GetReplacedBonus(ReplacedSet const *a_pReplaced, bCString const &a_strTag, EAttribModOperation a_enuOp);

        // Number of points the hero lacks for requirement a_iIndex of the item, 0 if it is met (or there is none).
        //   Hero value  = current value of the attribute
        //                 - bonus of the item itself            (only if a_bOwnBonusApplied: the item is already linked)
        //                 - bonuses of the items being replaced (a_pReplaced, the equipment that is taken off)
        //   Requirement = template value + equip bonus (-10 for self-forged items, flag 0x80), never below 0.
        // o_pstrTag (optional) receives the tag of the attribute when points are missing, o_piHave / o_piRequired the
        // compared values (for the log).
        GEInt GetAttribShortfall(gCItem_PS const &a_Item, gCInventoryStack const *a_pStack, GEInt a_iIndex,
                                 GEBool a_bOwnBonusApplied, bCString *o_pstrTag = nullptr,
                                 ReplacedSet const *a_pReplaced = nullptr, GEInt *o_piHave = nullptr,
                                 GEInt *o_piRequired = nullptr)
        {
            bCString const &strTag = ItemField<bCString>(a_Item, REQ_TAG_OFFSET, a_iIndex);
            GEInt const iTemplateRequired = ItemField<GEInt>(a_Item, REQ_VALUE_OFFSET, a_iIndex);
            if (strTag.IsEmpty() || iTemplateRequired <= 0)
                return 0;

            GEBool const bMax = ItemField<GEInt>(a_Item, REQ_OP_OFFSET, a_iIndex) == EAttribReqOperation_MaxGreaterEqual;
            GEInt const iRequired = GetEffectiveRequirement(a_pStack, strTag, bMax, iTemplateRequired);
            if (iRequired <= 0)
                return 0; // the self-forged bonus covers the whole requirement

            GEInt iCurrent = 0;
            if (!ReadAttribute(strTag, bMax, iCurrent))
                return 0; // unknown attribute, nothing to compare against

            EAttribModOperation const enuOp = bMax ? EAttribModOperation_AddToMax : EAttribModOperation_AddToVal;
            GEInt iBonus = a_bOwnBonusApplied ? GetOwnBonus(a_Item, strTag, enuOp, a_pStack) : 0;
            iBonus += GetReplacedBonus(a_pReplaced, strTag, enuOp);

            GEInt const iHave = iCurrent - iBonus;
            if (o_piHave)
                *o_piHave = iHave;
            if (o_piRequired)
                *o_piRequired = iRequired;
            if (iHave >= iRequired)
                return 0;

            if (o_pstrTag)
                *o_pstrTag = strTag;
            return iRequired - iHave;
        }

        GEBool IsAttribViolated(gCItem_PS const &a_Item, gCInventoryStack const *a_pStack, GEInt a_iIndex,
                                GEBool a_bOwnBonusApplied)
        {
            return GetAttribShortfall(a_Item, a_pStack, a_iIndex, a_bOwnBonusApplied) > 0;
        }

        GEBool IsAnyAttribViolated(gCItem_PS const &a_Item, gCInventoryStack const *a_pStack,
                                   GEBool a_bOwnBonusApplied = GETrue)
        {
            for (GEInt i = 0; i < REQ_COUNT; ++i)
            {
                if (IsAttribViolated(a_Item, a_pStack, i, a_bOwnBonusApplied))
                    return GETrue;
            }
            return GEFalse;
        }

        // Skills: the native CanEquip fills the list of missing skills. The list of missing attributes is ignored on
        // purpose: the engine would count the bonus of the item itself.
        GEBool IsAnySkillMissing(gCInventory_PS const &a_Inventory, GEInt a_iStackIndex)
        {
            bTObjArray<gCInventory_PS::SMissingAttrib> arrMissingAttribs;
            bTValArray<gCInventory_PS::SMissingSkill> arrMissingSkills;
            a_Inventory.CanEquip(a_iStackIndex, arrMissingAttribs, arrMissingSkills);
            return arrMissingSkills.GetCount() > 0;
        }

        gCItem_PS const *GetItemPropertySet(gCInventorySlot const &a_Slot)
        {
            eCEntity const *pItem = a_Slot.GetItem().GetEntity();
            if (!pItem)
                pItem = a_Slot.GetTemplate().GetEntity();
            return pItem ? static_cast<gCItem_PS const *>(pItem->GetPropertySet(eEPropertySetType_Item)) : nullptr;
        }

        //
        // Equipment that is taken off when a new item is equipped.
        //
        // The attribute values of the hero include the bonuses of everything that is linked right now, so while the old
        // sword (+10 DEX) is still on the hero, a 130 DEX hero "has" 140 DEX and passes the 140 DEX requirement of the
        // rapier that replaces it. The requirements of the new item must be compared against the CLEAN values, i.e.
        // without the bonuses of the items that the engine unlinks for it.
        //
        // Which items those are (verified in Game.dll 1.8.7, gCInventory_PS::EquipStack / HoldStackRightHand):
        //   - gCInventorySlot::GetEquipSlots(new item) gives the primary slot P of the new item: 1H / 2H / staff ->
        //     BackRight, shield -> BackLeft, bow / crossbow -> Bow, armor -> Armor, ...
        //   - the item that is linked to P is unlinked (it is replaced);
        //   - an item that is DRAWN sits in a hand slot instead (RightHand for the melee weapon, LeftHand for the shield,
        //     either hand for a bow). It is replaced as well, if it belongs to the same equipment group (its own primary
        //     slot is P);
        //   - a shield additionally takes a two-handed weapon / staff out of the right hand.
        // The stack of the new item itself is never part of the set.
        void AddReplaced(ReplacedSet &a_Set, gCInventoryStack const *a_pStack)
        {
            if (!a_pStack)
                return;
            for (GEInt i = 0; i < a_Set.iCount; ++i)
            {
                if (a_Set.arrStack[i] == a_pStack)
                    return;
            }
            if (a_Set.iCount < ReplacedSet::MAX_COUNT)
                a_Set.arrStack[a_Set.iCount++] = a_pStack;
        }

        // a_bAlways: the item in the slot is replaced for sure (the primary slot of the new item). Otherwise it is only
        // replaced if it belongs to the same equipment group, i.e. its own primary slot is a_enuPrimary.
        void AddReplacedFromSlot(gCInventory_PS const &a_Inventory, GEInt a_iNewIndex, ReplacedSet &a_Set, gESlot a_enuSlot,
                                 gESlot a_enuPrimary, GEBool a_bAlways)
        {
            GEInt const iIndex = a_Inventory.FindStackIndex(a_enuSlot);
            if (iIndex < 0 || iIndex == a_iNewIndex)
                return;

            gCInventoryStack const *pStack = a_Inventory.GetStack(iIndex);
            if (!pStack || !pStack->IsLinked() || !pStack->IsEquipment())
                return;

            if (!a_bAlways)
            {
                gESlot enuPrimary = gESlot_None;
                gESlot enuAlternative = gESlot_None;
                pStack->GetEquipSlots(enuPrimary, enuAlternative, GetHeroEntity());
                if (enuPrimary != a_enuPrimary)
                    return;
            }
            AddReplaced(a_Set, pStack);
        }

        void CollectReplacedStacks(gCInventory_PS const &a_Inventory, GEInt a_iNewIndex,
                                   gCInventoryStack const &a_NewStack, ReplacedSet &o_Set)
        {
            gESlot enuPrimary = gESlot_None;
            gESlot enuAlternative = gESlot_None;
            a_NewStack.GetEquipSlots(enuPrimary, enuAlternative, GetHeroEntity());
            if (enuPrimary == gESlot_None)
                return;

            // The slot of the new item itself: whatever is there is replaced.
            AddReplacedFromSlot(a_Inventory, a_iNewIndex, o_Set, enuPrimary, enuPrimary, GETrue);

            // The same equipment group while the old item is drawn (it is linked to a hand slot, not to the back slot).
            switch (enuPrimary)
            {
                case gESlot_BackRight: AddReplacedFromSlot(a_Inventory, a_iNewIndex, o_Set, gESlot_RightHand, enuPrimary, GEFalse); break;
                case gESlot_BackLeft: AddReplacedFromSlot(a_Inventory, a_iNewIndex, o_Set, gESlot_LeftHand, enuPrimary, GEFalse); break;
                case gESlot_Bow:
                case gESlot_Crossbow:
                    AddReplacedFromSlot(a_Inventory, a_iNewIndex, o_Set, gESlot_RightHand, enuPrimary, GEFalse);
                    AddReplacedFromSlot(a_Inventory, a_iNewIndex, o_Set, gESlot_LeftHand, enuPrimary, GEFalse);
                    break;
                default: break;
            }

            // A shield takes a two-handed weapon / staff out of the right hand (back or drawn).
            if (enuPrimary == gESlot_BackLeft)
            {
                gESlot const arrRightSlots[] = {gESlot_BackRight, gESlot_RightHand};
                for (gESlot enuSlot : arrRightSlots)
                {
                    GEInt const iIndex = a_Inventory.FindStackIndex(enuSlot);
                    if (iIndex < 0 || iIndex == a_iNewIndex)
                        continue;
                    gCInventoryStack const *pStack = a_Inventory.GetStack(iIndex);
                    if (!pStack || !pStack->IsLinked())
                        continue;
                    gEUseType const enuUseType = pStack->GetUseType();
                    if (enuUseType == gEUseType_2H || enuUseType == gEUseType_Staff)
                        AddReplaced(o_Set, pStack);
                }
            }
        }

        GEInt GetReplacedBonus(ReplacedSet const *a_pReplaced, bCString const &a_strTag, EAttribModOperation a_enuOp)
        {
            if (!a_pReplaced)
                return 0;

            GEInt iSum = 0;
            for (GEInt i = 0; i < a_pReplaced->iCount; ++i)
            {
                gCItem_PS const *pItem = a_pReplaced->arrStack[i] ? GetItemPropertySet(*a_pReplaced->arrStack[i]) : nullptr;
                if (pItem)
                    iSum += GetOwnBonus(*pItem, a_strTag, a_enuOp, a_pReplaced->arrStack[i]);
            }
            return iSum;
        }

        // Unequips the first equipped item that violates a rule. Returns GETrue if an item was unequipped.
        GEBool UnequipFirstViolatingItem(gCInventory_PS &a_Inventory, gCInventorySlot const *a_pExcluded)
        {
            GEInt const iCount = a_Inventory.GetStackCount();
            for (GEInt i = 0; i < iCount; ++i)
            {
                gCInventoryStack const *pStack = a_Inventory.GetStack(i);
                if (!pStack || static_cast<gCInventorySlot const *>(pStack) == a_pExcluded)
                    continue;
                if (!pStack->IsLinked() || !pStack->IsEquipment() || pStack->IsPermanent())
                    continue;

                gCItem_PS const *pItem = GetItemPropertySet(*pStack);
                if (!pItem)
                    continue;

                GELPCChar strReason = nullptr;
                if (IsAnyAttribViolated(*pItem, pStack))
                    strReason = "attribute";
                else if (IsAnySkillMissing(a_Inventory, i))
                    strReason = "skill";
                else
                    continue;

                DebugLog::GetLogger().LogFormat("[RequirementsCheck] Unequip stack %d: %s requirement not met.\n", i,
                                             strReason);
                if (a_Inventory.UnEquipStack(i))
                    return GETrue;
            }
            return GEFalse;
        }

        //
        // Check scheduling.
        //
        // A trigger only marks the check as pending. The check runs as soon as no inventory operation / stats
        // callback is active anymore (depth 0), i.e. after the original functions have finished updating the
        // values and the inventory is consistent.
        //
        Trigger g_enuPendingTrigger = Trigger_InventoryOperation;
        gCInventorySlot const *g_pPendingExcluded = nullptr;

#if RC_DIAGNOSTICS
        //
        // Diagnostic log. Armed by a press of a quick slot key, active for RC_DIAG_WINDOW_MS afterwards.
        //
        GEU32 g_u32DiagStart = 0;
        GEU32 g_u32DiagUntil = 0;

        GEBool DiagActive()
        {
            return g_u32DiagUntil != 0 && static_cast<GEInt>(::GetTickCount() - g_u32DiagUntil) < 0;
        }

        void DiagArm()
        {
            g_u32DiagStart = ::GetTickCount();
            g_u32DiagUntil = g_u32DiagStart + RC_DIAG_WINDOW_MS;
            if (g_u32DiagUntil == 0)
                g_u32DiagUntil = 1;
        }

        // "Module.dll+0xRVA" of a code address.
        void DiagDescribeAddress(GELPVoid a_pAddress, char *a_pBuffer, size_t a_uSize)
        {
            HMODULE hModule = nullptr;
            if (a_pAddress &&
                ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCSTR>(a_pAddress), &hModule) &&
                hModule)
            {
                char strPath[MAX_PATH] = {};
                ::GetModuleFileNameA(hModule, strPath, MAX_PATH);
                char const *pName = strrchr(strPath, '\\');
                pName = pName ? pName + 1 : strPath;
                _snprintf_s(a_pBuffer, a_uSize, _TRUNCATE, "%s+0x%X", pName,
                            static_cast<unsigned>(reinterpret_cast<GEU32>(a_pAddress) - reinterpret_cast<GEU32>(hModule)));
            }
            else
                _snprintf_s(a_pBuffer, a_uSize, _TRUNCATE, "0x%p", a_pAddress);
        }

        void DiagLogF(GELPCChar a_strWhat, GELPVoid a_pCaller, GELPCChar a_strFormat, ...)
        {
            char strDetail[256] = {};
            va_list Args;
            va_start(Args, a_strFormat);
            _vsnprintf_s(strDetail, sizeof(strDetail), _TRUNCATE, a_strFormat, Args);
            va_end(Args);

            char strCaller[300] = {};
            DiagDescribeAddress(a_pCaller, strCaller, sizeof(strCaller));
            DebugLog::GetLogger().LogFormat("[RequirementsCheck][diag] +%u ms %s | caller %s | %s\n",
                                         static_cast<unsigned>(::GetTickCount() - g_u32DiagStart), a_strWhat, strCaller,
                                         strDetail);
        }
#endif

        GELPCChar GetTriggerName(Trigger a_enuTrigger)
        {
            switch (a_enuTrigger)
            {
                case Trigger_ItemApplied: return "item applied";
                case Trigger_ItemUnapplied: return "item unapplied";
                case Trigger_AttributeChanged: return "attribute changed";
                case Trigger_MaxStatChanged: return "max HP/MP/SP changed";
                default: return "inventory operation";
            }
        }

        // Runs the check, unequips violating items. Returns the number of unequipped items.
        GEInt Enforce(gCInventorySlot const *a_pExcluded)
        {
            if (g_bEnforcing)
                return 0;

            gCInventory_PS *pInventory = GetHeroInventory();
            if (!pInventory)
                return 0;

            EnforceScope Scope;
            GEInt iUnequipped = 0;
            // Removing an item changes attributes / skills, which may invalidate other items: repeat until stable.
            for (GEInt iPass = 0; iPass < RC_MAX_PASSES; ++iPass)
            {
                if (!UnequipFirstViolatingItem(*pInventory, a_pExcluded))
                    break;
                ++iUnequipped;
            }
            return iUnequipped;
        }

        void QueueCheck(Trigger a_enuTrigger, gCInventorySlot const *a_pExcluded)
        {
            if (g_bEnforcing)
                return;
            // Keep an already requested exclusion (an unapplied item) when further triggers arrive.
            if (!g_bPending || a_pExcluded)
                g_pPendingExcluded = a_pExcluded;
            g_bPending = GETrue;
            g_enuPendingTrigger = a_enuTrigger;
        }

        void Flush()
        {
            if (g_iOperationDepth > 0 || g_bEnforcing || !g_bPending)
                return;
            // The game is consuming an item right now. The check stays pending and runs as soon as the consumption
            // is over (see MemorySetIsConsumingItem) or the next trigger arrives.
            if (IsHeroConsumingItem())
                return;

            g_bPending = GEFalse;
            gCInventorySlot const *pExcluded = g_pPendingExcluded;
            g_pPendingExcluded = nullptr;
            if (Enforce(pExcluded) > 0)
                DebugLog::GetLogger().LogFormat("[RequirementsCheck] Trigger: %s.\n", GetTriggerName(g_enuPendingTrigger));
        }

        // Brackets every hooked operation (original function in between).
        void BeginOperation()
        {
            ++g_iOperationDepth;
        }

        void EndOperation()
        {
            --g_iOperationDepth;
            Flush();
        }

        //
        // Trigger filters.
        //

        // GETrue, if the attribute object belongs to the hero (changes of NPC attributes are none of our business).
        GEBool IsHeroAttribute(gCAttribute const *a_pAttribute)
        {
            gCPlayerMemory_PS const *pMemory = GetHeroMemory();
            return pMemory && a_pAttribute && pMemory->GetAttribute(a_pAttribute->GetTag()) == a_pAttribute;
        }

        // gCAttribute::SetModifier / SetBaseValue: main attributes, buffs, potions, permanent plants.
        // Changes of the CURRENT value of HP / MP / SP (gCStat, value lives in BaseValue) never trigger.
        void OnAttributeChanged(gCAttribute const *a_pAttribute)
        {
            if (IsDynamicStat(a_pAttribute) || !IsHeroAttribute(a_pAttribute))
                return;
            QueueCheck(Trigger_AttributeChanged, nullptr);
        }

        // gCStat::SetBaseMaximum / SetMaximumModifier / SetMaximum: PERMANENT maximum of HP / MP / SP.
        void OnMaxStatChanged(gCAttribute const *a_pAttribute)
        {
#if RC_TRIGGER_ON_MAX_STAT
            if (!IsHeroAttribute(a_pAttribute))
                return;
            QueueCheck(Trigger_MaxStatChanged, nullptr);
#else
            (void)a_pAttribute;
#endif
        }

        // GETrue, if a change of a gCAttribute / gCStat must pass through the plugin completely unnoticed: no
        // operation bracket, no queued check, no flush of a pending check. That is every change of the CURRENT value
        // (and modifier) of HP / MP / SP, i.e. the regeneration ticks of potions, meat and herbs, damage, running, ...
        GEBool IsIgnoredStatChange(gCAttribute const *a_pAttribute)
        {
            return IsDynamicStat(a_pAttribute);
        }

        // OnApplyStats / OnUnApplyStats receive two entities. Which one is the owner is not part of the SDK, so the
        // trigger fires if the hero is one of them (equipment of NPCs is ignored).
        void OnItemStats(gCInventorySlot *a_pSlot, eCEntity *a_pFirst, eCEntity *a_pSecond, Trigger a_enuTrigger)
        {
            eCEntity *pHero = GetHeroEntity();
            if (!pHero || (a_pFirst != pHero && a_pSecond != pHero))
                return;
            // Only equipment can break a requirement. Potions, food, etc. are linked to a hand slot when they are
            // used; this happens in the middle of native inventory operations (LinkStackToSlot, HoldStack*, stack
            // splitting), when stack indices are not valid yet. A check at that moment unequips the wrong items and
            // leaves the game with dangling stack pointers.
            if (a_pSlot && !a_pSlot->IsEquipment())
                return;
            // The item that is just being unapplied is leaving the equipment, it is not evaluated.
            QueueCheck(a_enuTrigger, a_enuTrigger == Trigger_ItemUnapplied ? a_pSlot : nullptr);
        }

        void OnInventoryOperation(gCInventory_PS *a_pInventory)
        {
            if (a_pInventory && a_pInventory == GetHeroInventory())
                QueueCheck(Trigger_InventoryOperation, nullptr);
        }

        //
        // Hooks. Member functions are hooked with ThisCall(): 'this' is the first argument of the hook function.
        // Every hook calls the ORIGINAL function first, so the trigger is raised strictly after the value has been
        // updated, and the check runs when the outermost hooked call returns.
        //
        mCFunctionHook Hook_SlotOnApplyStats;
        void GE_STDCALL SlotOnApplyStats(gCInventorySlot *a_pThis, eCEntity *a_pFirst, eCEntity *a_pSecond)
        {
            using mFOriginal = void(GE_STDCALL *)(gCInventorySlot *, eCEntity *, eCEntity *);
            BeginOperation();
            Hook_SlotOnApplyStats.GetOriginalFunction<mFOriginal>()(a_pThis, a_pFirst, a_pSecond);
            OnItemStats(a_pThis, a_pFirst, a_pSecond, Trigger_ItemApplied);
            EndOperation();
        }

        mCFunctionHook Hook_SlotOnUnApplyStats;
        void GE_STDCALL SlotOnUnApplyStats(gCInventorySlot *a_pThis, eCEntity *a_pFirst, eCEntity *a_pSecond)
        {
            using mFOriginal = void(GE_STDCALL *)(gCInventorySlot *, eCEntity *, eCEntity *);
            BeginOperation();
            Hook_SlotOnUnApplyStats.GetOriginalFunction<mFOriginal>()(a_pThis, a_pFirst, a_pSecond);
            OnItemStats(a_pThis, a_pFirst, a_pSecond, Trigger_ItemUnapplied);
            EndOperation();
        }

        // NOTE: gCInventoryStack::OnApplyStats / OnUnApplyStats are plain `jmp` thunks to the gCInventorySlot
        // functions in Game.dll 1.8.7 (verified in the binary), so hooking the gCInventorySlot functions above
        // already covers equipment stacks. Hooking the thunks as well would intercept every call twice.

        // gCInventory_PS equip / unequip operations. They are not triggers on their own (OnApplyStats /
        // OnUnApplyStats are), but they bracket the whole operation: OnApplyStats is called by EquipStack while the
        // inventory is still being modified, so the check has to wait until the outermost operation has returned.
        mCFunctionHook Hook_EquipStack;
        GEBool GE_STDCALL InventoryEquipStack(gCInventory_PS *a_pThis, GEInt a_iIndex)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCInventory_PS *, GEInt);
            // Decided BEFORE the original runs: afterwards stacks may have been split / merged and the index is stale.
            GEBool const bEquipment = IsEquipmentStack(a_pThis, a_iIndex);
            BeginOperation();
            GEBool const bResult = Hook_EquipStack.GetOriginalFunction<mFOriginal>()(a_pThis, a_iIndex);
            if (bEquipment)
                OnInventoryOperation(a_pThis);
            EndOperation();
            return bResult;
        }

        mCFunctionHook Hook_EquipStackToSlot;
        GEBool GE_STDCALL InventoryEquipStackToSlot(gCInventory_PS *a_pThis, GEInt a_iIndex, gESlot a_enuSlot)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCInventory_PS *, GEInt, gESlot);
#if RC_DIAGNOSTICS
            if (DiagActive() && a_pThis == GetHeroInventory())
                DiagLogF("EquipStackToSlot", _ReturnAddress(), "stack=%d slot=%d", a_iIndex, static_cast<int>(a_enuSlot));
#endif
            GEBool const bEquipment = IsEquipmentStack(a_pThis, a_iIndex);
            BeginOperation();
            GEBool const bResult = Hook_EquipStackToSlot.GetOriginalFunction<mFOriginal>()(a_pThis, a_iIndex, a_enuSlot);
            if (bEquipment)
                OnInventoryOperation(a_pThis);
            EndOperation();
            return bResult;
        }

        mCFunctionHook Hook_UnEquipStack;
        GEBool GE_STDCALL InventoryUnEquipStack(gCInventory_PS *a_pThis, GEInt a_iIndex)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCInventory_PS *, GEInt);
            GEBool const bEquipment = IsEquipmentStack(a_pThis, a_iIndex);
            BeginOperation();
            GEBool const bResult = Hook_UnEquipStack.GetOriginalFunction<mFOriginal>()(a_pThis, a_iIndex);
            if (bEquipment)
                OnInventoryOperation(a_pThis);
            EndOperation();
            return bResult;
        }

        // Pure brackets: these gCInventory_PS functions link / unlink stacks (and therefore call OnApplyStats /
        // OnUnApplyStats) without being one of the functions above. Using a potion / meat / elixir goes through
        // HoldStack* / LinkStackToSlot. They are NOT triggers, they only keep the check from running while the
        // inventory is still being modified (depth > 0). A pending check runs when the outermost call returns.
        mCFunctionHook Hook_LinkStackToSlot;
        GEBool GE_STDCALL InventoryLinkStackToSlot(gCInventory_PS *a_pThis, GEInt a_iIndex, gESlot a_enuSlot,
                                                   GEBool a_bFlag)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCInventory_PS *, GEInt, gESlot, GEBool);
#if RC_DIAGNOSTICS
            if (DiagActive() && a_pThis == GetHeroInventory())
                DiagLogF("LinkStackToSlot", _ReturnAddress(), "stack=%d slot=%d flag=%d", a_iIndex,
                         static_cast<int>(a_enuSlot), static_cast<int>(a_bFlag));
#endif
            BeginOperation();
            GEBool const bResult =
                Hook_LinkStackToSlot.GetOriginalFunction<mFOriginal>()(a_pThis, a_iIndex, a_enuSlot, a_bFlag);
            EndOperation();
            return bResult;
        }

        mCFunctionHook Hook_UnlinkStackFromSlot;
        GEInt GE_STDCALL InventoryUnlinkStackFromSlot(gCInventory_PS *a_pThis, gESlot a_enuSlot)
        {
            using mFOriginal = GEInt(GE_STDCALL *)(gCInventory_PS *, gESlot);
            BeginOperation();
            GEInt const iResult = Hook_UnlinkStackFromSlot.GetOriginalFunction<mFOriginal>()(a_pThis, a_enuSlot);
            EndOperation();
            return iResult;
        }

        mCFunctionHook Hook_HoldStacks;
        GEBool GE_STDCALL InventoryHoldStacks(gCInventory_PS *a_pThis, GEInt a_iIndexLeft, GEInt a_iIndexRight)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCInventory_PS *, GEInt, GEInt);
#if RC_DIAGNOSTICS
            if (DiagActive() && a_pThis == GetHeroInventory())
                DiagLogF("HoldStacks", _ReturnAddress(), "left=%d right=%d", a_iIndexLeft, a_iIndexRight);
#endif
            BeginOperation();
            GEBool const bResult =
                Hook_HoldStacks.GetOriginalFunction<mFOriginal>()(a_pThis, a_iIndexLeft, a_iIndexRight);
            EndOperation();
            return bResult;
        }

        mCFunctionHook Hook_HoldStackLeftHand;
        GEBool GE_STDCALL InventoryHoldStackLeftHand(gCInventory_PS *a_pThis, GEInt a_iIndex)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCInventory_PS *, GEInt);
#if RC_DIAGNOSTICS
            if (DiagActive() && a_pThis == GetHeroInventory())
                DiagLogF("HoldStackLeftHand", _ReturnAddress(), "stack=%d", a_iIndex);
#endif
            BeginOperation();
            GEBool const bResult = Hook_HoldStackLeftHand.GetOriginalFunction<mFOriginal>()(a_pThis, a_iIndex);
            EndOperation();
            return bResult;
        }

        mCFunctionHook Hook_HoldStackRightHand;
        GEBool GE_STDCALL InventoryHoldStackRightHand(gCInventory_PS *a_pThis, GEInt a_iIndex)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(gCInventory_PS *, GEInt);
#if RC_DIAGNOSTICS
            if (DiagActive() && a_pThis == GetHeroInventory())
                DiagLogF("HoldStackRightHand", _ReturnAddress(), "stack=%d", a_iIndex);
#endif
            BeginOperation();
            GEBool const bResult = Hook_HoldStackRightHand.GetOriginalFunction<mFOriginal>()(a_pThis, a_iIndex);
            EndOperation();
            return bResult;
        }

        //
        // THE blocking point: PSInventory::PrintMissingRequirements(int a_iStackIndex, bool a_bPrint) const (Script.dll).
        //
        // Found by following the text markers of the "not enough stats" warning (Script_Game.dll strings
        // "HUD_MissingAttrib" / "HUD_MissingSkill", Script.dll strings "HUD_MissingAttrib", "ATTRIB_", "$(name)",
        // "$(value)"). Disassembly of Script.dll 1.8.7 (RVA 0x7280):
        //   - this->m_pEngineEntityPropertySet is the gCInventory_PS; null -> returns false;
        //   - it calls gCInventory_PS::CanEquip(stack, missingAttribs, missingSkills) and keeps its result in AL;
        //   - a_bPrint == false -> returns that result silently;
        //   - a_bPrint == true  -> for every missing skill / attribute it builds the localized message
        //     (eCLocString "HUD_MissingSkill" / "HUD_MissingAttrib", "$(name)" = eCLocString("ATTRIB_" + tag),
        //     "$(value)" = missing points) and shows it with gCGUIManager::PrintGameMessage(text, gEGameMessageType_Failure).
        // Script_Game.dll calls it with a_bPrint = true before the hero uses a weapon: PS_HUD_Equip (inventory), the quick
        // slot use (PS_Melee / PS_Ranged / PS_Magic / PS_QuickUse, i.e. the HUD hotkeys) and the NPC routine
        // _AI_HoldInventoryItems. Every caller does "test al, al / je -> return 0" BEFORE it starts the draw animation or
        // touches the inventory, so a GEFalse result makes the ENGINE ITSELF cancel both the hand animation and the
        // equipping, from the inventory as well as from the HUD. Nothing is forced out of the hand later and no input is
        // swallowed.
        //
        // The native function runs first (skills, the native attribute check, the native message). Only if it says
        // "usable", the plugin applies its strict attribute rule (clean stats of the hero against the template
        // requirement, lowered by RC_SELF_FORGED_REQ_REDUCTION for self-forged items, quality flag 0x80); if that fails it
        // shows the same native message and returns GEFalse.
        //

        // Shows the native warning for one missing attribute (same texts and placeholders as Script.dll).
        void PrintMissingAttribMessage(bCString const &a_strTag, GEInt a_iMissing)
        {
            gCSession *pSession = g_ppCurrentSession ? *g_ppCurrentSession : nullptr;
            gCGUIManager *pGUIManager = pSession ? pSession->GetGUIManager() : nullptr;
            if (!pGUIManager)
                return;

            bCUnicodeString strMessage(eCLocString(bCString("HUD_MissingAttrib")).GetString());
            bCUnicodeString const strName(eCLocString("ATTRIB_" + a_strTag).GetString());
            bCUnicodeString strValue;
            strValue.Format(L"%d", a_iMissing);

            strMessage.Replace(L"$(name)", static_cast<GELPCUnicodeChar>(strName));
            strMessage.Replace(L"$(value)", static_cast<GELPCUnicodeChar>(strValue));
            pGUIManager->PrintGameMessage(strMessage, gEGameMessageType_Failure);
        }

        // GETrue, if the hero lacks attribute points for the (not yet linked) stack. The comparison runs against the CLEAN
        // stats of the hero: the bonus of the item itself is not part of them yet, and the bonuses of the weapon / shield
        // that the engine takes off for the new item are subtracted from the current values (CollectReplacedStacks).
        // Example: 130 DEX + 10 DEX of the old sword = 140 DEX shown by the game; the rapier needs 140 DEX -> clean value
        // 130 < 140 -> lacking. Already linked stacks (drawing / sheathing an equipped weapon) are never blocked here,
        // the post-check validates them.
        GEBool HeroLacksAttributes(gCInventory_PS &a_Inventory, GEInt a_iIndex, GEBool a_bPrint)
        {
            if (a_iIndex < 0 || a_iIndex >= a_Inventory.GetStackCount())
                return GEFalse;

            gCInventoryStack const *pStack = a_Inventory.GetStack(a_iIndex);
            // Potions, food, keys, ... never have requirements that matter here.
            if (!pStack || !pStack->IsEquipment() || pStack->IsPermanent() || pStack->IsLinked())
                return GEFalse;

            gCItem_PS const *pItem = GetItemPropertySet(*pStack);
            if (!pItem)
                return GEFalse;

            ReplacedSet Replaced;
#if RC_SUBTRACT_REPLACED_BONUS
            CollectReplacedStacks(a_Inventory, a_iIndex, *pStack, Replaced);
#endif

            GEBool bLacking = GEFalse;
            for (GEInt i = 0; i < REQ_COUNT; ++i)
            {
                bCString strTag;
                GEInt iHave = 0;
                GEInt iRequired = 0;
                GEInt const iMissing =
                    GetAttribShortfall(*pItem, pStack, i, GEFalse, &strTag, &Replaced, &iHave, &iRequired);
                if (iMissing <= 0)
                    continue;

                bLacking = GETrue;
                DebugLog::GetLogger().LogFormat("[RequirementsCheck] PrintMissingRequirements: stack %d, attribute '%s': "
                                             "clean value %d < required %d (lacks %d, %d equipped item(s) are "
                                             "replaced and not counted), the item cannot be used.\n",
                                             a_iIndex, static_cast<GELPCChar>(strTag), iHave, iRequired, iMissing,
                                             Replaced.iCount);
                if (a_bPrint)
                    PrintMissingAttribMessage(strTag, iMissing);
            }
            return bLacking;
        }

#if RC_HOOK_PRINT_MISSING_REQUIREMENTS
        mCFunctionHook Hook_PrintMissingRequirements;
        GEBool GE_STDCALL ScriptPrintMissingRequirements(PSInventory const *a_pThis, GEInt a_iIndex, GEBool a_bPrint)
        {
            using mFOriginal = GEBool(GE_STDCALL *)(PSInventory const *, GEInt, GEBool);

            // Only the hero is validated (the same function is used by the NPC routines).
            gCInventory_PS *pInventory =
                a_pThis ? static_cast<gCInventory_PS *>(a_pThis->m_pEngineEntityPropertySet) : nullptr;
            GEBool const bHero = pInventory && !g_bEnforcing && pInventory == GetHeroInventory();
#if RC_DIAGNOSTICS
            if (bHero)
            {
                DiagArm();
                DiagLogF("PrintMissingRequirements", _ReturnAddress(), "stack=%d print=%d", a_iIndex,
                         static_cast<int>(a_bPrint));
            }
#endif
            // The native function first: skills, the native attribute check and its own message.
            GEBool const bNativeResult =
                Hook_PrintMissingRequirements.GetOriginalFunction<mFOriginal>()(a_pThis, a_iIndex, a_bPrint);
            if (!bNativeResult || !bHero)
                return bNativeResult;

            // The native check passed, the strict attribute rule of the plugin decides.
            if (HeroLacksAttributes(*pInventory, a_iIndex, a_bPrint))
                return GEFalse; // the engine cancels the animation and the equipping by itself
            return bNativeResult;
        }
#endif

        //
        // Hooks: the script state of the hero (v1.0.40.0: always installed, the arena judge follows the magic mode). OnPlayerUseQuickSlot starts the draw animation by switching the
        // state of the player routine (PS_Melee / PS_Ranged / PS_Normal / PS_QuickUse), these functions are the
        // candidates for the engine-level starter. They only LOG (inside the window), the original always runs.
        //
        GEBool IsHeroRoutine(gCScriptRoutine_PS *a_pRoutine)
        {
            eCEntity const *pOwner = a_pRoutine ? static_cast<eCEntityPropertySet *>(a_pRoutine)->GetEntity() : nullptr;
            if (!pOwner)
                return GEFalse;
            if (pOwner == GetHeroEntity())
                return GETrue;
            // The player can be a separate transformed body. Forward its task/state
            // events too; equipment validation still uses GetHeroEntity unchanged.
            return g_ppCurrentSession && *g_ppCurrentSession && (*g_ppCurrentSession)->IsGameRunning() &&
                   pOwner == (*g_ppCurrentSession)->GetPlayer();
        }

        mCFunctionHook Hook_DiagSetCurrentState;
        void GE_STDCALL DiagSetCurrentState(gCScriptRoutine_PS *a_pThis, bCString const *a_pstrState)
        {
            using mFOriginal = void(GE_STDCALL *)(gCScriptRoutine_PS *, bCString const *);
#if RC_DIAGNOSTICS
            if (DiagActive() && IsHeroRoutine(a_pThis))
                DiagLogF("SetCurrentState", _ReturnAddress(), "state='%s'", a_pstrState ? a_pstrState->GetText() : "?");
#endif
            Hook_DiagSetCurrentState.GetOriginalFunction<mFOriginal>()(a_pThis, a_pstrState);
            if (a_pstrState && IsHeroRoutine(a_pThis))
                PluginRuntime::OnHeroRoutineChange(a_pstrState->GetText(), false);
        }

        mCFunctionHook Hook_DiagSetCurrentTask;
        void GE_STDCALL DiagSetCurrentTask(gCScriptRoutine_PS *a_pThis, bCString const *a_pstrTask)
        {
            using mFOriginal = void(GE_STDCALL *)(gCScriptRoutine_PS *, bCString const *);
#if RC_DIAGNOSTICS
            if (DiagActive() && IsHeroRoutine(a_pThis))
                DiagLogF("SetCurrentTask", _ReturnAddress(), "task='%s'", a_pstrTask ? a_pstrTask->GetText() : "?");
#endif
            Hook_DiagSetCurrentTask.GetOriginalFunction<mFOriginal>()(a_pThis, a_pstrTask);
            if (a_pstrTask && IsHeroRoutine(a_pThis))
                PluginRuntime::OnHeroRoutineChange(a_pstrTask->GetText(), true);
        }

        mCFunctionHook Hook_DiagAISetState;
        void GE_STDCALL DiagAISetState(gCScriptRoutine_PS *a_pThis, bCString const *a_pstrState)
        {
            using mFOriginal = void(GE_STDCALL *)(gCScriptRoutine_PS *, bCString const *);
#if RC_DIAGNOSTICS
            if (DiagActive() && IsHeroRoutine(a_pThis))
                DiagLogF("AISetState", _ReturnAddress(), "state='%s'", a_pstrState ? a_pstrState->GetText() : "?");
#endif
            Hook_DiagAISetState.GetOriginalFunction<mFOriginal>()(a_pThis, a_pstrState);
            if (a_pstrState && IsHeroRoutine(a_pThis))
                PluginRuntime::OnHeroRoutineChange(a_pstrState->GetText(), false);
        }

        mCFunctionHook Hook_DiagAISetTask;
        void GE_STDCALL DiagAISetTask(gCScriptRoutine_PS *a_pThis, bCString const *a_pstrTask)
        {
            using mFOriginal = void(GE_STDCALL *)(gCScriptRoutine_PS *, bCString const *);
#if RC_DIAGNOSTICS
            if (DiagActive() && IsHeroRoutine(a_pThis))
                DiagLogF("AISetTask", _ReturnAddress(), "task='%s'", a_pstrTask ? a_pstrTask->GetText() : "?");
#endif
            Hook_DiagAISetTask.GetOriginalFunction<mFOriginal>()(a_pThis, a_pstrTask);
            if (a_pstrTask && IsHeroRoutine(a_pThis))
                PluginRuntime::OnHeroRoutineChange(a_pstrTask->GetText(), true);
        }

        // gCPlayerMemory_PS::SetIsConsumingItem(bool const &). When the hero finishes consuming, a check that was
        // postponed meanwhile (see Flush) runs now.
        mCFunctionHook Hook_SetIsConsumingItem;
        void GE_STDCALL MemorySetIsConsumingItem(gCPlayerMemory_PS *a_pThis, GEBool const *a_pbValue)
        {
            using mFOriginal = void(GE_STDCALL *)(gCPlayerMemory_PS *, GEBool const *);
            // Stage 2: the hero starts to consume food / meat / a potion. In a physically started arena fight this is
            // a defeat (PunishArenaPotions), decided BEFORE the item is used.
            GEBool const bHero = a_pThis && a_pThis == GetHeroMemory();
            GEBool const bFinished = bHero && a_pbValue && !*a_pbValue && a_pThis->GetIsConsumingItem();
            if (bHero && a_pbValue && *a_pbValue)
                PluginRuntime::OnHeroConsumeStarted();
            BeginOperation();
            Hook_SetIsConsumingItem.GetOriginalFunction<mFOriginal>()(a_pThis, a_pbValue);
            if (bFinished)
                QueueCheck(Trigger_AttributeChanged, nullptr);
            EndOperation();
        }

        // gCAttribute::SetModifier(GEInt const &) / SetBaseValue(GEInt const &). A reference is a pointer at the ABI
        // level. The original runs first, the trigger is evaluated with the already updated value.
        mCFunctionHook Hook_SetModifier;
        void GE_STDCALL AttributeSetModifier(gCAttribute *a_pThis, GEInt const *a_piValue)
        {
            using mFOriginal = void(GE_STDCALL *)(gCAttribute *, GEInt const *);
            if (IsIgnoredStatChange(a_pThis))
            {
                // HP / MP / SP tick: straight through, the plugin does not notice it.
                Hook_SetModifier.GetOriginalFunction<mFOriginal>()(a_pThis, a_piValue);
                return;
            }
            BeginOperation();
            Hook_SetModifier.GetOriginalFunction<mFOriginal>()(a_pThis, a_piValue);
            OnAttributeChanged(a_pThis);
            EndOperation();
        }

        mCFunctionHook Hook_SetBaseValue;
        void GE_STDCALL AttributeSetBaseValue(gCAttribute *a_pThis, GEInt const *a_piValue)
        {
            using mFOriginal = void(GE_STDCALL *)(gCAttribute *, GEInt const *);
            if (IsIgnoredStatChange(a_pThis))
            {
                // Current HP / MP / SP (BaseValue of a gCStat): straight through, the plugin does not notice it.
                Hook_SetBaseValue.GetOriginalFunction<mFOriginal>()(a_pThis, a_piValue);
                return;
            }
            BeginOperation();
            Hook_SetBaseValue.GetOriginalFunction<mFOriginal>()(a_pThis, a_piValue);
            OnAttributeChanged(a_pThis);
            EndOperation();
        }

        // Permanent maximum of HP / MP / SP (gCStat).
        mCFunctionHook Hook_SetBaseMaximum;
        void GE_STDCALL StatSetBaseMaximum(gCStat *a_pThis, GEInt const *a_piValue)
        {
            using mFOriginal = void(GE_STDCALL *)(gCStat *, GEInt const *);
#if RC_TRIGGER_ON_MAX_STAT
            BeginOperation();
            Hook_SetBaseMaximum.GetOriginalFunction<mFOriginal>()(a_pThis, a_piValue);
            OnMaxStatChanged(a_pThis);
            EndOperation();
#else
            Hook_SetBaseMaximum.GetOriginalFunction<mFOriginal>()(a_pThis, a_piValue); // not a trigger, see RC_TRIGGER_ON_MAX_STAT
#endif
        }

        mCFunctionHook Hook_SetMaximumModifier;
        void GE_STDCALL StatSetMaximumModifier(gCStat *a_pThis, GEInt const *a_piValue)
        {
            using mFOriginal = void(GE_STDCALL *)(gCStat *, GEInt const *);
#if RC_TRIGGER_ON_MAX_STAT
            BeginOperation();
            Hook_SetMaximumModifier.GetOriginalFunction<mFOriginal>()(a_pThis, a_piValue);
            OnMaxStatChanged(a_pThis);
            EndOperation();
#else
            Hook_SetMaximumModifier.GetOriginalFunction<mFOriginal>()(a_pThis, a_piValue); // not a trigger, see RC_TRIGGER_ON_MAX_STAT
#endif
        }

        mCFunctionHook Hook_SetMaximum;
        void GE_STDCALL StatSetMaximum(gCStat *a_pThis, GEInt a_iValue)
        {
            using mFOriginal = void(GE_STDCALL *)(gCStat *, GEInt);
#if RC_TRIGGER_ON_MAX_STAT
            BeginOperation();
            Hook_SetMaximum.GetOriginalFunction<mFOriginal>()(a_pThis, a_iValue);
            OnMaxStatChanged(a_pThis);
            EndOperation();
#else
            Hook_SetMaximum.GetOriginalFunction<mFOriginal>()(a_pThis, a_iValue); // not a trigger, see RC_TRIGGER_ON_MAX_STAT
#endif
        }

        //
        // gCPlayerMemory_PS: the REAL paths through which the game changes the attributes of the hero.
        //
        // Disassembly of Game.dll 1.8.7: gCAttribute::SetModifier / SetBaseValue and gCStat::SetBaseMaximum /
        // SetMaximumModifier (hooked above as required) have NO callers: not a single call / jmp inside Game.dll,
        // no vtable entry, no import from another DLL. The game writes the fields inline instead:
        //   - gCPlayerMemory_PS::Add* / Set* write m_iBaseValue / m_iModifier directly,
        //   - gCPlayerMemory_PS::ApplyMod / UnapplyMod / Consume (buffs, potions, plants, equipment bonuses) end in
        //     gCAttribute::ApplyTemp / UnapplyTemp / ApplyPerm, which add to m_iModifier directly.
        // Without the hooks below, buffs, potions and permanent plants would never trigger a check.
        //
        enum MemoryKind
        {
            MemoryKind_Value,    // current / base value (for gCStat: the CURRENT HP / MP / SP)
            MemoryKind_Modifier, // modifier (for gCStat always 0)
            MemoryKind_Maximum,  // maximum (for gCStat: the PERMANENT maximum)
            MemoryKind_Operation // ApplyMod / UnapplyMod / Consume with an EAttribModOperation
        };

        GEBool IsMaximumOperation(GEInt a_iOperation)
        {
            return a_iOperation == EAttribModOperation_AddToMax || a_iOperation == EAttribModOperation_AddPercentageToMax ||
                   a_iOperation == EAttribModOperation_SetMax || a_iOperation == EAttribModOperation_SetMaxToPercentage;
        }

        void OnMemoryChanged(gCPlayerMemory_PS *a_pMemory, bCString const *a_pTag, MemoryKind a_enuKind,
                             GEInt a_iOperation)
        {
            if (!a_pMemory || !a_pTag || a_pMemory != GetHeroMemory())
                return;

#if !RC_TRIGGER_ON_MAX_STAT
            (void)a_enuKind;
            (void)a_iOperation;
#endif
            gCAttribute const *pAttribute = a_pMemory->GetAttribute(*a_pTag);
            if (!pAttribute)
                return;

            if (IsDynamicStat(pAttribute))
            {
                // HP / MP / SP: never the current value. The permanent maximum only if RC_TRIGGER_ON_MAX_STAT is set
                // (ShouldIgnoreMemoryChange lets such calls through to this point only in that case).
#if RC_TRIGGER_ON_MAX_STAT
                GEBool const bMaximum = a_enuKind == MemoryKind_Maximum ||
                                        (a_enuKind == MemoryKind_Operation && IsMaximumOperation(a_iOperation));
                if (bMaximum)
                    QueueCheck(Trigger_MaxStatChanged, nullptr);
#endif
                return;
            }
            QueueCheck(Trigger_AttributeChanged, nullptr);
        }

        // GETrue, if a gCPlayerMemory_PS change must pass through the plugin completely unnoticed (no operation
        // bracket, no queued check, no flush of a pending check). Decided BEFORE the original function runs:
        //   - memory of anybody but the hero,
        //   - any change of HP / MP / SP (gCStat): the regeneration ticks of potions, meat and herbs (Update Pack),
        //     damage, running, ... With RC_TRIGGER_ON_MAX_STAT=1 only changes of the permanent maximum are observed.
        // Everything else (strength, dexterity, ancient knowledge, buffs, plants, ...) is a real trigger.
        GEBool ShouldIgnoreMemoryChange(gCPlayerMemory_PS *a_pMemory, bCString const *a_pTag, MemoryKind a_enuKind,
                                        GEInt a_iOperation)
        {
            if (!a_pMemory || !a_pTag || a_pMemory != GetHeroMemory())
                return GETrue;

            gCAttribute const *pAttribute = a_pMemory->GetAttribute(*a_pTag);
            if (!pAttribute || !IsIgnoredStatChange(pAttribute))
                return GEFalse;

#if RC_TRIGGER_ON_MAX_STAT
            return !(a_enuKind == MemoryKind_Maximum ||
                     (a_enuKind == MemoryKind_Operation && IsMaximumOperation(a_iOperation)));
#else
            (void)a_enuKind;
            (void)a_iOperation;
            return GETrue;
#endif
        }

        // gCPlayerMemory_PS::Xxx(bCString const &a_strTag, GEInt a_iValue)  ->  GEBool
#define RC_DEFINE_MEMORY_HOOK(NAME, KIND)                                                                              \
    mCFunctionHook Hook_Memory##NAME;                                                                                  \
    GEBool GE_STDCALL Memory##NAME(gCPlayerMemory_PS *a_pThis, bCString const *a_pTag, GEInt a_iValue)                 \
    {                                                                                                                  \
        using mFOriginal = GEBool(GE_STDCALL *)(gCPlayerMemory_PS *, bCString const *, GEInt);                         \
        if (ShouldIgnoreMemoryChange(a_pThis, a_pTag, KIND, -1))                                                       \
            return Hook_Memory##NAME.GetOriginalFunction<mFOriginal>()(a_pThis, a_pTag, a_iValue);                     \
        BeginOperation();                                                                                              \
        GEBool const bResult = Hook_Memory##NAME.GetOriginalFunction<mFOriginal>()(a_pThis, a_pTag, a_iValue);         \
        OnMemoryChanged(a_pThis, a_pTag, KIND, -1);                                                                    \
        EndOperation();                                                                                                \
        return bResult;                                                                                                \
    }

        // gCPlayerMemory_PS::Xxx(bCString const &a_strTag, GEInt a_iValue, EAttribModOperation a_enuOp)  ->  GEBool
#define RC_DEFINE_MEMORY_OP_HOOK(NAME)                                                                                 \
    mCFunctionHook Hook_Memory##NAME;                                                                                  \
    GEBool GE_STDCALL Memory##NAME(gCPlayerMemory_PS *a_pThis, bCString const *a_pTag, GEInt a_iValue,                 \
                                   EAttribModOperation a_enuOp)                                                        \
    {                                                                                                                  \
        using mFOriginal = GEBool(GE_STDCALL *)(gCPlayerMemory_PS *, bCString const *, GEInt, EAttribModOperation);    \
        if (ShouldIgnoreMemoryChange(a_pThis, a_pTag, MemoryKind_Operation, a_enuOp))                                  \
            return Hook_Memory##NAME.GetOriginalFunction<mFOriginal>()(a_pThis, a_pTag, a_iValue, a_enuOp);            \
        BeginOperation();                                                                                              \
        GEBool const bResult = Hook_Memory##NAME.GetOriginalFunction<mFOriginal>()(a_pThis, a_pTag, a_iValue, a_enuOp);\
        OnMemoryChanged(a_pThis, a_pTag, MemoryKind_Operation, a_enuOp);                                               \
        EndOperation();                                                                                                \
        return bResult;                                                                                                \
    }

        RC_DEFINE_MEMORY_HOOK(AddBaseValue, MemoryKind_Value)
        RC_DEFINE_MEMORY_HOOK(AddValue, MemoryKind_Value)
        RC_DEFINE_MEMORY_HOOK(SetBaseValue, MemoryKind_Value)
        RC_DEFINE_MEMORY_HOOK(SetValue, MemoryKind_Value)
        RC_DEFINE_MEMORY_HOOK(AddModifier, MemoryKind_Modifier)
        RC_DEFINE_MEMORY_HOOK(SetModifier, MemoryKind_Modifier)
        RC_DEFINE_MEMORY_HOOK(AddMaximum, MemoryKind_Maximum)
        RC_DEFINE_MEMORY_HOOK(SetMaximum, MemoryKind_Maximum)
        RC_DEFINE_MEMORY_OP_HOOK(ApplyMod)
        RC_DEFINE_MEMORY_OP_HOOK(UnapplyMod)

        GEBool InstallHook(mCFunctionHook &a_Hook, GELPCChar a_strName, GELPVoid a_pNew,
                           GELPCChar a_strModule = "Game.dll")
        {
            GELPVoid pOriginal = ResolveExport(a_strModule, a_strName);
            if (!pOriginal)
                return GEFalse;

            if (!a_Hook.Prepare(pOriginal, a_pNew).ThisCall().Hook())
            {
                DebugLog::GetLogger().LogFormat("[RequirementsCheck] Failed to hook %s\n", a_strName);
                return GEFalse;
            }
            return GETrue;
        }
    } // namespace

    GEBool IsDynamicStat(gCAttribute const *a_pAttribute)
    {
        // Exact class check through the vtable of gCStat (HP, MP, SP). The attribute hooks are only installed
        // when the vtable is known, see Install().
        return a_pAttribute && g_pStatVtable && *reinterpret_cast<GELPVoid const *>(a_pAttribute) == g_pStatVtable;
    }

    void RequestCheck(Trigger a_enuTrigger, gCInventorySlot const *a_pExcluded)
    {
        QueueCheck(a_enuTrigger, a_pExcluded);
        Flush();
    }

    GEInt EnforceNow(gCInventorySlot const *a_pExcluded)
    {
        return Enforce(a_pExcluded);
    }

    GEBool Install()
    {
        // The exported symbols are resolved by name from Game.dll.
        g_pStatVtable = ResolveGameExport(NAME_StatVtable);
        g_ppCurrentSession = reinterpret_cast<gCSession **>(ResolveGameExport(NAME_CurrentSession));
        if (!g_ppCurrentSession)
            return GEFalse; // without the session pointer the hero cannot be resolved safely

        GEBool bOk = GETrue;
        // Triggers: item equipped / unequipped.
        bOk &= InstallHook(Hook_SlotOnApplyStats, NAME_OnApplyStats, reinterpret_cast<GELPVoid>(&SlotOnApplyStats));
        bOk &= InstallHook(Hook_SlotOnUnApplyStats, NAME_OnUnApplyStats,
                           reinterpret_cast<GELPVoid>(&SlotOnUnApplyStats));
        // Triggers: attributes (main attributes, buffs, potions, permanent plants). Without the gCStat vtable the
        // current HP / MP / SP could not be told apart from real attributes and would fire the check on every hit,
        // so these two hooks are skipped in that case.
        if (g_pStatVtable)
        {
            bOk &= InstallHook(Hook_SetModifier, NAME_SetModifier, reinterpret_cast<GELPVoid>(&AttributeSetModifier));
            bOk &=
                InstallHook(Hook_SetBaseValue, NAME_SetBaseValue, reinterpret_cast<GELPVoid>(&AttributeSetBaseValue));

            // The paths the game really uses (see the comment at RC_DEFINE_MEMORY_HOOK).
#define RC_INSTALL_MEMORY_HOOK(NAME)                                                                                   \
    bOk &= InstallHook(Hook_Memory##NAME, NAME_Memory##NAME, reinterpret_cast<GELPVoid>(&Memory##NAME));
            RC_INSTALL_MEMORY_HOOK(AddBaseValue)
            RC_INSTALL_MEMORY_HOOK(AddValue)
            RC_INSTALL_MEMORY_HOOK(SetBaseValue)
            RC_INSTALL_MEMORY_HOOK(SetValue)
            RC_INSTALL_MEMORY_HOOK(AddModifier)
            RC_INSTALL_MEMORY_HOOK(SetModifier)
            RC_INSTALL_MEMORY_HOOK(AddMaximum)
            RC_INSTALL_MEMORY_HOOK(SetMaximum)
            RC_INSTALL_MEMORY_HOOK(ApplyMod)
            RC_INSTALL_MEMORY_HOOK(UnapplyMod)
        }
        else
        {
            DebugLog::GetLogger().LogFormat("[RequirementsCheck] gCStat vtable not found: SetModifier / SetBaseValue "
                                         "hooks NOT installed (HP / MP / SP could not be filtered).\n");
            bOk = GEFalse;
        }
        // Triggers: permanent maximum of HP / MP / SP.
        bOk &= InstallHook(Hook_SetBaseMaximum, NAME_SetBaseMaximum, reinterpret_cast<GELPVoid>(&StatSetBaseMaximum));
        bOk &= InstallHook(Hook_SetMaximumModifier, NAME_SetMaximumModifier,
                           reinterpret_cast<GELPVoid>(&StatSetMaximumModifier));
        bOk &= InstallHook(Hook_SetMaximum, NAME_SetMaximum, reinterpret_cast<GELPVoid>(&StatSetMaximum));
        // Operation brackets (deferral of the check until the inventory is consistent).
        bOk &= InstallHook(Hook_EquipStack, NAME_EquipStack, reinterpret_cast<GELPVoid>(&InventoryEquipStack));
        bOk &= InstallHook(Hook_EquipStackToSlot, NAME_EquipStackToSlot,
                           reinterpret_cast<GELPVoid>(&InventoryEquipStackToSlot));
        bOk &= InstallHook(Hook_UnEquipStack, NAME_UnEquipStack, reinterpret_cast<GELPVoid>(&InventoryUnEquipStack));
        // Brackets for the remaining link / unlink paths (consuming potions, food, elixirs) and the consume flag.
        bOk &= InstallHook(Hook_LinkStackToSlot, NAME_LinkStackToSlot,
                           reinterpret_cast<GELPVoid>(&InventoryLinkStackToSlot));
        bOk &= InstallHook(Hook_UnlinkStackFromSlot, NAME_UnlinkStackFromSlot,
                           reinterpret_cast<GELPVoid>(&InventoryUnlinkStackFromSlot));
        bOk &= InstallHook(Hook_HoldStacks, NAME_HoldStacks, reinterpret_cast<GELPVoid>(&InventoryHoldStacks));
        bOk &= InstallHook(Hook_HoldStackLeftHand, NAME_HoldStackLeftHand,
                           reinterpret_cast<GELPVoid>(&InventoryHoldStackLeftHand));
        bOk &= InstallHook(Hook_HoldStackRightHand, NAME_HoldStackRightHand,
                           reinterpret_cast<GELPVoid>(&InventoryHoldStackRightHand));
        bOk &= InstallHook(Hook_SetIsConsumingItem, NAME_SetIsConsumingItem,
                           reinterpret_cast<GELPVoid>(&MemorySetIsConsumingItem));
        // Routine hooks (log in the diagnostic window + the arena judge). A failure here is reported but does not count as a
        // failed installation.
        InstallHook(Hook_DiagSetCurrentState, NAME_DiagSetCurrentState,
                    reinterpret_cast<GELPVoid>(&DiagSetCurrentState));
        InstallHook(Hook_DiagSetCurrentTask, NAME_DiagSetCurrentTask, reinterpret_cast<GELPVoid>(&DiagSetCurrentTask));
        InstallHook(Hook_DiagAISetState, NAME_DiagAISetState, reinterpret_cast<GELPVoid>(&DiagAISetState));
        InstallHook(Hook_DiagAISetTask, NAME_DiagAISetTask, reinterpret_cast<GELPVoid>(&DiagAISetTask));
#if RC_HOOK_PRINT_MISSING_REQUIREMENTS
        // The earliest native requirement check of the game: PSInventory::PrintMissingRequirements (Script.dll), see
        // ScriptPrintMissingRequirements. Script.dll is already loaded (the plugin imports it), the export is resolved
        // from the loaded module. Unlike the former hooks of LinkStackToSlot / HoldStacks / OnPlayerUseQuickSlot and the
        // input hooks, it blocks nothing by force: it only changes the answer of the native check.
        bOk &= InstallHook(Hook_PrintMissingRequirements, NAME_PrintMissingRequirements,
                           reinterpret_cast<GELPVoid>(&ScriptPrintMissingRequirements), NAME_ScriptModule);
#endif
        return bOk;
    }
} // namespace RequirementsCheck

