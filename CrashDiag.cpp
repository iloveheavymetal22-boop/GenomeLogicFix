#include "DebugLog.h"
#include "CrashDiag.h"

#include <g3sdk/Script.h>

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace CrashDiag
{
    namespace
    {
        volatile LONG g_lGuard = 0;   // how many guarded native calls are running now (the plugin handles a fault inside)
        volatile LONG g_lBusy = 0;    // the handler is running (the nested faults of its own probes are not reported)
        volatile LONG g_lReports = 0; // how many reports were written (a limit against benign first-chance exceptions)
        DWORD g_dwLastEip = 0;
        int g_iSameEip = 0;
        char g_strCrashPath[MAX_PATH] = {};
        char g_strMainLogPath[MAX_PATH] = {};
        PVOID g_pHandler = nullptr;

        int const MAX_REPORTS = 16;
        int const STACK_SCAN_DWORDS = 2048; // 8 KB above ESP
        int const MAX_FRAMES = 48;
        int const MAX_ENTITIES = 16;
        DWORD const LOG_TAIL_BYTES = 6000;

        using PFNGetEntityName = void *(__thiscall *)(void *);

        // v1.0.68.0: the ring of the companion records (see Note). A plain array: the game logic runs on one thread, the handler only reads.
        int const NOTE_COUNT = 48;
        int const NOTE_LENGTH = 400;
        char g_arrNotes[NOTE_COUNT][NOTE_LENGTH] = {};
        volatile LONG g_lNoteNext = 0;

        void Write(HANDLE a_hFile, char const *a_strFormat, ...)
        {
            char strBuffer[640] = {};
            va_list Args;
            va_start(Args, a_strFormat);
            _vsnprintf_s(strBuffer, sizeof(strBuffer), _TRUNCATE, a_strFormat, Args);
            va_end(Args);
            DWORD dwWritten = 0;
            ::WriteFile(a_hFile, strBuffer, static_cast<DWORD>(strlen(strBuffer)), &dwWritten, nullptr);
        }

        // The 32-bit value at the address, false when the address cannot be read (the nested fault is caught here; the handler
        // ignores faults that happen while it runs, see g_lBusy).
        bool ReadDword(DWORD a_dwAddress, DWORD *o_pValue)
        {
            __try
            {
                *o_pValue = *reinterpret_cast<volatile DWORD const *>(a_dwAddress);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool ReadByte(DWORD a_dwAddress, BYTE *o_pValue)
        {
            __try
            {
                *o_pValue = *reinterpret_cast<volatile BYTE const *>(a_dwAddress);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // "Module.dll+0xOFFSET" of an address, or a plain address when it is in no module.
        void Describe(DWORD a_dwAddress, char *o_pBuffer, size_t a_uSize)
        {
            HMODULE hModule = nullptr;
            if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCSTR>(a_dwAddress), &hModule) &&
                hModule)
            {
                char strPath[MAX_PATH] = {};
                ::GetModuleFileNameA(hModule, strPath, MAX_PATH);
                char const *pName = strrchr(strPath, '\\');
                pName = pName ? pName + 1 : strPath;
                _snprintf_s(o_pBuffer, a_uSize, _TRUNCATE, "%s+0x%X", pName,
                            static_cast<unsigned>(a_dwAddress - reinterpret_cast<DWORD>(hModule)));
            }
            else
                _snprintf_s(o_pBuffer, a_uSize, _TRUNCATE, "0x%08X (no module)", static_cast<unsigned>(a_dwAddress));
        }

        // The value is an address in executable memory that follows a call instruction (a real return address).
        bool IsReturnAddress(DWORD a_dwValue)
        {
            if (a_dwValue < 0x10000)
                return false;
            MEMORY_BASIC_INFORMATION Info = {};
            if (!::VirtualQuery(reinterpret_cast<LPCVOID>(a_dwValue), &Info, sizeof(Info)))
                return false;
            DWORD const dwExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (Info.State != MEM_COMMIT || !(Info.Protect & dwExec))
                return false;
            BYTE b5 = 0, b6a = 0, b6b = 0, b2a = 0, b2b = 0;
            if (ReadByte(a_dwValue - 5, &b5) && b5 == 0xE8) // call rel32
                return true;
            if (ReadByte(a_dwValue - 6, &b6a) && ReadByte(a_dwValue - 5, &b6b) && b6a == 0xFF &&
                (b6b == 0x15 || b6b == 0x90 || b6b == 0x93))
                return true; // call [abs] / call [reg+disp32]
            if (ReadByte(a_dwValue - 2, &b2a) && ReadByte(a_dwValue - 1, &b2b) && b2a == 0xFF && (b2b & 0xF8) == 0xD0)
                return true; // call reg
            return false;
        }

        // The name of an eCEntity (Engine.dll: eCEntity::GetName), false when it cannot be read.
        bool EntityName(DWORD a_dwEntity, char *o_pBuffer, size_t a_uSize)
        {
            static PFNGetEntityName s_pfnGetName = nullptr;
            if (!s_pfnGetName)
            {
                HMODULE hEngine = ::GetModuleHandleA("Engine.dll");
                if (hEngine)
                    s_pfnGetName =
                        reinterpret_cast<PFNGetEntityName>(::GetProcAddress(hEngine, "?GetName@eCEntity@@QBEABVbCString@@XZ"));
            }
            if (!s_pfnGetName)
                return false;
            __try
            {
                void *pName = s_pfnGetName(reinterpret_cast<void *>(a_dwEntity));
                if (!pName)
                    return false;
                char const *strText = reinterpret_cast<bCString const *>(pName)->GetText();
                if (!strText)
                    return false;
                _snprintf_s(o_pBuffer, a_uSize, _TRUNCATE, "%s", strText);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        struct EntityList
        {
            DWORD arrEntity[MAX_ENTITIES];
            char arrFrom[MAX_ENTITIES][40];
            int iCount;
        };

        void AddEntity(EntityList &a_List, DWORD a_dwEntity, char const *a_strFrom)
        {
            if (a_List.iCount >= MAX_ENTITIES)
                return;
            for (int i = 0; i < a_List.iCount; ++i)
                if (a_List.arrEntity[i] == a_dwEntity)
                    return;
            a_List.arrEntity[a_List.iCount] = a_dwEntity;
            _snprintf_s(a_List.arrFrom[a_List.iCount], sizeof(a_List.arrFrom[0]), _TRUNCATE, "%s", a_strFrom);
            ++a_List.iCount;
        }

        // A candidate value (a register / a dword of the stack) is an eCEntity, or an animation object that belongs to one: its
        // owner (the entity of the property set, at +0xC, the same layout as eCEntityPropertySet::GetEntity) is listed then.
        void Classify(DWORD a_dwValue, DWORD a_dwVtEntity, DWORD a_dwVtAnimation, char const *a_strFrom, EntityList &a_List)
        {
            if (a_dwValue < 0x10000 || (a_dwValue & 3) != 0)
                return;
            DWORD dwVtable = 0;
            if (!ReadDword(a_dwValue, &dwVtable))
                return;
            if (a_dwVtEntity && dwVtable == a_dwVtEntity)
                AddEntity(a_List, a_dwValue, a_strFrom);
            else if (a_dwVtAnimation && dwVtable == a_dwVtAnimation)
            {
                DWORD dwOwner = 0;
                DWORD dwOwnerVt = 0;
                if (ReadDword(a_dwValue + 0xC, &dwOwner) && dwOwner >= 0x10000 && ReadDword(dwOwner, &dwOwnerVt) &&
                    dwOwnerVt == a_dwVtEntity)
                    AddEntity(a_List, dwOwner, a_strFrom);
            }
        }

        void WriteLogTail(HANDLE a_hOut)
        {
            if (g_strMainLogPath[0] == 0)
                return;
            HANDLE hLog = ::CreateFileA(g_strMainLogPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hLog == INVALID_HANDLE_VALUE)
            {
                Write(a_hOut, "  (the plugin log cannot be opened)\r\n");
                return;
            }
            DWORD const dwSize = ::GetFileSize(hLog, nullptr);
            DWORD const dwFrom = dwSize > LOG_TAIL_BYTES ? dwSize - LOG_TAIL_BYTES : 0;
            ::SetFilePointer(hLog, static_cast<LONG>(dwFrom), nullptr, FILE_BEGIN);
            static char s_strBuffer[LOG_TAIL_BYTES + 1];
            DWORD dwRead = 0;
            ::ReadFile(hLog, s_strBuffer, LOG_TAIL_BYTES, &dwRead, nullptr);
            ::CloseHandle(hLog);
            s_strBuffer[dwRead] = 0;
            char const *pStart = s_strBuffer;
            if (dwFrom > 0)
            {
                char const *pLine = strchr(s_strBuffer, '\n'); // the first line is cut: start with the next one
                if (pLine)
                    pStart = pLine + 1;
            }
            DWORD dwWritten = 0;
            ::WriteFile(a_hOut, pStart, static_cast<DWORD>(strlen(pStart)), &dwWritten, nullptr);
        }

        bool IsFatalCode(DWORD a_dwCode)
        {
            switch (a_dwCode)
            {
                case EXCEPTION_ACCESS_VIOLATION:
                case EXCEPTION_ILLEGAL_INSTRUCTION:
                case EXCEPTION_PRIV_INSTRUCTION:
                case EXCEPTION_IN_PAGE_ERROR:
                case EXCEPTION_INT_DIVIDE_BY_ZERO:
                case 0xC0000374: // STATUS_HEAP_CORRUPTION
                    return true;
                default: return false;
            }
        }

        void WriteRegisters(HANDLE a_hOut, CONTEXT const *a_pContext)
        {
            Write(a_hOut, "Registers: EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X EIP=%08X\r\n",
                  static_cast<unsigned>(a_pContext->Eax), static_cast<unsigned>(a_pContext->Ebx),
                  static_cast<unsigned>(a_pContext->Ecx), static_cast<unsigned>(a_pContext->Edx),
                  static_cast<unsigned>(a_pContext->Esi), static_cast<unsigned>(a_pContext->Edi),
                  static_cast<unsigned>(a_pContext->Ebp), static_cast<unsigned>(a_pContext->Esp),
                  static_cast<unsigned>(a_pContext->Eip));
        }

        void WriteReport(EXCEPTION_POINTERS *a_pInfo)
        {
            EXCEPTION_RECORD const *pRecord = a_pInfo->ExceptionRecord;
            CONTEXT const *pContext = a_pInfo->ContextRecord;

            HANDLE hOut = ::CreateFileA(g_strCrashPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hOut == INVALID_HANDLE_VALUE)
                return;

            SYSTEMTIME Time = {};
            ::GetLocalTime(&Time);
            char strWhere[320] = {};
            Describe(reinterpret_cast<DWORD>(pRecord->ExceptionAddress), strWhere, sizeof(strWhere));

            Write(hOut, "==================================================================================\r\n");
            Write(hOut, "[CrashDiag] %02d:%02d:%02d  (tick %u)  report %ld\r\n", Time.wHour, Time.wMinute, Time.wSecond,
                  static_cast<unsigned>(::GetTickCount()), g_lReports);
            Write(hOut, "Exception 0x%08X at %s\r\n", static_cast<unsigned>(pRecord->ExceptionCode), strWhere);
            if (pRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2)
                Write(hOut, "  access violation: %s address 0x%08X\r\n",
                      pRecord->ExceptionInformation[0] == 0 ? "READ of"
                                                            : (pRecord->ExceptionInformation[0] == 8 ? "EXECUTE of" : "WRITE to"),
                      static_cast<unsigned>(pRecord->ExceptionInformation[1]));
            Write(hOut, "  %s\r\n",
                  g_lGuard > 0 ? "INSIDE a call guarded by the plugin (ZS_Attack_Loop): the plugin handles this fault, the game goes on"
                               : "NOT guarded by the plugin: if nobody else handles it, the game goes down (see ge3logN.log)");
            WriteRegisters(hOut, pContext);

            // The vtables of the engine classes (to recognise an object by its first dword).
            DWORD dwVtEntity = 0;
            DWORD dwVtAnimation = 0;
            HMODULE hEngine = ::GetModuleHandleA("Engine.dll");
            if (hEngine)
            {
                dwVtEntity = reinterpret_cast<DWORD>(::GetProcAddress(hEngine, "??_7eCEntity@@6B@"));
                dwVtAnimation = reinterpret_cast<DWORD>(::GetProcAddress(hEngine, "??_7eCVisualAnimation_PS@@6B@"));
            }

            static EntityList s_Entities;
            s_Entities.iCount = 0;
            Classify(pContext->Eax, dwVtEntity, dwVtAnimation, "EAX", s_Entities);
            Classify(pContext->Ebx, dwVtEntity, dwVtAnimation, "EBX", s_Entities);
            Classify(pContext->Ecx, dwVtEntity, dwVtAnimation, "ECX", s_Entities);
            Classify(pContext->Edx, dwVtEntity, dwVtAnimation, "EDX", s_Entities);
            Classify(pContext->Esi, dwVtEntity, dwVtAnimation, "ESI", s_Entities);
            Classify(pContext->Edi, dwVtEntity, dwVtAnimation, "EDI", s_Entities);
            Classify(pContext->Ebp, dwVtEntity, dwVtAnimation, "EBP", s_Entities);

            // The frames: every dword of the stack that is a return address (after a call instruction), in stack order.
            Write(hOut, "Call stack (the stack is scanned for return addresses, %d dwords above ESP, innermost first):\r\n",
                  STACK_SCAN_DWORDS);
            int iFrames = 0;
            for (int i = 0; i < STACK_SCAN_DWORDS; ++i)
            {
                DWORD dwValue = 0;
                if (!ReadDword(pContext->Esp + static_cast<DWORD>(i) * 4, &dwValue))
                    break;
                if (iFrames < MAX_FRAMES && IsReturnAddress(dwValue))
                {
                    char strFrame[320] = {};
                    Describe(dwValue, strFrame, sizeof(strFrame));
                    Write(hOut, "  [esp+0x%04X] 0x%08X  %s\r\n", i * 4, static_cast<unsigned>(dwValue), strFrame);
                    ++iFrames;
                }
                char strFrom[40] = {};
                _snprintf_s(strFrom, sizeof(strFrom), _TRUNCATE, "stack esp+0x%X", i * 4);
                Classify(dwValue, dwVtEntity, dwVtAnimation, strFrom, s_Entities);
            }
            if (iFrames == 0)
                Write(hOut, "  (no return address found)\r\n");

            Write(hOut, "Entities (eCEntity) in the registers / on the stack, and owners of animation objects found there:\r\n");
            if (s_Entities.iCount == 0)
                Write(hOut, "  (none recognised)\r\n");
            for (int i = 0; i < s_Entities.iCount; ++i)
            {
                char strName[128] = "?";
                EntityName(s_Entities.arrEntity[i], strName, sizeof(strName));
                Write(hOut, "  0x%08X  '%s'   (from %s)\r\n", static_cast<unsigned>(s_Entities.arrEntity[i]), strName,
                      s_Entities.arrFrom[i]);
            }

            Write(hOut, "Companion records (the last teleport stones / arena sessions, oldest first):\r\n");
            {
                LONG const lNext = g_lNoteNext;
                int iWritten = 0;
                for (int i = 0; i < NOTE_COUNT; ++i)
                {
                    char const *pLine = g_arrNotes[(lNext + i) % NOTE_COUNT];
                    if (pLine[0] == 0)
                        continue;
                    Write(hOut, "  %s\r\n", pLine);
                    ++iWritten;
                }
                if (iWritten == 0)
                    Write(hOut, "  (none)\r\n");
            }

            Write(hOut, "The end of Script_GenomeLogicFix.log (what the plugin did right before):\r\n");
            WriteLogTail(hOut);
            Write(hOut, "\r\n");
            ::CloseHandle(hOut);
        }

        LONG CALLBACK Handler(EXCEPTION_POINTERS *a_pInfo)
        {
            if (!a_pInfo || !a_pInfo->ExceptionRecord || !a_pInfo->ContextRecord ||
                !IsFatalCode(a_pInfo->ExceptionRecord->ExceptionCode))
                return EXCEPTION_CONTINUE_SEARCH;
            if (g_lBusy != 0 || g_lReports >= MAX_REPORTS)
                return EXCEPTION_CONTINUE_SEARCH;

            // The same fault at the same instruction again and again is one report, not a hundred.
            DWORD const dwEip = reinterpret_cast<DWORD>(a_pInfo->ExceptionRecord->ExceptionAddress);
            if (dwEip == g_dwLastEip)
            {
                if (++g_iSameEip > 2)
                    return EXCEPTION_CONTINUE_SEARCH;
            }
            else
            {
                g_dwLastEip = dwEip;
                g_iSameEip = 1;
            }

            ::InterlockedExchange(&g_lBusy, 1);
            ::InterlockedIncrement(&g_lReports);
            __try
            {
                WriteReport(a_pInfo);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            ::InterlockedExchange(&g_lBusy, 0);
            return EXCEPTION_CONTINUE_SEARCH; // never handles anything: the engine / the guard of the plugin go on as before
        }
    } // namespace

    void GuardEnter()
    {
        if (!DebugLog::IsEnabled())
            return;
        ::InterlockedIncrement(&g_lGuard);
    }

    void GuardLeave()
    {
        if (!DebugLog::IsEnabled())
            return;
        if (g_lGuard > 0)
            ::InterlockedDecrement(&g_lGuard);
    }

    void Note(char const *a_strLine)
    {
        if (!DebugLog::IsEnabled())
            return;
        if (!a_strLine)
            return;
        LONG const lSlot = (::InterlockedIncrement(&g_lNoteNext) - 1) % NOTE_COUNT;
        strncpy_s(g_arrNotes[lSlot], NOTE_LENGTH, a_strLine, _TRUNCATE);
        // the line ends without a line break: the report adds its own
        size_t const uLen = strlen(g_arrNotes[lSlot]);
        if (uLen > 0 && g_arrNotes[lSlot][uLen - 1] == '\n')
            g_arrNotes[lSlot][uLen - 1] = 0;
    }

    void Install(char const *a_strMainLogPath)
    {
        if (!DebugLog::IsEnabled())
            return;
        if (g_pHandler || !a_strMainLogPath || a_strMainLogPath[0] == 0)
            return;
        strncpy_s(g_strMainLogPath, sizeof(g_strMainLogPath), a_strMainLogPath, _TRUNCATE);

        // ...\Script_GenomeLogicFix.log -> ...\Script_GenomeLogicFix.crash.log (the previous one -> .crash.prev.log)
        size_t const uLen = strlen(g_strMainLogPath);
        if (uLen <= 4 || _stricmp(g_strMainLogPath + uLen - 4, ".log") != 0 || uLen + 12 >= MAX_PATH)
            return;
        strncpy_s(g_strCrashPath, sizeof(g_strCrashPath), g_strMainLogPath, uLen - 4);
        strcat_s(g_strCrashPath, sizeof(g_strCrashPath), ".crash.log");
        char strPrev[MAX_PATH] = {};
        strncpy_s(strPrev, sizeof(strPrev), g_strMainLogPath, uLen - 4);
        strcat_s(strPrev, sizeof(strPrev), ".crash.prev.log");
        ::MoveFileExA(g_strCrashPath, strPrev, MOVEFILE_REPLACE_EXISTING);

        g_pHandler = ::AddVectoredExceptionHandler(1, Handler);
    }
} // namespace CrashDiag

