#include "GroupLogic.h"
#include "PluginRuntime.h"
#include "ArenaJudge.h"
#include "GroupBrawl.h"
#include "SocialLogic.h"
#include <g3sdk/util/ScriptInterface.h>
#include <cstring>
#include "DebugLog.h"
#include "CrashDiag.h"
#include <g3sdk/util/ScriptUtil.h>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

namespace GroupLogic
{
    namespace
    {
        GEFloat const FAR_COMPANION_DISTANCE = 10000.0f;
    }
        GEBool IsHeroTransformedNow()
        {
            // v1.0.34.0: the body of a form ("Transform_*") answers IsNPC() == false (the log shows faction -1 / party type -1
            // for it), so IsNPC() must NOT be required here, otherwise a demon was taken for the human form.
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return GEFalse;
            if (Hero.IsNPC() && Hero.NPC.IsTransformed())
                return GETrue;
            bCString const strName = Hero.GetName();
            if (strName.GetText() == nullptr || strName.GetText()[0] == 0)
                return GEFalse;
            return _stricmp(strName.GetText(), "PC_Hero") != 0 ? GETrue : GEFalse;
        }

        GEBool IsHeroFormEntity(Entity &a_Npc)
        {
            // v1.0.34.0: no IsNPC() here - the body of the form is not an NPC for the wrapper.
            if (a_Npc == None || a_Npc == Entity::GetPlayer())
                return GEFalse;
            bCString const strName = a_Npc.GetName();
            return _strnicmp(strName.GetText(), "Transform_", 10) == 0 ? GETrue : GEFalse;
        }

        GEBool IsHeroControlled(Entity &a_Npc)
        {
            Entity Hero = Entity::GetPlayer();
            if (a_Npc == None || a_Npc == Hero || !a_Npc.IsNPC())
                return GEFalse;
            Entity Leader = a_Npc.Party.GetPartyLeader();
            GEBool bLeaderIsHero = Leader == Hero;
            // The beast form: the transformation (MagicTransform -> StartTransform) makes the spawned creature the player
            // (GetPlayer() returns it), while the party of the hero still belongs to his own body "PC_Hero", which is
            // hidden meanwhile. The army follows the master in any form, so that body counts as the master too.
            if (!bLeaderIsHero && Leader != None && IsHeroTransformedNow())
            {
                Entity Body = Entity::GetEntity(bCString("PC_Hero"));
                if (Body != None)
                {
                    if (a_Npc == Body)
                        return GEFalse;
                    bLeaderIsHero = Leader == Body;
                }
            }
            if (!bLeaderIsHero)
                return GEFalse;
            return a_Npc.Party.PartyMemberType != gEPartyMemberType_None ? GETrue : GEFalse;
        }
        GEInt const TELEPORT_FOLLOWERS_MAX = 16;
        GEFloat const TELEPORT_FOLLOWER_MIN_DISTANCE = 300.0f; // beside the hero already: nothing to move (a failed cast)
        GEFloat const TELEPORT_HERO_MIN_MOVE = 1000.0f;        // the hero really went somewhere (a stone, not a step)
        GEU32 const TELEPORT_PENDING_MS = 4000;                // a technical limit of the late move below (not a game timing)
        bCString g_arrTeleportFollowers[TELEPORT_FOLLOWERS_MAX];
        GEInt g_iTeleportFollowers = 0;
        bCVector g_vTeleportHeroBefore;
        GEBool g_bTeleportPending = GEFalse; // the native script returned, the hero has not changed the place yet
        GEU32 g_u32TeleportPendingTick = 0;
        GEBool g_bTeleportScriptRuns = GEFalse; // inside the native MagicTeleport: the script WaitPlayerParty may be asked

        // v1.0.50.0: the companion was collected for the move (the native script must not put him to wait nor print the plaque).
        GEBool IsTeleportFollower(Entity &a_Npc)
        {
            if (a_Npc == None)
                return GEFalse;
            bCString const strName = a_Npc.GetName();
            for (GEInt i = 0; i < g_iTeleportFollowers; ++i)
                if (g_arrTeleportFollowers[i] == strName)
                    return GETrue;
            return GEFalse;
        }

        // The companion was NOT carried to the hero (the move failed / the hero did not leave the place): what the native script
        // would have done is done now - he waits where he is, and the plaque "<name> is waiting" is shown.
        void RestoreNativeWait(Entity &a_Npc)
        {
            a_Npc.Party.Waiting = GETrue;
            a_Npc.Routine.FullStop();
            a_Npc.Routine.SetTask("ZS_WaitForPlayer");
            Host::PrintCompanionWaitMessage(a_Npc);
        }

        GEFloat DistanceBetween(bCVector const &a_vFirst, bCVector const &a_vSecond)
        {
            GEFloat const fDx = a_vFirst.m_fX - a_vSecond.m_fX;
            GEFloat const fDy = a_vFirst.m_fY - a_vSecond.m_fY;
            GEFloat const fDz = a_vFirst.m_fZ - a_vSecond.m_fZ;
            return std::sqrt(fDx * fDx + fDy * fDy + fDz * fDz);
        }

        // v1.0.68.0: ONE LINE PER COMPANION PER TELEPORT STONE: the name, the party type, the AI task, Waiting, the distance to the hero, the
        // WaiterType and the exact reason why he was carried / skipped / stopped. The line goes to the plugin log and to the ring of the
        // crash report (CrashDiag::Note), so a crash right after a stone shows who was doing what.
        void LogTeleportRecord(GELPCChar a_strStage, Entity &a_Npc, Entity &a_Hero, GELPCChar a_strDecision, GELPCChar a_strReason)
        {
            bCString const strTask = a_Npc.Routine.GetCurrentTask();
            gEPartyMemberType const enuType = a_Npc.Party.PartyMemberType;
            char strLine[420] = {};
            _snprintf_s(strLine, sizeof(strLine), _TRUNCATE,
                        "[GenomeLogicFix][teleport] %s | '%s' | party type %d | task '%s' | Waiting=%d | distance %.0f | WaiterType=%s | %s: %s",
                        a_strStage, a_Npc.GetName().GetText(), static_cast<int>(enuType), strTask.GetText() ? strTask.GetText() : "?",
                        static_cast<int>(a_Npc.Party.Waiting ? 1 : 0),
                        a_Hero != None ? static_cast<double>(a_Npc.GetDistanceTo(a_Hero)) : -1.0, Host::WaiterTypeText(Host::GetWaiterType(a_Npc)),
                        a_strDecision, a_strReason);
            DebugLog::GetLogger().LogFormat("%s\n", strLine);
            CrashDiag::Note(strLine);
        }

        GEInt const TELEPORT_SEEN_MAX = 48;
        bCString g_arrTeleportSeen[TELEPORT_SEEN_MAX];
        GEInt g_iTeleportSeen = 0;

        // v1.0.69.0: ONE candidate for the teleport. a_bPartyMember: he stands in the member list of the party of the hero - the very list the
        // native script MagicTeleport walks through (WaitPlayerParty is run for every member of the types 1..3). Such a member is a
        // companion even when the check "the leader of his own party is the hero" fails (IsHeroControlled): that was the reason why
        // companions were put to wait and left behind - they were not collected, but the native script found them all the same.
        // Every skip is written down, none is silent any more.
        void ConsiderTeleportCandidate(Entity &Npc, Entity &Hero, GEBool a_bPartyMember)
        {
            if (Npc == None || Npc == Hero || !Npc.IsNPC())
                return;
            gEPartyMemberType const enuType = Npc.Party.PartyMemberType;
            GEBool const bPermanent = enuType == gEPartyMemberType_Party || enuType == gEPartyMemberType_Mob ||
                                      enuType == gEPartyMemberType_Slave;
            GEBool const bControlled = IsHeroControlled(Npc);
            if (!bControlled && !(a_bPartyMember && bPermanent))
                return; // not a companion of the hero at all: not recorded
            if (g_iTeleportFollowers >= TELEPORT_FOLLOWERS_MAX)
            {
                LogTeleportRecord("collect", Npc, Hero, "SKIPPED", "the list of the carried companions is full");
                return;
            }
            if (Npc.IsDead())
            {
                LogTeleportRecord("collect", Npc, Hero, "SKIPPED", "he is dead");
                return;
            }
            // v1.0.69.0: a companion who is knocked down is NOT dropped any more (before, he was skipped in silence here, the native script
            // put him to wait, and he stayed behind). He is carried too; only his position is changed, his AI is left as it is.
            {
                // each companion is judged (and written down) once per stone, whichever list shows him first
                bCString const strSeen = Npc.GetName();
                for (GEInt i = 0; i < g_iTeleportSeen; ++i)
                    if (g_arrTeleportSeen[i] == strSeen)
                        return;
                if (g_iTeleportSeen < TELEPORT_SEEN_MAX)
                    g_arrTeleportSeen[g_iTeleportSeen++] = strSeen;
            }
            if (!bControlled)
                LogTeleportRecord("collect", Npc, Hero, "NOTE",
                                  "a member of the party of the hero, but the leader of his own party is NOT the hero: taken by the member list");
            {
                if (!bPermanent)
                {
                    LogTeleportRecord("collect", Npc, Hero, "SKIPPED", "party type 4 / 5 (a tamed / summoned creature): left to the native script");
                    return;
                }
                // v1.0.51.0: THE BLACK LIST (IgnoreTeleportNames of the INI), checked FIRST, by the native name. Such a companion
                // is never collected, so he is never carried: the native script WaitPlayerParty runs for him as in the pure game
                // (he stays at the old place, "waits", the plaque is shown), whether he followed the hero or already waited.
                if (PluginRuntime::IsTeleportNameIgnored(Npc.GetName().GetText()))
                {
                    LogTeleportRecord("collect", Npc, Hero, "SKIPPED", "the name is in IgnoreTeleportNames: stays and waits natively");
                    return;
                }
                // v1.0.68.0: THE WAITER TYPE decides, the native flag Waiting alone does not. A wait that the PLAYER ordered (USER, or an
                // old wait that no mark describes) is never broken: the companion stays. A wait that the plugin / the game ordered (AUTO:
                // the start of an arena duel, a failed carry, a stone that put him to wait) is ignored: he is carried like a follower.
                if (Npc.Party.Waiting)
                {
                    WaiterType const enuWait = Host::GetWaiterType(Npc);
                    if (enuWait != WaiterType_Auto)
                    {
                        LogTeleportRecord("collect", Npc, Hero, "SKIPPED",
                                          enuWait == WaiterType_User ? "Waiting by the order of the PLAYER (USER): he stays"
                                                                       : "Waiting with no mark (an older order of the player): he stays");
                        return;
                    }
                    g_arrTeleportFollowers[g_iTeleportFollowers++] = Npc.GetName();
                    LogTeleportRecord("collect", Npc, Hero, "CARRIED",
                                      "Waiting by an AUTO order (the plugin / the game): the native flag is ignored, he is carried");
                    return;
                }
                Host::ClearWaiterMarks(Npc); // he follows: an old mark of a wait that is over is dropped
                g_arrTeleportFollowers[g_iTeleportFollowers++] = Npc.GetName();
                LogTeleportRecord("collect", Npc, Hero, "CARRIED", "follows the hero (Waiting=0)");
            }
        }

        // BEFORE the native MagicTeleport: the companions (types 1..3) that follow the hero, and the place of the hero.
        // v1.0.69.0: two sources. (1) every NPC of the world whose party leader is the hero (the old way); (2) the member list of the party
        // of the hero - the list the native script itself walks. A companion that is in (2) but not in (1) was silently dropped before, put to
        // wait by the native script and left behind (the log showed "native wait" with no "collect" line at all).
        void CollectTeleportFollowers()
        {
            g_iTeleportFollowers = 0;
            g_iTeleportSeen = 0;
            g_bTeleportPending = GEFalse;
            Entity Hero = Entity::GetPlayer();
            g_vTeleportHeroBefore = Hero.GetPosition();
            // v1.0.68.0: a teleport ends the arena session of the plugin: the hero is leaving the arena; whoever waited by the order of
            // the session (AUTO) is carried below like any other AUTO waiter.
            if (Host::IsArenaSessionHeld())
                Host::EndArenaSession("a teleport", GEFalse, 0);
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                ConsiderTeleportCandidate(Npc, Hero, GEFalse);
            }
            GEInt iMembers = 0;
            {
                bTValArray<eCEntity *> arrMembers = Hero.Party.GetMembers(GEFalse);
                for (GEInt i = 0; i < arrMembers.GetCount(); ++i)
                {
                    eCEntity *pMember = arrMembers.GetAt(i);
                    if (!pMember)
                        continue;
                    ++iMembers;
                    Entity Member(static_cast<eCEntity const *>(pMember));
                    ConsiderTeleportCandidate(Member, Hero, GETrue);
                }
            }
            char strLine[240] = {};
            _snprintf_s(strLine, sizeof(strLine), _TRUNCATE,
                        "[GenomeLogicFix][teleport] stone: %d NPC(s) in the world, %d member(s) in the party of the hero, %d companion(s) carried",
                        static_cast<int>(arrNPCs.GetCount()), static_cast<int>(iMembers), static_cast<int>(g_iTeleportFollowers));
            DebugLog::GetLogger().LogFormat("%s\n", strLine);
            CrashDiag::Note(strLine);
        }

        // v1.0.69.0: THE NET. The native script asks WaitPlayerParty for a member of the party that the collection did not take (for whatever
        // reason). Instead of letting the script leave him behind waiting, he is collected NOW - the hero has not moved yet, so the carry
        // after the cast takes him along. Never for a wait of the player, never for the black list, never for the types 4 / 5.
        GEBool TryLateCollect(Entity &a_Member)
        {
            if (a_Member == None || !a_Member.IsNPC() || a_Member.IsDead() || g_iTeleportFollowers >= TELEPORT_FOLLOWERS_MAX)
                return GEFalse;
            gEPartyMemberType const enuType = a_Member.Party.PartyMemberType;
            if (enuType != gEPartyMemberType_Party && enuType != gEPartyMemberType_Mob && enuType != gEPartyMemberType_Slave)
                return GEFalse;
            if (PluginRuntime::IsTeleportNameIgnored(a_Member.GetName().GetText()))
                return GEFalse;
            if (a_Member.Party.Waiting && Host::GetWaiterType(a_Member) != WaiterType_Auto)
                return GEFalse; // the player's wait stays
            Entity Hero = Entity::GetPlayer();
            g_arrTeleportFollowers[g_iTeleportFollowers++] = a_Member.GetName();
            LogTeleportRecord("late collect", a_Member, Hero, "CARRIED",
                              "the native script asked for him but the collection missed him: taken now, no wait, no plaque");
            return GETrue;
        }

        // v1.0.68.0: THE SAFEGUARD AGAINST THE RUN ACROSS THE MAP. After a teleport a companion of the types 1..3 who is farther than
        // FAR_COMPANION_DISTANCE from the hero (he stayed in another city) and whose wait is not the player's gets the order to stay where he
        // is (FullStop + ZS_WaitForPlayer, AUTO, no plaque): the native follow would send him across the whole map. A USER wait is never
        // touched. A companion who already waits (AUTO) is left as he is. The next teleport stone carries every AUTO waiter to the hero.
        void SecureFarCompanions(GELPCChar a_strWhy)
        {
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return;
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < arrNPCs.GetCount(); ++i)
            {
                Entity Npc = arrNPCs.GetAt(i);
                if (Npc == None || Npc == Hero || !Npc.IsNPC() || Npc.IsDead() || !IsHeroControlled(Npc))
                    continue;
                gEPartyMemberType const enuType = Npc.Party.PartyMemberType;
                if (enuType != gEPartyMemberType_Party && enuType != gEPartyMemberType_Mob && enuType != gEPartyMemberType_Slave)
                    continue;
                if (Npc.GetDistanceTo(Hero) <= FAR_COMPANION_DISTANCE)
                    continue;
                WaiterType const enuWait = Host::GetWaiterType(Npc);
                if (Npc.Party.Waiting)
                {
                    LogTeleportRecord(a_strWhy, Npc, Hero, "FAR, LEFT",
                                      enuWait == WaiterType_Auto ? "already waits by an AUTO order: stays where he is, no run"
                                                                   : "waits by the order of the player: never touched");
                    continue;
                }
                Npc.Routine.FullStop();
                Npc.Party.Waiting = GETrue;
                Npc.Routine.SetTask("ZS_WaitForPlayer");
                Host::MarkWaiterAuto(Npc);
                LogTeleportRecord(a_strWhy, Npc, Hero, "FAR, STOPPED",
                                  "more than 10000 away and not carried: FullStop + wait (AUTO) so he does not run across the map");
            }
        }

        // AFTER the native MagicTeleport: the hero stands at the new place; the collected companions are put to him by the info
        // command MoveTo and go on following him.
        void MoveTeleportFollowers(GELPCChar a_strWhy)
        {
            Entity Hero = Entity::GetPlayer();
            GEInt iMoved = 0;
            for (GEInt i = 0; i < g_iTeleportFollowers; ++i)
            {
                Entity Npc = Entity::GetEntity(g_arrTeleportFollowers[i]);
                if (Hero == None)
                    continue;
                if (Npc == None || !Npc.IsNPC() || Npc.IsDead())
                {
                    char strGone[200] = {};
                    _snprintf_s(strGone, sizeof(strGone), _TRUNCATE, "[GenomeLogicFix][teleport] moved | '%s' | SKIPPED: the entity is gone or dead",
                                g_arrTeleportFollowers[i].GetText());
                    DebugLog::GetLogger().LogFormat("%s\n", strGone);
                    CrashDiag::Note(strGone);
                    continue;
                }
                gEPartyMemberType const enuType = Npc.Party.PartyMemberType;
                if (enuType == gEPartyMemberType_None || enuType == gEPartyMemberType_Controlled ||
                    enuType == gEPartyMemberType_Summoned)
                {
                    LogTeleportRecord("moved", Npc, Hero, "SKIPPED", "left the party meanwhile, or is not a permanent companion");
                    continue;
                }
                if (Npc.IsDown())
                {
                    // knocked down: only the place is changed, his AI (the knock-down task) is not touched
                    GEFloat const fDownBefore = Npc.GetDistanceTo(Hero);
                    if (fDownBefore > TELEPORT_FOLLOWER_MIN_DISTANCE)
                    {
                        Npc.MoveTo(Hero);
                        if (Npc.GetDistanceTo(Hero) > TELEPORT_FOLLOWER_MIN_DISTANCE)
                            Npc.Teleport(Hero);
                        ++iMoved;
                    }
                    char strDown[220] = {};
                    _snprintf_s(strDown, sizeof(strDown), _TRUNCATE,
                                "[GenomeLogicFix][teleport] moved | '%s' | knocked down | distance %.0f -> %.0f | CARRIED: only the place is changed",
                                Npc.GetName().GetText(), static_cast<double>(fDownBefore), static_cast<double>(Npc.GetDistanceTo(Hero)));
                    DebugLog::GetLogger().LogFormat("%s\n", strDown);
                    CrashDiag::Note(strDown);
                    continue;
                }
                GEFloat const fBefore = Npc.GetDistanceTo(Hero);
                GEBool bCarried = GETrue; // beside the hero already counts as carried
                if (fBefore > TELEPORT_FOLLOWER_MIN_DISTANCE)
                {
                    Npc.MoveTo(Hero); // the info command "MoveTo": silent, instant
                    GEFloat fAfter = Npc.GetDistanceTo(Hero);
                    if (fAfter > TELEPORT_FOLLOWER_MIN_DISTANCE)
                    {
                        Npc.Teleport(Hero); // the second way of the game to put a character to another one
                        fAfter = Npc.GetDistanceTo(Hero);
                    }
                    bCarried = fAfter <= TELEPORT_FOLLOWER_MIN_DISTANCE;
                    if (bCarried)
                        ++iMoved;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Teleport (%s): the companion '%s' (party type %d) follows the "
                                                 "hero: distance %.0f -> %.0f (%s).\n",
                                                 a_strWhy, Npc.GetName().GetText(), static_cast<int>(enuType),
                                                 static_cast<double>(fBefore), static_cast<double>(fAfter),
                                                 bCarried ? "carried, no plaque" : "NOT carried, he waits");
                }
                if (bCarried)
                {
                    // The native script was not allowed to put him to wait (WaitPlayerParty is held back): he just goes on following
                    // the hero, with no plaque on the screen.
                    // v1.0.68.0: an AUTO waiter (the arena order, a failed carry) is carried too: the wait is over, no mark is left.
                    Host::ClearWaiterMarks(Npc);
                    Npc.Party.Waiting = GEFalse;
                    Npc.Routine.FullStop();
                    Npc.Routine.SetTask("ZS_FollowPlayer");
                    LogTeleportRecord("moved", Npc, Hero, "CARRIED", "he stands at the hero and follows him (ZS_FollowPlayer)");
                }
                else
                {
                    RestoreNativeWait(Npc); // not carried: the native behaviour and the plaque "<name> is waiting"
                    Host::MarkWaiterAuto(Npc);    // v1.0.68.0: this wait is the plugin's, not the player's: the next stone carries him
                    LogTeleportRecord("moved", Npc, Hero, "NOT CARRIED", "the move failed: he waits where he is (AUTO), the next stone carries him");
                }
            }
            if (iMoved == 0)
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Teleport (%s): nobody had to be moved.\n", a_strWhy);
            SecureFarCompanions(a_strWhy);
            g_iTeleportFollowers = 0;
            g_bTeleportPending = GEFalse;
            for (GEInt i = 0; i < TELEPORT_FOLLOWERS_MAX; ++i)
                g_arrTeleportFollowers[i] = bCString();
        }

        // AFTER the native MagicTeleport script: when the hero is already at the new place the followers are moved at once; when
        // his coordinates have not changed yet (the move can be finished by the engine a moment later), the followers stay
        // collected and PollTeleportFollowers (every tick of every NPC) moves them in the frame the hero has moved.
        void FinishTeleportFollowers(GELPCChar a_strWhy)
        {
            GEFloat const fMoved = DistanceBetween(Entity::GetPlayer().GetPosition(), g_vTeleportHeroBefore);
            if (g_iTeleportFollowers == 0)
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Teleport (%s): nobody is carried (see the records above, one per companion).\n",
                                             a_strWhy);
                if (fMoved >= TELEPORT_HERO_MIN_MOVE)
                    SecureFarCompanions(a_strWhy); // v1.0.68.0: nobody is carried, but a far companion must not run across the map
                return;
            }
            if (fMoved >= TELEPORT_HERO_MIN_MOVE)
            {
                MoveTeleportFollowers(a_strWhy);
                return;
            }
            g_bTeleportPending = GETrue;
            g_u32TeleportPendingTick = ::GetTickCount() | 1;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Teleport (%s): the hero has moved only %.0f so far - the companions wait "
                                         "for his arrival.\n",
                                         a_strWhy, static_cast<double>(fMoved));
        }

        void PollTeleportFollowers()
        {
            if (!g_bTeleportPending)
                return;
            if (static_cast<GEInt>(::GetTickCount() - g_u32TeleportPendingTick) > static_cast<GEInt>(TELEPORT_PENDING_MS))
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Teleport: the hero did not leave the place, the companions stay "
                                             "and wait (the native plaque).\n");
                g_bTeleportPending = GEFalse;
                for (GEInt i = 0; i < g_iTeleportFollowers; ++i)
                {
                    Entity Npc = Entity::GetEntity(g_arrTeleportFollowers[i]);
                    if (Npc != None && Npc.IsNPC() && !Npc.IsDead() && !Npc.IsDown())
                    {
                        RestoreNativeWait(Npc);
                        Host::MarkWaiterAuto(Npc); // v1.0.68.0: the plugin's wait, the next stone carries him
                        Entity TimeoutHero = Entity::GetPlayer();
                        LogTeleportRecord("timeout", Npc, TimeoutHero, "NOT CARRIED", "the hero did not leave the place: he waits (AUTO)");
                    }
                }
                g_iTeleportFollowers = 0;
                return;
            }
            if (DistanceBetween(Entity::GetPlayer().GetPosition(), g_vTeleportHeroBefore) >= TELEPORT_HERO_MIN_MOVE)
                MoveTeleportFollowers("the hero has arrived");
        }

    void SetTeleportScriptRunning(GEBool running) { g_bTeleportScriptRuns = running; }
    GEBool IsTeleportScriptRunning() { return g_bTeleportScriptRuns; }
}

