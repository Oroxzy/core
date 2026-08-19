/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Custom arena scripts (see src/game/Battlegrounds/Arena.h):
 *  - custom_gobj_arena_master   : the arena orb, queue / leave queue / spectate / admin settings
 *  - custom_npc_arena_watcher   : ready check npc inside the arena starting rooms
 *  - custom_npc_arena_announcer : optional npc next to the orb that yells queue joins
 *  - custom_npc_nagrand_tornado : the tornado of the Nagrand arena
 */

#include "scriptPCH.h"
#include "custom.h"
#include "ScriptedAI.h"
#include "Arena.h"
#include "BattleGround.h"
#include "BattleGroundMgr.h"
#include "Group.h"
#include "GridMap.h"
#include "Utilities/EventMap.h"
#include "Utilities/Random.h"

#include <sstream>
#include <algorithm>

/*********************************************************/
/***                      HELPERS                      ***/
/*********************************************************/

namespace
{
    enum ArenaOrbGossip
    {
        // sender values
        SENDER_QUEUE                = GOSSIP_SENDER_MAIN,   // action = arena type index (0..3)
        SENDER_LEAVE_QUEUE          = 100,                  // action = BattleGroundQueueTypeId
        SENDER_SPECTATE_LIST        = 200,
        SENDER_SPECTATE_MATCH       = 201,                  // action = arena instance id
        SENDER_ADMIN                = 300,                  // action = ACTION_ADMIN_* (from the admin submenu)
        SENDER_NOOP                 = 301,                  // back to the main menu
        SENDER_ADMIN_MENU           = 302,                  // open the admin submenu
        SENDER_ADMIN_MAP_MENU       = 303,                  // open the map picker
        SENDER_ADMIN_MAP_PICK       = 304,                  // action = ArenaMapType, lists that map's brackets
        SENDER_ADMIN_MAP_QUEUE      = 305,                  // action = map * ARENA_TYPES_COUNT + arena type index

        ARENA_SPECTATE_LIST_MAX     = 20,                   // gossip menus hold 32 buttons

        // admin actions
        ACTION_ADMIN_MAX_ITEM_LEVEL     = 1,
        ACTION_ADMIN_MAX_ITEM_PATCH     = 2,
        ACTION_ADMIN_TOGGLE_ITEM_SWAP   = 3,
        ACTION_ADMIN_TOGGLE_TRINKET_SWAP = 4,
        ACTION_ADMIN_MAP_ANY            = 5,                 // release the pinned map again
    };

    // Arena names for the gamemaster's map picker, indexed by ArenaMapType.
    char const* GetArenaMapName(ArenaMapType map)
    {
        switch (map)
        {
            case ARENA_MAP_NAGRAND:     return "Nagrand Arena";
            case ARENA_MAP_BLADES_EDGE: return "Blade's Edge Arena";
            case ARENA_MAP_LORDAERON:   return "Ruins of Lordaeron";
            case ARENA_MAP_DALARAN:     return "Dalaran Sewers";
            case ARENA_MAP_TIGERS_PEAK: return "The Tiger's Peak";
            case ARENA_MAP_TOLVIRON:    return "Tol'Viron Arena";
            default:                    return "Unknown";
        }
    }

    char const* GetWeatherName(WeatherType type)
    {
        switch (type)
        {
            case WEATHER_TYPE_FINE:  return "Clear sky";
            case WEATHER_TYPE_RAIN:  return "Rain";
            case WEATHER_TYPE_SNOW:  return "Snow";
            case WEATHER_TYPE_STORM: return "Sandstorm";
        }
        return "Unknown";
    }

    enum ArenaWatcherGossip
    {
        WATCHER_ACTION_READY            = 1,
        WATCHER_ACTION_LEAVE            = 2,
        WATCHER_ACTION_CONFIRM_LEAVE    = 3,
        WATCHER_ACTION_WEATHER_MENU     = 4,                 // admin: weather of this running match
        WATCHER_ACTION_WEATHER_TOGGLE   = 5,                 // admin: flip Arena.RandomWeather
        WATCHER_ACTION_WEATHER_SET      = 10,                // admin: + WeatherType, sets it right away
        WATCHER_ACTION_START_NOW        = 6,                 // admin: end the preparation immediately
        WATCHER_NPC_TEXT_HELLO          = 800100,
        WATCHER_NPC_TEXT_LEAVE_CONFIRM  = 800101,
        ORB_NPC_TEXT_HELLO              = 800102,
    };

    char const* GetClassNameForPlayer(Player const* player)
    {
        switch (player->GetClass())
        {
            case CLASS_WARRIOR: return "Warrior";
            case CLASS_PALADIN: return "Paladin";
            case CLASS_HUNTER:  return "Hunter";
            case CLASS_ROGUE:   return "Rogue";
            case CLASS_PRIEST:  return "Priest";
            case CLASS_SHAMAN:  return "Shaman";
            case CLASS_MAGE:    return "Mage";
            case CLASS_WARLOCK: return "Warlock";
            case CLASS_DRUID:   return "Druid";
        }
        return "Unknown";
    }

    // Any arena template of the wanted size (all maps of one size share the same level range).
    BattleGround* GetArenaTemplate(ArenaType type)
    {
        for (uint8 map = 0; map < ARENA_MAPS_COUNT; ++map)
            if (BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(GetArenaBattleGroundTypeId(ArenaMapType(map), type)))
                return bg;
        return nullptr;
    }

    // The queue the player is actually sitting in for this size, or BATTLEGROUND_QUEUE_NONE. The menu
    // needs the concrete queue id to offer leaving it, and it identifies the arena map he was put on.
    BattleGroundQueueTypeId GetArenaQueueOfType(Player const* player, ArenaType type)
    {
        for (uint8 map = 0; map < ARENA_MAPS_COUNT; ++map)
        {
            BattleGroundQueueTypeId const queueTypeId = BattleGroundMgr::BgQueueTypeId(GetArenaBattleGroundTypeId(ArenaMapType(map), type));
            if (player->InBattleGroundQueueForBattleGroundQueueType(queueTypeId))
                return queueTypeId;
        }
        return BATTLEGROUND_QUEUE_NONE;
    }

    bool IsInArenaQueueOfType(Player const* player, ArenaType type)
    {
        return GetArenaQueueOfType(player, type) != BATTLEGROUND_QUEUE_NONE;
    }

