#pragma once

#include <g3sdk/Game.h>

//
// RequirementsCheck - strict equip requirements for the equipment of the hero.
//
// Rules, evaluated for EVERY equipped item whenever a trigger fires:
//
//  1. Attributes (ReqAttrib1..4 of gCItem_PS):
//       ValGreaterEqual : (Attrib.GetValue()   - OwnBonus(AddToVal)) >= Required
//       MaxGreaterEqual : (Attrib.GetMaximum() - OwnBonus(AddToMax)) >= Required
//     OwnBonus is the bonus that the checked item gives itself (gCItem_PS::GetModValue).
//     Required is the template value, lowered by RC_SELF_FORGED_REQ_REDUCTION for Update Pack self-forged items
//     (the same correction as the game's OnPlayerGetEquipBonus, never below 0).
//  2. Skills: gCInventory_PS::CanEquip must not report any missing skill (SMissingSkill).
//
// An item violating a rule is unequipped with gCInventory_PS::UnEquipStack. The check is repeated until the
// equipment is stable (removing an item can invalidate the requirements of other items).
//
// Triggers (always evaluated AFTER the original function has updated the value):
//   - gCInventorySlot::OnApplyStats / OnUnApplyStats        (equip / unequip of an item)
//   - gCAttribute::SetModifier / SetBaseValue               (main attributes, buffs, potions, permanent plants)
//   - gCStat::SetBaseMaximum / SetMaximumModifier / SetMaximum   (PERMANENT maximum of HP, MP, SP)
//   - gCPlayerMemory_PS::Add/Set Value, BaseValue, Modifier, Maximum and ApplyMod / UnapplyMod / Consume
// NOT a trigger: any change of the CURRENT value of HP, MP or SP (damage, running, healing, the gradual
// regeneration ticks of potions / meat / herbs from the Update Pack). Those are gCStat objects (class check
// through the gCStat vtable), whose current value lives in BaseValue. Such ticks are passed straight through to
// the original function: they do not open an operation bracket, do not queue a check and do not flush a pending
// one. The current value of HP / MP / SP is also never read by the requirement check (ReadAttribute), because it
// is in an intermediate state in the middle of a tick. By default the maximum of HP / MP / SP is not a trigger
// either (see RC_TRIGGER_ON_MAX_STAT).
//
// IMPORTANT (verified by disassembling Game.dll 1.8.7): gCAttribute::SetModifier / SetBaseValue and
// gCStat::SetBaseMaximum / SetMaximumModifier are exported but are NEVER called inside the game, it writes the
// fields inline. They are hooked as requested, but the triggers that really fire for buffs, potions and plants
// are the gCPlayerMemory_PS hooks (gCPlayerMemory_PS::ApplyMod ends in gCAttribute::ApplyTemp / ApplyPerm).
//

// Source of the current attribute values.
//   0: gCStats::GetAttrib(index)->GetValue() / GetMaximum() (as specified). If gCStats and gCPlayerMemory_PS
//      disagree, gCPlayerMemory_PS is used and a warning is logged.
//   1: always gCPlayerMemory_PS (the objects that the gCAttribute hooks observe), gCStats only as fallback.
#ifndef RC_PREFER_PLAYER_MEMORY
#define RC_PREFER_PLAYER_MEMORY 0
#endif

// 1: the equipment is only checked while gCSession::IsGameRunning() (not while a savegame is being loaded).
#ifndef RC_REQUIRE_GAME_RUNNING
#define RC_REQUIRE_GAME_RUNNING 1
#endif

// 1: a change of the PERMANENT maximum of HP / MP / SP (gCStat::SetBaseMaximum / SetMaximumModifier / SetMaximum,
//    AddMaximum / SetMaximum / AddToMax operations) triggers the check. 0 (default): it does not, only manual
//    equipping / unequipping and changes of the other attributes (strength, dexterity, ancient knowledge, ...) do.
#ifndef RC_TRIGGER_ON_MAX_STAT
#define RC_TRIGGER_ON_MAX_STAT 0
#endif