namespace GroupLogic::Brawl
{
    using Host::IsHeroSide;
    using PluginRuntime::HeroState::IsHeroKnockedOut;
    using Host::GetDamageOwnerNpc;
    using GroupLogic::Combat::GetHitDamageType;
    using Host::IsPointMagicSource;
    using Host::StartFleeWatch;
    using Host::HasTask;
    using Host::PollArenaQuestEnd;
        void ClearPersonalSpectators();
        void StartPersonalSpectators(Entity &opponent);
        void FinishPersonalSpectators(Entity &opponent);
        void PollPersonalSpectators();
        void CancelPersonalSpectator(Entity &npc, GEBool restore);
        GEInt const BRAWL_MAX = 32;
        GEU32 const BRAWL_WINDOW_MS = 10000; // the second blow must come within this time, otherwise the counter is reset
        GEU32 const BRAWL_MAX_MS = 90000;    // a fight never lasts longer than this
        bCString g_arrBrawlNames[BRAWL_MAX];
        bCPropertyID g_arrBrawlVictimID[BRAWL_MAX];
        GEU32 g_arrBrawlTargetLostTick[BRAWL_MAX] = {};
        GEInt g_arrBrawlCount[BRAWL_MAX] = {};
        GEU32 g_arrBrawlLastBlow[BRAWL_MAX] = {}; // 0: the slot is free
        GEU32 g_arrBrawlStart[BRAWL_MAX] = {};    // 0: the creature does not fight the hero

        GEBool g_arrBrawlReacted[BRAWL_MAX] = {}; // the reaction of a green NPC to the first blow has been made
        bCPropertyID g_arrBrawlProjectileID[BRAWL_MAX]; // identity of the last counted projectile, not the spell type
        GEBool g_arrBrawlGrudge[BRAWL_MAX] = {};
        GEBool g_arrBrawlWasWaiting[BRAWL_MAX] = {};
        GEU32 g_arrBrawlUpSince[BRAWL_MAX] = {};
        GEBool g_arrBrawlWeaponSeen[BRAWL_MAX] = {};
        GEBool g_arrBrawlSheatheRefused[BRAWL_MAX] = {};
        GEU32 g_arrBrawlWeaponEmptySince[BRAWL_MAX] = {};
        gCSession **g_ppBrawlSession = nullptr;
        GEU32 g_u32BrawlEpoch = 0;
        GEBool g_bBrawlEpochLoaded = GEFalse;
        GEBool g_bPersonalTeleportPending = GEFalse;
        bCVector g_vPersonalTeleportBefore;
        GEU32 g_u32PersonalTeleportTick = 0;

        Entity BrawlMemoryOwner()
        {
            // The save-owning human memory is authoritative even when a mod gives
            // the transformed body a separate PlayerMemory property set.
            if (g_ppBrawlSession && *g_ppBrawlSession && (*g_ppBrawlSession)->IsGameRunning())
            {
                eCEntity *body = (*g_ppBrawlSession)->GetHero();
                if (body)
                {
                    Entity memory(static_cast<eCEntity const *>(body));
                    if (memory.PlayerMemory.IsValid()) return memory;
                }
            }
            Entity body = Entity::GetEntity(bCString("PC_Hero"));
            if (body != None && body.PlayerMemory.IsValid()) return body;
            return Entity::GetPlayer();
        }

        bCString BrawlEventKey(Entity &npc, GELPCChar kind, GEBool withEpoch)
        {
            char key[180] = {};
            bCString const id = npc.GetInstance()->GetID().GetText();
            if (withEpoch)
                _snprintf_s(key, sizeof(key), _TRUNCATE, "GLF_Brawl_%s_%08X_%s", kind, g_u32BrawlEpoch, id.GetText());
            else
                _snprintf_s(key, sizeof(key), _TRUNCATE, "GLF_Brawl_%s_%s", kind, id.GetText());
            return bCString(key);
        }

        void LoadBrawlEpoch()
        {
            if (g_bBrawlEpochLoaded)
                return;
            Entity memory = BrawlMemoryOwner();
            if (memory == None || !memory.PlayerMemory.IsValid())
                return;
            g_u32BrawlEpoch = 0;
            for (GEInt bit = 0; bit < 32; ++bit)
            {
                char key[64] = {};
                _snprintf_s(key, sizeof(key), _TRUNCATE, "GLF_Brawl_TeleportEpoch_%02d", bit);
                if (memory.PlayerMemory.IsSetGameEvent(bCString(key)))
                    g_u32BrawlEpoch |= GEU32(1) << bit;
            }
            g_bBrawlEpochLoaded = GETrue;
        }

        GEBool BrawlEvent(Entity &npc, GELPCChar kind, GEBool withEpoch)
        {
            if (npc == None)
                return GEFalse;
            LoadBrawlEpoch();
            Entity memory = BrawlMemoryOwner();
            return memory != None && memory.PlayerMemory.IsValid() &&
                memory.PlayerMemory.IsSetGameEvent(BrawlEventKey(npc, kind, withEpoch));
        }

        void SetBrawlEvent(Entity &npc, GELPCChar kind, GEBool enabled, GEBool withEpoch)
        {
            if (npc == None)
                return;
            LoadBrawlEpoch();
            Entity memory = BrawlMemoryOwner();
            if (memory == None || !memory.PlayerMemory.IsValid())
                return;
            bCString const key = BrawlEventKey(npc, kind, withEpoch);
            if (enabled)
                memory.PlayerMemory.SetGameEvent(key);
            else
                memory.PlayerMemory.ClearGameEvent(key);
        }

        GEBool BrawlFullHealth(Entity &npc)
        {
            if (npc == None || npc.IsDead() || !npc.DamageReceiver.IsValid())
                return GEFalse;
            GEInt const maximum = npc.DamageReceiver.HitPointsMax;
            return maximum > 0 && static_cast<GEInt>(npc.DamageReceiver.HitPoints) >= maximum;
        }

        GEInt FindBrawlSlot(Entity &npc, GEBool create)
        {
            if (npc == None)
                return -1;
            bCPropertyID const id = npc.GetInstance()->GetID();
            GEU32 const now = ::GetTickCount();
            GEInt available = -1;
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
            {
                if (g_arrBrawlVictimID[i] == id)
                    return i;
                if (available < 0 && g_arrBrawlStart[i] == 0 &&
                    (g_arrBrawlLastBlow[i] == 0 || static_cast<GEInt>(now - g_arrBrawlLastBlow[i]) > static_cast<GEInt>(BRAWL_WINDOW_MS)))
                    available = i; // Grudges are saved per identity, so an inactive cache slot may be reused.
            }
            if (!create || available < 0)
                return -1;
            g_arrBrawlNames[available] = npc.GetName();
            g_arrBrawlVictimID[available] = id;
            g_arrBrawlCount[available] = 0;
            g_arrBrawlStart[available] = 0;
            g_arrBrawlLastBlow[available] = 0;
            g_arrBrawlReacted[available] = GEFalse;
            g_arrBrawlProjectileID[available] = bCPropertyID();
            g_arrBrawlTargetLostTick[available] = 0;
            g_arrBrawlGrudge[available] = BrawlEvent(npc, "Grudge", GETrue);
            g_arrBrawlWasWaiting[available] = npc.Party.Waiting;
            g_arrBrawlUpSince[available] = 0;
            g_arrBrawlWeaponSeen[available] = GEFalse;
            g_arrBrawlSheatheRefused[available] = GEFalse;
            g_arrBrawlWeaponEmptySince[available] = 0;
            return available;
        }

        void ClearBrawls()
        {
            ClearPersonalSpectators();
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
            {
                g_arrBrawlUpSince[i] = 0;
                g_arrBrawlWeaponSeen[i] = GEFalse;
                g_arrBrawlSheatheRefused[i] = GEFalse;
                g_arrBrawlWeaponEmptySince[i] = 0;
                g_arrBrawlNames[i] = bCString();
                g_arrBrawlVictimID[i] = bCPropertyID();
                g_arrBrawlTargetLostTick[i] = 0;
                g_arrBrawlCount[i] = 0;
                g_arrBrawlLastBlow[i] = 0;
                g_arrBrawlStart[i] = 0;
                g_arrBrawlReacted[i] = GEFalse;
                g_arrBrawlProjectileID[i] = bCPropertyID();
                g_arrBrawlGrudge[i] = GEFalse;
                g_arrBrawlWasWaiting[i] = GEFalse;
            }
            g_bBrawlEpochLoaded = GEFalse;
            g_bPersonalTeleportPending = GEFalse;
        }


        // THE UNCONDITIONAL RESET OF THE COMBAT STATE (knock-out of the master, time-out, the end of the fight). Nothing of the
        // fight is left on the creature, so it cannot hang in an unnatural pose: no target (the combat focus), no attacker,
        // no alternative target, no attack reason, no combat state; the routine is stopped and the creature is put back to
        // the peaceful task of following the hero (ContinueRoutine picks the native one, ZS_FollowPlayer is set by hand when
        // the routine has not left the combat / stumble task). A waiting creature keeps waiting.
        void ResetCombatState(Entity &a_Npc)
        {
            a_Npc.Routine.FullStop();
            a_Npc.NPC.SetCurrentTarget(None);
            a_Npc.NPC.SetCurrentAttacker(None);
            a_Npc.NPC.SetLastAttacker(None);
            a_Npc.NPC.SetAlternativeTarget(None);
            a_Npc.NPC.AttackReason = gEAttackReason_None;
            a_Npc.NPC.CombatState = 0;
            a_Npc.Routine.ContinueRoutine();
            // Forced back to the peaceful task of following (a creature that was told to wait keeps waiting).
            if (!a_Npc.Party.Waiting)
                a_Npc.Routine.SetTask("ZS_FollowPlayer");
        }

        GEBool IsBrawlFighting(Entity &npc);
        GEBool IsCompanionDownOrRising(Entity &npc)
        {
            if (npc.IsDown() || npc.Routine.AIMode == gEAIMode_Down) return GETrue;
            bCString const task = npc.Routine.GetCurrentTask(), state = npc.Routine.GetCurrentState();
            for (GELPCChar prefix : {"ZS_Unconscious", "ZS_LieKnockDown", "ZS_PiercedKO", "ZS_SitKnockDown", "ZS_StandUp", "ZS_Fall"})
                if (_strnicmp(task.GetText(), prefix, strlen(prefix)) == 0 || _strnicmp(state.GetText(), prefix, strlen(prefix)) == 0)
                    return GETrue;
            gEAniState const ani = npc.Routine.AniState;
            if (ani == gEAniState_Dummy0 || ani == gEAniState_Dummy1 || ani == gEAniState_SitKnockDown ||
                ani == gEAniState_LieKnockDown || ani == gEAniState_LieKnockOut || ani == gEAniState_LiePiercedKO)
                return GETrue;
            gEAction const action = npc.Routine.Action;
            return action == gEAction_GetUpAttack || action == gEAction_GetUpParade || action == gEAction_LieKnockDown ||
                action == gEAction_LieKnockOut || action == gEAction_LiePiercedKO || action == gEAction_SitKnockDown;
        }

        

        GEBool IsRecoveringPersonalCompanion(Entity &npc)
        {
            return npc != None && npc.IsNPC() && !npc.IsDead() && IsHeroControlled(npc) &&
                BrawlEvent(npc, "RecoveryPending") && IsCompanionDownOrRising(npc);
        }

        thread_local GEInt g_iDialogueMemberQuery = 0;

DialogueMemberQuery::DialogueMemberQuery() { ++g_iDialogueMemberQuery; }
DialogueMemberQuery::~DialogueMemberQuery() { --g_iDialogueMemberQuery; }