    // "Leave 2v2 queue (Arena (2v2))" - built in one place because the main menu and the admin map
    // picker both show it, each in the slot where the queue entry for that size would otherwise be.
    std::string LeaveQueueLabel(ArenaType type, BattleGroundQueueTypeId queueTypeId)
    {
        std::ostringstream ss;
        ss << "Leave " << GetArenaTypeName(type) << " queue";
        if (BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(BattleGroundMgr::BgTemplateId(queueTypeId)))
            ss << " (" << bg->GetName() << ")";
        return ss.str();
    }

    uint32 GetWaitingPlayersCount(Player const* player, ArenaType type)
    {
        BattleGround* bgTemplate = GetArenaTemplate(type);
        if (!bgTemplate)
            return 0;

        BattleGroundBracketId const bracketId = player->GetBattleGroundBracketIdFromLevel(bgTemplate->GetTypeID());
        uint32 count = 0;
        for (uint8 map = 0; map < ARENA_MAPS_COUNT; ++map)
            count += sBattleGroundMgr.GetArenaPlayersWaitingCount(GetArenaBattleGroundTypeId(ArenaMapType(map), type), bracketId);
        return count;
    }

    // Healer specs are not allowed in 1v1 when Arena.1v1.BlockHealerSpecs is set.
    bool IsHealerSpec(Player const* player)
    {
        static uint32 const healerTabs[] = { 201, 202, 382, 262, 282 };   // Discipline, Holy (priest), Holy (paladin), Restoration (shaman), Restoration (druid)
        std::map<uint32, uint32> pointsPerTab;
        player->GetTalentPointsPerTab(pointsPerTab);
        uint32 healerPoints = 0;
        for (uint32 tab : healerTabs)
        {
            auto itr = pointsPerTab.find(tab);
            if (itr != pointsPerTab.end())
                healerPoints += itr->second;
        }
        return healerPoints >= 31;
    }

    void Refuse(Player* player, GameObject* orb, char const* text)
    {
        if (orb)
            orb->PlayDirectSound(SOUND_ARENA_ORB_DENIED, player);
        player->GetSession()->SendNotification("%s", text);
        player->CLOSE_GOSSIP_MENU();
    }

    uint32 GetArenaMinLevel(BattleGround const* anyTemplate)
    {
        return std::max<uint32>(anyTemplate->GetMinLevel(), sWorld.getConfig(CONFIG_UINT32_ARENA_MIN_LEVEL));
    }

    // Level / combat / GM gates for everybody who wants to queue at the orb. Checked when the menu is built
    // AND when a queue action arrives - the client can send a gossip action without having seen the menu.
    bool PassesOrbGates(Player* player, GameObject* orb, BattleGround const* anyTemplate)
    {
        uint32 const minLevel = GetArenaMinLevel(anyTemplate);
        if (player->GetLevel() < minLevel)
        {
            std::ostringstream ss;
            ss << "You must be level " << minLevel << " or higher.";
            Refuse(player, orb, ss.str().c_str());
            return false;
        }

        if (player->IsInCombat())
        {
            Refuse(player, orb, "You are in combat.");
            return false;
        }

        if (player->IsGameMaster())
        {
            Refuse(player, orb, "Please disable GM mode.");
            return false;
        }

        // the stock battlemaster refuses deserters as well - otherwise the invite is voided at accept time
        // and the opponent waits through the whole preparation for nobody
        if (!player->CanJoinToBattleground())
        {
            Refuse(player, orb, "You can not queue while you are a deserter.");
            return false;
        }

        return true;
    }

    void AnnounceQueueJoin(Player* player, GameObject* orb, ArenaType type, bool asGroup)
    {
        if (!sWorld.getConfig(CONFIG_BOOL_ARENA_ANNOUNCE_QUEUE))
            return;

        Creature* announcer = orb->FindNearestCreature(NPC_ARENA_ANNOUNCER, 20.0f);
        if (!announcer)
            return;

        std::ostringstream ss;
        ss << player->GetName() << (asGroup ? " and group queued up for " : " queued up for ") << GetArenaTypeName(type) << " arena!";
        announcer->MonsterYell(ss.str().c_str());
        announcer->HandleEmoteCommand(EMOTE_ONESHOT_SHOUT);
    }

    // Puts the player (or his group) into the queue of an arena of the given size.
    // Returns false and tells the player why if it is not possible.
    bool JoinArenaQueue(Player* player, GameObject* orb, ArenaType type)
    {
        BattleGround* anyTemplate = GetArenaTemplate(type);
        if (!anyTemplate)
        {
            Refuse(player, orb, "This arena size is not available.");
            return false;
        }

        if (player->InBattleGround())
            return false;

        if (!PassesOrbGates(player, orb, anyTemplate))
            return false;

        if (IsInArenaQueueOfType(player, type))
        {
            Refuse(player, orb, "You are already queued for this arena size.");
            return false;
        }

        std::string reason;
        if (player->HasForbiddenArenaItems(type, &reason))
        {
            player->PSendSysMessage("%s", reason.c_str());
            Refuse(player, orb, "You are wearing items that are not allowed in the arena.");
            return false;
        }

        if (type == ARENA_TYPE_1V1 && sWorld.getConfig(CONFIG_BOOL_ARENA_1V1_BLOCK_HEALER_SPECS) && IsHealerSpec(player))
        {
            Refuse(player, orb, "Healer specs can not queue for 1v1 arenas.");
            return false;
        }

        BattleGroundBracketId const bracketId = player->GetBattleGroundBracketIdFromLevel(anyTemplate->GetTypeID());
        if (bracketId == BG_BRACKET_ID_NONE)
        {
            Refuse(player, orb, "Your level is not allowed in the arena.");
            return false;
        }

        BattleGroundTypeId const bgTypeId = sBattleGroundMgr.SelectArenaBattleGroundTypeId(type, bracketId);
        BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId);
        if (!bg)
            return false;

        BattleGroundQueueTypeId const bgQueueTypeId = BattleGroundMgr::BgQueueTypeId(bgTypeId);
        BattleGroundQueue& bgQueue = sBattleGroundMgr.m_battleGroundQueues[bgQueueTypeId];

