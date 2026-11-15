#include "DebugLog.h"

#include <g3sdk/Script.h>
#include <g3sdk/util/Hook.h>
#include <cstring>

namespace DebugLog
{
    namespace
    {
        mCFunctionHook g_Insert;
        mCFunctionHook g_Transfer;
        mCFunctionHook g_TransferAll;
        mCFunctionHook g_TransferStack;
        GEBool g_LootDiagnostics = GEFalse;
        GEU32 g_Sequence = 0;
        gCSession **g_ppCurrentSession = nullptr;

        // Diagnostic reads are confined to a running session. Both the current
        // player body and the original hero may own an inventory during a form.
        GEBool IsPlayerInventory(gCInventory_PS *inventory)
        {
            if (!inventory || !g_ppCurrentSession || !*g_ppCurrentSession ||
                !(*g_ppCurrentSession)->IsGameRunning())
                return GEFalse;
            Entity player = Entity::GetPlayer();
            eCEntity *hero = (*g_ppCurrentSession)->GetHero();
            return (player != None && player.Inventory.m_pEngineEntityPropertySet == inventory) ||
                   (hero && hero->GetPropertySet(eEPropertySetType_Inventory) == inventory);
        }

        GEI64 TotalAmount(gCInventory_PS *inventory)
        {
            GEI64 total = 0;
            for (GEInt i = 0; i < inventory->GetStackCount(); ++i)
            {
                gCInventoryStack const *stack = inventory->GetStack(i);
                if (stack)
                    total += stack->GetAmount();
            }
            return total;
        }

        void Context(GEU32 id, GELPCChar operation, gCInventory_PS *inventory)
        {
            Entity player = Entity::GetPlayer();
            bCString const name = player.GetName();
            bCString const task = player.Routine.GetCurrentTask();
            bCString const state = player.Routine.GetCurrentState();
            GetLogger().LogFormat("[DebugLoot] #%u tick=%u %s BEGIN inventory=%p player='%s' task='%s' state='%s'\n",
                                  id, ::GetTickCount(), operation, inventory, name.GetText(), task.GetText(), state.GetText());
        }

        GEInt GE_STDCALL Insert(gCInventory_PS *inventory, eCEntity *item)
        {
            using Original = GEInt(GE_STDCALL *)(gCInventory_PS *, eCEntity *);
            GEBool const watched = g_LootDiagnostics && IsPlayerInventory(inventory);
            GEU32 const id = watched ? ++g_Sequence : 0;
            GEI64 const before = watched ? TotalAmount(inventory) : 0;
            if (watched)
            {
                Context(id, "InsertStack", inventory);
                Entity source(static_cast<eCEntity const *>(item));
                GetLogger().LogFormat("[DebugLoot] #%u item=%p name='%s' totalBefore=%lld\n",
                                      id, item, source != None ? source.GetName().GetText() : "<none>", before);
            }
            GEInt const result = g_Insert.GetOriginalFunction<Original>()(inventory, item);
            // The item can be destroyed or merged by the original: never read it here.
            if (watched)
                GetLogger().LogFormat("[DebugLoot] #%u InsertStack END result=%d totalAfter=%lld\n",
                                      id, result, TotalAmount(inventory));
            return result;
        }

        GEBool GE_STDCALL Transfer(gCInventory_PS *source, gCInventory_PS *destination,
                                   GEInt index, GEInt amount, gEStackType type)
        {
            using Original = GEBool(GE_STDCALL *)(gCInventory_PS *, gCInventory_PS *, GEInt, GEInt, gEStackType);
            GEBool const watched = g_LootDiagnostics && IsPlayerInventory(destination);
            GEU32 const id = watched ? ++g_Sequence : 0;
            if (watched)
            {
                Context(id, "TransferItemsTo", destination);
                gCInventoryStack const *stack = source && index >= 0 && index < source->GetStackCount()
                                                   ? source->GetStack(index) : nullptr;
                GetLogger().LogFormat("[DebugLoot] #%u source=%p index=%d requested=%d sourceAmount=%d totalBefore=%lld\n",
                                      id, source, index, amount, stack ? stack->GetAmount() : -1, TotalAmount(destination));
            }
            GEBool const result = g_Transfer.GetOriginalFunction<Original>()(source, destination, index, amount, type);
            if (watched)
                GetLogger().LogFormat("[DebugLoot] #%u TransferItemsTo END result=%d totalAfter=%lld\n",
                                      id, static_cast<int>(result), TotalAmount(destination));
            return result;
        }
        GEBool GE_STDCALL TransferAll(gCInventory_PS *source, gCInventory_PS *destination, gEStackType type)
        {
            using Original = GEBool(GE_STDCALL *)(gCInventory_PS *, gCInventory_PS *, gEStackType);
            return g_TransferAll.GetOriginalFunction<Original>()(source, destination, type);
        }
        GEBool GE_STDCALL TransferStack(gCInventory_PS *source, gCInventory_PS *destination, GEInt index, gEStackType type)
        {
            using Original = GEBool(GE_STDCALL *)(gCInventory_PS *, gCInventory_PS *, GEInt, gEStackType);
            return g_TransferStack.GetOriginalFunction<Original>()(source, destination, index, type);
        }
    }

    void InstallLootDiagnostics()
    {
        g_LootDiagnostics = IsEnabled();
        if (!g_LootDiagnostics)
            return;
        HMODULE game = ::GetModuleHandleA("Game.dll");
        g_ppCurrentSession = game ? reinterpret_cast<gCSession **>(::GetProcAddress(game, "?ms_pCurrentSession@gCSession@@1PAV1@A")) : nullptr;
        if (!g_ppCurrentSession)
        {
            GetLogger().LogFormat("[DebugLoot] Session export missing; diagnostics not installed.\n");
            return;
        }
        auto install = [game](mCFunctionHook &hook, GELPCChar name, GELPVoid replacement)
        {
            GELPVoid original = game ? reinterpret_cast<GELPVoid>(::GetProcAddress(game, name)) : nullptr;
            GEBool const ok = original && hook.Prepare(original, replacement).ThisCall().Hook();
            GetLogger().LogFormat("[DebugLoot] %s: %s\n", name, ok ? "hooked" : "FAILED");
        };
        install(g_Insert, "?InsertStack@gCInventory_PS@@QAEHPAVeCEntity@@@Z", reinterpret_cast<GELPVoid>(&Insert));
        install(g_Transfer, "?TransferItemsTo@gCInventory_PS@@QAE_NPAV1@HHW4gEStackType@@@Z", reinterpret_cast<GELPVoid>(&Transfer));
        install(g_TransferAll, "?TransferAllTo@gCInventory_PS@@QAE_NPAV1@W4gEStackType@@@Z", reinterpret_cast<GELPVoid>(&TransferAll));
        install(g_TransferStack, "?TransferStackTo@gCInventory_PS@@QAE_NPAV1@HW4gEStackType@@@Z", reinterpret_cast<GELPVoid>(&TransferStack));
    }
}