        void FilterDialogueMembers(bTValArray<eCEntity *> &members)
        {
            for (GEInt i = members.GetCount() - 1; i >= 0; --i)
            {
                Entity npc(members.GetAt(i));
                if (!IsRecoveringPersonalCompanion(npc)) continue;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][dialogue] '%s': excluded from this dialogue's member-list copy; native knockout left running.\n",
                    npc.GetName().GetText());
                members.RemoveAt(i);
            }
        }

        mCFunctionHook Hook_DialoguePartyMembers;
        bTValArray<eCEntity *> *GE_STDCALL DialoguePartyMembers(PSParty *party, bTValArray<eCEntity *> *result, GEBool includeSelf)
        {
            using Original = bTValArray<eCEntity *> *(GE_STDCALL *)(PSParty *, bTValArray<eCEntity *> *, GEBool);
            auto *returned = Hook_DialoguePartyMembers.GetOriginalFunction<Original>()(party, result, includeSelf);
            if (result) GroupLogic::Lifecycle::AdjustSummonPartyMembers(party, *result);
            // GetMembers returns a value copy. Never remove anybody from PSParty itself.
            if (g_iDialogueMemberQuery > 0 && result) FilterDialogueMembers(*result);
            return returned;
        }

        

        

        void UpdatePersonalRecovery(Entity &npc)
        {
            if (!BrawlEvent(npc, "RecoveryPending")) return;
            if (npc.IsDead() || !IsHeroControlled(npc))
            {
                SetBrawlEvent(npc, "RecoveryPending", GEFalse);
                return;
            }
            if (IsBrawlFighting(npc)) return;
            GEInt const slot = FindBrawlSlot(npc, GETrue);
            if (IsCompanionDownOrRising(npc))
            {
                SetBrawlEvent(npc, "RecoveryPending", GETrue);
                if (slot >= 0) g_arrBrawlUpSince[slot] = 0;
                return;
            }
            if (BrawlEvent(npc, "RecoveryPending"))
            {
                if (slot < 0) return;
                GEU32 const now = ::GetTickCount();
                if (!g_arrBrawlUpSince[slot]) g_arrBrawlUpSince[slot] = now | 1;
                if (static_cast<GEInt>(now - g_arrBrawlUpSince[slot]) < 600) return;
            }
            SetBrawlEvent(npc, "RecoveryPending", GEFalse);
            if (slot >= 0) g_arrBrawlUpSince[slot] = 0;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': recovery finished; normal dialogue eligibility restored.\n", npc.GetName().GetText());
        }

        // The fight of this creature with the hero is over: the slot is closed and the combat state is reset.
        void EndBrawlOf(Entity &npc, GELPCChar why)
        {
            GEInt const slot = FindBrawlSlot(npc, GEFalse);
            GEBool const fighting = slot >= 0 && g_arrBrawlStart[slot] != 0;
            SetBrawlEvent(npc, "ExitApproval", GEFalse);
            if (fighting)
                FinishPersonalSpectators(npc);
            if (slot >= 0)
            {
                g_arrBrawlStart[slot] = 0;
                g_arrBrawlTargetLostTick[slot] = 0;
                g_arrBrawlCount[slot] = 0;
                g_arrBrawlLastBlow[slot] = 0;
                g_arrBrawlReacted[slot] = GEFalse;
                if (fighting)
                {
                    g_arrBrawlGrudge[slot] = !npc.IsDead();
                    SetBrawlEvent(npc, "Active", GEFalse, GETrue);
                    SetBrawlEvent(npc, "Grudge", !npc.IsDead(), GETrue);
                    if (npc.IsDead() || npc.IsDown())
                        SetBrawlEvent(npc, "NoReward", GETrue);
                }
                npc.Party.Waiting = g_arrBrawlWasWaiting[slot];
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': %s -> %s (%s)\n", npc.GetName().GetText(),
                fighting ? "FIGHT" : "WARNING", fighting && !npc.IsDead() ? "GRUDGE" : "NORMAL", why);
            if (!npc.IsDead() && !IsCompanionDownOrRising(npc))
            {
                ResetCombatState(npc);
                if (fighting && IsHeroKnockedOut())
                {
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': victory over master; silent native stand/holster exit scheduled.\n", npc.GetName().GetText());
                    npc.Routine.SetTask("GLF_PersonalBrawlExit");
                }
                else if (npc.Party.Waiting)
                    npc.Routine.SetTask("ZS_WaitForPlayer");
            }
        }

        GEBool IsBrawlFighting(Entity &a_Npc)
        {
            GEInt const iSlot = FindBrawlSlot(a_Npc, GEFalse);
            return iSlot >= 0 && g_arrBrawlStart[iSlot] != 0;
        }

        // Read-only diagnostics. Store IDs, never entity pointers across frames or loads.
        struct RecoveryDiagnostic
        {
            bCPropertyID id;
            GEBool used = GEFalse;
            GEU32 start = 0, last = 0;
            bCString snapshot;
        };
        RecoveryDiagnostic g_RecoveryDiagnostics[32];
        GEInt g_iAimDiagnosticLines = 0;
        thread_local GEBool g_bBrawlDiagnosticBusy = GEFalse;
        struct BrawlDiagnosticScope
        {
            BrawlDiagnosticScope() { g_bBrawlDiagnosticBusy = GETrue; }
            ~BrawlDiagnosticScope() { g_bBrawlDiagnosticBusy = GEFalse; }
        };

        GEBool CanReadBrawlDiagnostics()
        {
            return g_ppBrawlSession && *g_ppBrawlSession && (*g_ppBrawlSession)->IsGameRunning();
        }

        void ResetBrawlDiagnostics()
        {
            for (auto &record : g_RecoveryDiagnostics) record = RecoveryDiagnostic();
            g_iAimDiagnosticLines = 0;
        }

        void LogAimEntity(GELPCChar label, Entity &npc)
        {
            if (npc == None || !npc.IsNPC()) return;
            GEInt const slot = FindBrawlSlot(npc, GEFalse);
            Entity const target = npc.NPC.GetCurrentTarget();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][aim] %s='%s' id=%s party=%d controlled=%d waiting=%d dead=%d down=%d HP=%d/%d task='%s' state='%s' mode=%d reason=%d combat=%u target='%s' defeatedByPlayer=%d brawlSlot=%d count=%d active=%d grudge=%d.\n",
                label, npc.GetName().GetText(), npc.GetInstance()->GetID().GetText().GetText(),
                npc.Party.IsValid() ? static_cast<int>(npc.Party.PartyMemberType) : -1,
                static_cast<int>(IsHeroControlled(npc)), npc.Party.IsValid() ? static_cast<int>(npc.Party.Waiting) : -1,
                static_cast<int>(npc.IsDead()), static_cast<int>(npc.IsDown()),
                npc.DamageReceiver.IsValid() ? static_cast<int>(npc.DamageReceiver.HitPoints) : -1,
                npc.DamageReceiver.IsValid() ? static_cast<int>(npc.DamageReceiver.HitPointsMax) : -1,
                npc.Routine.GetCurrentTask().GetText(), npc.Routine.GetCurrentState().GetText(),
                static_cast<int>(npc.Routine.AIMode), static_cast<int>(npc.NPC.AttackReason),
                static_cast<unsigned>(npc.NPC.CombatState), target != None ? target.GetName().GetText() : "-",
                static_cast<int>(npc.NPC.DefeatedByPlayer), slot, slot >= 0 ? g_arrBrawlCount[slot] : 0,
                static_cast<int>(slot >= 0 && g_arrBrawlStart[slot] != 0),
                static_cast<int>(slot >= 0 && g_arrBrawlGrudge[slot]));
        }

        void LogMagicAim(GELPCChar where)
        {
            if (g_bBrawlDiagnosticBusy || !CanReadBrawlDiagnostics() || g_iAimDiagnosticLines >= 600) return;
            BrawlDiagnosticScope guard;
            ++g_iAimDiagnosticLines;
            Entity hero = Entity::GetPlayer();
            if (hero == None) return;
            // Focus getters require a fully initialized aiming context. A teleport selected
            // in inventory can enter PS_Magic before that context exists; do not query it here.
            Entity spellTarget = hero.NPC.IsValid() ? hero.NPC.GetSpellTarget() : Entity();
            Entity current = hero.NPC.IsValid() ? hero.NPC.GetCurrentTarget() : Entity();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][aim] tick=%u event='%s' lastSpell='%s' heroTask='%s' heroState='%s' spellTarget='%s' currentTarget='%s' focusRead=disabled.\n",
                static_cast<unsigned>(::GetTickCount()), where,
                hero.NPC.IsValid() ? bCString(hero.NPC.LastSpell).GetText() : "-",
                hero.Routine.GetCurrentTask().GetText(), hero.Routine.GetCurrentState().GetText(),
                spellTarget != None ? spellTarget.GetName().GetText() : "-", current != None ? current.GetName().GetText() : "-");
            LogAimEntity("spellTarget", spellTarget);
            if (current != spellTarget) LogAimEntity("currentTarget", current);
            bTObjArray<Entity> npcs = Entity::GetNPCs();
            GEInt logged = 0;
            for (GEInt i = 0; i < npcs.GetCount() && logged < 16; ++i)
            {
                Entity npc = npcs.GetAt(i);
                if (npc == None || !npc.IsNPC() || !IsHeroControlled(npc) || npc.GetDistanceTo(hero) > 3000.0f) continue;
                LogAimEntity("nearbyParty", npc);
                ++logged;
            }
        }

        void ObserveRecovery(Entity &npc, GELPCChar where, GEBool begin, GEBool force)
        {
            if (g_bBrawlDiagnosticBusy || !CanReadBrawlDiagnostics() || npc == None || !npc.IsNPC()) return;
            BrawlDiagnosticScope guard;
            GEU32 const now = ::GetTickCount();
            RecoveryDiagnostic *found = nullptr;
            RecoveryDiagnostic *free = nullptr;
            for (auto &record : g_RecoveryDiagnostics)
            {
                if (record.used && static_cast<GEInt>(now - record.start) > 60000) record = RecoveryDiagnostic();
                if (record.used && record.id == npc.GetInstance()->GetID()) found = &record;
                if (!record.used && !free) free = &record;
            }
            if (begin)
            {
                if (!found) found = free;
                if (!found) return;
                *found = RecoveryDiagnostic();
                found->used = GETrue;
                found->id = npc.GetInstance()->GetID();
                found->start = now;
            }
            if (!found || (!begin && !force && found->last && static_cast<GEInt>(now - found->last) < 250)) return;
            found->last = now;
            Entity const target = npc.NPC.GetCurrentTarget();
            char snapshot[1200] = {};
            _snprintf_s(snapshot, sizeof(snapshot), _TRUNCATE,
                "HP=%d/%d down=%d dead=%d task='%s' state='%s' mode=%d reason=%d combat=%u target='%s' waiting=%d defeatedByPlayer=%d action=%d aniState=%d",
                npc.DamageReceiver.IsValid() ? static_cast<int>(npc.DamageReceiver.HitPoints) : -1,
                npc.DamageReceiver.IsValid() ? static_cast<int>(npc.DamageReceiver.HitPointsMax) : -1,
                static_cast<int>(npc.IsDown()), static_cast<int>(npc.IsDead()), npc.Routine.GetCurrentTask().GetText(),
                npc.Routine.GetCurrentState().GetText(), static_cast<int>(npc.Routine.AIMode),
                static_cast<int>(npc.NPC.AttackReason), static_cast<unsigned>(npc.NPC.CombatState),
                target != None ? target.GetName().GetText() : "-", static_cast<int>(npc.Party.Waiting),
                static_cast<int>(npc.NPC.DefeatedByPlayer), static_cast<int>(npc.Routine.Action), static_cast<int>(npc.Routine.AniState));
            if (!force && found->snapshot == bCString(snapshot)) return;
            found->snapshot = bCString(snapshot);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][recovery] tick=%u +%u ms '%s' id=%s event='%s' %s taskTime=%.3f stateTime=%.3f.\n",
                static_cast<unsigned>(now), static_cast<unsigned>(now - found->start), npc.GetName().GetText(),
                found->id.GetText().GetText(), where, snapshot,
                static_cast<double>(npc.Routine.GetTaskTime()), static_cast<double>(npc.Routine.GetStateTime()));
        }

        void LogDialogueRecovery(GELPCChar where, eCEntity *first, eCEntity *second)
        {
            if (g_bBrawlDiagnosticBusy || !CanReadBrawlDiagnostics()) return;
            Entity a(first), b(second);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][dialogue] tick=%u %s first='%s' second='%s'.\n",
                static_cast<unsigned>(::GetTickCount()), where, a != None ? a.GetName().GetText() : "-", b != None ? b.GetName().GetText() : "-");
            for (auto const &record : g_RecoveryDiagnostics)
                if (record.used)
                {
                    Entity npc(record.id);
                    ObserveRecovery(npc, where, GEFalse, GETrue);
                }
        }

        mCFunctionHook Hook_DiagnosticStartDialogue;
        GEBool GE_STDCALL DiagnosticStartDialogue(gCInfoManager_PS *manager, eCEntity *first, eCEntity *second)
        {
            using Original = GEBool(GE_STDCALL *)(gCInfoManager_PS *, eCEntity *, eCEntity *);
            LogDialogueRecovery("StartInfoManager before native", first, second);
            GEBool const result = Hook_DiagnosticStartDialogue.GetOriginalFunction<Original>()(manager, first, second);
            LogDialogueRecovery(result ? "StartInfoManager after native: started" : "StartInfoManager after native: rejected", first, second);
            return result;
        }

        mCFunctionHook Hook_DiagnosticProjectileShoot;
        void GE_STDCALL DiagnosticProjectileShoot(PSProjectile *projectile)
        {
            using Original = void(GE_STDCALL *)(PSProjectile *);
            if (!g_bBrawlDiagnosticBusy && CanReadBrawlDiagnostics() && projectile && projectile->IsValid() && g_iAimDiagnosticLines < 600)
            {
                auto *ps = static_cast<gCProjectile_PS *>(projectile->m_pEngineEntityPropertySet);
                Entity item(ps->GetEntity());
                Entity hero = Entity::GetPlayer();
                if (item != None && hero != None && item.GetDistanceTo(hero) <= 5000.0f)
                {
                    LogMagicAim("projectile Shoot (before native launch)");
                    BrawlDiagnosticScope guard;
                    Entity target = projectile->GetTarget();
                    Entity owner = GetDamageOwnerNpc(item);
                    Entity spell = item.Interaction.IsValid() ? item.Interaction.GetSpell() : Entity();
                    bCVector const direction = projectile->TargetDirection;
                    bCVector const position = projectile->TargetPosition;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][projectile] tick=%u item='%s' id=%s owner='%s' spellObject='%s' target='%s' targetId=%s path=%d direction=(%.3f,%.3f,%.3f) targetPos=(%.1f,%.1f,%.1f).\n",
                        static_cast<unsigned>(::GetTickCount()), item.GetName().GetText(), item.GetInstance()->GetID().GetText().GetText(),
                        owner != None ? owner.GetName().GetText() : "-", spell != None ? spell.GetName().GetText() : "-",
                        target != None ? target.GetName().GetText() : "-",
                        target != None ? target.GetInstance()->GetID().GetText().GetText() : "-", static_cast<int>(projectile->PathStyle),
                        static_cast<double>(direction.m_fX), static_cast<double>(direction.m_fY), static_cast<double>(direction.m_fZ),
                        static_cast<double>(position.m_fX), static_cast<double>(position.m_fY), static_cast<double>(position.m_fZ));
                }
            }
            Hook_DiagnosticProjectileShoot.GetOriginalFunction<Original>()(projectile);
        }

        void ReconcileBrawl(Entity &npc, GELPCChar why)
        {
            GEInt const slot = FindBrawlSlot(npc, GEFalse);
            if (slot >= 0 && g_arrBrawlStart[slot] != 0)
                return;
            if (!BrawlEvent(npc, "Grudge", GETrue) && (slot < 0 || !g_arrBrawlGrudge[slot]))
                return;
            SetBrawlEvent(npc, "Grudge", GEFalse, GETrue);
            SetBrawlEvent(npc, "SheatheAccepted", GEFalse, GETrue);
            if (slot >= 0)
            {
                g_arrBrawlGrudge[slot] = GEFalse;
                g_arrBrawlCount[slot] = 0;
                g_arrBrawlLastBlow[slot] = 0;
                g_arrBrawlProjectileID[slot] = bCPropertyID();
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': GRUDGE -> NORMAL (%s)\n", npc.GetName().GetText(), why);
        }

        

        GEBool HeroHasDrawnWeapon()
        {
            Entity hero = Entity::GetPlayer();
            if (hero == None) return GEFalse;
            // Verified Game.dll GetCombatMode: 1 = no held weapon, 2..4 = held combat item.
            // It reads the right-hand inventory item, not a remembered PS_* task name.
            GEInt const mode = static_cast<GEInt>(gCEntity::GetCombatMode(hero.GetInstance()));
            return mode >= 2 && mode <= 4;
        }

        void PollPersonalSheathe()
        {
            if (g_bPersonalTeleportPending || IsHeroKnockedOut() || IsHeroTransformedNow()) return;
            GEBool const drawn = HeroHasDrawnWeapon();
            GEU32 const now = ::GetTickCount();
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
            {
                if (!g_arrBrawlStart[i]) continue;
                if (drawn)
                {
                    g_arrBrawlWeaponSeen[i] = GETrue;
                    g_arrBrawlWeaponEmptySince[i] = 0;
                    continue;
                }
                if (!g_arrBrawlWeaponSeen[i]) continue;
                if (!g_arrBrawlWeaponEmptySince[i]) g_arrBrawlWeaponEmptySince[i] = now | 1;
                // Ignore a transient empty hand while native code switches/equips a spell.
                if (static_cast<GEInt>(now - g_arrBrawlWeaponEmptySince[i]) < 400) continue;
                Entity npc(g_arrBrawlVictimID[i]);
                if (npc == None || npc.IsDead() || IsCompanionDownOrRising(npc)) continue;
                // Consume this drawn-to-empty transition, including a refused surrender.
                // Drawing again rearms it; a continuously empty hand never repeats speech.
                g_arrBrawlWeaponSeen[i] = GEFalse;
                g_arrBrawlWeaponEmptySince[i] = 0;
                GEBool const acceptedBefore = BrawlEvent(npc, "SheatheAccepted", GETrue);
                if (acceptedBefore)
                {
                    g_arrBrawlSheatheRefused[i] = GETrue;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': repeated weapon put-away refused; fight continues.\n", npc.GetName().GetText());
                    continue;
                }
                SetBrawlEvent(npc, "SheatheAccepted", GETrue, GETrue);
                EndBrawlOf(npc, "confirmed weapon put-away during this fight");
                // HoldStacks changes equipment without the native weapon-away animation.
                // Run the same resumable stand/holster steps as ZS_Attack_End instead.
                SetBrawlEvent(npc, "ExitApproval", GETrue);
                npc.Routine.SetTask("GLF_PersonalBrawlExit");
                if (BrawlFullHealth(npc)) ReconcileBrawl(npc, "already at full health when combat ended");
            }
        }

        void BeginPersonalTeleport()
        {
            Entity player = Entity::GetPlayer();
            if (player == None)
                return;
            bTObjArray<Entity> companions = Entity::GetNPCs();
            for (GEInt i = 0; i < companions.GetCount(); ++i)
            { Entity npc = companions.GetAt(i); CancelPersonalSpectator(npc, GETrue); }
            g_vPersonalTeleportBefore = player.GetPosition();
            g_u32PersonalTeleportTick = ::GetTickCount() | 1;
            g_bPersonalTeleportPending = GETrue;
        }

        void PollPersonalTeleport()
        {
            if (!g_bPersonalTeleportPending)
                return;
            Entity player = Entity::GetPlayer();
            if (player == None)
                return;
            bCVector const pos = player.GetPosition();
            GEFloat const dx = pos.m_fX - g_vPersonalTeleportBefore.m_fX;
            GEFloat const dy = pos.m_fY - g_vPersonalTeleportBefore.m_fY;
            GEFloat const dz = pos.m_fZ - g_vPersonalTeleportBefore.m_fZ;
            if (dx*dx + dy*dy + dz*dz < 1000.0f*1000.0f)
            {
                if (static_cast<GEInt>(::GetTickCount() - g_u32PersonalTeleportTick) > 10000)
                    g_bPersonalTeleportPending = GEFalse; // A failed cast does not forgive anything.
                return;
            }
            g_bPersonalTeleportPending = GEFalse;
            ArenaJudge::Loops::OnHeroTeleported();
            bTObjArray<Entity> npcs = Entity::GetNPCs();
            for (GEInt i = 0; i < npcs.GetCount(); ++i)
            {
                Entity npc = npcs.GetAt(i);
                if (IsBrawlFighting(npc))
                    EndBrawlOf(npc, "the player teleported");
            }
            LoadBrawlEpoch();
            ++g_u32BrawlEpoch; // Invalidates grudges even for companions not currently loaded.
            Entity memory = BrawlMemoryOwner();
            if (memory != None && memory.PlayerMemory.IsValid())
                for (GEInt bit = 0; bit < 32; ++bit)
                {
                    char key[64] = {};
                    _snprintf_s(key, sizeof(key), _TRUNCATE, "GLF_Brawl_TeleportEpoch_%02d", bit);
                    if (g_u32BrawlEpoch & (GEU32(1) << bit)) memory.PlayerMemory.SetGameEvent(bCString(key));
                    else memory.PlayerMemory.ClearGameEvent(bCString(key));
                }
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
                if (g_arrBrawlStart[i] == 0)
                {
                    g_arrBrawlGrudge[i] = GEFalse;
                    g_arrBrawlCount[i] = 0;
                    g_arrBrawlLastBlow[i] = 0;
                    g_arrBrawlProjectileID[i] = bCPropertyID();
                }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] Teleport confirmed: all ended grudges cleared, epoch=%u.\n", g_u32BrawlEpoch);
        }

        void RestoreSavedBrawlState(Entity &npc)
        {
            UpdatePersonalRecovery(npc);
            if (IsBrawlFighting(npc))
                return;
            if (BrawlEvent(npc, "Active", GETrue))
            {
                GEInt const slot = FindBrawlSlot(npc, GETrue);
                SetBrawlEvent(npc, "Active", GEFalse, GETrue);
                SetBrawlEvent(npc, "Grudge", GETrue, GETrue);
                if (slot >= 0) g_arrBrawlGrudge[slot] = GETrue;
                npc.Party.Waiting = BrawlEvent(npc, "WasWaiting");
                if (slot >= 0) g_arrBrawlWasWaiting[slot] = npc.Party.Waiting;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': saved active conflict restored as GRUDGE.\n", npc.GetName().GetText());
                if (!IsCompanionDownOrRising(npc)) ResetCombatState(npc);
            }
            if (BrawlFullHealth(npc))
            {
                ReconcileBrawl(npc, "100 percent health outside combat, including after loading");
                if (!npc.IsDown() && BrawlEvent(npc, "NoReward"))
                    SetBrawlEvent(npc, "NoReward", GEFalse);
            }
        }

        GEInt const PERSONAL_SPECTATOR_MAX = 32;
        struct PersonalSpectator
        {
            bCPropertyID id, opponent;
            GEBool used, applauding = GEFalse, waiting = GEFalse, overlay = GEFalse;
            bCString task;
            gEAIMode mode = gEAIMode_PlayerParty;
        };
        PersonalSpectator g_PersonalSpectators[PERSONAL_SPECTATOR_MAX];
        GELPCChar const PERSONAL_SPECTATOR_TASK = "GLF_PersonalBrawlSpectator";

        GEInt FindPersonalSpectator(Entity &npc)
        {
            if (npc == None) return -1;
            for (GEInt i = 0; i < PERSONAL_SPECTATOR_MAX; ++i)
                if (g_PersonalSpectators[i].used && g_PersonalSpectators[i].id == npc.GetInstance()->GetID())
                    return i;
            return -1;
        }
        void ClearPersonalSpectators()
        {
            for (GEInt i = 0; i < PERSONAL_SPECTATOR_MAX; ++i)
                g_PersonalSpectators[i] = PersonalSpectator();
        }
        GEBool OwnsPersonalSpectatorTask(Entity &npc)
        {
            return npc != None && strcmp(npc.Routine.GetCurrentTask().GetText(), PERSONAL_SPECTATOR_TASK) == 0;
        }
        GEBool IsPersonalPickupTask(Entity &npc)
        {
            bCString const task = npc.Routine.GetCurrentTask();
            bCString const state = npc.Routine.GetCurrentState();
            return strstr(task.GetText(), "FetchWeapon") || strstr(state.GetText(), "FetchWeapon") ||
                strstr(task.GetText(), "RansackWeapon") || strstr(state.GetText(), "RansackWeapon");
        }
        void CancelPersonalSpectator(Entity &npc, GEBool restore)
        {
            GEInt const i = FindPersonalSpectator(npc);
            if (i < 0) return;
            PersonalSpectator const saved = g_PersonalSpectators[i];
            g_PersonalSpectators[i] = PersonalSpectator(); // Release before native calls can re-enter.
            GEBool const own = OwnsPersonalSpectatorTask(npc);
            if (saved.overlay) npc.StopPlayOverlayAni();
            if (own)
            {
                npc.StopPlayAni();
                if (restore && !npc.IsDead() && !npc.IsDown())
                {
                    npc.Routine.FullStop();
                    npc.NPC.SetCurrentTarget(None);
                    npc.NPC.SetAlternativeTarget(None);
                    npc.Party.Waiting = saved.waiting;
                    npc.Routine.AIMode = saved.mode;
                    npc.Routine.SetTask(saved.waiting ? "ZS_WaitForPlayer" : "ZS_FollowPlayer");
                }
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][spectator] '%s': released%s.\n", npc.GetName().GetText(), restore ? " to previous follow/wait" : " for a native task / command");
        }
        GEBool CanStartPersonalSpectator(Entity &npc, Entity &opponent)
        {
            if (npc == None || npc == opponent || IsHeroSide(npc) || !npc.IsNPC() || npc.IsDead() || npc.IsDown() ||
                !IsHeroControlled(npc) || IsBrawlFighting(npc) || !AI_Decision::IsHumanoid(npc) || !npc.Animation.IsValid())
                return GEFalse;
            // Do not interrupt weapon recovery, dialogue, work, or any other native task.
            bCString const task = npc.Routine.GetCurrentTask();
            if (task != bCString("ZS_FollowPlayer") && task != bCString("ZS_WaitForPlayer")) return GEFalse;
            if (npc.NPC.AttackReason != gEAttackReason_None || npc.NPC.CombatState != 0 ||
                    npc.Routine.AIMode == gEAIMode_Combat || npc.Routine.AIMode == gEAIMode_Flee || npc.Routine.AIMode == gEAIMode_Talk || IsPersonalPickupTask(npc))
                return GEFalse;
            Entity hero = Entity::GetPlayer();
            if (hero == None || npc.GetDistanceTo(hero) > 2500.0f || !npc.IsFreeLOS(hero, GEFalse)) return GEFalse;
            bCString const watch = npc.GetAni(gEAction_WatchFight, gEPhase_Loop);
            bCString const clap = npc.GetOverlayAni(gEAction_Applaud, gEPhase_Loop);
            return !watch.IsEmpty() && !clap.IsEmpty();
        }
        void StartPersonalSpectators(Entity &opponent)
        {
            bTObjArray<Entity> npcs = Entity::GetNPCs();
            for (GEInt n = 0; n < npcs.GetCount(); ++n)
            {
                Entity npc = npcs.GetAt(n);
                GEInt existing = FindPersonalSpectator(npc);
                if (existing >= 0)
                {
                    if (!g_PersonalSpectators[existing].applauding) continue;
                    CancelPersonalSpectator(npc, GETrue);
                }
                if (!CanStartPersonalSpectator(npc, opponent)) continue;
                GEInt slot = -1;
                for (GEInt i = 0; i < PERSONAL_SPECTATOR_MAX; ++i)
                    if (!g_PersonalSpectators[i].used) { slot = i; break; }
                if (slot < 0) break;
                PersonalSpectator &record = g_PersonalSpectators[slot];
                record.id = npc.GetInstance()->GetID();
                record.opponent = opponent.GetInstance()->GetID();
                record.waiting = npc.Party.Waiting;
                record.task = npc.Routine.GetCurrentTask();
                record.mode = npc.Routine.AIMode;
                record.used = GETrue;
                npc.Routine.FullStop();
                npc.NPC.SetCurrentTarget(Entity::GetPlayer());
                npc.NPC.SetAlternativeTarget(opponent);
                npc.Routine.SetTask(PERSONAL_SPECTATOR_TASK);
                npc.Routine.AIMode = gEAIMode_Reaction;
                npc.SetAlignmentTarget(Entity::GetPlayer());
                bCString const watch = npc.GetAni(gEAction_WatchFight, gEPhase_Loop);
                npc.StartPlayAni(watch, 0, GETrue, -1.0f, GETrue);
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][spectator] '%s': watching personal fight with '%s'.\n", npc.GetName().GetText(), opponent.GetName().GetText());
            }
        }
        void FinishPersonalSpectators(Entity &opponent)
        {
            bTObjArray<Entity> npcs = Entity::GetNPCs();
            for (GEInt n = 0; n < npcs.GetCount(); ++n)
            {
                Entity npc = npcs.GetAt(n);
                GEInt const slot = FindPersonalSpectator(npc);
                if (slot < 0 || !(g_PersonalSpectators[slot].opponent == opponent.GetInstance()->GetID())) continue;
                if (!OwnsPersonalSpectatorTask(npc) || npc.IsDead() || npc.IsDown() ||
                    npc.NPC.AttackReason != gEAttackReason_None || npc.NPC.CombatState != 0 ||
                    npc.Routine.AIMode == gEAIMode_Combat || npc.Routine.AIMode == gEAIMode_Flee || npc.Routine.AIMode == gEAIMode_Talk || IsPersonalPickupTask(npc))
                { CancelPersonalSpectator(npc, GEFalse); continue; }
                bCString const clap = npc.GetOverlayAni(gEAction_Applaud, gEPhase_Loop);
                if (clap.IsEmpty()) { CancelPersonalSpectator(npc, GETrue); continue; }
                PersonalSpectator &record = g_PersonalSpectators[slot];
                record.applauding = GETrue;
                record.overlay = GETrue;
                npc.StopPlayAni();
                // Script_Game.dll ZS_Applaud: four repetitions at a random native speed of 0.9..1.1.
                GEFloat const speed = 0.9f + (static_cast<GEFloat>(std::rand()) / RAND_MAX) * 0.2f;
                npc.StartPlayOverlayAni(clap, 4, GEFalse, speed);
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][spectator] '%s': native applause, speed %.3f; no time limit.\n", npc.GetName().GetText(), static_cast<double>(speed));
            }
        }
        void PollPersonalSpectators()
        {
            static GEU32 last = 0;
            GEU32 const now = ::GetTickCount();
            if (last && static_cast<GEInt>(now - last) < 100) return;
            last = now;
            GEBool any = GEFalse;
            for (GEInt i = 0; i < PERSONAL_SPECTATOR_MAX; ++i) if (g_PersonalSpectators[i].used) any = GETrue;
            if (!any) return;
            GEBool seen[PERSONAL_SPECTATOR_MAX] = {};
            bTObjArray<Entity> npcs = Entity::GetNPCs();
            for (GEInt n = 0; n < npcs.GetCount(); ++n)
            {
                Entity npc = npcs.GetAt(n);
                GEInt const slot = FindPersonalSpectator(npc);
                if (slot < 0) continue;
                seen[slot] = GETrue;
                if (!OwnsPersonalSpectatorTask(npc) || npc.IsDead() || npc.IsDown() || !IsHeroControlled(npc) ||
                    IsBrawlFighting(npc) || npc.NPC.AttackReason != gEAttackReason_None || npc.NPC.CombatState != 0 ||
                    npc.Routine.AIMode == gEAIMode_Combat || npc.Routine.AIMode == gEAIMode_Flee || npc.Routine.AIMode == gEAIMode_Talk || IsPersonalPickupTask(npc))
                { CancelPersonalSpectator(npc, GEFalse); continue; }
                Entity hero = Entity::GetPlayer();
                if (hero == None || npc.GetDistanceTo(hero) > 3000.0f)
                { CancelPersonalSpectator(npc, GETrue); continue; }
                PersonalSpectator &record = g_PersonalSpectators[slot];
                if (!record.applauding)
                {
                    Entity opponent(record.opponent);
                    if (opponent == None) { CancelPersonalSpectator(npc, GETrue); continue; }
                    if (!IsBrawlFighting(opponent)) { FinishPersonalSpectators(opponent); continue; }
                }
                if (record.applauding)
                {
                    // StartPlayOverlayAni uses motion slot 4. Let the native motion finish, including its fade-out.
                    eCVisualAnimation_PS const *animation = npc.Animation.IsValid()
                        ? static_cast<eCVisualAnimation_PS const *>(npc.Animation.m_pEngineEntityPropertySet) : nullptr;
                    eCWrapper_emfx2Actor const *actor = animation ? animation->GetActor() : nullptr;
                    if (!actor || !actor->IsActorPlaying(static_cast<eCWrapper_emfx2Actor::eEMotionType>(4)))
                    { CancelPersonalSpectator(npc, GETrue); continue; }
                }
                npc.SetAlignmentTarget(hero);
            }
            for (GEInt i = 0; i < PERSONAL_SPECTATOR_MAX; ++i)
                if (g_PersonalSpectators[i].used && !seen[i]) g_PersonalSpectators[i] = PersonalSpectator();
        }

        GEBool IsPersonalBrawlPair(Entity &first, Entity &second)
        {
            if (first == None || second == None)
                return GEFalse;
            return (IsHeroSide(first) && IsBrawlFighting(second)) ||
                   (IsHeroSide(second) && IsBrawlFighting(first));
        }

        GEBool CanDamageBrawlOpponent(Entity &attacker, Entity &victim)
        {
            return IsPersonalBrawlPair(attacker, victim) && !IsHeroKnockedOut() &&
                !attacker.IsDead() && !victim.IsDead() &&
                !(attacker.IsNPC() && attacker.IsDown()) && !(victim.IsNPC() && victim.IsDown());
        }

        GEBool IsFatalPersonalProjectile(Entity &source, Entity &victim)
        {
            return GetHitDamageType(source, victim) == gEDamageType_Missile ||
                (source != None && !source.IsNPC() && IsPointMagicSource(&source));
        }

        GEBool PlayerSupportsPersonalKnockout(Entity &player)
        {
            if (!IsHeroSide(player))
                return GEFalse;
            // These are the same humanoid/non-humanoid distinctions used by
            // native IsDeadlyDamage. A monster form is never given a fake KO.
            return !IsHeroTransformedNow() || (player.IsNPC() && AI_Decision::IsHumanoid(player));
        }

        mCFunctionHook Hook_BrawlSetHitPoints;
        void GE_STDCALL BrawlSetHitPoints(gCDamageReceiver_PS *self, GEI32 const *value)
        {
            using Original = void(GE_STDCALL *)(gCDamageReceiver_PS *, GEI32 const *);
            GEI32 const previous = self ? self->GetHitPoints() : 0;
            Hook_BrawlSetHitPoints.GetOriginalFunction<Original>()(self, value);
            // Observe healing only. Never clamp HP or manufacture a monster KO.
            if (self && value && *value > previous && self->GetHitPointsMax() > 0 &&
                self->GetHitPoints() >= self->GetHitPointsMax() && g_ppBrawlSession && *g_ppBrawlSession &&
                (*g_ppBrawlSession)->IsGameRunning())
            {
                eCEntity *owner = self->GetEntity();
                if (owner)
                {
                    Entity npc(static_cast<eCEntity const *>(owner));
                    if (npc.IsNPC() && IsHeroControlled(npc) && !IsBrawlFighting(npc))
                        ReconcileBrawl(npc, "health restored to 100 percent");
                }
            }
        }

        GEInt g_iPersonalRewardDepth = 0;
        bCPropertyID g_PersonalRewardVictim;