        Group* group = player->GetGroup();
        if (!group)
        {
            if (player->GetBattleGroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
                return false;

            if (!player->HasFreeBattleGroundQueueId())
            {
                Refuse(player, orb, "You are already in the maximum number of queues.");
                return false;
            }

            GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, bgTypeId, bracketId, false, 0, nullptr);
            uint32 const avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketId);
            uint32 const queueSlot = player->AddBattleGroundQueueId(bgQueueTypeId);
            player->SetBattleGroundEntryPoint(player, false);
            player->GetSession()->SendPacket(sBattleGroundMgr.BuildBattleGroundStatusPacket(bg, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0));
        }
        else
        {
            if (group->GetLeaderGuid() != player->GetObjectGuid())
            {
                Refuse(player, orb, "Only the group leader can queue the group.");
                return false;
            }

            if (group->isRaidGroup() || group->GetMembersCount() > uint32(type))
            {
                Refuse(player, orb, "Your group is too large for this arena size.");
                return false;
            }

            std::vector<uint32> excludedMembers;
            uint32 const err = group->CanJoinBattleGroundQueue(bgTypeId, bgQueueTypeId, 0, bg->GetMaxPlayersPerTeam(), player, &excludedMembers);
            if (err != BG_JOIN_ERR_OK)
            {
                player->GetSession()->SendBattleGroundJoinError(err);
                return false;
            }

            // a member in another level bracket would silently be left behind and the team would fight undersized
            if (!excludedMembers.empty())
            {
                Refuse(player, orb, "A group member is not in your level bracket.");
                return false;
            }

            // everybody in the group must pass the same gates as the leader (CanJoinBattleGroundQueue only
            // checks the level bracket, deserter and this exact queue) and the gear checks
            uint32 const minLevel = GetArenaMinLevel(anyTemplate);
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (!member || member == player)
                    continue;

                std::ostringstream why;
                if (member->GetLevel() < minLevel)
                    why << member->GetName() << " must be level " << minLevel << " or higher.";
                else if (member->InBattleGround())
                    why << member->GetName() << " is inside a battleground.";
                else if (member->IsInCombat())
                    why << member->GetName() << " is in combat.";
                else if (member->IsGameMaster())
                    why << member->GetName() << " is in GM mode.";
                else if (IsInArenaQueueOfType(member, type))
                    why << member->GetName() << " is already queued for this arena size.";

                if (!why.str().empty())
                {
                    Refuse(player, orb, why.str().c_str());
                    return false;
                }

                std::string memberReason;
                if (member->HasForbiddenArenaItems(type, &memberReason))
                {
                    member->PSendSysMessage("%s", memberReason.c_str());
                    player->PSendSysMessage("%s is wearing items that are not allowed in the arena.", member->GetName());
                    Refuse(player, orb, "A group member is wearing items that are not allowed in the arena.");
                    return false;
                }
            }

            GroupQueueInfo* ginfo = bgQueue.AddGroup(player, group, bgTypeId, bracketId, false, 0, &excludedMembers);
            uint32 const avgTime = bgQueue.GetAverageQueueWaitTime(ginfo, bracketId);
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (!member)
                    continue;

                if (std::find(excludedMembers.begin(), excludedMembers.end(), member->GetGUIDLow()) != excludedMembers.end())
                {
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_4_2
                    member->GetSession()->SendPacket(sBattleGroundMgr.BuildGroupJoinedBattlegroundPacket(BG_GROUPJOIN_FAILED));
#endif
                    continue;
                }

                uint32 const queueSlot = member->AddBattleGroundQueueId(bgQueueTypeId);
                member->SetBattleGroundEntryPoint(player, false);
                member->GetSession()->SendPacket(sBattleGroundMgr.BuildBattleGroundStatusPacket(bg, queueSlot, STATUS_WAIT_QUEUE, avgTime, 0));
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_4_2
                member->GetSession()->SendPacket(sBattleGroundMgr.BuildGroupJoinedBattlegroundPacket(bg->GetMapId()));
#endif
            }
        }

        sBattleGroundMgr.ScheduleQueueUpdate(bgQueueTypeId, bgTypeId, bracketId);
        AnnounceQueueJoin(player, orb, type, group != nullptr);
        return true;
    }

    // Teleports a player into the running arena instance as an invisible spectator.
    bool SpectateArena(Player* player, GameObject* orb, uint32 instanceId)
    {
        BattleGround* bg = sBattleGroundMgr.GetBattleGround(instanceId, BATTLEGROUND_TYPE_NONE);
        if (!bg || !bg->IsArena() || bg->GetStatus() != STATUS_IN_PROGRESS)
        {
            Refuse(player, orb, "This match is over.");
            return false;
        }

        if (player->InBattleGround() || player->IsInCombat())
            return false;

        // a free status slot lets the client show the "leave battleground" button - without one the visitor
        // could not get out before the match ends (arena queues are left below, so their slots count as free)
        bool slotAvailable = false;
        for (uint8 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES && !slotAvailable; ++slot)
        {
            BattleGroundQueueTypeId const queueTypeId = player->GetBattleGroundQueueTypeId(slot);
            slotAvailable = queueTypeId == BATTLEGROUND_QUEUE_NONE || BattleGroundMgr::IsArenaQueue(queueTypeId);
        }
        if (!slotAvailable)
        {
            Refuse(player, orb, "Leave a battleground queue first.");
            return false;
        }

        // group members of a fighter would see his health and position in the party frames
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (member && member != player && bg->IsPlayerInBattleGround(member->GetObjectGuid()))
                {
                    Refuse(player, orb, "You can not watch a match of your own group.");
                    return false;
                }
            }
        }

        // somebody to land next to: prefer a fighter who is still alive
        Player* target = nullptr;
        for (const auto& itr : bg->GetPlayers())
        {
            Player* participant = sObjectMgr.GetPlayer(itr.first);
            if (!participant || !participant->IsInWorld() || participant->GetMapId() != bg->GetMapId())
                continue;

            bool const better = !target
                || (target->IsArenaSpectator() && !participant->IsArenaSpectator())
                || (!target->IsAlive() && participant->IsAlive());
            if (better)
                target = participant;
        }
        if (!target)
        {
            Refuse(player, orb, "This match is over.");
            return false;
        }

        // no queue popping while watching
        sBattleGroundMgr.RemovePlayerFromArenaQueues(player);

        uint32 statusSlot = 0;
        while (statusSlot < PLAYER_MAX_BATTLEGROUND_QUEUES && player->GetBattleGroundQueueTypeId(statusSlot) != BATTLEGROUND_QUEUE_NONE)
            ++statusSlot;

        // the spectator state itself is applied on arrival (WorldSession::HandleMoveWorldportAck)
        player->SetBattleGroundEntryPoint(player, false);   // current position, must be set before the bg id
        player->SetBattleGroundId(bg->GetInstanceID(), bg->GetTypeID());

        float x, y, z;
        target->GetPosition(x, y, z);
        if (!player->TeleportTo(target->GetMapId(), x, y, z + 3.0f, player->GetAngle(target), TELE_TO_GM_MODE))
        {
            player->SetBattleGroundId(0, BATTLEGROUND_TYPE_NONE);
            return false;
        }

        if (statusSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)
            player->GetSession()->SendPacket(sBattleGroundMgr.BuildBattleGroundStatusPacket(bg, statusSlot, STATUS_IN_PROGRESS, 0, bg->GetStartTime()));

        return true;
    }
}