// Update Pack "Self-forged" weapons (forged by the player at the anvil, HUD_Quality_Selfmade) lower the requirements
// by RC_SELF_FORGED_REQ_REDUCTION. The Update Pack does it in the native gCInventory_PS::CanEquip: it calls the script
// function OnPlayerGetEquipBonus (Script_Game.dll), which returns -10 for the quality flag gEItemQuality_Forged (0x80),
// and adds the result to ALL four ReqAttrib values (clamped to >= 0). The template of the item still holds the "bare"
// requirement, so the plugin repeats the correction, otherwise such weapons are unequipped by mistake. The flags are
// read with gCInventoryStack::GetCombinedQuality() (stack quality | item quality), as the game does.
#ifndef RC_SELF_FORGED_REQ_REDUCTION
#define RC_SELF_FORGED_REQ_REDUCTION 10
#endif
#ifndef RC_SELF_FORGED_QUALITY_FLAG
#define RC_SELF_FORGED_QUALITY_FLAG gEItemQuality_Forged
#endif
// 0 (default): like the game, the reduction applies to every requirement slot. 1: only to the "value" requirements
// of STR / DEX.
#ifndef RC_SELF_FORGED_ONLY_STR_DEX
#define RC_SELF_FORGED_ONLY_STR_DEX 0
#endif

// 1 (default): hook of the NATIVE requirement check of the game, PSInventory::PrintMissingRequirements(int stack,
//    bool print) const (Script.dll). It is the function behind the built-in "not enough attribute / skill" warning of
//    the HUD (loc strings HUD_MissingAttrib / HUD_MissingSkill, printed with gCGUIManager::PrintGameMessage). The
//    scripts of Script_Game.dll call it before the hero uses a weapon (HUD hotkeys 1-0, equipping from the inventory)
//    and abort with "return 0" if it reports GEFalse, BEFORE the draw animation starts and BEFORE the inventory is
//    touched. The hook lets the native function run first (skills, native attribute check, native message). If it
//    reports "usable", the plugin checks the CLEAN stats of the hero against the requirements of the item, taking the
//    self-forged correction into account (quality flag 0x80, RC_SELF_FORGED_REQ_REDUCTION, see above). If points are
//    missing, the same native message is shown and GEFalse is returned, so the ENGINE cancels the hand animation and
//    the equipping by itself. Already linked stacks (drawing / sheathing an equipped weapon) are never blocked.
//    The post-check (Enforce) stays active as a safety net for everything that does not go through this function.
//    The former hooks of LinkStackToSlot / HoldStacks / HoldStackLeft/RightHand no longer block anything (they only
//    bracket the operations), and the input / OnPlayerUseQuickSlot hooks are gone.
#ifndef RC_HOOK_PRINT_MISSING_REQUIREMENTS
#define RC_HOOK_PRINT_MISSING_REQUIREMENTS 1
#endif

// 1 (default): the requirements of a NEW item are compared against the CLEAN stats of the hero, i.e. the bonuses of the
//    weapon / shield that the engine takes off for the new item are subtracted from the current attribute values first.
//    The game still counts those bonuses while the old item is on the hero (130 DEX + 10 DEX of the old sword = 140 DEX),
//    so a rapier that needs 140 DEX would pass without the message. The replaced items are the ones in the primary slot of
//    the new item (back slot) and the drawn item of the same equipment group (hand slot); a shield also replaces a
//    two-handed weapon / staff. 0: only the bonus of the item itself is considered (the former, faulty behaviour).
#ifndef RC_SUBTRACT_REPLACED_BONUS
#define RC_SUBTRACT_REPLACED_BONUS 1
#endif