PersonalRewardScope::PersonalRewardScope(Entity &target, GEBool enabled) : depth(g_iPersonalRewardDepth), victim(g_PersonalRewardVictim)
            {
                if (enabled && target != None)
                {
                    ++g_iPersonalRewardDepth;
                    g_PersonalRewardVictim = target.GetInstance()->GetID();
                    SetBrawlEvent(target, "NoReward", GETrue);
                }
                else g_iPersonalRewardDepth = 0;
            }
PersonalRewardScope::~PersonalRewardScope()
            {
                g_iPersonalRewardDepth = depth;
                g_PersonalRewardVictim = victim;
            }


        GEBool ShouldSuppressBrawlXP(Entity &defeated)
        {
            return defeated != None &&
                ((g_iPersonalRewardDepth > 0 && defeated.GetInstance()->GetID() == g_PersonalRewardVictim) ||
                 IsBrawlFighting(defeated) ||
                 ((defeated.IsDead() || defeated.IsDown()) && BrawlEvent(defeated, "NoReward")));
        }

        

        GEInt g_iPersonalKnockoutDepth = 0;
        bCPropertyID g_PersonalKnockoutVictim;

PersonalKnockoutScope::PersonalKnockoutScope(Entity &npc, GEBool enabled) : depth(g_iPersonalKnockoutDepth), victim(g_PersonalKnockoutVictim)
            {
                if (!enabled || npc == None || !npc.IsNPC() || npc.IsDead() || !IsHeroControlled(npc)) return;
                ++g_iPersonalKnockoutDepth;
                g_PersonalKnockoutVictim = npc.GetInstance()->GetID();
                SetBrawlEvent(npc, "RecoveryPending", GETrue);
                GEInt const slot = FindBrawlSlot(npc, GEFalse);
                if (slot >= 0) g_arrBrawlUpSince[slot] = 0;
                if (npc.Inventory.IsValid()) npc.Inventory.HoldStacks(-1, -1);
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][weapon] '%s': original equipment holstered before native knockout.\n", npc.GetName().GetText());
            }
PersonalKnockoutScope::~PersonalKnockoutScope() { g_iPersonalKnockoutDepth = depth; g_PersonalKnockoutVictim = victim; }


        mCFunctionHook Hook_PersonalSlotDrop;
        eCEntity *GE_STDCALL PersonalSlotDrop(gCInventory_PS *inventory, gESlot slot, GEInt amount, bCMatrix const *matrix)
        {
            using Original = eCEntity *(GE_STDCALL *)(gCInventory_PS *, gESlot, GEInt, bCMatrix const *);
            Entity owner(inventory ? inventory->GetEntity() : nullptr);
            GEInt const index = inventory ? inventory->FindStackIndex(slot) : -1;
            GEBool const personal = owner != None && owner.IsNPC() && !owner.IsDead() && IsHeroControlled(owner) &&
                ((g_iPersonalKnockoutDepth > 0 && owner.GetInstance()->GetID() == g_PersonalKnockoutVictim) ||
                 (BrawlEvent(owner, "RecoveryPending") && IsCompanionDownOrRising(owner)));
            if (personal && index >= 0 && inventory->GetStackCategory(index) == gEItemCategory_Weapon)
            {
                inventory->HoldStacks(-1, -1); // Native hand-to-equipment-slot operation; no extraction or new item.
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][weapon] '%s': native weapon drop replaced by holstering (slot=%d stack=%d).\n",
                    owner.GetName().GetText(), static_cast<int>(slot), index);
                return nullptr;
            }
            return Hook_PersonalSlotDrop.GetOriginalFunction<Original>()(inventory, slot, amount, matrix);
        }
        

        // The SAFETY NET against a creature stuck in ZS_Stumble after a blow of the hero (it is called by PollFleeWatch about
        // one second after the blow). A creature that has accepted the fight goes on attacking the hero, any other one
        // goes back to the routine.
        void ReleaseStumbledNpc(Entity &a_Npc)
        {
            a_Npc.Routine.FullStop();
            if (IsBrawlFighting(a_Npc) && !IsHeroKnockedOut())
            {
                a_Npc.NPC.SetCurrentTarget(Entity::GetPlayer());
                a_Npc.NPC.AttackReason = gEAttackReason_Angry;
                a_Npc.Routine.SetTask("ZS_Attack");
                return;
            }
            a_Npc.NPC.SetCurrentTarget(None);
            a_Npc.Routine.ContinueRoutine();
        }

        // THE MASTER IS KNOCKED OUT: a creature of his party that fights him (or still keeps him as a target) stops at once
        // and goes back to the routine of following. Nobody of the party finishes, robs or hits the fallen master.
        // Returns true when the creature was released.
        GEBool StandDownFromMaster(Entity &a_Npc, GELPCChar a_strWhere)
        {
            if (a_Npc == None || !a_Npc.IsNPC() || !IsHeroControlled(a_Npc) || !IsHeroKnockedOut())
                return GEFalse;
            // Fighting the master in any way: a started brawl, the master as the target or as the current attacker.
            Entity Target = a_Npc.NPC.GetCurrentTarget();
            Entity Attacker = a_Npc.NPC.GetCurrentAttacker();
            GEBool const bAgainstMaster = IsBrawlFighting(a_Npc) || (Target != None && IsHeroSide(Target)) ||
                                          (Attacker != None && IsHeroSide(Attacker));
            if (!bAgainstMaster)
                return GEFalse;
            EndBrawlOf(a_Npc, a_strWhere);
            return GETrue;
        }

        // A confirmed direct magic-projectile impact on a party member (from AssessHit).
        void OnHeroBlowOnArmy(Entity &a_Victim, bCPropertyID const &a_ProjectileID)
        {
            if (a_Victim == None || !a_Victim.IsNPC() || a_Victim.IsDead() || a_Victim.IsDown() || IsHeroKnockedOut())
                return;
            GEU32 const u32Now = ::GetTickCount();
            GEInt const iSlot = FindBrawlSlot(a_Victim, GETrue);
            if (iSlot < 0 || g_arrBrawlStart[iSlot] != 0)
                return; // Once fighting, damage no longer belongs to the warning counter.
            // THE WINDOW OF THE SECOND BLOW: no second blow within BRAWL_WINDOW_MS (10 s) of the first one - the counter is reset
            // and the creature cools down (this blow is the FIRST one again).
            if (g_arrBrawlStart[iSlot] == 0 &&
                (g_arrBrawlLastBlow[iSlot] == 0 || static_cast<GEInt>(u32Now - g_arrBrawlLastBlow[iSlot]) >
                                                       static_cast<GEInt>(BRAWL_WINDOW_MS)))
            {
                g_arrBrawlCount[iSlot] = 0;
                g_arrBrawlReacted[iSlot] = GEFalse;
            }
            // Repeated notifications / lingering effects of the SAME projectile
            // must not open a fight. A new projectile may use any allowed spell.
            if ((g_arrBrawlCount[iSlot] > 0 || g_arrBrawlGrudge[iSlot]) && g_arrBrawlProjectileID[iSlot] == a_ProjectileID)
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': repeated projectile ignored.\n",
                                               a_Victim.GetName().GetText());
                return;
            }
            if (g_arrBrawlGrudge[iSlot])
                g_arrBrawlCount[iSlot] = 1; // One new permitted projectile restarts a resentful fight.
            g_arrBrawlProjectileID[iSlot] = a_ProjectileID;
            g_arrBrawlLastBlow[iSlot] = u32Now | 1;
            ++g_arrBrawlCount[iSlot];
            StartFleeWatch(a_Victim); // the watch makes the reaction to the first blow and keeps the safety net
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Blow %d of the hero on '%s' (party type %d).\n",
                                         static_cast<int>(g_arrBrawlCount[iSlot]), a_Victim.GetName().GetText(),
                                         static_cast<int>(a_Victim.Party.PartyMemberType));
            if (g_arrBrawlCount[iSlot] < 2 || g_arrBrawlStart[iSlot] != 0)
                return;
            GEBool const brokeTruce = g_arrBrawlGrudge[iSlot] && BrawlEvent(a_Victim, "SheatheAccepted", GETrue);
            // THE SECOND BLOW: the creature accepts the fight and attacks the hero.
            g_arrBrawlStart[iSlot] = u32Now | 1;
            g_arrBrawlWeaponSeen[iSlot] = HeroHasDrawnWeapon();
            g_arrBrawlSheatheRefused[iSlot] = GEFalse;
            g_arrBrawlWeaponEmptySince[iSlot] = 0;
            g_arrBrawlGrudge[iSlot] = GEFalse;
            g_arrBrawlWasWaiting[iSlot] = a_Victim.Party.Waiting;
            a_Victim.Party.Waiting = GEFalse;
            SetBrawlEvent(a_Victim, "Grudge", GEFalse, GETrue);
            SetBrawlEvent(a_Victim, "WasWaiting", g_arrBrawlWasWaiting[iSlot]);
            SetBrawlEvent(a_Victim, "Active", GETrue, GETrue);
            Entity Hero = Entity::GetPlayer();
            a_Victim.Routine.FullStop();
            a_Victim.NPC.SetCurrentTarget(Hero);
            a_Victim.NPC.AttackReason = gEAttackReason_Angry;
            a_Victim.Routine.SetTask("ZS_Attack");
            StartPersonalSpectators(a_Victim);
            if (brokeTruce && (a_Victim.NPC.Species == gESpecies_Human || a_Victim.NPC.Species == gESpecies_Orc))
            {
                ::StartSaySVM(a_Victim, Hero, bCString("YouAskedForIt"), 3);
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': truce broken by a new hit; refusal voice played.\n", a_Victim.GetName().GetText());
            }
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The second blow: '%s' accepts the fight and attacks the hero "
                                         "('%s'). The rest of the army stays neutral.\n",
                                         a_Victim.GetName().GetText(), Hero.GetName().GetText());
        }



        void PollBrawl()
        {
            PollArenaQuestEnd();
            PollPersonalTeleport();
            PollPersonalSpectators();
            static GEU32 lastService = 0;
            GEU32 const serviceNow = ::GetTickCount();
            if (lastService != 0 && static_cast<GEInt>(serviceNow - lastService) < 250)
                return;
            lastService = serviceNow;
            PollPersonalSheathe();
            if (g_ppBrawlSession && *g_ppBrawlSession && (*g_ppBrawlSession)->IsGameRunning())
            {
                bTObjArray<Entity> party = Entity::GetNPCs();
                for (GEInt i = 0; i < party.GetCount(); ++i)
                {
                    Entity npc = party.GetAt(i);
                    if (npc == None || !npc.IsNPC() || !IsHeroControlled(npc) || npc.IsDead())
                        continue;
                    ObserveRecovery(npc, "party poll");
                    RestoreSavedBrawlState(npc);
                }
            }
            GEBool bAny = GEFalse;
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
                if (g_arrBrawlStart[i] != 0 || g_arrBrawlLastBlow[i] != 0)
                    bAny = GETrue;
            if (!bAny)
                return;
            static GEU32 s_u32Last = 0;
            GEU32 const u32Now = ::GetTickCount();
            if (s_u32Last != 0 && static_cast<GEInt>(u32Now - s_u32Last) < 250)
                return;
            s_u32Last = u32Now;
            GEBool const bHeroDown = IsHeroKnockedOut();
            bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
            for (GEInt i = 0; i < BRAWL_MAX; ++i)
            {
                // THE COOL-DOWN: the first blow was borne and no second blow came within BRAWL_WINDOW_MS (10 s) - the counter
                // is reset, the combat state of the creature is cleared and it goes back to following the hero.
                if (g_arrBrawlStart[i] == 0 && g_arrBrawlLastBlow[i] != 0 &&
                    (bHeroDown || static_cast<GEInt>(u32Now - g_arrBrawlLastBlow[i]) > static_cast<GEInt>(BRAWL_WINDOW_MS)))
                {
                    for (GEInt k = 0; k < arrNPCs.GetCount(); ++k)
                    {
                        Entity Npc = arrNPCs.GetAt(k);
                        if (Npc == None || !(Npc.GetInstance()->GetID() == g_arrBrawlVictimID[i]))
                            continue;
                        if (!Npc.IsDead() && !Npc.IsDown())
                            EndBrawlOf(Npc, bHeroDown ? "the master is knocked out" : "no second blow within 10 s: cooled down");
                        break;
                    }
                    g_arrBrawlCount[i] = 0;
                    g_arrBrawlLastBlow[i] = 0;
                    g_arrBrawlReacted[i] = GEFalse;
                    continue;
                }
                if (g_arrBrawlStart[i] == 0)
                    continue;
                GEBool const bTimeOver =
                    static_cast<GEInt>(u32Now - g_arrBrawlStart[i]) > static_cast<GEInt>(BRAWL_MAX_MS);
                GEBool bFound = GEFalse;
                for (GEInt k = 0; k < arrNPCs.GetCount(); ++k)
                {
                    Entity Npc = arrNPCs.GetAt(k);
                    if (Npc == None || !(Npc.GetInstance()->GetID() == g_arrBrawlVictimID[i]))
                        continue;
                    bFound = GETrue;
                    Entity Target = Npc.NPC.GetCurrentTarget();
                    GEBool lostTarget = GEFalse;
                    if (Target != None && IsHeroSide(Target))
                        g_arrBrawlTargetLostTick[i] = 0;
                    else
                    {
                        bCString const task = Npc.Routine.GetCurrentTask();
                        bCString const state = Npc.Routine.GetCurrentState();
                        GEBool const transitioning = Target == None &&
                            (_strnicmp(task.GetText(), "ZS_Attack", 9) == 0 ||
                             _strnicmp(task.GetText(), "ZS_Stumble", 10) == 0 ||
                             _strnicmp(state.GetText(), "ZS_Stumble", 10) == 0 ||
                             _strnicmp(state.GetText(), "ZS_QuickStumble", 15) == 0);
                        if (g_arrBrawlTargetLostTick[i] == 0)
                            g_arrBrawlTargetLostTick[i] = u32Now | 1;
                        lostTarget = !transitioning || static_cast<GEInt>(u32Now - g_arrBrawlTargetLostTick[i]) >= 2000;
                    }
                    GEBool const bOut = Npc.IsDead() || Npc.IsDown();
                    // Native AI may abandon its target when the hero puts a weapon away.
                    // Only repair an empty target after a refused surrender in this fight;
                    // defeats, teleportation and switching to an outside enemy retain priority.
                    if (lostTarget && Target == None && g_arrBrawlSheatheRefused[i] &&
                        !bHeroDown && !bOut && !bTimeOver && !g_bPersonalTeleportPending &&
                        IsHeroControlled(Npc) && !IsCompanionDownOrRising(Npc))
                    {
                        Npc.NPC.SetCurrentTarget(Entity::GetPlayer());
                        Npc.NPC.AttackReason = gEAttackReason_Angry;
                        Npc.Routine.SetTask("ZS_Attack");
                        g_arrBrawlTargetLostTick[i] = 0;
                        lostTarget = GEFalse;
                    }
                    if (bHeroDown || bTimeOver || bOut || lostTarget)
                    {
                        EndBrawlOf(Npc, bHeroDown ? "the master is knocked out" : (bOut ? "the companion is defeated" : (lostTarget ? "the player is no longer the attack target" : "time is over")));
                        bFound = GEFalse; // the slot is closed below
                    }
                    break;
                }
                if (!bFound)
                {
                    g_arrBrawlStart[i] = 0;
                    g_arrBrawlCount[i] = 0;
                    g_arrBrawlLastBlow[i] = 0;
                    g_arrBrawlReacted[i] = GEFalse;
                }
            }
        }


        // Called by the blow watch (PollFleeWatch) after the stumble release; a_iStage 0 is +300 ms, 1 is +1000 ms,
        // 2 is +2000 ms, 3 is +4000 ms after the last blow of the hero.
        void BrawlStageAction(Entity &a_Npc, GEInt a_iStage)
        {
            if (!IsHeroControlled(a_Npc) || a_Npc.IsDead() || a_Npc.IsDown() || IsHeroKnockedOut())
                return;
            GEInt const iSlot = FindBrawlSlot(a_Npc, GEFalse);
            if (iSlot < 0 || g_arrBrawlStart[iSlot] != 0)
                return;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None)
                return;
            if (a_iStage <= 1 && !g_arrBrawlReacted[iSlot] && g_arrBrawlCount[iSlot] == 1 &&
                !HasTask(a_Npc, "ZS_Stumble") && !HasTask(a_Npc, "ZS_Attack"))
            {
                // THE FIRST BLOW: the reaction of an ordinary green NPC (the weapon, the stance, the manoeuvring in front of
                // the hero, one on one). A stumble in progress is let finish (stage 0) or was released (stage 1).
                g_arrBrawlReacted[iSlot] = GETrue;
                if (!HasTask(a_Npc, "ZS_ReactToDamage"))
                {
                    a_Npc.Routine.FullStop();
                    a_Npc.NPC.SetCurrentTarget(Hero);
                    a_Npc.NPC.AttackReason = gEAttackReason_ReactToDamage;
                    a_Npc.Routine.SetTask("ZS_ReactToDamage");
                }
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] The first blow: '%s' reacts as a green NPC (ZS_ReactToDamage "
                                             "in front of the hero, one on one).\n",
                                             a_Npc.GetName().GetText());
            }
            else if (a_iStage == 3 && !HasTask(a_Npc, "ZS_Attack") && !HasTask(a_Npc, "ZS_ReactToDamage") &&
                     !HasTask(a_Npc, "ZS_FollowPlayer") && !a_Npc.Party.Waiting && a_Npc.GetDistanceTo(Hero) > 900.0f)
            {
                // THE ENDLESS RETREAT is stopped: he is far from the hero and still does not follow him.
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] '%s' retreated too far (%.0f) after the blow: back to the "
                                             "hero.\n",
                                             a_Npc.GetName().GetText(), static_cast<double>(a_Npc.GetDistanceTo(Hero)));
                a_Npc.Routine.FullStop();
                a_Npc.NPC.SetCurrentTarget(None);
                a_Npc.NPC.AttackReason = gEAttackReason_None;
                a_Npc.Routine.ContinueRoutine();
            }
        }

GEBool GE_STDCALL RunPersonalBrawlExit(bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU)
{
        INIT_SCRIPT_STATE();
        // A new fight/knockout or another native command takes priority over cleanup.
        if (SelfEntity == None || !SelfEntity.IsNPC() || SelfEntity.IsDead() ||
            IsCompanionDownOrRising(SelfEntity) || !IsHeroControlled(SelfEntity) ||
            IsBrawlFighting(SelfEntity) ||
            SelfEntity.Routine.GetCurrentTask() != bCString("GLF_PersonalBrawlExit"))
            return GETrue;
        if (SelfEntity.NPC.AttackReason != gEAttackReason_None || SelfEntity.NPC.CombatState != 0)
        {
            SelfEntity.Routine.ContinueRoutine();
            return GETrue;
        }
        BREAK_BLOCK
        {
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': native stand/holster sequence started.\n", SelfEntity.GetName().GetText());
            RUN_AI_STANDUP();
        }
        BREAK_BLOCK
        {
            if (SelfEntity.Inventory.IsValid())
            {
                RUN_AI_HOLD_INVENTORY_ITEMS(-1, -1);
            }
        }
        BREAK_BLOCK
        {
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] '%s': native stand/holster sequence completed; follow/wait restored.\n", SelfEntity.GetName().GetText());
            GEBool const approveTruce = BrawlEvent(SelfEntity, "ExitApproval", GEFalse);
            SetBrawlEvent(SelfEntity, "ExitApproval", GEFalse);
            if (approveTruce && (SelfEntity.NPC.Species == gESpecies_Human || SelfEntity.NPC.Species == gESpecies_Orc))
            {
                Entity hero = Entity::GetPlayer();
                if (hero != None) ::StartSaySVM(SelfEntity, hero, bCString("Good"), 9);
            }
            SelfEntity.Routine.SetTask(SelfEntity.Party.Waiting ? "ZS_WaitForPlayer" : "ZS_FollowPlayer");
        }
        return GETrue;
    }

GEBool GE_STDCALL RunPersonalBrawlSpectator(bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU)
{
        (void)a_rRunTimeStack;
        Entity Self(a_pSPU->GetSelfEntity());
        PollPersonalSpectators();
        if (Self == None || FindPersonalSpectator(Self) < 0)
        {
            if (Self != None && OwnsPersonalSpectatorTask(Self) && !Self.IsDead() && !Self.IsDown())
            {
                Self.StopPlayAni();
                Self.StopPlayOverlayAni();
                Self.NPC.SetCurrentTarget(None);
                Self.NPC.SetAlternativeTarget(None);
                Self.Routine.SetTask(Self.Party.Waiting ? "ZS_WaitForPlayer" : "ZS_FollowPlayer");
            }
            return GETrue;
        }
        return GEFalse;
    }

GEBool HandlePersonalAggressor(Entity &Self, Entity &Other)
{
        if (Self != None && Other != None && Self.IsNPC() && !Self.IsDead() && IsHeroControlled(Self) && IsHeroSide(Other))
        {
            if (IsHeroKnockedOut())
            {
                StandDownFromMaster(Self, "AssessAgressor");
                return 1;
            }
            GEInt const iSlot = FindBrawlSlot(Self, GEFalse);
            GEBool const bFight = iSlot >= 0 && g_arrBrawlStart[iSlot] != 0;
            if (!bFight)
            {
                // THE FIRST BLOW: the reaction of a green NPC, not the fight.
                Entity Hero = Entity::GetPlayer();
                if (iSlot >= 0)
                    g_arrBrawlReacted[iSlot] = GETrue;
                Self.NPC.SetCurrentTarget(Hero);
                Self.NPC.AttackReason = gEAttackReason_ReactToDamage;
                Self.Routine.SetTask("ZS_ReactToDamage");
                static GEInt s_iLines = 0;
                if (s_iLines < 60)
                {
                    ++s_iLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AssessAgressor: the first blow of the hero on '%s' (party "
                                                 "type %d): ZS_ReactToDamage instead of the immediate attack.\n",
                                                 Self.GetName().GetText(), static_cast<int>(Self.Party.PartyMemberType));
                }
                return 1;
            }
        }
    return GEFalse;
}

void InitializeSessionAccess() { g_ppBrawlSession = reinterpret_cast<gCSession **>(::GetProcAddress(::GetModuleHandleA("Game.dll"), "?ms_pCurrentSession@gCSession@@1PAV1@A")); }

}

namespace GroupLogic::Combat
{
    using namespace Brawl;
    using Host::HasTask;
    using PluginRuntime::HeroState::IsHeroKnockedOut;
    using Host::PrintCompanionWaitMessage;
    using SocialLogic::IsPeaceful;
    using Host::WasArenaDuelActive;
    using Host::IsDuelTaboo;
    using Host::FindArenaOpponent;
    using Host::HasRunningArenaQuest;
    using Host::IsInsideArenaZone;
    using Host::DamageTypeName;
    using GroupLogic::Combat::GetSourceElementalType;
    using Host::MarkWaiterUserPublic;
    using Host::ClearWaiterMarksPublic;

namespace
{


