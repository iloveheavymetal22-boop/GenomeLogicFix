#pragma once

//
// CrashDiag (v1.0.66.0): the crash reporter of the plugin. The engine writes ge3logN.log on a crash, but it has no names: only raw
// addresses, and most of the frames of the stack are missing. This module installs a vectored exception handler that, for a
// FATAL-type exception (access violation, illegal / privileged instruction, in-page error, division by zero, heap corruption),
// writes <Documents>\Gothic3\logs\Script_GenomeLogicFix.crash.log:
//   - the exception (code, address as module+offset, the kind and the address of a bad access), the registers;
//   - the frames of the call stack found by scanning the stack for return addresses (module+offset of every one: the plugin, the
//     scripts, the engine), which the engine log does not have;
//   - the NAMES of the entities (eCEntity) that are in the registers and on the stack, and of the owners of the animation objects
//     (eCVisualAnimation_PS) found there: the NPC / item whose object the engine was processing when it fell;
//   - the last lines of Script_GenomeLogicFix.log (what the plugin did right before).
// The handler never handles anything: it always lets the exception go on (the engine log is written as before). An exception that
// happens inside a call the plugin guards itself (GuardEnter / GuardLeave) is reported too, marked as guarded.
// The previous crash report is kept as Script_GenomeLogicFix.crash.prev.log. INI key CrashDiagnostics=true (default) turns it on.
//
namespace CrashDiag
{
    // a_strMainLogPath: the full path of Script_GenomeLogicFix.log (the crash report goes next to it).
    void Install(char const *a_strMainLogPath);

    // The plugin calls a native function under its own structured exception handler: a fault inside is expected and handled.
    void GuardEnter();
    void GuardLeave();

    // v1.0.68.0: the ring of the last companion records (one line per companion per teleport stone: the name, the party type, the AI task,
    // Waiting, the distance, WaiterType and the exact reason why he was carried or skipped). The lines are also written to the plugin
    // log; the ring is dumped into the crash report as the section "Companion records", so a crash right after a teleport shows them.
    void Note(char const *a_strLine);
} // namespace CrashDiag