/*********************************************************/
/***                     ARENA ORB                     ***/
/*********************************************************/

// Everything the orb does runs in the WORLD thread. The gossip opcodes are PACKET_PROCESS_MAP, i.e. they
// are handled inside the map update of the orb's continent while all other maps update in parallel - but
// queue joins/leaves, the list of running matches and spectating touch global battleground state that
// other map threads (a second orb on another continent, an arena that ends) touch as well. The stock join
// opcode CMSG_BATTLEMASTER_JOIN is PACKET_PROCESS_WORLD for exactly this reason. World::Update executes
// the messager before the map updates start, so the deferred body never runs next to a map thread.
// Cost: the menu shows up one world tick (~50 ms) later.
template <typename Func>
static void RunOrbActionInWorldThread(Player* player, GameObject* orb, Func func)
{
    ObjectGuid const playerGuid = player->GetObjectGuid();
    ObjectGuid const orbGuid = orb->GetObjectGuid();
    sWorld.GetMessager().AddMessage([playerGuid, orbGuid, func](World*)
    {
        Player* player = sObjectMgr.GetPlayer(playerGuid);
        if (!player || !player->IsInWorld() || player->IsBeingTeleported())
            return;

        GameObject* orb = player->GetMap()->GetGameObject(orbGuid);
        if (!orb)   // moved to another map in the meantime
            return;

        func(player, orb);
    });
}

static bool ShowArenaOrbMenu(Player* player, GameObject* orb);

// admin submenu: gear rules (kept out of the main menu so it does not clutter the queue list)
static bool ShowArenaAdminMenu(Player* player, GameObject* orb)
{
    player->PlayerTalkClass->ClearMenus();

    std::ostringstream ilvl, patch, swap, trinket;
    ilvl << "Arena.MaxItemLevel = " << sWorld.getConfig(CONFIG_UINT32_ARENA_MAX_ITEM_LEVEL);
    patch << "Arena.MaxItemPatch = " << sWorld.getConfig(CONFIG_UINT32_ARENA_MAX_ITEM_PATCH) << " (" << ArenaMgr::GetPatchName(uint8(sWorld.getConfig(CONFIG_UINT32_ARENA_MAX_ITEM_PATCH))) << ")";
    swap << "Arena.AllowItemSwap = " << (sWorld.getConfig(CONFIG_BOOL_ARENA_ALLOW_ITEM_SWAP) ? "on" : "off");
    trinket << "Arena.AllowTrinketSwap = " << (sWorld.getConfig(CONFIG_BOOL_ARENA_ALLOW_TRINKET_SWAP) ? "on" : "off");
    player->ADD_GOSSIP_ITEM_EXTENDED(GOSSIP_ICON_INTERACT_1, ilvl.str().c_str(), SENDER_ADMIN, ACTION_ADMIN_MAX_ITEM_LEVEL, "New value:", true);
    player->ADD_GOSSIP_ITEM_EXTENDED(GOSSIP_ICON_INTERACT_1, patch.str().c_str(), SENDER_ADMIN, ACTION_ADMIN_MAX_ITEM_PATCH, "New value (0 = 1.2 ... 10 = 1.12):", true);
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, swap.str().c_str(), SENDER_ADMIN, ACTION_ADMIN_TOGGLE_ITEM_SWAP);
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, trinket.str().c_str(), SENDER_ADMIN, ACTION_ADMIN_TOGGLE_TRINKET_SWAP);

    std::ostringstream pinned;
    pinned << "Queue on a chosen arena (currently ";
    ArenaMapType const forced = sBattleGroundMgr.GetForcedArenaMap();
    pinned << (forced < ARENA_MAPS_COUNT ? GetArenaMapName(forced) : "any map") << ")";
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, pinned.str().c_str(), SENDER_ADMIN_MAP_MENU, 0);

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Back", SENDER_NOOP, 0);
    player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
    return true;
}

// admin submenu: pick the arena map to test on. Picking one pins every arena that starts from now on to
// that map - the queue normally rolls for it - so the picker also offers releasing it again.
static bool ShowArenaMapMenu(Player* player, GameObject* orb)
{
    player->PlayerTalkClass->ClearMenus();

    for (uint8 map = 0; map < ARENA_MAPS_COUNT; ++map)
    {
        // an arena without a battleground template is not set up on this realm - do not offer it
        if (!sBattleGroundMgr.GetBattleGroundTemplate(GetArenaBattleGroundTypeId(ArenaMapType(map), ARENA_TYPE_2V2)) &&
            !sBattleGroundMgr.GetBattleGroundTemplate(GetArenaBattleGroundTypeId(ArenaMapType(map), ARENA_TYPE_1V1)))
            continue;

        std::ostringstream ss;
        ss << GetArenaMapName(ArenaMapType(map));
        if (sBattleGroundMgr.GetForcedArenaMap() == ArenaMapType(map))
            ss << "  <pinned>";
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, ss.str().c_str(), SENDER_ADMIN_MAP_PICK, map);
    }

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Release the map again (random as usual)", SENDER_ADMIN, ACTION_ADMIN_MAP_ANY);
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Back", SENDER_ADMIN_MENU, 0);
    player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
    return true;
}