        GEInt const FLEE_WATCH_MAX = 8;
        GEInt const FLEE_STAGES = 4;
        GEU32 const g_arrFleeOffsets[FLEE_STAGES] = {300, 1000, 2000, 4000};
        bCString g_arrFleeNames[FLEE_WATCH_MAX];
        GEU32 g_arrFleeStart[FLEE_WATCH_MAX] = {};
        GEInt g_arrFleeStage[FLEE_WATCH_MAX] = {};
        GEBool g_bFleeWatchActive = GEFalse;
        GEInt const REC_MAX = 32;
        GEU32 const COMBAT_MS = 60000;
        bCString g_arrCombatNames[REC_MAX];
        GEU32 g_arrCombatTick[REC_MAX] = {};
        GEInt const DEFEND_MAX = 16;
        GEU32 const DEFEND_MS = 20000;
        GEFloat const DEFEND_RADIUS = 2500.0f;
        bCString g_arrDefendNames[DEFEND_MAX];
        GEU32 g_arrDefendTick[DEFEND_MAX] = {};
        GEInt const FOCUS_MAX = 24;
        GEU32 const FOCUS_MS = 15000;
        bCString g_arrFocusEnemy[FOCUS_MAX];
        bCString g_arrFocusVictim[FOCUS_MAX];
        GEU32 g_arrFocusTick[FOCUS_MAX] = {};
}

        // The hero in any form: the player entity, the body of the form, the hidden human body.
        GEBool IsHeroSide(Entity &a_Npc)
        {
            if (a_Npc == None)
                return GEFalse;
            if (a_Npc == Entity::GetPlayer() || IsHeroFormEntity(a_Npc))
                return GETrue;
            return _stricmp(a_Npc.GetName().GetText(), "PC_Hero") == 0 ? GETrue : GEFalse;
        }

        void LogAiSnapshot(GELPCChar a_strWhen, Entity &a_Npc)
        {
            Entity Hero = Entity::GetPlayer();
            Entity Leader = a_Npc.Party.GetPartyLeader();
            Entity Target = a_Npc.NPC.GetCurrentTarget();
            Entity Attacker = a_Npc.NPC.GetCurrentAttacker();
            bCString const strTask = a_Npc.Routine.GetCurrentTask();
            bCString const strState = a_Npc.Routine.GetCurrentState();
            DebugLog::GetLogger().LogFormat(
                "[GenomeLogicFix][diag][flee] %s: '%s' (%p, party type %d, leader '%s', waiting=%d) task='%s' state='%s' "
                "aiMode=%d attackReason=%d combatState=%u target='%s' currentAttacker='%s' distToHero=%.0f dead=%d "
                "down=%d\n",
                a_strWhen, a_Npc.GetName().GetText(), static_cast<void *>(a_Npc.GetInstance()),
                static_cast<int>(a_Npc.Party.PartyMemberType), Leader != None ? Leader.GetName().GetText() : "-",
                static_cast<int>(a_Npc.Party.Waiting), strTask.GetText(), strState.GetText(),
                static_cast<int>(a_Npc.Routine.AIMode), static_cast<int>(a_Npc.NPC.AttackReason),
                static_cast<unsigned>(a_Npc.NPC.CombatState), Target != None ? Target.GetName().GetText() : "-",
                Attacker != None ? Attacker.GetName().GetText() : "-",
                static_cast<double>(Hero != None ? a_Npc.GetDistanceTo(Hero) : -1.0f),
                static_cast<int>(a_Npc.IsDead()), static_cast<int>(a_Npc.IsDown()));
        }

        void StartFleeWatch(Entity &a_Victim)
        {
            bCString const strName = a_Victim.GetName();
            GEInt iSlot = -1;
            for (GEInt i = 0; i < FLEE_WATCH_MAX; ++i)
            {
                if (g_arrFleeStart[i] != 0 && g_arrFleeNames[i] == strName)
                {
                    iSlot = i; // hit again: the watch starts anew
                    break;
                }
                if (iSlot < 0 && g_arrFleeStart[i] == 0)
                    iSlot = i;
            }
            if (iSlot < 0)
                iSlot = 0;
            g_arrFleeNames[iSlot] = strName;
            g_arrFleeStart[iSlot] = ::GetTickCount() | 1;
            g_arrFleeStage[iSlot] = 0;
            g_bFleeWatchActive = GETrue;
        }

        // Called from the hooks that run all the time (attitude / target). Technically throttled, reads only.



        void PollFleeWatch()
        {
            PollBrawl();
            if (!g_bFleeWatchActive)
                return;
            static GEU32 s_u32Last = 0;
            GEU32 const u32Now = ::GetTickCount();
            if (s_u32Last != 0 && static_cast<GEInt>(u32Now - s_u32Last) < 100)
                return;
            s_u32Last = u32Now;
            GEBool bAny = GEFalse;
            for (GEInt i = 0; i < FLEE_WATCH_MAX; ++i)
            {
                if (g_arrFleeStart[i] == 0)
                    continue;
                GEInt const iStage = g_arrFleeStage[i];
                if (iStage >= FLEE_STAGES)
                {
                    g_arrFleeStart[i] = 0;
                    continue;
                }
                bAny = GETrue;
                GEU32 const u32Elapsed = u32Now - g_arrFleeStart[i];
                if (static_cast<GEInt>(u32Elapsed) < static_cast<GEInt>(g_arrFleeOffsets[iStage]))
                    continue;
                g_arrFleeStage[i] = iStage + 1;
                bTObjArray<Entity> arrNPCs = Entity::GetNPCs();
                for (GEInt k = 0; k < arrNPCs.GetCount(); ++k)
                {
                    Entity Npc = arrNPCs.GetAt(k);
                    if (!(Npc.GetName() == g_arrFleeNames[i]))
                        continue;
                    char strWhen[64] = {};
                    _snprintf_s(strWhen, sizeof(strWhen), _TRUNCATE, "+%u ms after the blow of the hero",
                                static_cast<unsigned>(u32Elapsed));
                    LogAiSnapshot(strWhen, Npc);
                    if (iStage >= 1 && IsHeroControlled(Npc) && HasTask(Npc, "ZS_Stumble"))
                    {
                        ReleaseStumbledNpc(Npc); // THE SAFETY NET: an army member left in ZS_Stumble about 1 s after a blow is released
                        DebugLog::GetLogger().LogFormat("[GenomeLogicFix] '%s' was stuck in ZS_Stumble after a blow: released.\n",
                                                     Npc.GetName().GetText());
                    }
                    BrawlStageAction(Npc, iStage);
                    break;
                }
            }
            if (!bAny)
                g_bFleeWatchActive = GEFalse;
        }
        // One line per blow on a member of the army. a_strFunction: the native function that recorded the blow.
        // a_pSource may be the damaging object (projectile) - the owner chain gives the real attacker.
        void LogArmyHit(GELPCChar a_strFunction, Entity &a_Victim, Entity &a_Source, GEInt a_iDamageType)
        {
            static GEInt s_iLines = 0;
            if (s_iLines >= 600 || a_Victim == None || !a_Victim.IsNPC())
                return;
            if (!IsHeroControlled(a_Victim))
                return;
            Entity Attacker = GetDamageOwnerNpc(a_Source);
            if (Attacker == None)
                Attacker = a_Source;
            Entity Hero = Entity::GetPlayer();
            GEBool const bHeroSide = Attacker != None && IsHeroSide(Attacker);
            GEBool const bArmy = Attacker != None && IsHeroControlled(Attacker);
            GELPCChar strKind = bHeroSide ? "HERO" : (bArmy ? "ARMY" : "OUTSIDER");
            ++s_iLines;
            DebugLog::GetLogger().LogFormat(
                "[GenomeLogicFix][diag][hit] %s: attacker='%s' (%p, %s, faction %d, party type %d) -> victim='%s' (%p, "
                "party type %d) damage=%s(%d) source object='%s' hero(GetPlayer)='%s'\n",
                a_strFunction, Attacker != None ? Attacker.GetName().GetText() : "-",
                Attacker != None ? static_cast<void *>(Attacker.GetInstance()) : nullptr, strKind,
                Attacker != None && Attacker.IsNPC() ? static_cast<int>(Attacker.NPC.PoliticalAlignment) : -1,
                Attacker != None && Attacker.IsNPC() ? static_cast<int>(Attacker.Party.PartyMemberType) : -1,
                a_Victim.GetName().GetText(), static_cast<void *>(a_Victim.GetInstance()),
                static_cast<int>(a_Victim.Party.PartyMemberType), DamageTypeName(a_iDamageType), a_iDamageType,
                a_Source != None ? a_Source.GetName().GetText() : "-", Hero.GetName().GetText());
            // The hero (in any form) hurts his own creature: the AI is watched for the next seconds.
            if (bHeroSide)
            {
                LogAiSnapshot("right after the blow of the hero", a_Victim);
                StartFleeWatch(a_Victim);
            }
        }

        void NoteName(bCString *a_pNames, GEU32 *a_pTicks, bCString const &a_strName, GEU32 a_u32Span)
        {
            GEU32 const u32Now = ::GetTickCount();
            GEInt iFree = -1;
            for (GEInt i = 0; i < REC_MAX; ++i)
            {
                if (a_pTicks[i] != 0 && a_pNames[i] == a_strName)
                {
                    a_pTicks[i] = u32Now | 1;
                    return;
                }
                if (iFree < 0 && (a_pTicks[i] == 0 || static_cast<GEInt>(u32Now - a_pTicks[i]) >= static_cast<GEInt>(a_u32Span)))
                    iFree = i;
            }
            if (iFree < 0)
                iFree = 0;
            a_pNames[iFree] = a_strName;
            a_pTicks[iFree] = u32Now | 1;
        }

        GEBool HasName(bCString const *a_pNames, GEU32 const *a_pTicks, bCString const &a_strName, GEU32 a_u32Span)
        {
            GEU32 const u32Now = ::GetTickCount();
            for (GEInt i = 0; i < REC_MAX; ++i)
                if (a_pTicks[i] != 0 && a_pNames[i] == a_strName &&
                    static_cast<GEInt>(u32Now - a_pTicks[i]) < static_cast<GEInt>(a_u32Span))
                    return GETrue;
            return GEFalse;
        }

        void ClearCombatRecords()
        {
            for (GEInt i = 0; i < REC_MAX; ++i)
            {
                g_arrCombatNames[i] = bCString();
                g_arrCombatTick[i] = 0;
            }
        }

        GEBool HasCombatRecord(Entity &a_Outsider)
        {
            return HasName(g_arrCombatNames, g_arrCombatTick, a_Outsider.GetName(), COMBAT_MS);
        }



        // The engine stored a_Attacker as the attacker of a_Victim: a real blow.
        void RecordCombat(Entity &a_Victim, Entity &a_Attacker)
        {
            if (ArenaJudge::Punishment::IsArenaPunishmentBlow(a_Victim, a_Attacker)) return;

            if (a_Victim == None || a_Attacker == None || a_Victim == a_Attacker)
                return;
            // Attacker setters can run several times for one impact, without
            // identifying a projectile. Count party brawls only at AssessHit.
            if (IsHeroSide(a_Attacker) && IsHeroControlled(a_Victim))
                return;
            GEBool const bVictimSide = IsHeroSide(a_Victim) || IsHeroControlled(a_Victim);
            GEBool const bAttackerSide = IsHeroSide(a_Attacker) || IsHeroControlled(a_Attacker);
            if (bVictimSide == bAttackerSide)
                return;
            Entity &Outsider = bVictimSide ? a_Attacker : a_Victim;
            if (IsDuelTaboo(Outsider))
                return; // the duel opponent never becomes an enemy of the army
            if (Outsider.IsNPC())
                NoteName(g_arrCombatNames, g_arrCombatTick, Outsider.GetName(), COMBAT_MS);
        }

        // The attack reasons that do not need a record: the command of the player, the arena, the duel, the revolution, prey.
        GEBool IsOrderedAttackReason(GEInt a_iReason)
        {
            return a_iReason == gEAttackReason_PlayerCommand || a_iReason == gEAttackReason_Arena ||
                   a_iReason == gEAttackReason_Duel || a_iReason == gEAttackReason_Revolution ||
                   a_iReason == gEAttackReason_Prey;
        }
        // THE FIGHT HAS STARTED. An outsider (not the hero, not of the army) and a creature of the army (or the hero) are in
        // a real fight when the engine has written one down as the attacker of the other, or when the combat record of the
        // outsider exists (SetCurrentAttacker / SetLastAttacker recorded a real blow).
        GEBool IsInRealFight(Entity &a_Self, Entity &a_Other)
        {
            if (ArenaJudge::Punishment::IgnoreArenaPunishmentTarget(a_Self, a_Other) ||
                ArenaJudge::Punishment::IgnoreArenaPunishmentTarget(a_Other, a_Self)) return GEFalse;

            if (a_Self == None || a_Other == None)
                return GEFalse;
            Entity *pArmy = nullptr;
            Entity *pOutsider = nullptr;
            if (IsHeroControlled(a_Self) && a_Other.IsNPC() && !IsHeroSide(a_Other) && !IsHeroControlled(a_Other))
            {
                pArmy = &a_Self;
                pOutsider = &a_Other;
            }
            else if (IsHeroControlled(a_Other) && a_Self.IsNPC() && !IsHeroSide(a_Self) && !IsHeroControlled(a_Self))
            {
                pArmy = &a_Other;
                pOutsider = &a_Self;
            }
            if (!pArmy || !pOutsider)
                return GEFalse;
            return HasCombatRecord(*pOutsider) || pArmy->NPC.GetCurrentAttacker() == *pOutsider ||
                           pArmy->NPC.GetLastAttacker() == *pOutsider || pOutsider->NPC.GetCurrentAttacker() == *pArmy ||
                           pOutsider->NPC.GetLastAttacker() == *pArmy
                       ? GETrue
                       : GEFalse;
        }
        // A creature of the army must not take an outsider for a target while there is no real fight with him.
        GEBool ArmyMustNotAttack(Entity &a_Self, Entity &a_Other, GEInt a_iReason)
        {
            if (ArenaJudge::Punishment::IgnoreArenaPunishmentTarget(a_Self, a_Other)) return GETrue;

            if (a_Self == None || a_Other == None || !a_Other.IsNPC() || !IsHeroControlled(a_Self))
                return GEFalse;
            if (IsDuelTaboo(a_Other))
                return GETrue; // the duel opponent is invisible for the army, whatever the reason
            if (IsHeroSide(a_Other) || IsHeroControlled(a_Other) || IsOrderedAttackReason(a_iReason))
                return GEFalse;
            if (HasCombatRecord(a_Other) || a_Self.NPC.GetCurrentAttacker() == a_Other || a_Self.NPC.GetLastAttacker() == a_Other)
                return GEFalse;
            return GETrue;
        }

        // The BODY OF THE FORM: the log of the demon test shows that the hits of the hero come from an entity named
        // "Transform_Demon_Potion" while GetPlayer() never returned it (no beast-form diagnostics were ever written, they are
        // all guarded by "the player is not PC_Hero"). So the transformation spawns a separate creature (the template
        // "Transform_<form>[_Potion]") for the hero, and the NPCs of the world see THAT creature, not the hero entity. The
        // world judges it by the faction / species rules of a monster (a non-humanoid with the faction None -> every
        // humanoid, a slave included, takes it for an enemy). It is told by the name of its template.




        GEBool IsRedForHero(Entity &a_Npc)
        {
            gEAttitude const enuAttitude = AI_Decision::GetAttitudeToPlayer(a_Npc);
            return enuAttitude == gEAttitude_Hostile || enuAttitude == gEAttitude_Predator;
        }

        // The NPC who made the damage: the damaging object itself (melee: the attacker) or the owner up its chain.
        Entity GetDamageOwnerNpc(Entity &a_Source)
        {
            Entity Current = a_Source;
            for (GEInt i = 0; i < 4 && Current != None; ++i)
            {
                // v1.0.34.0: the hero in a transformed body is not an NPC for the wrapper, but he is the attacker all the same.
                if (Current.IsNPC() || IsHeroSide(Current))
                    return Current;
                if (!Current.Interaction.IsValid())
                    break;
                Entity Owner = Current.Interaction.GetOwner();
                if (Owner == None || Owner == Current)
                    break;
                Current = Owner;
            }
            return Entity();
        }

