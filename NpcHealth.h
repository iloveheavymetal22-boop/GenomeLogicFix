#pragma once

class gCSession;

namespace NpcHealth
{
    void LoadConfig(char const *path);
    void InstallHook();
    void Update(gCSession &session);
}