// admin submenu: the brackets of one chosen arena, so a gamemaster queues map and size in one click
static bool ShowArenaMapBracketMenu(Player* player, GameObject* orb, ArenaMapType map)
{
    player->PlayerTalkClass->ClearMenus();

    for (uint8 index = 0; index < ARENA_TYPES_COUNT; ++index)
    {
        ArenaType const type = GetArenaTypeByIndex(index);
        BattleGroundTypeId const bgTypeId = GetArenaBattleGroundTypeId(map, type);
        if (!sBattleGroundMgr.GetBattleGroundTemplate(bgTypeId))
            continue;

        // same rule as the main menu: where the gamemaster is already queued, the entry becomes the
        // way out of that queue instead of a second way in
        BattleGroundQueueTypeId const queued = GetArenaQueueOfType(player, type);
        if (queued != BATTLEGROUND_QUEUE_NONE)
        {
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, LeaveQueueLabel(type, queued).c_str(), SENDER_LEAVE_QUEUE, queued);
            continue;
        }

        std::ostringstream ss;
        ss << GetArenaMapName(map) << " - " << GetArenaTypeName(type);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, ss.str().c_str(), SENDER_ADMIN_MAP_QUEUE, map * ARENA_TYPES_COUNT + index);
    }

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Back", SENDER_ADMIN_MAP_MENU, 0);
    player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
    return true;
}

// main menu (world thread)
static bool ShowArenaOrbMenu(Player* player, GameObject* orb)
{
    player->PlayerTalkClass->ClearMenus();

    if (!sWorld.getConfig(CONFIG_BOOL_ARENA_ENABLED))
    {
        Refuse(player, orb, "The arenas are closed.");
        return true;
    }

    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    BattleGround* anyTemplate = GetArenaTemplate(ARENA_TYPE_2V2);
    if (!anyTemplate)
        anyTemplate = GetArenaTemplate(ARENA_TYPE_1V1);
    if (!anyTemplate)
    {
        Refuse(player, orb, "The arenas are closed.");
        return true;
    }

    bool const admin = player->GetSession()->GetSecurity() >= SEC_ADMINISTRATOR;

    // an admin in GM mode still gets to the settings, only queueing needs GM mode off
    if (admin && player->IsGameMaster())
    {
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Disable GM mode to queue for an arena.", SENDER_NOOP, 0);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "<Admin> Arena settings", SENDER_ADMIN_MENU, 0);
        player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
        return true;
    }

    if (!PassesOrbGates(player, orb, anyTemplate))
        return true;

    Group* group = player->GetGroup();
    if (group && group->GetLeaderGuid() != player->GetObjectGuid())
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Only your group leader can queue the group.", SENDER_NOOP, 0);

    for (uint8 index = 0; index < ARENA_TYPES_COUNT; ++index)
    {
        ArenaType const type = GetArenaTypeByIndex(index);
        if (!GetArenaTemplate(type))
            continue;

        // already queued for this size: the leave entry takes the place of the queue entry, so the
        // list keeps one line per bracket instead of growing a second block at the bottom
        BattleGroundQueueTypeId const queued = GetArenaQueueOfType(player, type);
        if (queued != BATTLEGROUND_QUEUE_NONE)
        {
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, LeaveQueueLabel(type, queued).c_str(), SENDER_LEAVE_QUEUE, queued);
            continue;
        }

        // groups: leader only, group must fit into a team; solo: everything
        if (group)
        {
            if (group->GetLeaderGuid() != player->GetObjectGuid() || group->isRaidGroup() || group->GetMembersCount() > uint32(type) || type == ARENA_TYPE_1V1)
                continue;
        }

        std::ostringstream ss;
        ss << (group ? "Group queue for " : "Queue for ") << GetArenaTypeName(type) << " arena";
        if (uint32 waiting = GetWaitingPlayersCount(player, type))
            ss << " (" << waiting << (waiting == 1 ? " player" : " players") << " waiting)";
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, ss.str().c_str(), SENDER_QUEUE, index);
    }

    // running matches
    bool anyMatch = false;
    for (uint32 bgTypeId = BATTLEGROUND_ARENA_FIRST; bgTypeId <= BATTLEGROUND_ARENA_LAST && !anyMatch; ++bgTypeId)
        for (BattleGroundSet::iterator itr = sBattleGroundMgr.GetBattleGroundsBegin(BattleGroundTypeId(bgTypeId)); itr != sBattleGroundMgr.GetBattleGroundsEnd(BattleGroundTypeId(bgTypeId)); ++itr)
            if (itr->second->GetStatus() == STATUS_IN_PROGRESS && itr->second->GetPlayersSize())
            {
                anyMatch = true;
                break;
            }
    if (anyMatch)
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Spectate a match", SENDER_SPECTATE_LIST, 0);

    // admins can adjust the gear rules (own submenu, see ShowArenaAdminMenu)
    if (admin)
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "<Admin> Arena settings", SENDER_ADMIN_MENU, 0);

    player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
    return true;
}