        // True: the controlled creature must not kill this victim (any NPC / beast / monster that is not red for the hero).
        GEBool FollowerMustSpare(Entity &a_Attacker, Entity &a_Victim, GELPCChar a_strWhere)
        {
            if (a_Attacker == None || a_Victim == None || a_Attacker == a_Victim || !a_Victim.IsNPC())
                return GEFalse;
            if (!IsHeroControlled(a_Attacker))
                return GEFalse;
            if (a_Victim == Entity::GetPlayer() || IsHeroControlled(a_Victim))
                return GEFalse;
            // The ORIGINAL rule of the game stays for every attacker that is not a humanoid (animals, monsters, ...): the
            // native IsDeadlyDamage asks IsHumanoid of BOTH sides and, when one of them is not, the blow is deadly at once.
            // Only a humanoid attacker (a humanoid companion / summon) fighting a humanoid can leave the other knocked
            // down - and only there the hero's "not a red enemy" rule below makes sense. So a wolf, a snapper or a monster
            // of the party kills everybody it brawls with, as it always did.
            if (!AI_Decision::IsHumanoid(a_Attacker))
                return GEFalse;
            // Only a creature that CAN be knocked down is spared. The game itself tells them apart: IsDeadlyDamage /
            // HasAttackReasonToKill ask IsHumanoid of the victim (an animal, a monster, ... has no knock-down animation and
            // always dies), so those are never spared, whatever their attitude to the hero is.
            if (!AI_Decision::IsHumanoid(a_Victim))
                return GEFalse;
            if (IsRedForHero(a_Victim))
                return GEFalse; // a fight to the death against a real enemy of the hero
            static GEInt s_iLines = 0;
            if (s_iLines < 80)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] %s: '%s' (controlled by the hero, party type %d) must not "
                                             "kill '%s': he is not red for the hero (attitude %d).\n",
                                             a_strWhere, a_Attacker.GetName().GetText(),
                                             static_cast<int>(a_Attacker.Party.PartyMemberType),
                                             a_Victim.GetName().GetText(),
                                             static_cast<int>(AI_Decision::GetAttitudeToPlayer(a_Victim)));
            }
            return GETrue;
        }
        // The hero (any entity of his side) in a TRANSFORMED body: the body of the form ("Transform_*"), the player entity of
        // the form or his hidden human body while the form lasts.
        GEBool IsHeroAttackerInForm(Entity &a_Attacker)
        {
            return IsHeroFormEntity(a_Attacker) || (IsHeroSide(a_Attacker) && IsHeroTransformedNow());
        }

        // A SINGLE-TARGET MAGIC PROJECTILE of the hero (a fireball, an ice lance, a lightning bolt): the damaging object is
        // not a creature (a blow of a weapon comes with the attacker himself as the object, an arrow is not elemental) and
        // carries an elemental damage type of its own (the type stored on the victim is not used, it belongs to the previous
        // hit). An area spell never comes here: it reaches Entity::DoDamage (a_bAreaDamage = true).
        GEBool IsPointMagicSource(Entity *a_pSource)
        {
            if (!a_pSource || *a_pSource == None || a_pSource->IsNPC() || IsHeroSide(*a_pSource))
                return GEFalse;
            // v1.0.34.0: a PHYSICAL projectile is never magic: an arrow / a bolt carries the damage type Missile (a blade /
            // impact / physics object likewise), whatever the hero cast before.
            if (a_pSource->Damage.IsValid())
            {
                gEDamageType const enuOwn = a_pSource->Damage.DamageType;
                if (enuOwn == gEDamageType_Missile || enuOwn == gEDamageType_Blade)
                    return GEFalse;
            }
            return GetSourceElementalType(*a_pSource) != gEDamageType_None ? GETrue : GEFalse;
        }

        // FRIENDLY FIRE (v1.0.34.0, rules of 1.0.33.0). The hero damages a member of his army (party types 1..5):
        //  1. The hero in ANY TRANSFORMED FORM (a demon, a wolf, a bloodfly, a gorge ...): ALWAYS blocked, everything.
        //  2. The hero in the HUMAN FORM (PC_Hero):
        //     - any melee blow, any shot of a bow / crossbow, any area spell (a_bAreaDamage = true: meteor, fire rain, frost
        //       wave ...): ALWAYS blocked (IsFriendlyFire = true);
        //     - the ONLY exception: a single-target magic projectile of the hero shot at a creature of his party (a fireball,
        //       an ice lance, a lightning bolt): passes (IsFriendlyFire = false); it opens the manual counter of the brawl.
        //  3. Both the attacker and the victim are of the army: always blocked.
        GEBool IsFriendlyFire(Entity &a_Attacker, Entity &a_Victim, GEBool a_bAreaDamage, Entity *a_pSource)
        {
            if (a_Attacker == None || a_Victim == None || a_Attacker == a_Victim)
                return GEFalse;
            if (!IsHeroControlled(a_Victim))
                return GEFalse;
            GEBool const bHeroSide = IsHeroSide(a_Attacker);
            if (bHeroSide && CanDamageBrawlOpponent(a_Attacker, a_Victim))
                return GEFalse; // Any damage type, only against this active opponent.
            if (!bHeroSide && !IsHeroControlled(a_Attacker))
                return GEFalse;
            if (bHeroSide)
            {
                GEBool const bForm = IsHeroAttackerInForm(a_Attacker);
                if (!bForm && !a_bAreaDamage && IsPointMagicSource(a_pSource))
                    return GEFalse; // the one exception: a point magic projectile of the human form
                static GEInt s_iHeroLines = 0;
                if (s_iHeroLines < 60)
                {
                    ++s_iHeroLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Damage of the hero ('%s', %s%s) on '%s' (party type %d) is "
                                                 "blocked.\n",
                                                 a_Attacker.GetName().GetText(), bForm ? "transformed" : "human form",
                                                 a_bAreaDamage ? ", area" : "", a_Victim.GetName().GetText(),
                                                 static_cast<int>(a_Victim.Party.PartyMemberType));
                }
                return GETrue;
            }
            static GEInt s_iLines = 0;
            if (s_iLines < 60)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Friendly fire inside the army of the hero is blocked: '%s' "
                                             "(party type %d) -> '%s' (party type %d).\n",
                                             a_Attacker.GetName().GetText(),
                                             static_cast<int>(a_Attacker.Party.PartyMemberType),
                                             a_Victim.GetName().GetText(),
                                             static_cast<int>(a_Victim.Party.PartyMemberType));
            }
            return GETrue;
        }
        GEBool IsHeroInBeastForm()
        {
            return IsHeroTransformedNow();
        }
        GEBool IsPrimaryAggressionOnSummon(Entity &a_Self, Entity &a_Other)
        {
            if (a_Self == None || a_Other == None || !a_Self.IsNPC() || !a_Other.IsNPC())
                return GEFalse;
            if (a_Self == Entity::GetPlayer() || a_Self == a_Other || IsHeroControlled(a_Self))
                return GEFalse;
            if (a_Self.NPC.PoliticalAlignment != gEPoliticalAlignment_None)
                return GEFalse;
            if (a_Other.Party.PartyMemberType != gEPartyMemberType_Summoned || !IsHeroControlled(a_Other))
                return GEFalse;
            // The summon has already hurt him: the fight is legal.
            if (a_Self.NPC.GetCurrentAttacker() == a_Other || a_Self.NPC.GetLastAttacker() == a_Other)
                return GEFalse;
            return GETrue;
        }

        GEBool IsDefendedTarget(bCString const &a_strName)
        {
            GEU32 const u32Now = ::GetTickCount();
            for (GEInt i = 0; i < DEFEND_MAX; ++i)
                if (g_arrDefendTick[i] != 0 && g_arrDefendNames[i] == a_strName &&
                    static_cast<GEInt>(u32Now - g_arrDefendTick[i]) < static_cast<GEInt>(DEFEND_MS))
                    return GETrue;
            return GEFalse;
        }

        void ClearDefendedTargets()
        {
            // The full reset of the fight records (v1.0.30.0: they used to be wiped on EVERY attitude query, inside
            // IsDefendedTarget, which also erased the timer "the hero struck" and the records of the brawl).
            ClearFocusRecords();
            ClearCombatRecords();
            ClearBrawls();
            for (GEInt i = 0; i < DEFEND_MAX; ++i)
            {
                g_arrDefendNames[i] = bCString();
                g_arrDefendTick[i] = 0;
            }
        }

        void RememberDefendedTarget(bCString const &a_strName)
        {
            GEU32 const u32Now = ::GetTickCount();
            GEInt iFree = -1;
            for (GEInt i = 0; i < DEFEND_MAX; ++i)
            {
                if (g_arrDefendTick[i] != 0 && g_arrDefendNames[i] == a_strName)
                {
                    g_arrDefendTick[i] = u32Now;
                    return;
                }
                if (iFree < 0 && (g_arrDefendTick[i] == 0 ||
                                  static_cast<GEInt>(u32Now - g_arrDefendTick[i]) >= static_cast<GEInt>(DEFEND_MS)))
                    iFree = i;
            }
            if (iFree < 0)
                iFree = 0;
            g_arrDefendNames[iFree] = a_strName;
            g_arrDefendTick[iFree] = u32Now;
        }

        void NoteFocusRecord(bCString const &a_strEnemy, bCString const &a_strVictim)
        {
            GEU32 const u32Now = ::GetTickCount();
            GEInt iFree = -1;
            for (GEInt i = 0; i < FOCUS_MAX; ++i)
            {
                if (g_arrFocusTick[i] != 0 && g_arrFocusEnemy[i] == a_strEnemy && g_arrFocusVictim[i] == a_strVictim)
                {
                    g_arrFocusTick[i] = u32Now;
                    return;
                }
                if (iFree < 0 && (g_arrFocusTick[i] == 0 ||
                                  static_cast<GEInt>(u32Now - g_arrFocusTick[i]) >= static_cast<GEInt>(FOCUS_MS)))
                    iFree = i;
            }
            if (iFree < 0)
                iFree = 0;
            g_arrFocusEnemy[iFree] = a_strEnemy;
            g_arrFocusVictim[iFree] = a_strVictim;
            g_arrFocusTick[iFree] = u32Now;
        }

        GEBool HasFocusRecord(bCString const &a_strEnemy, bCString const &a_strVictim)
        {
            GEU32 const u32Now = ::GetTickCount();
            for (GEInt i = 0; i < FOCUS_MAX; ++i)
                if (g_arrFocusTick[i] != 0 && g_arrFocusEnemy[i] == a_strEnemy && g_arrFocusVictim[i] == a_strVictim &&
                    static_cast<GEInt>(u32Now - g_arrFocusTick[i]) < static_cast<GEInt>(FOCUS_MS))
                    return GETrue;
            return GEFalse;
        }

        void ClearFocusRecords()
        {
            for (GEInt i = 0; i < FOCUS_MAX; ++i)
            {
                g_arrFocusEnemy[i] = bCString();
                g_arrFocusVictim[i] = bCString();
                g_arrFocusTick[i] = 0;
            }
        }

        bool CanRetargetSummon(Entity &summon, Entity &target)
        {
            if (summon.Party.PartyMemberType != gEPartyMemberType_Summoned ||
                summon.NPC.CombatState != 0 || (target != None && !target.IsDead()) ||
                !summon.Routine.IsValid() || !HasTask(summon, "ZS_Attack"))
                return false;

            bCString const state = summon.Routine.GetCurrentState();
            return state == "ZS_Attack_End" || state == "ZS_Attack_Wait" ||
                state == "ZS_Attack_GoBack" || state == "ZS_Attack_Goto";
        }

        // a_Victim: the member of the army (or the hero) who is attacked / taken into focus, a_Attacker: the outsider.
        // Every free (or waiting) creature of the army in sight takes the outsider for an enemy and attacks him.
        void DefendAllies(Entity &a_Victim, Entity &a_Attacker)
        {
            if (ArenaJudge::Punishment::IsArenaPunishmentBlow(a_Victim, a_Attacker)) return;

            static GEBool busy = GEFalse;
            if (busy || a_Victim == None || a_Attacker == None || a_Victim == a_Attacker)
                return;
            // Internal player/party fights are never a request for assistance.
            if (!a_Attacker.IsNPC() || a_Attacker.IsDead() || IsHeroSide(a_Attacker) || IsHeroControlled(a_Attacker))
                return;
            bool const defendingHero = IsHeroSide(a_Victim);
            if (!defendingHero && !IsHeroControlled(a_Victim))
            {
                if (a_Victim.IsNPC() && a_Victim.Party.PartyMemberType != gEPartyMemberType_None)
                {
                    Entity leader = a_Victim.Party.GetPartyLeader();
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][defence] victim '%s' SKIPPED: not recognised as the player's party, leader='%s', transformed=%d\n",
                        a_Victim.GetName().GetText(), leader != None ? leader.GetName().GetText() : "<none>", static_cast<int>(IsHeroTransformedNow()));
                }
                return;
            }
            if (IsDuelTaboo(a_Attacker))
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][defence] attacker '%s' SKIPPED: duel protection\n", a_Attacker.GetName().GetText());
                return;
            }
            Entity Opponent;
            if (FindArenaOpponent(Opponent) && Opponent == a_Attacker)
                return;
            busy = GETrue;
            Entity Hero = Entity::GetPlayer();
            if (!defendingHero) RememberDefendedTarget(a_Attacker.GetName());
            GEBool const quest = HasRunningArenaQuest() || WasArenaDuelActive();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][defence] attacker='%s' victim='%s' player='%s'\n",
                a_Attacker.GetName().GetText(), a_Victim.GetName().GetText(), Hero != None ? Hero.GetName().GetText() : "<none>");
            GEInt helpers = 0;
            bTObjArray<Entity> npcs = Entity::GetNPCs();
            for (GEInt i = 0; i < npcs.GetCount(); ++i)
            {
                Entity Ally = npcs.GetAt(i);
                if (Ally == None || Ally == a_Victim || Ally == a_Attacker || IsHeroSide(Ally) || !Ally.IsNPC())
                    continue;
                bool const isSummon = Ally.Party.IsValid() && Ally.Party.PartyMemberType == gEPartyMemberType_Summoned;
                if (defendingHero && !isSummon) continue;
                if (!IsHeroControlled(Ally))
                {
                    if (Ally.Party.PartyMemberType != gEPartyMemberType_None)
                    {
                        Entity leader = Ally.Party.GetPartyLeader();
                        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][defence] '%s' SKIPPED: not recognised as the player's party, leader='%s'\n",
                            Ally.GetName().GetText(), leader != None ? leader.GetName().GetText() : "<none>");
                    }
                    continue;
                }
                GELPCChar reason = nullptr;
                GEBool const waiting = Ally.Party.Waiting;
                Entity previousTarget = Ally.NPC.GetCurrentTarget();
                bool retargeting = false;
                if (Ally.IsDead() || Ally.IsDown()) reason = "dead or knocked down";
                else if (IsBrawlFighting(Ally)) reason = "personal brawl with the player";
                else if (previousTarget == a_Attacker && HasTask(Ally, "ZS_Attack")) reason = "already attacking this enemy";
                else if (Ally.GetDistanceTo(a_Attacker) > DEFEND_RADIUS) reason = "outside defence radius";
                else if (!Ally.IsFreeLOS(a_Attacker, GEFalse)) reason = "no line of sight";
                else if (!waiting && !IsPeaceful(Ally))
                {
                    retargeting = CanRetargetSummon(Ally, previousTarget);
                    if (!retargeting) reason = "already in another combat or reaction";
                }
                else if (waiting && quest && IsInsideArenaZone(Ally)) reason = "waiting at an active arena";
                if (isSummon)
                {
                    Entity owner = Ally.Interaction.IsValid() ? Ally.Interaction.GetOwner() : None;
                    DebugLog::GetLogger().LogFormat(
                        "[GenomeLogicFix][summon-defence] '%s' id=%s owner='%s' victim='%s' attacker='%s' target='%s' task='%s' state='%s' combat=%d: %s.\n",
                        Ally.GetName().GetText(), Ally.GetGameEntity()->GetID().GetText().GetText(),
                        owner != None ? owner.GetName().GetText() : "<none>", a_Victim.GetName().GetText(),
                        a_Attacker.GetName().GetText(), previousTarget != None ? previousTarget.GetName().GetText() : "<none>",
                        Ally.Routine.GetCurrentTask().GetText(), Ally.Routine.GetCurrentState().GetText(),
                        static_cast<int>(Ally.NPC.CombatState), reason ? reason : "defence order accepted");
                }
                if (reason)
                {
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][defence] '%s' SKIPPED: %s\n", Ally.GetName().GetText(), reason);
                    continue;
                }
                if (waiting)
                {
                    ClearWaiterMarksPublic(Ally);
                    Ally.Party.Waiting = GEFalse;
                }
                if (retargeting)
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix][summon-target] '%s' switches from '%s' to '%s' to defend '%s'.\n",
                        Ally.GetName().GetText(), previousTarget != None ? previousTarget.GetName().GetText() : "<none>",
                        a_Attacker.GetName().GetText(), a_Victim.GetName().GetText());
                if (defendingHero && helpers == 0) RecordCombat(a_Victim, a_Attacker);
                Ally.Routine.FullStop();
                AI_Perception::AssessTarget(Ally, a_Attacker, gEAttackReason_Enemy);
                if (!(Ally.NPC.GetCurrentTarget() == a_Attacker) || !HasTask(Ally, "ZS_Attack"))
                {
                    Ally.NPC.SetCurrentTarget(a_Attacker);
                    Ally.NPC.AttackReason = gEAttackReason_Enemy;
                    Ally.Routine.SetTask("ZS_Attack");
                }
                ++helpers;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][defence] '%s' HELPS '%s' against '%s'\n",
                    Ally.GetName().GetText(), a_Victim.GetName().GetText(), a_Attacker.GetName().GetText());
            }
            busy = GEFalse;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Mutual defence: '%s' attacks '%s' -> %d helper(s).\n",
                a_Attacker.GetName().GetText(), a_Victim.GetName().GetText(), helpers);
        }

        // A tamed animal or a dominated person / orc of the hero (party member type 4), alive and on his feet.
        GEBool IsTamedOrDominated(Entity &a_Npc)
        {
            if (a_Npc == None || !a_Npc.IsNPC() || a_Npc.IsDead() || a_Npc.IsDown())
                return GEFalse;
            if (!IsHeroControlled(a_Npc))
                return GEFalse;
            return a_Npc.Party.PartyMemberType == gEPartyMemberType_Controlled ? GETrue : GEFalse;
        }

        // The partner of the running dialogue is such a creature.
        GEBool GetDialogPartner(gCInfo_PS const *a_pInfo, Entity &o_Npc)
        {
            gCInfoManager_PS *pManager = a_pInfo->m_pInfoManager ? a_pInfo->m_pInfoManager
                                                                  : gCInfoManager_PS::GetInstance();
            if (!pManager)
                return GEFalse;
            gCEntity *pPartner = pManager->GetDialogPartnerNPC();
            if (!pPartner)
                return GEFalse;
            o_Npc = Entity(static_cast<eCEntity const *>(pPartner));
            if (!IsTamedOrDominated(o_Npc))
                return GEFalse;
            return GETrue;
        }

        void OrderWait(Entity &a_Npc)
        {
            CancelPersonalSpectator(a_Npc, GETrue);
            EndBrawlOf(a_Npc, "dialogue: wait here"); // the combat state is wiped
            MarkWaiterUserPublic(a_Npc);              // v1.0.68.0: the player's order
            a_Npc.Party.Waiting = GETrue;
            a_Npc.Routine.FullStop();
            a_Npc.Routine.SetTask("ZS_WaitForPlayer");
            PrintCompanionWaitMessage(a_Npc);
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Dialogue: '%s' (party type %d) is told to wait here.\n",
                                         a_Npc.GetName().GetText(), static_cast<int>(a_Npc.Party.PartyMemberType));
        }

        void OrderFollow(Entity &a_Npc)
        {
            ClearWaiterMarksPublic(a_Npc); // v1.0.68.0
            a_Npc.Party.Waiting = GEFalse;
            CancelPersonalSpectator(a_Npc, GETrue);
            EndBrawlOf(a_Npc, "dialogue: follow me"); // wipes the combat state, sets ZS_FollowPlayer + ContinueRoutine
            a_Npc.Party.Waiting = GEFalse;
            a_Npc.Routine.SetTask("ZS_FollowPlayer");
            a_Npc.Routine.ContinueRoutine();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix] Dialogue: '%s' (party type %d) is told to follow the hero.\n",
                                         a_Npc.GetName().GetText(), static_cast<int>(a_Npc.Party.PartyMemberType));
        }
        // The hero in a transformed body (the player is the spawned creature, or the body "Transform_*"): the hidden
        // human body "PC_Hero" is the entity that the native fight scripts were written for. Fills a_Body with it.
        GEBool FindHumanBodyFor(Entity &a_Attacker, Entity &a_Body)
        {
            if (a_Attacker == None || !a_Attacker.IsNPC())
                return GEFalse;
            GEBool const bForm = IsHeroFormEntity(a_Attacker) ||
                                 (a_Attacker == Entity::GetPlayer() && IsHeroTransformedNow());
            if (!bForm)
                return GEFalse;
            Entity Body = Entity::GetEntity(bCString("PC_Hero"));
            if (Body == None || Body == a_Attacker || !Body.IsNPC() || Body.IsDead())
                return GEFalse;
            a_Body = Body;
            return GETrue;
        }
void ResetFleeWatch()
{
            for (GEInt i = 0; i < FLEE_WATCH_MAX; ++i)
            {
                g_arrFleeNames[i] = bCString();
                g_arrFleeStart[i] = 0;
                g_arrFleeStage[i] = 0;
            }
            g_bFleeWatchActive = GEFalse;
}

}

namespace GroupLogic::Waiting
{
    namespace {
        using namespace GroupLogic::Brawl;
        using namespace GroupLogic::Combat;
        constexpr GEInt INFO_COND_JOIN = 14, INFO_COND_WAIT = 34, INFO_COND_DISMISS = 15, INFO_COND_MOBDISMISS = 33;
        Entity ResolveEntity(GELPVoid entity, eCEntity *fallback)
        { return entity ? *static_cast<Entity *>(entity) : Entity(fallback); }
        GEInt GetInfoCondType(gCInfo_PS const *info)
        { return *reinterpret_cast<GEInt const *>(reinterpret_cast<GEU8 const *>(info) + 0x28); }
        GEBool IsHeroInfo(gCInfo_PS const *info)
        { return _stricmp(info->GetOwner().GetText(), "PC_Hero") == 0 ? GETrue : GEFalse; }
        void MarkWaiterAutoPublic(Entity &npc) { MarkWaiterAuto(npc); }
        void MarkWaiterUserPublic(Entity &npc) { MarkWaiterUser(npc); }
        void ClearWaiterMarksPublic(Entity &npc) { ClearWaiterMarks(npc); }
    }
}

namespace GroupLogic::Waiting
{
    using mEWaiterType = GroupLogic::WaiterType;
    constexpr auto mEWaiterType_None = WaiterType_None;
    constexpr auto mEWaiterType_User = WaiterType_User;
    constexpr auto mEWaiterType_Auto = WaiterType_Auto;
    namespace {
        GEInt const WTYPE_MAX = 48;
        bCString g_arrUserWait[WTYPE_MAX];
        GEInt g_iUserWait = 0;
        bCString g_arrAutoWait[WTYPE_MAX];
        GEInt g_iAutoWait = 0;

        GEInt FindNameIn(bCString const *a_pArray, GEInt a_iCount, bCString const &a_strName)
        {
            for (GEInt i = 0; i < a_iCount; ++i)
                if (a_pArray[i] == a_strName)
                    return i;
            return -1;
        }

        void AddNameTo(bCString *a_pArray, GEInt &a_iCount, GEInt a_iMax, bCString const &a_strName)
        {
            if (a_iCount >= a_iMax || FindNameIn(a_pArray, a_iCount, a_strName) >= 0)
                return;
            a_pArray[a_iCount++] = a_strName;
        }

        void RemoveNameFrom(bCString *a_pArray, GEInt &a_iCount, bCString const &a_strName)
        {
            GEInt const iIndex = FindNameIn(a_pArray, a_iCount, a_strName);
            if (iIndex < 0)
                return;
            a_pArray[iIndex] = a_pArray[a_iCount - 1];
            a_pArray[a_iCount - 1] = bCString();
            --a_iCount;
        }
    }

        mEWaiterType GetWaiterType(Entity &a_Npc)
        {
            bCString const strName = a_Npc.GetName();
            if (FindNameIn(g_arrUserWait, g_iUserWait, strName) >= 0)
                return mEWaiterType_User;
            if (Host::HasArenaWaiterName(strName) || FindNameIn(g_arrAutoWait, g_iAutoWait, strName) >= 0)
                return mEWaiterType_Auto;
            return mEWaiterType_None;
        }
        GELPCChar WaiterTypeText(mEWaiterType a_enuType)
        {
            return a_enuType == mEWaiterType_User ? "USER" : (a_enuType == mEWaiterType_Auto ? "AUTO" : "none");
        }

        void ClearWaiterMarks(Entity &a_Npc)
        {
            bCString const strName = a_Npc.GetName();
            RemoveNameFrom(g_arrUserWait, g_iUserWait, strName);
            RemoveNameFrom(g_arrAutoWait, g_iAutoWait, strName);
            Host::RemoveArenaWaiterName(strName);
        }

        // The player ordered "wait here": the wait is his, the plugin never breaks it.
        void MarkWaiterUser(Entity &a_Npc)
        {
            ClearWaiterMarks(a_Npc);
            AddNameTo(g_arrUserWait, g_iUserWait, WTYPE_MAX, a_Npc.GetName());
        }

        // The plugin / the game ordered the wait: the next teleport carries him.
        void MarkWaiterAuto(Entity &a_Npc)
        {
            bCString const strName = a_Npc.GetName();
            RemoveNameFrom(g_arrUserWait, g_iUserWait, strName);
            AddNameTo(g_arrAutoWait, g_iAutoWait, WTYPE_MAX, strName);
        }
    void ResetWaiterMarks()
    {

            for (GEInt i = 0; i < WTYPE_MAX; ++i)
            {
                g_arrUserWait[i] = bCString();
                g_arrAutoWait[i] = bCString();
            }
            g_iUserWait = 0;
            g_iAutoWait = 0;
    }
    void RemoveUserMark(bCString const &name) { RemoveNameFrom(g_arrUserWait, g_iUserWait, name); }
    GEBool HasUserMark(bCString const &name) { return FindNameIn(g_arrUserWait, g_iUserWait, name) >= 0; }
    void RemoveAutoMark(bCString const &name) { RemoveNameFrom(g_arrAutoWait, g_iAutoWait, name); }
    GEBool HasAutoMark(bCString const &name) { return FindNameIn(g_arrAutoWait, g_iAutoWait, name) >= 0; }
    GEInt MagicTeleport(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Caster = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (Caster == None || !GroupLogic::Combat::IsHeroSide(Caster))
            return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);

        // Before the native script: who of the types 1..3 follows the hero (the native script puts them to wait and prints
        // "<name> is waiting"). The types 4 / 5 are left to the native script completely (v1.0.50.0: no tracker, no messages).
        Entity Hero = Entity::GetPlayer();
        bool const recall = Caster == Hero && Caster.NPC.IsValid() &&
            bCString(Caster.NPC.LastSpell) == "Spl_Teleport_Broken";
        if (!recall)
            BeginPersonalTeleport();
        CollectTeleportFollowers();
        GroupLogic::SetTeleportScriptRunning(GETrue); // the plaque "<name> is waiting" of a collected companion is held back meanwhile
        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        GroupLogic::SetTeleportScriptRunning(GEFalse);
        if (recall)
        {
            MoveTeleportFollowers("Spl_Teleport_Broken");
            auto const members = Hero.Party.GetMembers(GEFalse);
            for (GEInt index = 0; index < members.GetCount(); ++index)
            {
                Entity member(static_cast<eCEntity const *>(members.GetAt(index)));
                if (member == None || !member.NPC.IsValid() || member.IsDead() ||
                    PluginRuntime::IsTeleportNameIgnored(member.GetName().GetText()) ||
                    (member.Party.Waiting && Host::GetWaiterType(member) != WaiterType_Auto))
                    continue;
                auto const type = static_cast<gEPartyMemberType>(member.Party.PartyMemberType);
                auto const species = static_cast<gESpecies>(member.NPC.Species);
                if (type == gEPartyMemberType_Summoned ||
                    (type == gEPartyMemberType_Controlled && species != gESpecies_Human && species != gESpecies_Orc))
                {
                    if (member.GetDistanceTo(Hero) > TELEPORT_FOLLOWER_MIN_DISTANCE)
                        member.Teleport(Hero);
                }
            }
        }
        else
        {
            FinishTeleportFollowers("MagicTeleport");
            PollPersonalTeleport();
        }
        return iResult;
        }
    GEInt WaitPlayerParty(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Commanded = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (Commanded != None) CancelPersonalSpectator(Commanded, GETrue);
        if (GroupLogic::IsTeleportScriptRunning())
        {
            Entity Member = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
            if (Member != None && IsTeleportFollower(Member))
                return 1; // he is carried to the hero: neither the plaque nor the waiting
            if (Member != None && Member != Entity::GetPlayer() && TryLateCollect(Member))
                return 1; // v1.0.69.0: the net - the collection missed him, he is carried all the same
            // v1.0.68.0: the native script puts the rest of the party to wait during the cast: that wait is the GAME's (AUTO), not the
            // player's - unless he already waited before the cast (the order of the player, kept as it is).
            if (Member != None && !Member.Party.Waiting)
            {
                GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
                if (Member.Party.Waiting)
                {
                    MarkWaiterAutoPublic(Member);
                    Entity NativeWaitHero = Entity::GetPlayer();
                    LogTeleportRecord("native wait", Member, NativeWaitHero, "AUTO",
                                      "the native script WaitPlayerParty put him to wait during the cast; the next stone carries him");
                }
                return iResult;
            }
        }
        return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
    GEBool GE_STDCALL InfoExecute(gCInfo_PS *a_pThis, gCInfoManager_PS *a_pManager, InfoExecuteOriginal original)
    {

            using mFOriginal = GEBool(GE_STDCALL *)(gCInfo_PS *, gCInfoManager_PS *);
            if (a_pThis && IsHeroInfo(a_pThis))
            {
                GEInt const iType = GetInfoCondType(a_pThis);
                Entity Npc;
                if ((iType == INFO_COND_WAIT || iType == INFO_COND_JOIN) && GetDialogPartner(a_pThis, Npc))
                {
                    if (iType == INFO_COND_WAIT)
                        OrderWait(Npc);
                    else
                        OrderFollow(Npc);
                    gCInfoManager_PS *pManager = a_pManager ? a_pManager : a_pThis->m_pInfoManager;
                    if (pManager)
                        pManager->EndInfoManager(nullptr);
                    *(reinterpret_cast<GEU8 *>(a_pThis) + 0x18) = 0; // the line can be chosen again and again
                    return GETrue;
                }
                // v1.0.68.0: THE ORDER OF THE PLAYER FOR AN ORDINARY COMPANION (party type 1..3). The native script runs unchanged; here only
                // the source of the order is written down: "Wait here" -> WaiterType USER (never carried by a teleport, never released by the
                // plugin), "Follow me" -> the mark is dropped.
                if (iType == INFO_COND_WAIT || iType == INFO_COND_JOIN)
                {
                    gCInfoManager_PS *pPartnerManager = a_pManager ? a_pManager : (a_pThis->m_pInfoManager ? a_pThis->m_pInfoManager
                                                                                                           : gCInfoManager_PS::GetInstance());
                    gCEntity *pPartner = pPartnerManager ? pPartnerManager->GetDialogPartnerNPC() : nullptr;
                    if (pPartner)
                    {
                        Entity Companion(static_cast<eCEntity const *>(pPartner));
                        if (Companion != None && Companion.IsNPC() && !Companion.IsDead() && IsHeroControlled(Companion))
                        {
                            if (iType == INFO_COND_WAIT)
                            {
                                MarkWaiterUserPublic(Companion);
                                char strLine[200] = {};
                                _snprintf_s(strLine, sizeof(strLine), _TRUNCATE,
                                            "[GenomeLogicFix][teleport] dialogue | '%s' | party type %d | told to WAIT by the player: WaiterType=USER",
                                            Companion.GetName().GetText(), static_cast<int>(Companion.Party.PartyMemberType));
                                DebugLog::GetLogger().LogFormat("%s\n", strLine);
                                CrashDiag::Note(strLine);
                            }
                            else
                                ClearWaiterMarksPublic(Companion);
                        }
                    }
                }
            }
            return original(a_pThis, a_pManager);
            }
}