// 1 (default): diagnostic log of the use of weapons. For RC_DIAG_WINDOW_MS milliseconds after every call of the native
//    requirement check for the hero (PrintMissingRequirements) the plugin logs the calls of the functions that can
//    start the draw animation of the HERO: gCScriptRoutine_PS::SetCurrentState / SetCurrentTask / AISetState /
//    AISetTask (state "PS_Melee", "PS_Ranged", "PS_QuickUse", ...), gCInventory_PS::EquipStackToSlot / HoldStacks /
//    HoldStackLeftHand / HoldStackRightHand / LinkStackToSlot. Every line contains the module and the RVA of the code
//    that called the function (Documents\gothic3\ge3log*.log / Lastlog_GE3.log). Outside of the window nothing is
//    logged (no spam).
#ifndef RC_DIAGNOSTICS
#define RC_DIAGNOSTICS 1
#endif
#ifndef RC_DIAG_WINDOW_MS
#define RC_DIAG_WINDOW_MS 2000
#endif

// Upper bound of unequip passes per check, guards against endless loops.
#ifndef RC_MAX_PASSES
#define RC_MAX_PASSES 64
#endif

namespace RequirementsCheck
{
    enum Trigger
    {
        Trigger_ItemApplied,
        Trigger_ItemUnapplied,
        Trigger_AttributeChanged,
        Trigger_MaxStatChanged,
        Trigger_InventoryOperation
    };

    // Resolves the exports of Game.dll and installs all hooks. Returns GEFalse if any hook could not be installed.
    GEBool Install();

    // Requests a check of the hero equipment. If an inventory operation (EquipStack, UnEquipStack, ...) is in
    // progress, the check is postponed until the outermost operation has returned, i.e. until the inventory is in
    // a consistent state. Otherwise it runs immediately.
    // a_pExcluded: slot whose stats are being unapplied; it is never evaluated.
    void RequestCheck(Trigger a_enuTrigger, gCInventorySlot const *a_pExcluded = nullptr);

    // Runs the check immediately. Returns the number of unequipped items.
    GEInt EnforceNow(gCInventorySlot const *a_pExcluded = nullptr);

    // GETrue, if a_pAttribute is a gCStat (dynamic HP / MP / SP), i.e. its BaseValue is the CURRENT value.
    GEBool IsDynamicStat(gCAttribute const *a_pAttribute);

    // Genome Logic Fix configuration, file <Gothic 3>\Ini\Script_GenomeLogicFix.ini (section [Main]).
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

        // v1.0.66.0: true (default): the crash reporter (CrashDiag) writes <Documents>\Gothic3\logs\Script_GenomeLogicFix.crash.log when the game
        // hits a fatal exception (the call stack found on the stack with module+offset, the names of the NPCs whose objects the engine was
        // processing, the end of the plugin log). It never changes how the game behaves. INI line: CrashDiagnostics=true
        GEBool CrashDiagnostics = GETrue;
    };

    Config &GetConfig();
    // Reads the INI (writes it with the defaults if missing or if the key is absent).
    void LoadConfig();
    // Writes the current configuration.
    GEBool SaveConfig();
    // v1.0.51.0: GETrue, if the native name of an NPC is in Config::IgnoreTeleportNames (any case, spaces around are cut).
    GEBool IsTeleportNameIgnored(GELPCChar a_strName);

    // Arena judging (hooks of Script_Game.dll scripts), installed from Install() through the script hook registry.
    void InstallArenaHooks();

    // Stage 2: called when the hero starts to consume an item (food, meat, potion).
    void OnHeroConsumeStarted();

    // v1.0.40.0: the script task / state of the routine of the HERO has just been set (gCScriptRoutine_PS::SetCurrentTask /
    // AISetTask: a_bIsTask = true, SetCurrentState / AISetState: false). Called after the original, whatever RC_DIAGNOSTICS is.
    // The arena judge follows the magic mode of the hero through it (PS_Magic, PS_Magic_QuickCast, ...).
    void OnHeroRoutineChange(GELPCChar a_strName, GEBool a_bIsTask);
} // namespace RequirementsCheck