// menu selection (world thread)
static bool HandleArenaOrbSelect(Player* player, GameObject* orb, uint32 sender, uint32 action)
{
    switch (sender)
    {
        case SENDER_QUEUE:
        {
            if (action < ARENA_TYPES_COUNT)
                JoinArenaQueue(player, orb, GetArenaTypeByIndex(uint8(action)));
            break;
        }
        case SENDER_LEAVE_QUEUE:
        {
            if (action > BATTLEGROUND_QUEUE_NONE && action < MAX_BATTLEGROUND_QUEUE_TYPES && BattleGroundMgr::IsArenaQueue(BattleGroundQueueTypeId(action)))
                sBattleGroundMgr.m_battleGroundQueues[action].LeaveQueue(player);
            break;
        }
        case SENDER_SPECTATE_LIST:
        {
            player->PlayerTalkClass->ClearMenus();
            uint32 shownMatches = 0;
            for (uint32 bgTypeId = BATTLEGROUND_ARENA_FIRST; bgTypeId <= BATTLEGROUND_ARENA_LAST && shownMatches < ARENA_SPECTATE_LIST_MAX; ++bgTypeId)
            {
                for (BattleGroundSet::iterator itr = sBattleGroundMgr.GetBattleGroundsBegin(BattleGroundTypeId(bgTypeId)); itr != sBattleGroundMgr.GetBattleGroundsEnd(BattleGroundTypeId(bgTypeId)) && shownMatches < ARENA_SPECTATE_LIST_MAX; ++itr)
                {
                    BattleGround* bg = itr->second;
                    if (bg->GetStatus() != STATUS_IN_PROGRESS)
                        continue;

                    std::ostringstream ss;
                    ss << bg->GetName() << ":";
                    uint32 shown = 0;
                    for (const auto& playerItr : bg->GetPlayers())
                    {
                        Player* participant = sObjectMgr.GetPlayer(playerItr.first);
                        if (!participant)
                            continue;

                        ss << (shown ? ", " : " ") << participant->GetName() << " (" << participant->GetTalentSpecName() << " " << GetClassNameForPlayer(participant) << ")";
                        ++shown;
                    }

                    if (shown)
                    {
                        // the instance id survives participants leaving (a participant guid did not)
                        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ss.str().c_str(), SENDER_SPECTATE_MATCH, bg->GetInstanceID());
                        ++shownMatches;
                    }
                }
            }
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Back", SENDER_NOOP, 0);
            player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
            return true;
        }
        case SENDER_SPECTATE_MATCH:
        {
            player->CLOSE_GOSSIP_MENU();
            SpectateArena(player, orb, action);
            return true;
        }
        case SENDER_ADMIN_MENU:
        {
            if (player->GetSession()->GetSecurity() < SEC_ADMINISTRATOR)
                break;
            return ShowArenaAdminMenu(player, orb);
        }
        case SENDER_ADMIN_MAP_MENU:
        {
            if (player->GetSession()->GetSecurity() < SEC_ADMINISTRATOR)
                break;
            return ShowArenaMapMenu(player, orb);
        }
        case SENDER_ADMIN_MAP_PICK:
        {
            if (player->GetSession()->GetSecurity() < SEC_ADMINISTRATOR || action >= ARENA_MAPS_COUNT)
                break;
            sBattleGroundMgr.SetForcedArenaMap(ArenaMapType(action));
            player->GetSession()->SendAreaTriggerMessage("Arena pinned to %s until you release it.", GetArenaMapName(ArenaMapType(action)));
            return ShowArenaMapBracketMenu(player, orb, ArenaMapType(action));
        }
        case SENDER_ADMIN_MAP_QUEUE:
        {
            if (player->GetSession()->GetSecurity() < SEC_ADMINISTRATOR)
                break;
            uint32 const map = action / ARENA_TYPES_COUNT;
            uint32 const index = action % ARENA_TYPES_COUNT;
            if (map >= ARENA_MAPS_COUNT || index >= ARENA_TYPES_COUNT)
                break;
            sBattleGroundMgr.SetForcedArenaMap(ArenaMapType(map));
            JoinArenaQueue(player, orb, GetArenaTypeByIndex(uint8(index)));
            return true;
        }
        case SENDER_ADMIN:
        {
            if (player->GetSession()->GetSecurity() < SEC_ADMINISTRATOR)
                break;

            switch (action)
            {
                case ACTION_ADMIN_TOGGLE_ITEM_SWAP:
                    sWorld.setConfig(CONFIG_BOOL_ARENA_ALLOW_ITEM_SWAP, !sWorld.getConfig(CONFIG_BOOL_ARENA_ALLOW_ITEM_SWAP));
                    break;
                case ACTION_ADMIN_TOGGLE_TRINKET_SWAP:
                    sWorld.setConfig(CONFIG_BOOL_ARENA_ALLOW_TRINKET_SWAP, !sWorld.getConfig(CONFIG_BOOL_ARENA_ALLOW_TRINKET_SWAP));
                    break;
                case ACTION_ADMIN_MAP_ANY:
                    sBattleGroundMgr.SetForcedArenaMap(ArenaMapType(ARENA_MAPS_COUNT));
                    player->GetSession()->SendAreaTriggerMessage("%s", "Arena map released - the queue picks one as usual again.");
                    break;
            }
            // stay in the submenu, the changed value is shown right away
            return ShowArenaAdminMenu(player, orb);
        }
        default:
            break;
    }

    // back to the main menu
    return ShowArenaOrbMenu(player, orb);
}

// admin value input (world thread)
static bool HandleArenaOrbSelectWithCode(Player* player, GameObject* orb, uint32 sender, uint32 action, std::string const& value)
{
    if (sender != SENDER_ADMIN || player->GetSession()->GetSecurity() < SEC_ADMINISTRATOR)
        return true;

    if (value.empty() || value.length() > 3 || value.find_first_not_of("0123456789") != std::string::npos)
    {
        player->GetSession()->SendNotification("Invalid number.");
        player->CLOSE_GOSSIP_MENU();
        return true;
    }

    uint32 const number = uint32(atoi(value.c_str()));
    switch (action)
    {
        case ACTION_ADMIN_MAX_ITEM_LEVEL:
            if (number)
                sWorld.setConfig(CONFIG_UINT32_ARENA_MAX_ITEM_LEVEL, number);
            break;
        case ACTION_ADMIN_MAX_ITEM_PATCH:
            if (number <= 10)
                sWorld.setConfig(CONFIG_UINT32_ARENA_MAX_ITEM_PATCH, number);
            break;
    }

    // stay in the admin submenu
    return ShowArenaAdminMenu(player, orb);
}

// script entry points (map thread) - they only hand the request over to the world thread

bool GossipHello_ArenaOrb(Player* player, GameObject* orb)
{
    RunOrbActionInWorldThread(player, orb, [](Player* p, GameObject* o) { ShowArenaOrbMenu(p, o); });
    return true;
}

bool GossipSelect_ArenaOrb(Player* player, GameObject* orb, uint32 sender, uint32 action)
{
    RunOrbActionInWorldThread(player, orb, [sender, action](Player* p, GameObject* o) { HandleArenaOrbSelect(p, o, sender, action); });
    return true;
}

bool GossipSelectWithCode_ArenaOrb(Player* player, GameObject* orb, uint32 sender, uint32 action, char const* code)
{
    std::string const value = code ? code : "";
    RunOrbActionInWorldThread(player, orb, [sender, action, value](Player* p, GameObject* o) { HandleArenaOrbSelectWithCode(p, o, sender, action, value); });
    return true;
}