namespace GroupLogic::Combat
{
    using namespace GroupLogic::Brawl;
    GEInt ResolveTarget(Entity &Self, Entity &Other, Entity &Hero, gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {
        if (IsHeroControlled(Self))
        {
            if (IsDuelTaboo(Other))
            {
                static GEInt s_iTabooLines = 0;
                if (s_iTabooLines < 40)
                {
                    ++s_iTabooLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AssessTarget: '%s' (party type %d) does not see the duel "
                                                 "opponent '%s'.\n",
                                                 Self.GetName().GetText(), static_cast<int>(Self.Party.PartyMemberType),
                                                 Other.GetName().GetText());
                }
                return 0;
            }
            // The hidden human body of the hero counts as the master as well.
            GEBool const bMaster = Other == Hero || IsHeroFormEntity(Other) ||
                                   _stricmp(Other.GetName().GetText(), "PC_Hero") == 0;
            // THE MASTER BLOCK: until the SECOND blow of the hero (the counter of the brawl) the creature never takes him for a
            // target, the answer is a hard 0. The second blow opens the brawl slot, and only then the native answer passes.
            if (bMaster && !IsBrawlFighting(Self))
            {
                static GEInt s_iHeroLines = 0;
                if (s_iHeroLines < 40)
                {
                    ++s_iHeroLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AssessTarget: '%s' (controlled by the hero) does not "
                                                 "take his master '%s' for a target (transformed=%d).\n",
                                                 Self.GetName().GetText(), Other.GetName().GetText(),
                                                 static_cast<int>(IsHeroInBeastForm()));
                }
                return 0;
            }
            if (IsFriendlyFire(Self, Other))
                return 0;
            // The native rule of the army: no fight with an outsider before a REAL blow (he hurt the hero / the army, or
            // the hero / the army hurt him). A mob that has only decided to attack the hero and runs at him is left alone.
            if (ArmyMustNotAttack(Self, Other, a_iArgs))
            {
                static GEInt s_iNoLines = 0;
                if (s_iNoLines < 60)
                {
                    ++s_iNoLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AssessTarget: '%s' (party type %d) does not attack '%s': "
                                                 "no real blow yet (attack reason %d).\n",
                                                 Self.GetName().GetText(), static_cast<int>(Self.Party.PartyMemberType),
                                                 Other.GetName().GetText(), static_cast<int>(a_iArgs));
                }
                return 0;
            }
            return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
        if (IsHeroControlled(Other) && IsDuelTaboo(Self))
            return 0; // the duel opponent attacks nobody of the party of the hero
        if (IsPrimaryAggressionOnSummon(Self, Other))
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] AssessTarget: '%s' (no faction) ignores the summon '%s' of "
                                             "the hero until it hurts him first.\n",
                                             Self.GetName().GetText(), Other.GetName().GetText());
            }
            return 0;
        }
        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        // Native perception result: the outsider took a member of the army for his target = a recorded focus.
        if (Self.IsNPC() && !Self.IsDead() && !IsHeroSide(Self) && IsHeroControlled(Other) &&
            Self.NPC.GetCurrentTarget() == Other)
            NoteFocusRecord(Self.GetName(), Other.GetName());
        return iResult;
        }
    GEInt AdjustAttitude(Entity &Self, Entity &Other, GEInt iResult)
    {
        GEBool const bAggressive = iResult == gEAttitude_Angry || iResult == gEAttitude_Hostile ||
                                   iResult == gEAttitude_Predator;
        if (!IsHeroControlled(Self))
        {
            if (!bAggressive || (!IsPrimaryAggressionOnSummon(Self, Other) && !(IsHeroControlled(Other) && IsDuelTaboo(Self))))
                return iResult;
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] GetAttitude: '%s' (no faction) ignores the summon '%s' of "
                                             "the hero until it hurts him first (was %d).\n",
                                             Self.GetName().GetText(), Other.GetName().GetText(),
                                             static_cast<int>(iResult));
            }
            return gEAttitude_Neutral;
        }
        Entity Hero = Entity::GetPlayer();
        GEBool const bOtherIsHero = Other == Hero || IsHeroFormEntity(Other);
        if (bAggressive && IsDuelTaboo(Other))
            return gEAttitude_Neutral; // the opponent of the duel is invisible for the army
        if (bOtherIsHero && IsBrawlFighting(Self))
            return iResult; // the second blow opened the brawl: the native attitude to the hero is not overridden
        if (!bOtherIsHero && !IsHeroControlled(Other) && IsDefendedTarget(Other.GetName()))
            return gEAttitude_Hostile; // somebody attacks a member of the army: the army sees an enemy in him
        if (iResult == gEAttitude_Friendly)
            return iResult;
        if (bOtherIsHero || IsHeroControlled(Other))
        {
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] GetAttitude: '%s' (controlled by the hero) stays friendly "
                                             "to '%s' (was %d).\n",
                                             Self.GetName().GetText(), Other.GetName().GetText(),
                                             static_cast<int>(iResult));
            }
            return gEAttitude_Friendly;
        }
        return iResult;
        }
        // v1.0.34.0: a blocked blow leaves NO trace in the health bar. The hit points are taken off by the native script
        // AssessHit (it writes PSDamageReceiver::DamageAmount and calls AddHitPoints / SetHitPoints), and every blocked path
        // returns BEFORE the original script runs. The pending DamageAmount is also reset to 0, so a stale amount of an
        // earlier hit can never be applied to the companion by anything that reads it later.
        void ZeroPendingDamage(Entity &a_Victim)
        {
            if (a_Victim != None && a_Victim.DamageReceiver.IsValid())
                a_Victim.DamageReceiver.DamageAmount = 0;
        }
}

namespace GroupLogic::Waiting
{
    namespace {

        // v1.0.39.0: the text of the dismissal line: "Ты свободен." (any ending of . / ! / spaces).
        GEBool IsFreeLineText(bCUnicodeString const &a_strText)
        {
            static wchar_t const arrFree[] = L"\x0422\x044B \x0441\x0432\x043E\x0431\x043E\x0434\x0435\x043D"; // "Ты свободен"
            GELPCUnicodeChar pText = a_strText.GetText();
            size_t const uLen = wcslen(arrFree);
            if (!pText || wcsncmp(pText, arrFree, uLen) != 0)
                return GEFalse;
            for (pText += uLen; *pText; ++pText)
                if (*pText != L'.' && *pText != L'!' && *pText != L' ')
                    return GEFalse;
            return GETrue;
        }
    }
    GEBool GE_STDCALL InfoConditions(gCInfo_PS *a_pThis, InfoConditionsOriginal original)
    {

            using mFOriginal = GEBool(GE_STDCALL *)(gCInfo_PS *);
            // v1.0.39.0: FILTER BY TEXT. While a dialogue with a tamed / dominated creature (type 4) runs, the lines whose text
            // is "Ты свободен." are counted: exactly ONE of them stays - the first info of that text (in the list of the
            // manager) that belongs to the hero or to the partner himself; every other one is hidden, whatever its condition.
            if (a_pThis)
            {
                gCInfoManager_PS *pManager = a_pThis->m_pInfoManager ? a_pThis->m_pInfoManager
                                                                      : gCInfoManager_PS::GetInstance();
                GEBool const bRunning = pManager && *(reinterpret_cast<GEU8 const *>(pManager) + 0x14) != 0;
                Entity Partner;
                if (bRunning && GetDialogPartner(a_pThis, Partner) && IsFreeLineText(a_pThis->GetDialogDescription()))
                {
                    bCString const strPartner = Partner.GetName();
                    // The keeper: the info of the native PartyDismiss (cond 15, whose script handles type 4) when it carries
                    // the text, otherwise the first info of that text.
                    gCInfo_PS *pKeeper = nullptr;
                    gCInfo_PS *pFirst = nullptr;
                    bTPtrArray<gCInfo_PS *> const &arrInfos = pManager->GetInfos();
                    for (GEInt i = 0; i < arrInfos.GetCount(); ++i)
                    {
                        gCInfo_PS *pInfo = arrInfos.GetAt(i);
                        if (!pInfo)
                            continue;
                        GEBool const bHeroOrPartner = IsHeroInfo(pInfo) || pInfo->GetOwner() == strPartner;
                        if (!bHeroOrPartner || !IsFreeLineText(pInfo->GetDialogDescription()))
                            continue;
                        if (!pFirst)
                            pFirst = pInfo;
                        if (!pKeeper && GetInfoCondType(pInfo) == INFO_COND_DISMISS)
                            pKeeper = pInfo;
                    }
                    if (!pKeeper)
                        pKeeper = pFirst;
                    return pKeeper == a_pThis ? GETrue : GEFalse;
                }
            }
            if (a_pThis && IsHeroInfo(a_pThis))
            {
                GEInt const iType = GetInfoCondType(a_pThis);
                Entity Npc;
                if ((iType == INFO_COND_WAIT || iType == INFO_COND_JOIN || iType == INFO_COND_DISMISS ||
                     iType == INFO_COND_MOBDISMISS) &&
                    GetDialogPartner(a_pThis, Npc))
                {
                    // v1.0.37.0: MUTUALLY EXCLUSIVE lines by the flag Party.Waiting: a creature that follows the hero offers
                    // ONLY "Wait here", a creature that waits offers ONLY "Follow me".
                    GEBool const bWaiting = Npc.Party.Waiting;
                    if (iType == INFO_COND_WAIT)
                        return bWaiting ? GEFalse : GETrue;
                    if (iType == INFO_COND_JOIN)
                        return bWaiting ? GETrue : GEFalse;
                    // v1.0.37.0: EXACTLY ONE "You are free." Two native infos carry the line (PartyDismiss, cond 15, and
                    // MobDismiss, cond 33): the first stays (always offered), the duplicate is hidden.
                    return iType == INFO_COND_DISMISS ? GETrue : GEFalse;
                }
            }
            return original(a_pThis);
            }
    bCUnicodeString *GE_STDCALL InfoDescription(gCInfo_PS *a_pThis, bCUnicodeString *a_pRet, InfoDescriptionOriginal original)
    {

            using mFOriginal = bCUnicodeString *(GE_STDCALL *)(gCInfo_PS *, bCUnicodeString *);
            bCUnicodeString *pResult = original(a_pThis, a_pRet);
            if (a_pThis && a_pRet && IsHeroInfo(a_pThis))
            {
                GEInt const iType = GetInfoCondType(a_pThis);
                Entity Npc;
                if ((iType == INFO_COND_WAIT || iType == INFO_COND_JOIN) && GetDialogPartner(a_pThis, Npc))
                {
                    if (iType == INFO_COND_WAIT)
                        *a_pRet = L"\x0416\x0434\x0438 \x0437\x0434\x0435\x0441\x044C"; // "Жди здесь"
                    else
                        *a_pRet = L"\x0418\x0434\x0438 \x0437\x0430 \x043C\x043D\x043E\x0439"; // "Иди за мной"
                }
            }
            return pResult;
            }
    GEBool GE_STDCALL EntityHasInfo(Entity *a_pThis, Entity const *a_pOther, EntityHasInfoOriginal original)
    {

            using mFOriginal = GEBool(GE_STDCALL *)(Entity *, Entity const *);
            GEBool const bResult = original(a_pThis, a_pOther);
            if (bResult || !a_pThis || !a_pOther || a_pThis->GetInstance() == nullptr || a_pOther->GetInstance() == nullptr)
                return bResult;
            Entity Hero = Entity::GetPlayer();
            Entity First(*a_pThis);
            Entity Second(*a_pOther);
            if (First == Hero && IsTamedOrDominated(Second))
                return GETrue;
            if (Second == Hero && IsTamedOrDominated(First))
                return GETrue;
            return bResult;
            }
}

namespace GroupLogic::Combat
{
        GEBool IsMasterDownVictim(Entity &a_Self, Entity &a_Other)
        {
            return a_Self != None && a_Other != None && a_Self.IsNPC() && IsHeroControlled(a_Self) && IsHeroSide(a_Other);
        }

        GEBool LeaveMasterAlone(gCScriptProcessingUnit *a_pSPU, GELPCChar a_strWhat)
        {
            Entity Self(a_pSPU->GetSelfEntity());
            if (Self == None || !Self.IsNPC())
                return GEFalse;
            Entity Target(a_pSPU->GetTargetEntity());
            Entity Current = Self.NPC.GetCurrentTarget();
            if (!IsMasterDownVictim(Self, Target) && !IsMasterDownVictim(Self, Current))
                return GEFalse;
            static GEInt s_iLines = 0;
            if (s_iLines < 40)
            {
                ++s_iLines;
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix] %s: '%s' (party type %d) does not touch his master.\n",
                                             a_strWhat, Self.GetName().GetText(),
                                             static_cast<int>(Self.Party.PartyMemberType));
            }
            Self.Routine.FullStop();
            Self.NPC.SetCurrentTarget(None);
            Self.NPC.AttackReason = gEAttackReason_None;
            Self.Routine.ContinueRoutine();
            return GETrue;
        }
}

namespace GroupLogic::Lifecycle
{
    using namespace GroupLogic::Combat;
    using namespace GroupLogic::Brawl;
    namespace {
        Entity ResolveEntity(GELPVoid entity, eCEntity *fallback)
        { return entity ? *static_cast<Entity *>(entity) : Entity(fallback); }

        constexpr char SecondarySummonPrefix[] = "GLF_SecondarySummon_V1|";
        struct SecondarySummon
        {
            bCPropertyID caster;
            bCPropertyID member;
        };
        std::vector<SecondarySummon> pendingSummons;
        std::vector<SecondarySummon> secondarySummons;
        bool secondarySummonsLoaded = false;
        GEU32 lastSecondaryPoll = 0;
        GELPCChar secondaryQueueWaitReason = nullptr;
        GEBool heroLossSummonsHandled = GEFalse;
        std::vector<bCPropertyID> heroLossSummons;
        GEU32 lastSummonTargetReview = 0;
        thread_local unsigned heroSummonQueryDepth = 0;

        struct HeroSummonQuery
        {
            explicit HeroSummonQuery(bool active) : active(active)
            {
                if (active) ++heroSummonQueryDepth;
            }

            ~HeroSummonQuery()
            {
                if (active) --heroSummonQueryDepth;
            }

            bool const active;
        };

        gCPlayerMemory_PS *SecondaryMemory(Entity &hero)
        {
            return GetPropertySet<gCPlayerMemory_PS>(static_cast<eCEntity *>(hero), eEPropertySetType_PlayerMemory);
        }

        gCParty_PS *SecondaryParty(Entity &hero)
        {
            return GetPropertySet<gCParty_PS>(static_cast<eCEntity *>(hero), eEPropertySetType_Party);
        }

        bCString SecondaryRecord(SecondarySummon const &summon)
        {
            bCString record(SecondarySummonPrefix);
            record += summon.caster.GetText();
            record += "|";
            record += summon.member.GetText();
            return record;
        }

        void LoadSecondarySummons(gCPlayerMemory_PS &memory)
        {
            if (secondarySummonsLoaded) return;
            for (auto const &event : memory.GetPlayerKnows())
            {
                std::string const record(event.GetText());
                constexpr auto start = sizeof(SecondarySummonPrefix) - 1;
                if (record.compare(0, start, SecondarySummonPrefix) != 0) continue;
                auto const separator = record.find('|', start);
                SecondarySummon summon;
                if (separator != std::string::npos &&
                    summon.caster.SetText(record.substr(start, separator - start).c_str()) &&
                    summon.member.SetText(record.substr(separator + 1).c_str()))
                    secondarySummons.push_back(summon);
            }
            secondarySummonsLoaded = true;
        }

        bool IsSecondaryCaster(Entity &caster, Entity &hero)
        {
            if (caster == None || caster == hero || !caster.IsNPC() || caster.IsDead() ||
                !caster.Party.IsValid() || !IsHeroControlled(caster))
                return false;
            gEPartyMemberType const type = caster.Party.GetProperty<PSParty::PropertyPartyMemberType>();
            return type == gEPartyMemberType_Party || type == gEPartyMemberType_Summoned;
        }

        bool IsSecondaryMember(Entity &member)
        {
            return member != None && member.IsNPC() && !member.IsDead() && !member.GetGameEntity()->IsKilled() &&
                member.Party.IsValid() && member.Interaction.IsValid() &&
                member.Party.GetProperty<PSParty::PropertyPartyMemberType>() == gEPartyMemberType_Summoned;
        }

        bool HasPartyMember(gCParty_PS const &party, bCPropertyID const &id)
        {
            for (auto const &proxy : party.GetMembers())
                if (proxy.GetEntityID() == id) return true;
            return false;
        }

        void LogSecondaryQueueWait(GELPCChar reason)
        {
            if (pendingSummons.empty() || secondaryQueueWaitReason == reason) return;
            secondaryQueueWaitReason = reason;
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][secondary-summon] queue deferred: %s.\n", reason);
        }

        bool TransferSecondarySummon(Entity &hero, Entity &caster, Entity &member, gCParty_PS const &party)
        {
            if (!HasPartyMember(party, member.GetGameEntity()->GetID()))
            {
                if (!hero.Party.Add(member)) return false;
            }
            else if (!IsHeroControlled(member))
                member.Party.SetPartyLeader(hero);
            member.Interaction.SetOwner(caster);
            member.Party.AccessProperty<PSParty::PropertyPartyMemberType>() = gEPartyMemberType_Summoned;
            gEPoliticalAlignment const alignment = hero.NPC.GetProperty<PSNpc::PropertyPoliticalAlignment>();
            member.NPC.AccessProperty<PSNpc::PropertyPoliticalAlignment>() = alignment;
            if (member.Dialog.IsValid())
                member.Dialog.AccessProperty<PSDialog::PropertyPartyEnabled>() = GETrue;
            if (member.Routine.IsValid())
            {
                member.Routine.FullStop();
                member.Routine.SetTask("ZS_FollowPlayer");
            }
            return true;
        }

    }

    mCFunctionHook Hook_SecondarySummonType;

    void GE_STDCALL SecondarySummonType(gCParty_PS *party, bTPropertyContainer<gEPartyMemberType> const &type)
    {
        auto const original = Hook_SecondarySummonType.GetOriginalFunction(&SecondarySummonType);
        gEPartyMemberType const memberType = type;
        original(party, type);
        if (memberType != gEPartyMemberType_Summoned) return;

        auto *entity = party->GetEntity();
        if (!entity) return;
        Entity member(entity);
        if (member == None || !member.IsNPC() || !member.Party.IsValid()) return;
        Entity caster = member.Party.GetPartyLeader();
        if (caster != None && Combat::IsHeroSide(caster)) return;

        SecondarySummon summon;
        summon.member = member.GetGameEntity()->GetID();
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][secondary-summon] '%s' detected, leader='%s'.\n",
            member.GetName().GetText(), caster != None ? caster.GetName().GetText() : "<none>");
        pendingSummons.push_back(summon);
    }

    void ResetSecondarySummons()
    {
        pendingSummons.clear();
        secondarySummons.clear();
        secondarySummonsLoaded = false;
        lastSecondaryPoll = 0;
        secondaryQueueWaitReason = nullptr;
    }

    void AdjustSummonPartyMembers(PSParty *party, bTValArray<eCEntity *> &members)
    {
        if (!party || !party->m_pEngineEntityPropertySet) return;
        Entity caster(party->m_pEngineEntityPropertySet->GetEntity());
        Entity hero = BrawlMemoryOwner();
        if (hero == None) return;
        bool const heroSummonQuery = heroSummonQueryDepth != 0 && Combat::IsHeroSide(caster);
        if (!heroSummonQuery && !IsSecondaryCaster(caster, hero)) return;
        auto *memory = SecondaryMemory(hero);
        if (!memory) return;
        LoadSecondarySummons(*memory);
        if (secondarySummons.empty()) return;

        if (heroSummonQuery)
        {
            GEInt excluded = 0;
            for (GEInt i = members.GetCount() - 1; i >= 0; --i)
            {
                Entity member(members.GetAt(i));
                if (member == None) continue;
                auto const &id = member.GetGameEntity()->GetID();
                if (std::none_of(secondarySummons.begin(), secondarySummons.end(), [&](SecondarySummon const &summon) {
                        return summon.member == id;
                    }))
                    continue;
                members.RemoveAt(i);
                ++excluded;
            }
            if (excluded != 0)
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][summon-mana] excluded %d secondary summon(s) from the hero's summon query.\n",
                    excluded);
            return;
        }

        auto *partyOfHero = SecondaryParty(hero);
        if (!partyOfHero) return;
        auto const casterID = caster.GetGameEntity()->GetID();
        for (auto const &summon : secondarySummons)
        {
            if (summon.caster != casterID) continue;
            Entity member(summon.member);
            if (!IsSecondaryMember(member) || !HasPartyMember(*partyOfHero, summon.member)) continue;
            auto *entity = static_cast<eCEntity *>(member);
            if (!members.Contains(entity)) members.Add(entity);
        }
    }

    void UpdateSecondarySummons()
    {
        GEU32 const now = ::GetTickCount();
        if (pendingSummons.empty() && secondarySummonsLoaded &&
            (secondarySummons.empty() || now - lastSecondaryPoll < 100))
            return;
        Entity hero = BrawlMemoryOwner();
        if (hero == None)
        {
            LogSecondaryQueueWait("hero entity unavailable");
            return;
        }
        if (!hero.NPC.IsValid() || !hero.Party.IsValid())
        {
            LogSecondaryQueueWait(!hero.NPC.IsValid() ? "hero NPC property set unavailable" : "hero party unavailable");
            return;
        }
        if (heroLossSummonsHandled)
        {
            if (!pendingSummons.empty())
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][secondary-summon] queue cleared after hero defeat.\n");
            pendingSummons.clear();
            secondaryQueueWaitReason = nullptr;
            return;
        }
        auto *memory = SecondaryMemory(hero);
        auto *party = SecondaryParty(hero);
        if (!memory || !party)
        {
            LogSecondaryQueueWait(!memory ? "hero memory unavailable" : "hero party property set unavailable");
            return;
        }
        LoadSecondarySummons(*memory);
        lastSecondaryPoll = now;

        std::vector<SecondarySummon> candidates;
        candidates.swap(pendingSummons);
        secondaryQueueWaitReason = nullptr;
        if (!candidates.empty())
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][secondary-summon] session processing %u candidate(s).\n",
                static_cast<unsigned>(candidates.size()));
        for (auto &summon : candidates)
        {
            Entity member(summon.member);
            Entity caster = member != None && member.Party.IsValid() ? member.Party.GetPartyLeader() : None;
            GELPCChar rejection = nullptr;
            if (!IsSecondaryMember(member))
                rejection = "entity is unavailable, dead, or not configured as a summon";
            else if (!IsSecondaryCaster(caster, hero))
                rejection = "party leader is not an eligible caster in the hero's party";
            else if (member.Interaction.GetOwner() != caster)
                rejection = "interaction owner differs from the caster";
            else
            {
                summon.caster = caster.GetGameEntity()->GetID();
                if (!HasPartyMember(*party, summon.caster))
                    rejection = "caster is absent from the hero's party member list";
            }
            if (!rejection && std::any_of(secondarySummons.begin(), secondarySummons.end(), [&](auto const &known) {
                    return known.member == summon.member || known.member == summon.caster;
                }))
                rejection = "summon already registered or caster is a secondary summon";
            if (!rejection && !TransferSecondarySummon(hero, caster, member, *party))
                rejection = "hero's party rejected the transfer";

            if (rejection)
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][secondary-summon] '%s' from '%s' skipped: %s.\n",
                    member != None ? member.GetName().GetText() : "<unavailable>",
                    caster != None ? caster.GetName().GetText() : "<unavailable>", rejection);
                continue;
            }
            secondarySummons.push_back(summon);
            memory->AccessPlayerKnows().Add(SecondaryRecord(summon));
            memory->Modified();
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][secondary-summon] '%s' joined from '%s'.\n",
                member.GetName().GetText(), caster.GetName().GetText());
        }

        for (auto it = secondarySummons.begin(); it != secondarySummons.end();)
        {
            Entity caster(it->caster), member(it->member);
            bool const casterInParty = HasPartyMember(*party, it->caster);
            if (member == None || (caster == None && casterInParty))
            {
                ++it;
                continue;
            }
            if (IsSecondaryMember(member) && casterInParty && IsSecondaryCaster(caster, hero))
            {
                if (!HasPartyMember(*party, it->member) || !IsHeroControlled(member))
                    TransferSecondarySummon(hero, caster, member, *party);
                else if (member.Interaction.GetOwner() != caster)
                    member.Interaction.SetOwner(caster);
                ++it;
                continue;
            }
            memory->AccessPlayerKnows().Remove(SecondaryRecord(*it));
            memory->Modified();
            it = secondarySummons.erase(it);
            if (IsSecondaryMember(member))
                Routine_Commands::DismissPlayerParty(member);
        }
    }
    GEInt AssessTalk(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity self = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        Entity other = ResolveEntity(a_pOtherEntity, a_pSPU->GetOtherEntity());
        if (IsRecoveringPersonalCompanion(self) || IsRecoveringPersonalCompanion(other))
        {
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][dialogue] AssessTalk skipped for personal knockout: '%s' -> '%s'.\n",
                self != None ? self.GetName().GetText() : "-", other != None ? other.GetName().GetText() : "-");
            return 0;
        }
        DialogueMemberQuery query;
        return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
    GEInt GiveXP(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity defeated = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (ShouldSuppressBrawlXP(defeated))
        {
            DebugLog::GetLogger().LogFormat("[GenomeLogicFix][brawl] XP suppressed for personal victory over '%s' (amount=%d).\n",
                                           defeated.GetName().GetText(), a_iArgs);
            return 0;
        }
        return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
    GEInt JoinPlayerParty(gFScript original, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {

        Entity Commanded = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        if (Commanded != None) CancelPersonalSpectator(Commanded, GETrue);
        return original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
    GEBool ZS_RansackBody(AIStateOriginal original, bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU)
    {

        if (LeaveMasterAlone(a_pSPU, "ZS_RansackBody"))
            return GETrue;
        return original(a_rRunTimeStack, a_pSPU);
        }
    GEBool ZS_Attack_FinishingAttack(AIStateOriginal original, bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU)
    {

        Entity Self(a_pSPU->GetSelfEntity());
        Entity Target(a_pSPU->GetTargetEntity());
        if (IsPersonalBrawlPair(Self, Target) && !PlayerSupportsPersonalKnockout(Target))
            return original(a_rRunTimeStack, a_pSPU);
        if (LeaveMasterAlone(a_pSPU, "ZS_Attack_FinishingAttack"))
            return GETrue;
        return original(a_rRunTimeStack, a_pSPU);
        }
    GEInt SummonCast(gFScript original, GELPCChar name, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {
        Entity caster = ResolveEntity(a_pSelfEntity, a_pSPU->GetSelfEntity());
        bool heroCast = Combat::IsHeroSide(caster);
        if (!heroCast && caster != None && !caster.IsNPC() && caster.Interaction.IsValid())
        {
            Entity owner = caster.Interaction.GetOwner();
            heroCast = Combat::IsHeroSide(owner);
        }
        GEInt iResult;
        {
            HeroSummonQuery query(heroCast);
            iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        }
        ArenaJudge::Combat::OnHeroSummonCast(name, a_pSPU, a_pSelfEntity, a_pOtherEntity);
        return iResult;
    }
    GEInt HealCast(gFScript original, GELPCChar name, gCScriptProcessingUnit *a_pSPU, GELPVoid a_pSelfEntity, GELPVoid a_pOtherEntity, GEInt a_iArgs)
    {
        GEInt const iResult = original(a_pSPU, a_pSelfEntity, a_pOtherEntity, a_iArgs);
        ArenaJudge::Combat::OnHeroHealCast(name, a_pSPU, a_pSelfEntity);
        return iResult;
    }
}

namespace GroupLogic::Lifecycle
{
    namespace
    {
        GEBool IsConfirmedHeroLoss(Entity &hero)
        {
            if (hero == None) return GEFalse;
            if (hero.IsDead()) return GETrue;
            if (!hero.Routine.IsValid()) return GEFalse;
            bCString const task = hero.Routine.GetCurrentTask();
            bCString const state = hero.Routine.GetCurrentState();
            // Ordinary falls, hit reactions, paralysis and stand-up are not a defeat event.
            return _strnicmp(task.GetText(), "ZS_Unconscious", 14) == 0 ||
                   _strnicmp(state.GetText(), "ZS_Unconscious", 14) == 0 ||
                   _strnicmp(task.GetText(), "ZS_PiercedKO", 12) == 0 ||
                   _strnicmp(state.GetText(), "ZS_PiercedKO", 12) == 0;
        }
        bool IsLivingSummon(Entity &npc)
        {
            return npc != None && npc.IsNPC() && !npc.IsDead() && !npc.GetGameEntity()->IsKilled() &&
                npc.Party.IsValid() && npc.Party.PartyMemberType == gEPartyMemberType_Summoned;
        }

        void CollectHeroLossSummons(gCParty_PS const &party)
        {
            for (auto const &proxy : party.GetMembers())
            {
                Entity member(proxy);
                if (!IsLivingSummon(member) || !IsHeroControlled(member)) continue;
                auto const &id = proxy.GetEntityID();
                if (std::find(heroLossSummons.begin(), heroLossSummons.end(), id) == heroLossSummons.end())
                    heroLossSummons.push_back(id);
            }
        }

        bool CanReviewSummonTarget(Entity &summon)
        {
            if (!IsLivingSummon(summon) || !IsHeroControlled(summon) || summon.IsDown() ||
                summon.Party.Waiting || !summon.Routine.IsValid() || !summon.Navigation.IsValid() ||
                !summon.Navigation.IsInProcessingRange() || summon.NPC.CombatState != 0 || IsBrawlFighting(summon))
                return false;

            gEAction const action = summon.Routine.GetProperty<PSRoutine::PropertyAction>();
            switch (action)
            {
            case gEAction_None:
            case gEAction_Wait:
            case gEAction_Stand:
            case gEAction_Move:
            case gEAction_Fwd:
            case gEAction_Back:
            case gEAction_TurnLeft:
            case gEAction_TurnRight:
                break;
            default:
                return false;
            }

            bCString const task = summon.Routine.GetCurrentTask();
            if (task == "ZS_FollowPlayer" || task == "ZS_Ambient") return true;
            if (task != "ZS_Attack") return false;
            bCString const state = summon.Routine.GetCurrentState();
            return state == "ZS_Attack_End" || state == "ZS_Attack_Wait" || state == "ZS_Attack_GoBack" ||
                state == "ZS_Attack_Goto" || state == "ZS_Attack_Loop" ||
                state == "ZS_Attack_TurnLeft" || state == "ZS_Attack_TurnRight";
        }

        bool IsLiveSummonThreat(Entity &enemy)
        {
            return enemy != None && enemy.IsNPC() && !enemy.IsDead() && !enemy.IsDown() &&
                !Combat::IsHeroSide(enemy) && !IsHeroControlled(enemy) && !Host::IsDuelTaboo(enemy);
        }

        struct SummonThreat
        {
            Entity enemy;
            Entity attackedAlly;
        };

        void CollectSummonThreat(std::vector<SummonThreat> &threats, Entity &ally, bool incoming)
        {
            if (ally == None || !ally.NPC.IsValid() || ally.IsDead()) return;
            Entity enemy = incoming ? ally.NPC.GetCurrentAttacker() : ally.NPC.GetCurrentTarget();
            if (!IsLiveSummonThreat(enemy)) return;
            if (incoming)
            {
                if (enemy.NPC.GetCurrentTarget() != ally || enemy.NPC.CombatState == 0) return;
            }
            else if (ally.NPC.CombatState == 0 || !Combat::IsInRealFight(ally, enemy))
                return;
            if (ArenaJudge::Punishment::IgnoreArenaPunishmentTarget(ally, enemy)) return;
            auto const known = std::find_if(threats.begin(), threats.end(), [&](SummonThreat const &threat) {
                return threat.enemy == enemy;
            });
            if (known == threats.end()) threats.push_back({enemy, incoming ? ally : None});
        }
    }

    void ReviewSummonTargets()
    {
        GEU32 const now = ::GetTickCount();
        if (heroLossSummons.empty() || heroLossSummonsHandled || now - lastSummonTargetReview < 400) return;
        lastSummonTargetReview = now;
        Entity player = Entity::GetPlayer();
        if (player == None || player.IsDead() || PluginRuntime::HeroState::IsHeroKnockedOut()) return;
        Entity hero = BrawlMemoryOwner();
        auto *party = hero != None ? SecondaryParty(hero) : nullptr;
        if (!party) return;

        std::vector<SummonThreat> threats;
        auto const members = party->GetMembers();
        CollectSummonThreat(threats, player, true);
        for (auto const &proxy : members)
        {
            Entity ally(proxy);
            if (IsHeroControlled(ally)) CollectSummonThreat(threats, ally, true);
        }
        for (auto const &proxy : members)
        {
            Entity ally(proxy);
            if (IsHeroControlled(ally)) CollectSummonThreat(threats, ally, false);
        }
        if (threats.empty()) return;

        auto const summons = heroLossSummons;
        for (auto const &id : summons)
        {
            Entity summon(id);
            if (!CanReviewSummonTarget(summon)) continue;
            Entity previous = summon.NPC.GetCurrentTarget();
            if (IsLiveSummonThreat(previous))
            {
                Entity victim = previous.NPC.GetCurrentTarget();
                if ((previous.NPC.CombatState != 0 && (Combat::IsHeroSide(victim) || IsHeroControlled(victim))) ||
                    std::any_of(threats.begin(), threats.end(), [&](SummonThreat const &threat) {
                        return threat.enemy == previous;
                    }))
                    continue;
            }
            for (auto &threat : threats)
            {
                Entity &enemy = threat.enemy;
                if (!IsLiveSummonThreat(enemy) || ArenaJudge::Punishment::IgnoreArenaPunishmentTarget(summon, enemy) ||
                    summon.GetDistanceTo(enemy) > Combat::DEFEND_RADIUS || !summon.IsFreeLOS(enemy, GEFalse))
                    continue;
                if (threat.attackedAlly != None) Combat::RecordCombat(threat.attackedAlly, enemy);
                AI_Perception::AssessTarget(summon, enemy, gEAttackReason_Enemy);
                if (summon.NPC.GetCurrentTarget() != enemy) continue;
                GELPCChar const reason = previous == None ? "no current target" :
                    previous.IsDead() ? "previous target dead" : "previous target no longer fighting the group";
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][summon-target] '%s' rejoined group combat: '%s' -> '%s' (%s).\n",
                    summon.GetName().GetText(), previous != None ? previous.GetName().GetText() : "<none>",
                    enemy.GetName().GetText(), reason);
                break;
            }
        }
    }
    void ResetHeroLossSummons()
    {
        heroLossSummonsHandled = GEFalse;
        heroLossSummons.clear();
        lastSummonTargetReview = 0;
    }
    void CaptureHeroLossSummons(bool keepPrevious)
    {
        Entity player = Entity::GetPlayer();
        if (player == None || heroLossSummonsHandled || IsConfirmedHeroLoss(player)) return;
        Entity hero = BrawlMemoryOwner();
        auto *party = hero != None ? SecondaryParty(hero) : nullptr;
        if (!party) return;
        if (!keepPrevious) heroLossSummons.clear();
        CollectHeroLossSummons(*party);
    }
    void DismissSummonsOnHeroLoss(GEBool confirmedDefeat)
    {
        Entity hero = Entity::GetPlayer();
        if (hero == None) return;
        if (!confirmedDefeat && !IsConfirmedHeroLoss(hero))
        {
            heroLossSummonsHandled = GEFalse;
            return;
        }
        if (heroLossSummonsHandled) return;
        heroLossSummonsHandled = GETrue;
        Entity body = BrawlMemoryOwner();
        if (auto *party = body != None ? SecondaryParty(body) : nullptr)
            CollectHeroLossSummons(*party);
        std::vector<bCPropertyID> summons;
        summons.swap(heroLossSummons);
        GEInt dismissed = 0;
        GEInt detached = 0;
        for (auto const &id : summons)
        {
            Entity npc(id);
            if (!IsLivingSummon(npc)) continue;
            if (IsHeroControlled(npc))
            {
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][summon-loss] '%s': native dismissal after hero %s.\n",
                    npc.GetName().GetText(), hero.IsDead() ? "death" : "confirmed defeat");
                Routine_Commands::DismissPlayerParty(npc);
            }
            else
            {
                Entity leader = npc.Party.GetPartyLeader();
                DebugLog::GetLogger().LogFormat("[GenomeLogicFix][summon-loss] '%s' id=%s leader='%s': removing a summon tracked before hero loss.\n",
                    npc.GetName().GetText(), id.GetText().GetText(),
                    leader != None ? leader.GetName().GetText() : "<none>");
                npc.Kill();
                ++detached;
            }
            ++dismissed;
        }
        DebugLog::GetLogger().LogFormat("[GenomeLogicFix][summon-loss] %d hero summon(s) dismissed (%d already detached, %u tracked).\n",
            dismissed, detached, static_cast<unsigned>(summons.size()));
    }
    void OnHeroDefeated(Entity &defeated)
    {
        Entity hero = Entity::GetPlayer();
        if (hero != None && defeated == hero)
            DismissSummonsOnHeroLoss(GETrue);
    }
}

namespace GroupLogic::Combat
{
    using namespace GroupLogic::Brawl;

        // Diagnostics: the attacker stored by PSNpc::SetCurrentAttacker / SetLastAttacker on a member of the army. The
        // damage type is the one stored on the victim (it can still hold the previous blow here, see AssessHit).
        void LogAttackerSet(GELPCChar a_strFunction, PSNpc *a_pThis, Entity const *a_pAttacker)
        {
            if (!a_pThis || !a_pAttacker || !a_pThis->m_pEngineEntityPropertySet || a_pAttacker->GetInstance() == nullptr)
                return;
            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (!pOwner)
                return;
            Entity Victim(pOwner);
            Entity Attacker(*a_pAttacker);
            if (!Victim.IsNPC() || Victim == Attacker)
                return;
            GEInt const iType = Victim.DamageReceiver.IsValid() ? static_cast<GEInt>(Victim.DamageReceiver.DamageType) : 0;
            RecordCombat(Victim, Attacker); // a real blow: the native rules of the army open
            LogArmyHit(a_strFunction, Victim, Attacker, iType);
        }

        // ONE FOR ALL: an outsider became the current attacker of a member of the army (or of the hero in any form).
        void OnArmyAttacked(PSNpc *a_pThis, Entity const *a_pAttacker)
        {
            if (!a_pThis || !a_pAttacker || !a_pThis->m_pEngineEntityPropertySet || a_pAttacker->GetInstance() == nullptr)
                return;
            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (!pOwner)
                return;
            Entity Victim(pOwner);
            Entity Attacker(*a_pAttacker);
            if (Victim == None || Victim.IsDead() || (!Victim.IsNPC() && !IsHeroSide(Victim)))
                return;
            if (Victim.IsNPC()) CancelPersonalSpectator(Victim, GEFalse);
            static bCPropertyID lastVictim;
            static bCPropertyID lastAttacker;
            static GEU32 lastTick = 0;
            GEU32 const now = ::GetTickCount();
            if (lastTick != 0 && lastVictim == Victim.GetInstance()->GetID() &&
                lastAttacker == Attacker.GetInstance()->GetID() && static_cast<GEInt>(now - lastTick) < 700)
                return;
            lastVictim = Victim.GetInstance()->GetID();
            lastAttacker = Attacker.GetInstance()->GetID();
            lastTick = now | 1;
            DefendAllies(Victim, Attacker);
        }
}

namespace GroupLogic::Lifecycle
{
    GEBool ZS_Stumble(AIStateOriginal original, bTObjStack<gScriptRunTimeSingleState> &a_rRunTimeStack, gCScriptProcessingUnit *a_pSPU)
    {

        Entity Self(a_pSPU->GetSelfEntity());
        if (Self != None && Self.IsNPC() && IsHeroControlled(Self))
        {
            Entity Attacker = Self.NPC.GetCurrentAttacker();
            Entity Body;
            if (FindHumanBodyFor(Attacker, Body))
            {
                static GEInt s_iLines = 0;
                if (s_iLines < 40)
                {
                    ++s_iLines;
                    DebugLog::GetLogger().LogFormat("[GenomeLogicFix] ZS_Stumble: '%s' stumbles from '%s' as from '%s'.\n",
                                                 Self.GetName().GetText(), Attacker.GetName().GetText(),
                                                 Body.GetName().GetText());
                }
                Self.NPC.SetCurrentAttacker(Body);
                GEBool const bResult = original(a_rRunTimeStack, a_pSPU);
                Self.NPC.SetCurrentAttacker(Attacker);
                return bResult;
            }
        }
        return original(a_rRunTimeStack, a_pSPU);
        }
}

namespace GroupLogic::Lifecycle
{
    Entity * PartyGetLeader(PSParty *a_pThis, Entity *a_pRet, PartyGetLeaderOriginal original)
    {

            using mFOriginal = Entity *(GE_STDCALL *)(PSParty *, Entity *);
            Entity *pResult = original(a_pThis, a_pRet);
            if (!a_pRet || !a_pThis || !a_pThis->m_pEngineEntityPropertySet || a_pRet->GetInstance() == nullptr)
                return pResult;
            Entity Hero = Entity::GetPlayer();
            if (Hero == None || *a_pRet == Hero)
                return pResult;
            if (_stricmp(a_pRet->GetName().GetText(), "PC_Hero") != 0)
                return pResult; // the leader is somebody else: nothing to fix
            if (_stricmp(Hero.GetName().GetText(), "PC_Hero") == 0)
                return pResult; // the hero is in his own body
            eCEntity *pOwner = static_cast<eCEntityPropertySet *>(a_pThis->m_pEngineEntityPropertySet)->GetEntity();
            if (!pOwner)
                return pResult;
            Entity Owner(pOwner);
            if (Owner == Hero || _stricmp(Owner.GetName().GetText(), "PC_Hero") == 0 || !Owner.IsNPC() ||
                Owner.Party.PartyMemberType == gEPartyMemberType_None)
                return pResult;
            *a_pRet = Hero;
            return a_pRet;
            }
}

namespace GroupLogic::Combat
{

        // The TRUE creator of a damaging object. For a spell the damage object is the projectile entity (or the
        // spell container / burning effect), and its Owner can be the projectile itself or a spell container, not the
        // hero. The chain is followed link by link (Owner -> User -> parent Spell, at most MAX_LINKS deep) and the
        // hero is looked for in every link; the last inflictor stored on the victim is the final fallback.
        GEBool IsCreatedByHero(Entity &a_Victim, Entity &a_Source)
        {
            Entity Hero = Entity::GetPlayer();
            if (a_Source == Hero)
                return GETrue;

            Entity Current = a_Source;
            for (GEInt iLink = 0; iLink < 4 && Current != None; ++iLink)
            {
                if (!Current.Interaction.IsValid())
                    break;

                Entity Owner = Current.Interaction.GetOwner();
                Entity User = Current.Interaction.GetUser();
                if (Owner == Hero || User == Hero)
                    return GETrue;

                Entity Spell = Current.Interaction.GetSpell();
                if (Spell != None && Spell != Current && Spell.Interaction.IsValid() &&
                    (Spell.Interaction.GetOwner() == Hero || Spell.Interaction.GetUser() == Hero))
                    return GETrue;

                // Next link: the owner (the caster or the projectile that carries the spell).
                if (Owner == None || Owner == Current)
                    break;
                Current = Owner;
            }

            if (a_Victim.DamageReceiver.IsValid())
            {
                Entity Inflictor = a_Victim.DamageReceiver.GetLastInflictor();
                if (Inflictor == Hero)
                    return GETrue;
            }
            return GEFalse;
        }


        GEBool IsElementalDamage(gEDamageType a_enuType)
        {
            return a_enuType == gEDamageType_Fire || a_enuType == gEDamageType_Ice || a_enuType == gEDamageType_Lightning;
        }


        // The damage type of a hit. A spell reaches DoLogicalDamage as a projectile / spell container / burning effect:
        // the elemental type is searched in every link (object itself, its spell, its owner), then the first known type
        // and finally the type stored on the victim are used.
        gEDamageType GetHitDamageType(Entity &a_Source, Entity &a_Victim)
        {
            gEDamageType enuFirst = gEDamageType_None;
            Entity Current = a_Source;
            for (GEInt iLink = 0; iLink < 4 && Current != None; ++iLink)
            {
                if (Current.Damage.IsValid())
                {
                    gEDamageType const enuType = Current.Damage.DamageType;
                    if (IsElementalDamage(enuType))
                        return enuType;
                    if (enuFirst == gEDamageType_None)
                        enuFirst = enuType;
                }
                if (!Current.Interaction.IsValid())
                    break;

                Entity Spell = Current.Interaction.GetSpell();
                if (Spell != None && Spell != Current && Spell.Damage.IsValid() &&
                    IsElementalDamage(Spell.Damage.DamageType))
                    return Spell.Damage.DamageType;

                Entity Owner = Current.Interaction.GetOwner();
                if (Owner == None || Owner == Current)
                    break;
                Current = Owner;
            }
            if (a_Victim.DamageReceiver.IsValid() && IsElementalDamage(a_Victim.DamageReceiver.DamageType))
                return a_Victim.DamageReceiver.DamageType;
            return enuFirst;
        }


        // The elemental damage type of the damaging object ITSELF (its own chain: object, its spell, its owners). Unlike
        // GetHitDamageType the type stored on the victim is NOT used: it is the type of the PREVIOUS hit and would let a
        // sword blow pass as a spell. gEDamageType_None when the object is not elemental.
        gEDamageType GetSourceElementalType(Entity &a_Source)
        {
            // v1.0.34.0: the walk STOPS at the first creature (the owner of the projectile, the hero). His own Damage property
            // and his own last spell (Interaction.GetSpell) are not the type of THIS projectile: through them an arrow of the
            // hero was taken for a magic bolt (an elemental weapon / the last cast spell of the hero) and passed the filter.
            Entity Current = a_Source;
            for (GEInt iLink = 0; iLink < 4 && Current != None; ++iLink)
            {
                if (Current.IsNPC() || IsHeroSide(Current))
                    break;
                if (Current.Damage.IsValid() && IsElementalDamage(Current.Damage.DamageType))
                    return Current.Damage.DamageType;
                if (!Current.Interaction.IsValid())
                    break;
                Entity Spell = Current.Interaction.GetSpell();
                if (Spell != None && Spell != Current && Spell.Damage.IsValid() && IsElementalDamage(Spell.Damage.DamageType))
                    return Spell.Damage.DamageType;
                Entity Owner = Current.Interaction.GetOwner();
                if (Owner == None || Owner == Current)
                    break;
                Current = Owner;
            }
            return gEDamageType_None;
        }
}