/*********************************************************/
/***                   ARENA WATCHER                   ***/
/*********************************************************/

bool GossipHello_ArenaWatcher(Player* player, Creature* creature)
{
    BattleGround* bg = player->GetBattleGround();
    if (!bg || !bg->IsArena() || player->IsArenaSpectator())
        return false;

    if (bg->GetStatus() == STATUS_WAIT_JOIN && !static_cast<Arena*>(bg)->IsPlayerReady(player->GetObjectGuid()))
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, "I'm ready!", GOSSIP_SENDER_MAIN, WATCHER_ACTION_READY);
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "I want to leave the arena.", GOSSIP_SENDER_MAIN, WATCHER_ACTION_LEAVE);
    if (player->GetSession()->GetSecurity() >= SEC_ADMINISTRATOR)
    {
        if (bg->GetStatus() == STATUS_WAIT_JOIN)
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, "<Admin> Start the match now", GOSSIP_SENDER_MAIN, WATCHER_ACTION_START_NOW);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "<Admin> Weather", GOSSIP_SENDER_MAIN, WATCHER_ACTION_WEATHER_MENU);
    }
    player->SEND_GOSSIP_MENU(WATCHER_NPC_TEXT_HELLO, creature->GetObjectGuid());
    return true;
}

// admin submenu at the watcher: change the weather of the match you are standing in, without leaving it
static bool ShowWatcherWeatherMenu(Player* player, Creature* creature, Arena* arena)
{
    player->PlayerTalkClass->ClearMenus();

    WeatherType kinds[4];
    uint8 const count = arena->GetSuitableWeather(kinds, 4);
    for (uint8 i = 0; i < count; ++i)
    {
        std::ostringstream ss;
        ss << "Set: " << GetWeatherName(kinds[i]);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, ss.str().c_str(), GOSSIP_SENDER_MAIN, WATCHER_ACTION_WEATHER_SET + kinds[i]);
    }
    if (count == 1)
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "This arena has no sky - nothing else fits here.", GOSSIP_SENDER_MAIN, WATCHER_ACTION_WEATHER_MENU);

    std::ostringstream toggle;
    toggle << "Arena.RandomWeather = " << (sWorld.getConfig(CONFIG_BOOL_ARENA_RANDOM_WEATHER) ? "on" : "off");
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, toggle.str().c_str(), GOSSIP_SENDER_MAIN, WATCHER_ACTION_WEATHER_TOGGLE);

    player->SEND_GOSSIP_MENU(WATCHER_NPC_TEXT_HELLO, creature->GetObjectGuid());
    return true;
}

bool GossipSelect_ArenaWatcher(Player* player, Creature* creature, uint32 /*sender*/, uint32 action)
{
    BattleGround* bg = player->GetBattleGround();
    if (!bg || !bg->IsArena())
        return false;

    if (action == WATCHER_ACTION_START_NOW)
    {
        if (player->GetSession()->GetSecurity() < SEC_ADMINISTRATOR || bg->GetStatus() != STATUS_WAIT_JOIN)
            return false;

        static_cast<Arena*>(bg)->StartMatchNow();
        player->GetSession()->SendAreaTriggerMessage("%s", "The match starts now.");
        player->CLOSE_GOSSIP_MENU();
        return true;
    }

    if (action >= WATCHER_ACTION_WEATHER_MENU && action <= WATCHER_ACTION_WEATHER_SET + WEATHER_TYPE_STORM)
    {
        if (player->GetSession()->GetSecurity() < SEC_ADMINISTRATOR)
            return false;

        Arena* arena = static_cast<Arena*>(bg);
        if (action == WATCHER_ACTION_WEATHER_TOGGLE)
            sWorld.setConfig(CONFIG_BOOL_ARENA_RANDOM_WEATHER, !sWorld.getConfig(CONFIG_BOOL_ARENA_RANDOM_WEATHER));
        else if (action >= WATCHER_ACTION_WEATHER_SET)
        {
            WeatherType const type = WeatherType(action - WATCHER_ACTION_WEATHER_SET);
            arena->SetArenaWeather(type, type == WEATHER_TYPE_FINE ? 0.0f : frand(0.3f, 0.9f));
            player->GetSession()->SendAreaTriggerMessage("Weather set to %s.", GetWeatherName(type));
        }
        return ShowWatcherWeatherMenu(player, creature, arena);
    }

    switch (action)
    {
        case WATCHER_ACTION_READY:
        {
            if (bg->GetStatus() != STATUS_WAIT_JOIN)
                break;

            // The team colours are already on him - he wears them from the moment he enters. Being
            // ready is now its own flag on the arena, not something read back off the aura.
            Arena* readyArena = static_cast<Arena*>(bg);
            if (!readyArena->SetPlayerReady(player))
                break;
            player->PlayDirectSound(SOUND_ARENA_READY_CHECK, player);

            uint32 readyAlliance = 0, readyHorde = 0;
            for (const auto& itr : bg->GetPlayers())
            {
                if (!readyArena->IsPlayerReady(itr.first))
                    continue;
                if (itr.second.playerTeam == HORDE)
                    ++readyHorde;
                else
                    ++readyAlliance;
            }

            char const* announce = nullptr;
            if (readyAlliance + readyHorde >= bg->GetMaxPlayers())
                announce = "Both teams are ready to fight!";
            else if (readyHorde >= bg->GetMaxPlayersPerTeam())
                announce = "The Green Team is ready to fight!";
            else if (readyAlliance >= bg->GetMaxPlayersPerTeam())
                announce = "The Gold Team is ready to fight!";

            if (announce)
            {
                creature->MonsterYell(announce);
                creature->HandleEmoteCommand(EMOTE_ONESHOT_SHOUT);
            }
            break;
        }
        case WATCHER_ACTION_LEAVE:
        {
            player->PlayerTalkClass->ClearMenus();
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Yes, I am sure.", GOSSIP_SENDER_MAIN, WATCHER_ACTION_CONFIRM_LEAVE);
            player->SEND_GOSSIP_MENU(WATCHER_NPC_TEXT_LEAVE_CONFIRM, creature->GetObjectGuid());
            return true;
        }
        case WATCHER_ACTION_CONFIRM_LEAVE:
        {
            player->CLOSE_GOSSIP_MENU();
            player->LeaveBattleground();
            return true;
        }
    }

    player->CLOSE_GOSSIP_MENU();
    return true;
}

/*********************************************************/
/***                  ARENA ANNOUNCER                  ***/
/*********************************************************/

bool GossipHello_ArenaAnnouncer(Player* player, Creature* creature)
{
    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, creature->GetObjectGuid());
    return true;
}

/*********************************************************/
/***                  NAGRAND TORNADO                  ***/
/*********************************************************/

enum
{
    TORNADO_EVENT_KNOCKBACK     = 1,
    TORNADO_EVENT_DESPAWN       = 2,
    TORNADO_LIFETIME            = 60 * IN_MILLISECONDS,
    TORNADO_KNOCKBACK_INTERVAL  = 2 * IN_MILLISECONDS,
    TORNADO_KNOCKBACK_RANGE     = 5,            // yards
    TORNADO_MOVE_RADIUS_MIN     = 5,
    TORNADO_MOVE_RADIUS_MAX     = 45,
};

struct npc_nagrand_tornadoAI : public ScriptedAI
{
    explicit npc_nagrand_tornadoAI(Creature* creature) : ScriptedAI(creature)
    {
        m_creature->SetFactionTemplateId(35);

        // visual only: strip the damage effect of the tornado aura
        if (SpellAuraHolder* holder = m_creature->AddAura(SPELL_ARENA_TORNADO_VISUAL))
            holder->RemoveAura(EFFECT_INDEX_0);

        m_events.ScheduleEvent(TORNADO_EVENT_DESPAWN, TORNADO_LIFETIME);
        Reset();
    }

    EventMap m_events;

    void Reset() override
    {
        m_events.ScheduleEvent(TORNADO_EVENT_KNOCKBACK, TORNADO_KNOCKBACK_INTERVAL);
        MoveToRandomPoint(1);
    }

    void MoveToRandomPoint(uint32 pointId)
    {
        // random point in the Nagrand arena
        float const angle = frand(0.0f, 2.0f * M_PI_F);
        float const distance = float(urand(TORNADO_MOVE_RADIUS_MIN, TORNADO_MOVE_RADIUS_MAX));
        float const x = 4056.0f + distance * cos(angle);
        float const y = 2922.0f + distance * sin(angle);
        float z = 13.65f;
        float const groundZ = m_creature->GetMap()->GetHeight(x, y, z, true, 10.0f);
        if (groundZ > INVALID_HEIGHT)
            z = groundZ + 0.05f;

        m_creature->GetMotionMaster()->MovePoint(pointId, x, y, z, MOVE_PATHFINDING | MOVE_WALK_MODE);
    }

    void MovementInform(uint32 type, uint32 pointId) override
    {
        if (type != POINT_MOTION_TYPE)
            return;

        MoveToRandomPoint(pointId + 1);
    }

    void AttackStart(Unit* /*who*/) override {}
    void MoveInLineOfSight(Unit* /*who*/) override {}

    void UpdateAI(uint32 const diff) override
    {
        m_events.Update(diff);

        while (uint32 eventId = m_events.ExecuteEvent())
        {
            switch (eventId)
            {
                case TORNADO_EVENT_KNOCKBACK:
                {
                    // match over (everybody frozen at the scoreboard) or not started: vanish quietly
                    BattleGround* bg = m_creature->GetMap()->IsBattleGround() ? static_cast<BattleGroundMap*>(m_creature->GetMap())->GetBG() : nullptr;
                    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
                    {
                        m_creature->RemoveAurasDueToSpell(SPELL_ARENA_TORNADO_VISUAL);
                        m_creature->DespawnOrUnsummon();
                        break;
                    }

                    std::list<Player*> players;
                    m_creature->GetAlivePlayerListInRange(m_creature, players, TORNADO_KNOCKBACK_RANGE);
                    for (Player* target : players)
                    {
                        if (target->IsArenaSpectator() || target->IsGameMaster())
                            continue;

                        // TBC cyclone: knockback and 10-15% of the maximum health as nature damage
                        // (self inflicted so that the tornado never enters combat / the scoreboard)
                        target->KnockBackFrom(m_creature, 15.0f, 10.0f);
                        uint32 const damage = urand(target->GetMaxHealth() * 10 / 100, target->GetMaxHealth() * 15 / 100);
                        target->DealDamage(target, damage, nullptr, SPELL_DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NATURE, nullptr, false);
                    }
                    m_events.ScheduleEvent(TORNADO_EVENT_KNOCKBACK, TORNADO_KNOCKBACK_INTERVAL);
                    break;
                }
                case TORNADO_EVENT_DESPAWN:
                {
                    // fade out: no more knockbacks from an invisible tornado
                    m_events.CancelEvent(TORNADO_EVENT_KNOCKBACK);
                    m_creature->RemoveAurasDueToSpell(SPELL_ARENA_TORNADO_VISUAL);
                    m_creature->DespawnOrUnsummon(4000);
                    break;
                }
            }
        }
    }
};

CreatureAI* GetAI_npc_nagrand_tornado(Creature* creature)
{
    return new npc_nagrand_tornadoAI(creature);
}

/*********************************************************/
/***                    REGISTRATION                   ***/
/*********************************************************/

void AddSC_custom_arena()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "custom_gobj_arena_master";
    newscript->pGOGossipHello = &GossipHello_ArenaOrb;
    newscript->pGOGossipSelect = &GossipSelect_ArenaOrb;
    newscript->pGOGossipSelectWithCode = &GossipSelectWithCode_ArenaOrb;
    newscript->RegisterSelf(false);

    newscript = new Script;
    newscript->Name = "custom_npc_arena_watcher";
    newscript->pGossipHello = &GossipHello_ArenaWatcher;
    newscript->pGossipSelect = &GossipSelect_ArenaWatcher;
    newscript->RegisterSelf(false);

    newscript = new Script;
    newscript->Name = "custom_npc_arena_announcer";
    newscript->pGossipHello = &GossipHello_ArenaAnnouncer;
    newscript->RegisterSelf(false);

    newscript = new Script;
    newscript->Name = "custom_npc_nagrand_tornado";
    newscript->GetAI = &GetAI_npc_nagrand_tornado;
    newscript->RegisterSelf(false);
}
