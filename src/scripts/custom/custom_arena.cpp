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
#include "Chat.h"                                           // ChatHandler: the lines the orb writes into the chat log
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
        SENDER_RATING               = 306,                  // open the rating submenu (also the "back" of the ladder)
        SENDER_RATING_LADDER        = 307,                  // action = arena type index, that bracket's ladder

        ARENA_SPECTATE_LIST_MAX     = 20,                   // gossip menus hold 32 buttons
        ARENA_LADDER_LIST_MAX       = 15,                   // same limit, and a longer list is unreadable anyway

        // admin actions
        ACTION_ADMIN_MAX_ITEM_LEVEL     = 1,
        ACTION_ADMIN_MAX_ITEM_PATCH     = 2,
        ACTION_ADMIN_TOGGLE_ITEM_SWAP   = 3,
        ACTION_ADMIN_TOGGLE_TRINKET_SWAP = 4,
        ACTION_ADMIN_MAP_ANY            = 5,                 // release the pinned map again
    };

    // (the arena names live in SharedDefines.h now - the gamemaster commands need them too)

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
        // WEATHER_SET is a base: the handler adds a WeatherType on top, so 10..13 are taken.
        // Everything else is a single value.
        WATCHER_ACTION_WEATHER_MENU     = 4,                 // admin: weather of this running match
        WATCHER_ACTION_WEATHER_TOGGLE   = 5,                 // admin: flip Arena.RandomWeather
        WATCHER_ACTION_START_NOW        = 6,                 // admin: end the preparation immediately
        WATCHER_ACTION_BACK             = 7,                 // out of a submenu, back to the watcher's own menu
        WATCHER_ACTION_WEATHER_SET      = 10,                // admin: + WeatherType, sets it right away
        WATCHER_NPC_TEXT_HELLO          = 800100,
        WATCHER_NPC_TEXT_LEAVE_CONFIRM  = 800101,
        ORB_NPC_TEXT_HELLO              = 800102,
    };

    // (the class names live in ArenaMgr::ClassName now - they are mangos_strings, so the spectate
    // list names another player's class in the language of whoever is reading it)

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
    // The arena's own name comes out of battleground_template and is the same in every language, so
    // only the bracket and the frame around it are translated.
    std::string LeaveQueueLabel(Player const* player, ArenaType type, BattleGroundQueueTypeId queueTypeId)
    {
        BattleGround* bg = sBattleGroundMgr.GetBattleGroundTemplate(BattleGroundMgr::BgTemplateId(queueTypeId));
        return ArenaMgr::Textf(player, LANG_ARENA_MENU_LEAVE_QUEUE, ArenaMgr::BracketName(player, type),
                               bg ? bg->GetName() : "");
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

    /*
     * Refused at the orb: the denied sound, one line in the middle of the screen, and the window
     * closes.
     *
     * `entry` is a `mangos_string`, never a literal, so the line comes out in the player's own
     * language - see section 23 of sql/arena/world_arena.sql. Anything the entry needs (a level, a
     * group size) follows, exactly as it would for printf.
     */
    void Refuse(Player* player, GameObject* orb, int32 entry, ...)
    {
        char text[1024];
        va_list ap;
        va_start(ap, entry);
        vsnprintf(text, sizeof(text), ArenaMgr::Text(player, entry), ap);
        va_end(ap);

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
            Refuse(player, orb, LANG_ARENA_ORB_LEVEL, minLevel);
            return false;
        }

        if (player->IsInCombat())
        {
            Refuse(player, orb, LANG_ARENA_ORB_IN_COMBAT);
            return false;
        }

        if (player->IsGameMaster())
        {
            Refuse(player, orb, LANG_ARENA_ORB_GM_MODE);
            return false;
        }

        // the stock battlemaster refuses deserters as well - otherwise the invite is voided at accept time
        // and the opponent waits through the whole preparation for nobody
        if (!player->CanJoinToBattleground())
        {
            Refuse(player, orb, LANG_ARENA_ORB_DESERTER);
            return false;
        }

        return true;
    }

    /*
     * "Oroxzy hat sich fuer die 2vs2-Arena angemeldet!" - one packet per LANGUAGE in earshot.
     *
     * The stock WorldObject::MonsterYell(int32) already translates the sentence, but the bracket
     * inside it is an ARGUMENT, and an argument goes through whatever language the realm happens to
     * default to - a German player would have read "die 2v2-Arena". So the bracket is looked up per
     * locale here as well.
     *
     * Everything else is the core's own machinery: LocalizedPacketDo builds one packet per locale
     * index and hands the same buffer to everybody who shares it, and CameraDistWorker visits the
     * cells around the announcer instead of walking the whole map's player list.
     */
    class ArenaYellBuilder
    {
        public:
            ArenaYellBuilder(Creature const& announcer, int32 entry, char const* who, ArenaType type)
                : m_announcer(announcer), m_entry(entry), m_who(who ? who : ""), m_type(type) {}

            void operator()(WorldPacket& data, int32 locIdx)
            {
                char line[1024];
                snprintf(line, sizeof(line), sObjectMgr.GetMangosString(m_entry, locIdx), m_who.c_str(),
                         sObjectMgr.GetMangosString(LANG_ARENA_BRACKET_FIRST + GetArenaTypeIndex(m_type), locIdx));

                ChatHandler::BuildChatPacket(data, CHAT_MSG_MONSTER_YELL, line, LANG_UNIVERSAL,
                                             CHAT_TAG_NONE, m_announcer.GetObjectGuid(),
                                             m_announcer.GetNameForLocaleIdx(locIdx));
            }

        private:
            Creature const& m_announcer;
            int32 m_entry;
            std::string m_who;
            ArenaType m_type;
    };

    void AnnounceQueueJoin(Player* player, GameObject* orb, ArenaType type, bool asGroup)
    {
        /*
         * No orb, no announcer.
         *
         * The announcer is a creature standing beside the orb and is found BY the orb, so a queue
         * that did not come from an orb has nothing to search from. When the arena window learned to
         * queue, this line went looking for a creature near a null pointer and took the world server
         * down with it - Arena.AnnounceQueue is on by default, so it happened on the first join.
         *
         * Skipping is also the right answer rather than a patch over one: the yell is positional, it
         * is meant for the people standing at the orb, and a player queueing from the other end of
         * the world has no orb for it to come out of.
         */
        if (!orb || !sWorld.getConfig(CONFIG_BOOL_ARENA_ANNOUNCE_QUEUE))
            return;

        Creature* announcer = orb->FindNearestCreature(NPC_ARENA_ANNOUNCER, 20.0f);
        if (!announcer)
            return;

        int32 const entry = asGroup ? LANG_ARENA_YELL_GROUP : LANG_ARENA_YELL_SOLO;
        float const range = sWorld.getConfig(CONFIG_FLOAT_LISTEN_RANGE_YELL);

        ArenaYellBuilder builder(*announcer, entry, player->GetName(), type);
        MaNGOS::LocalizedPacketDo<ArenaYellBuilder> say(builder);
        MaNGOS::CameraDistWorker<MaNGOS::LocalizedPacketDo<ArenaYellBuilder>> worker(announcer, range, say);
        Cell::VisitWorldObjects(announcer, worker, range);

        announcer->HandleEmoteCommand(EMOTE_ONESHOT_SHOUT);
    }

    // Puts the player (or his group) into the queue of an arena of the given size.
    // Returns false and tells the player why if it is not possible.
    bool JoinArenaQueue(Player* player, GameObject* orb, ArenaType type)
    {
        BattleGround* anyTemplate = GetArenaTemplate(type);
        if (!anyTemplate)
        {
            Refuse(player, orb, LANG_ARENA_ORB_NO_SIZE);
            return false;
        }

        if (player->InBattleGround())
        {
            Refuse(player, orb, LANG_ARENA_ORB_IN_BG);
            return false;
        }

        if (!PassesOrbGates(player, orb, anyTemplate))
            return false;

        if (IsInArenaQueueOfType(player, type))
        {
            Refuse(player, orb, LANG_ARENA_ORB_QUEUED_SIZE);
            return false;
        }

        std::string reason;
        if (player->HasForbiddenArenaItems(type, &reason))
        {
            ChatHandler(player).SendSysMessage(reason.c_str());
            Refuse(player, orb, LANG_ARENA_ORB_ITEMS);
            return false;
        }

        // Refused here rather than on arrival: which piece to take off is the player's choice, so the
        // only place this can be told to him without stranding him inside is before he queues.
        if (ArenaMgr::HasExcessResistance(player, &reason))
        {
            ChatHandler(player).SendSysMessage(reason.c_str());
            Refuse(player, orb, LANG_ARENA_ORB_RESISTANCE);
            return false;
        }

        if (type == ARENA_TYPE_1V1 && sWorld.getConfig(CONFIG_BOOL_ARENA_1V1_BLOCK_HEALER_SPECS) && IsHealerSpec(player))
        {
            Refuse(player, orb, LANG_ARENA_ORB_HEALER);
            return false;
        }

        BattleGroundBracketId const bracketId = player->GetBattleGroundBracketIdFromLevel(anyTemplate->GetTypeID());
        if (bracketId == BG_BRACKET_ID_NONE)
        {
            Refuse(player, orb, LANG_ARENA_ORB_BRACKET);
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
            // the check above catches the same size on another map; this one is the exact queue
            if (player->GetBattleGroundQueueIndex(bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
            {
                Refuse(player, orb, LANG_ARENA_ORB_QUEUED_THIS);
                return false;
            }

            if (!player->HasFreeBattleGroundQueueId())
            {
                Refuse(player, orb, LANG_ARENA_ORB_MAX_QUEUES);
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
                // and the way out is worth naming: in a party he can only come along, never queue by
                // himself, so a member who wants to play alone has to leave first
                Refuse(player, orb, LANG_ARENA_ORB_NOT_LEADER);
                return false;
            }

            /*
             * Only the members standing in the world can be queued. An offline one has no player
             * object and therefore no group reference, so GetFirstMember walks straight past him:
             * nothing below checks him, AddGroup never puts him in the queue, and the leader is told
             * his party is in while he is in fact waiting alone. GetMembersCount counts the member
             * SLOTS instead, offline ones included, so the two numbers differ exactly when somebody
             * is missing - and it was the only number the size check ever looked at.
             */
            uint32 onlineMembers = 0;
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                if (itr->getSource())
                    ++onlineMembers;

            if (onlineMembers != group->GetMembersCount())
            {
                Refuse(player, orb, LANG_ARENA_ORB_MEMBER_OFFLINE);
                return false;
            }

            /*
             * A party plays this size or it does not queue for it. There is no half party: either it
             * fills the bracket by itself, or everybody queues alone and the queue puts a side
             * together - and then nobody is a premade and nothing is rated.
             *
             * The alternative, letting a party of two queue for 3v3 and be topped up with a stranger,
             * only looks friendlier. That match can never be rated (the side is not one party), the
             * stranger is glued to a pair who talk past him, and the queue has to hold three
             * differently shaped things apart for it. This way there are two group sizes in the whole
             * system, one and the bracket, which is what makes both the pairing and the rating
             * question answerable at all.
             */
            if (group->isRaidGroup() || onlineMembers != uint32(type))
            {
                Refuse(player, orb, LANG_ARENA_ORB_PARTY_SIZE, uint32(type));
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
                Refuse(player, orb, LANG_ARENA_ORB_MEMBER_BRACKET);
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

                // one line naming the member and what is wrong with him, in the leader's own language
                if (member->GetLevel() < minLevel)
                {
                    Refuse(player, orb, LANG_ARENA_MEMBER_LEVEL, member->GetName(), minLevel);
                    return false;
                }

                int32 memberProblem = 0;
                if (member->InBattleGround())
                    memberProblem = LANG_ARENA_MEMBER_IN_BG;
                else if (member->IsInCombat())
                    memberProblem = LANG_ARENA_MEMBER_COMBAT;
                else if (member->IsGameMaster())
                    memberProblem = LANG_ARENA_MEMBER_GM;
                else if (IsInArenaQueueOfType(member, type))
                    memberProblem = LANG_ARENA_MEMBER_QUEUED;

                if (memberProblem)
                {
                    Refuse(player, orb, memberProblem, member->GetName());
                    return false;
                }

                std::string memberReason;
                if (member->HasForbiddenArenaItems(type, &memberReason))
                {
                    // the detail goes to the member (it is written in his language), the summary to
                    // the leader who pressed the button (in his)
                    ChatHandler(member).SendSysMessage(memberReason.c_str());
                    ChatHandler(player).PSendSysMessage(LANG_ARENA_MEMBER_ITEMS, member->GetName());
                    Refuse(player, orb, LANG_ARENA_ORB_MEMBER_ITEMS);
                    return false;
                }

                // The resistance cap is the one gear rule with no second enforcement at the door: a
                // forbidden item is taken off the player on entering, resistance gear is not. Checking
                // only the leader made the cap a formality - anybody could walk it past by queueing
                // with a friend instead of alone.
                if (ArenaMgr::HasExcessResistance(member, &memberReason))
                {
                    ChatHandler(member).SendSysMessage(memberReason.c_str());
                    ChatHandler(player).PSendSysMessage(LANG_ARENA_MEMBER_RESISTANCE, member->GetName());
                    Refuse(player, orb, LANG_ARENA_ORB_MEMBER_RESISTANCE);
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
        // checked here as well as at the menu entry: the gossip action carries an instance id and
        // would otherwise still work for anybody who kept the menu open when the switch was turned off
        if (!sWorld.getConfig(CONFIG_BOOL_ARENA_SPECTATE))
        {
            Refuse(player, orb, LANG_ARENA_ORB_NO_SPECTATE);
            return false;
        }

        BattleGround* bg = sBattleGroundMgr.GetBattleGround(instanceId, BATTLEGROUND_TYPE_NONE);
        if (!bg || !bg->IsArena() || bg->GetStatus() != STATUS_IN_PROGRESS)
        {
            Refuse(player, orb, LANG_ARENA_ORB_MATCH_OVER);
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
            Refuse(player, orb, LANG_ARENA_ORB_LEAVE_QUEUE_FIRST);
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
                    Refuse(player, orb, LANG_ARENA_ORB_OWN_GROUP);
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
            Refuse(player, orb, LANG_ARENA_ORB_MATCH_OVER);
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
/***           WHAT THE ARENA WINDOW CALLS             ***/
/*********************************************************/

/*
 * The player's own arena window queues through here rather than through the orb's gossip menu.
 *
 * Declared in Arena.h so the chat command in the game library can reach it - see the comment there.
 * Everything below this line is the same code the orb runs; the only difference is that there is no
 * orb, which Refuse() already copes with (it only wants one to play its denial sound on).
 */
bool ArenaJoinQueueFromWindow(Player* player, ArenaType type)
{
    return JoinArenaQueue(player, nullptr, type);
}

bool ArenaLeaveQueueFromWindow(Player* player, ArenaType type)
{
    BattleGroundQueueTypeId const queueTypeId = GetArenaQueueOfType(player, type);
    if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
        return false;

    sBattleGroundMgr.m_battleGroundQueues[queueTypeId].LeaveQueue(player);
    return true;
}

/*
 * The window's spectate tab, and the orb's "watch this match" entry by another road.
 *
 * This IS that entry - every gate SpectateArena applies still applies: the config switch, the match
 * having to be running, not already being in a battleground, not being in combat, and a free queue
 * slot so the client gives him the way out again. Nothing is relaxed because the request arrived
 * over a chat command instead of a gossip menu.
 */
bool ArenaSpectateFromWindow(Player* player, uint32 instanceId)
{
    return SpectateArena(player, nullptr, instanceId);
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

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_BACK), SENDER_NOOP, 0);
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
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_BACK), SENDER_ADMIN_MENU, 0);
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
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, LeaveQueueLabel(player, type, queued).c_str(), SENDER_LEAVE_QUEUE, queued);
            continue;
        }

        std::ostringstream ss;
        ss << GetArenaMapName(map) << " - " << GetArenaTypeName(type);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, ss.str().c_str(), SENDER_ADMIN_MAP_QUEUE, map * ARENA_TYPES_COUNT + index);
    }

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_BACK), SENDER_ADMIN_MAP_MENU, 0);
    player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
    return true;
}

/*
 * The player's own rating, one line per bracket, each of them opening that bracket's ladder.
 *
 * The 1.12 client has no arena interface of any kind - no rating in the pvp tab, nothing in the
 * queue dialog - so the orb is where a player can look his numbers up. The matchmaking rating is
 * deliberately not shown: it is a matchmaking tool, and showing it invites people to play it
 * instead of the game.
 */
static bool ShowArenaRatingMenu(Player* player, GameObject* orb)
{
    player->PlayerTalkClass->ClearMenus();

    for (uint8 index = 0; index < ARENA_TYPES_COUNT; ++index)
    {
        ArenaType const type = GetArenaTypeByIndex(index);
        if (!GetArenaTemplate(type))
            continue;

        ArenaRatingEntry const entry = sArenaRatingMgr.Get(player->GetObjectGuid(), type);
        char const* bracket = ArenaMgr::BracketName(player, type);

        std::string const line = entry.games
            ? ArenaMgr::Textf(player, LANG_ARENA_MENU_RATING_LINE, bracket, entry.rating, entry.bestRating, entry.games, entry.wins)
            : ArenaMgr::Textf(player, LANG_ARENA_MENU_NO_RATED, bracket);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, line.c_str(), SENDER_RATING_LADDER, index);
    }

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_BACK), SENDER_NOOP, 0);
    player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
    return true;
}

// The best players of one bracket (world thread)
static bool ShowArenaLadderMenu(Player* player, GameObject* orb, ArenaType type)
{
    player->PlayerTalkClass->ClearMenus();

    uint32 const minGames = sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_LADDER_MIN_GAMES);

    std::vector<ArenaLadderRow> ladder;
    sArenaRatingMgr.GetLadder(type, ARENA_LADDER_LIST_MAX, minGames, ladder);

    char const* bracket = ArenaMgr::BracketName(player, type);

    if (ladder.empty())
    {
        std::string const line = minGames > 1
            ? ArenaMgr::Textf(player, LANG_ARENA_MENU_LADDER_EMPTY, minGames, bracket)
            : ArenaMgr::Textf(player, LANG_ARENA_MENU_LADDER_EMPTY_ONE, bracket);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, line.c_str(), SENDER_RATING, 0);
    }

    uint32 rank = 0;
    for (auto const& row : ladder)
    {
        std::string const line = ArenaMgr::Textf(player, LANG_ARENA_MENU_LADDER_ROW, ++rank,
                                                 row.name.c_str(), row.rating, row.games, row.wins);
        // every line leads back into the rating menu, an unhandled action would fall out to the main one
        player->ADD_GOSSIP_ITEM(row.guid == player->GetObjectGuid() ? GOSSIP_ICON_BATTLE : GOSSIP_ICON_CHAT,
                                line.c_str(), SENDER_RATING, 0);
    }

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_BACK), SENDER_RATING, 0);
    player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
    return true;
}

// main menu (world thread)
static bool ShowArenaOrbMenu(Player* player, GameObject* orb)
{
    player->PlayerTalkClass->ClearMenus();

    if (!sWorld.getConfig(CONFIG_BOOL_ARENA_ENABLED))
    {
        Refuse(player, orb, LANG_ARENA_ORB_CLOSED);
        return true;
    }

    player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);

    BattleGround* anyTemplate = GetArenaTemplate(ARENA_TYPE_2V2);
    if (!anyTemplate)
        anyTemplate = GetArenaTemplate(ARENA_TYPE_1V1);
    if (!anyTemplate)
    {
        Refuse(player, orb, LANG_ARENA_ORB_CLOSED);
        return true;
    }

    bool const admin = player->GetSession()->GetSecurity() >= SEC_ADMINISTRATOR;

    // an admin in GM mode still gets to the settings, only queueing needs GM mode off
    if (admin && player->IsGameMaster())
    {
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_GM_OFF), SENDER_NOOP, 0);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, ArenaMgr::Text(player, LANG_ARENA_MENU_ADMIN), SENDER_ADMIN_MENU, 0);
        player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
        return true;
    }

    // The gates decide who may QUEUE. Looking your own rating up is not queueing, and the orb is
    // the only place in a 1.12 client where it can be looked up at all - so a player who is in
    // combat, under level, or sitting out a deserter debuff still gets that one entry.
    if (!PassesOrbGates(player, orb, anyTemplate))
    {
        if (sArenaRatingMgr.IsRatingEnabled())
        {
            player->PlayerTalkClass->ClearMenus();          // Refuse() closed the window, open it again with just this
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, ArenaMgr::Text(player, LANG_ARENA_MENU_MY_RATING), SENDER_RATING, 0);
            player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
        }
        return true;
    }

    Group* group = player->GetGroup();
    if (group && group->GetLeaderGuid() != player->GetObjectGuid())
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_LEADER_ONLY), SENDER_NOOP, 0);

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
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, LeaveQueueLabel(player, type, queued).c_str(), SENDER_LEAVE_QUEUE, queued);
            continue;
        }

        // A group is offered the one size it fills exactly, and nothing else - which also takes 1v1
        // off the list, since no group of one exists. Alone, every size is offered.
        if (group)
        {
            if (group->GetLeaderGuid() != player->GetObjectGuid() || group->isRaidGroup() || group->GetMembersCount() != uint32(type))
                continue;
        }

        std::string line = ArenaMgr::Textf(player, group ? LANG_ARENA_MENU_GROUP_QUEUE : LANG_ARENA_MENU_QUEUE,
                                           ArenaMgr::BracketName(player, type));
        if (uint32 waiting = GetWaitingPlayersCount(player, type))
            line += ArenaMgr::Textf(player, LANG_ARENA_MENU_WAITING, waiting);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, line.c_str(), SENDER_QUEUE, index);
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
    if (anyMatch && sWorld.getConfig(CONFIG_BOOL_ARENA_SPECTATE))
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, ArenaMgr::Text(player, LANG_ARENA_MENU_SPECTATE), SENDER_SPECTATE_LIST, 0);

    if (sArenaRatingMgr.IsRatingEnabled())
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, ArenaMgr::Text(player, LANG_ARENA_MENU_MY_RATING), SENDER_RATING, 0);

    // admins can adjust the gear rules (own submenu, see ShowArenaAdminMenu)
    if (admin)
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, ArenaMgr::Text(player, LANG_ARENA_MENU_ADMIN), SENDER_ADMIN_MENU, 0);

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

                    // The arena's own name is what battleground_template calls it and reads the same
                    // everywhere; the "(rated)" mark and the class names are the reader's own.
                    std::ostringstream ss;
                    ss << bg->GetName();
                    if (static_cast<Arena*>(bg)->IsRated())
                        ss << ArenaMgr::Text(player, LANG_ARENA_MENU_RATED_MARK);
                    ss << ":";
                    uint32 shown = 0;
                    for (const auto& playerItr : bg->GetPlayers())
                    {
                        Player* participant = sObjectMgr.GetPlayer(playerItr.first);
                        if (!participant)
                            continue;

                        ss << (shown ? ", " : " ") << participant->GetName() << " (" << participant->GetTalentSpecName()
                           << " " << ArenaMgr::ClassName(player, participant->GetClass()) << ")";
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
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_BACK), SENDER_NOOP, 0);
            player->SEND_GOSSIP_MENU(ORB_NPC_TEXT_HELLO, orb->GetObjectGuid());
            return true;
        }
        case SENDER_SPECTATE_MATCH:
        {
            player->CLOSE_GOSSIP_MENU();
            SpectateArena(player, orb, action);
            return true;
        }
        case SENDER_RATING:
        {
            if (!sArenaRatingMgr.IsRatingEnabled())
                break;
            return ShowArenaRatingMenu(player, orb);
        }
        case SENDER_RATING_LADDER:
        {
            if (!sArenaRatingMgr.IsRatingEnabled() || action >= ARENA_TYPES_COUNT)
                break;
            return ShowArenaLadderMenu(player, orb, GetArenaTypeByIndex(uint8(action)));
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
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, ArenaMgr::Text(player, LANG_ARENA_MENU_READY), GOSSIP_SENDER_MAIN, WATCHER_ACTION_READY);
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_WANT_LEAVE), GOSSIP_SENDER_MAIN, WATCHER_ACTION_LEAVE);
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

    // Named exactly rather than as one range from the menu to the last weather type. As a range it
    // also swallowed everything numbered in between - "start the match now" only worked because it
    // happens to be handled above, and the supplies entry did not work at all.
    if (action == WATCHER_ACTION_WEATHER_MENU || action == WATCHER_ACTION_WEATHER_TOGGLE
        || (action >= WATCHER_ACTION_WEATHER_SET && action <= WATCHER_ACTION_WEATHER_SET + WEATHER_TYPE_STORM))
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
        case WATCHER_ACTION_BACK:
            return GossipHello_ArenaWatcher(player, creature);
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
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ArenaMgr::Text(player, LANG_ARENA_MENU_SURE), GOSSIP_SENDER_MAIN, WATCHER_ACTION_CONFIRM_LEAVE);
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

/*
 * The Nagrand cyclone, and what it took three sources to pin down.
 *
 * Wowpedia on the Ring of Trials: "Prior to Patch 2.1, a cyclone would appear one minute into the
 * fight and randomly spin into players, SLOWING AND DAMAGING them" - removed in 2.1.0 (2007-05-22),
 * put back for the Classic re-releases, and disliked then as now. That is what Arena.NagrandTornado
 * is for.
 *
 * TWO spells, and taking only the first one is what made this comment wrong once already:
 *
 *   34695 "Tornado"     Knock Back, EffectMiscValue 200 and 100 base points, plus School Damage
 *                       (Physical) of exactly ONE point. vmangos maps a knockback as
 *                       KnockBackFrom(caster, EffectMiscValue / 10, damage / 10), so 20 horizontal
 *                       and 10 vertical. That single point is not the damage - it is what makes the
 *                       hit a hostile interaction, which is how a cyclone breaks Vanish, Stealth,
 *                       Blind and a Sap that has not landed yet.
 *   25160 "Sand Storm"  triggers 25161 "Harsh Winds" every second in a 10 yard radius: -85% run
 *                       speed, Silence, and 1961 physical damage. On a Burning Crusade health pool
 *                       of some 9000 that is more than a fifth of it PER SECOND.
 *
 * 25160 is the aura this script puts on the tornado for its LOOK, with the damage effect stripped
 * off - so the patch has been carrying Blizzard's own damage spell all along and throwing away the
 * part that does the work.
 *
 * The slow, the silence and the ten yard reach are reproduced by applying 25161 ITSELF, once a
 * second, which is its own cadence and its own duration. Its damage effect is not applied, and that
 * is the one deliberate departure: 1961 is a flat number written for a level sixty player standing
 * in a Silithus sandstorm, while this arena also runs brackets down to level ten, where a fixed
 * 1961 is not a hazard but an execution.
 *
 * So the damage is a share of the health bar and means the same thing in every bracket:
 * Arena.NagrandTornado.DamagePercent, default 5, applied on the same one second beat. Player reports
 * of cyclones landing killing blows and taking somebody to 200 health are what says the damage has
 * to be real at all; setting the percentage to 0 leaves a single point, which is no damage worth the
 * name but still a hit, so stealth and crowd control still break.
 *
 * Whether the arena cyclone used Harsh Winds at all or only 34695 is the one thing no source settles
 * - what is certain from three directions is that it slowed, damaged, and could kill.
 */
enum
{
    TORNADO_EVENT_TOUCH         = 1,
    TORNADO_EVENT_DESPAWN       = 2,
    TORNADO_EVENT_MOVE          = 3,            // retry after a failed floor pick
    TORNADO_MOVE_RETRY_DELAY    = 1000,
    /*
     * How long the body stays behind its own visual.
     *
     * Taking the sandstorm aura off does not switch the cyclone off, it plays it OUT: the effect
     * lifts and dissolves upward into the air, which is worth watching and takes a moment. Deleting
     * the creature destroys everything attached to it, animation included, so a short delay chops
     * the dissolve off half way and the tornado appears to blink out. That is what "it does not
     * despawn nicely" was.
     *
     * There is no cost to being generous here: from the moment the aura goes the creature is
     * invisible, its touch event is cancelled, and it does nothing at all - it is only waiting for
     * the client to finish. Anything shorter is visible; anything longer is not.
     */
    TORNADO_DISSOLVE_TIME       = 10 * IN_MILLISECONDS,
    TORNADO_LIFETIME            = 60 * IN_MILLISECONDS,
    // Harsh Winds' own cadence and reach: Sand Storm retriggers it every second, and its auras last
    // exactly one second, so a slower tick would leave gaps in a slow that is meant to be continuous.
    TORNADO_TICK_INTERVAL       = 1 * IN_MILLISECONDS,
    TORNADO_RADIUS              = 10,           // yards, spell 25161
};

// spell 34695: EffectMiscValue 200 -> horizontal, 100 base points -> vertical, both over ten
float const TORNADO_KNOCKBACK_HORIZONTAL = 20.0f;
float const TORNADO_KNOCKBACK_VERTICAL   = 10.0f;

struct npc_nagrand_tornadoAI : public ScriptedAI
{
    explicit npc_nagrand_tornadoAI(Creature* creature) : ScriptedAI(creature)
    {
        /*
         * Visual only: the sandstorm aura is borrowed for the look, and its one effect - the tick
         * that would fire Harsh Winds with its flat 1961 damage - is taken back off.
         *
         * Unit::RemoveAura(Aura*) rather than SpellAuraHolder::RemoveAura(index), which is a bare
         * "m_auras[index] = nullptr" and leaves the Aura allocated AND still registered in the
         * creature's m_modAuras with a holder that no longer knows it. One leak per tornado, forever.
         * The Unit version unregisters it, unapplies the modifier and frees it - and leaves the
         * HOLDER standing, which is what carries the visual. Removing it through the holder-aware
         * path instead would take the last aura off the holder and the holder with it, and the
         * tornado would be invisible.
         */
        if (SpellAuraHolder* holder = m_creature->AddAura(SPELL_ARENA_TORNADO_VISUAL))
            if (Aura* trigger = holder->GetAuraByEffectIndex(EFFECT_INDEX_0))
                m_creature->RemoveAura(trigger);

        /*
         * Everything is armed here and NOT in Reset(), which is the bug this replaces. EventMap
         * inserts, it does not replace, so a Reset() that schedules is a Reset() that schedules
         * AGAIN - and the old code called it from the constructor as well as leaving it for the
         * framework. Two knockback events in the map means a knockback every second where the
         * constant says two, at double the damage. A tornado never fights, evades or respawns, so
         * Reset() has nothing to do and now does nothing.
         */
        m_events.ScheduleEvent(TORNADO_EVENT_DESPAWN, TORNADO_LIFETIME);
        m_events.ScheduleEvent(TORNADO_EVENT_TOUCH, TORNADO_TICK_INTERVAL);
        MoveToRandomPoint(1);
    }

    EventMap m_events;
    uint32 m_nextPoint = 1;                         // only used to carry a retry across a failed pick

    void Reset() override {}

    void MoveToRandomPoint(uint32 pointId)
    {
        float x, y, z;
        if (!ArenaMgr::PickNagrandFloorPoint(m_creature->GetMap(), x, y, z))
        {
            /*
             * Ten tries found no sand. Simply returning would park the tornado where it stands for
             * the rest of its minute - still ticking damage, silence and a snare on whoever happens
             * to be next to it - because the only thing that asks for a new point is MovementInform,
             * and that fires for a move that STARTED. So the next attempt is scheduled explicitly.
             */
            m_events.ScheduleEvent(TORNADO_EVENT_MOVE, TORNADO_MOVE_RETRY_DELAY);
            m_nextPoint = pointId;
            return;
        }

        /*
         * Straight lines at running pace, and neither of those is what it used to do.
         *
         * MOVE_WALK_MODE with the template's walk rate of 1.1 is 2.5 * 1.1 = 2.75 yards a second -
         * slower than a player who is WALKING, and a third of one who runs. Footage of the real
         * thing shows it crossing the sand at a pace you have to move to avoid, so it runs now, and
         * the rate lives in creature_template 19922 where it can be tuned without a compiler.
         *
         * No pathfinding either: the old MOVE_PATHFINDING made it hug the wall and take corners like
         * a patrol guard. A cyclone drifts, and it drifts through the four pillars rather than
         * walking around them.
         */
        m_creature->GetMotionMaster()->MovePoint(pointId, x, y, z, MOVE_RUN_MODE);
    }

    void MovementInform(uint32 type, uint32 pointId) override
    {
        if (type != POINT_MOTION_TYPE)
            return;

        MoveToRandomPoint(pointId + 1);
    }

    void AttackStart(Unit* /*who*/) override {}
    void MoveInLineOfSight(Unit* /*who*/) override {}

    /*
     * Everything the cyclone leaves behind, taken back before it goes.
     *
     * Nothing in the core removes an aura because its CASTER went away, and Harsh Winds outlives one
     * tick, so a player kept the silence and the eighty five percent snare for up to a second after
     * the tornado he could no longer see. Standing rooted and mute with nothing on screen to explain
     * it is the part that reads as broken - not the disappearance itself.
     *
     * The arena map's own player list is used rather than a radius search: a knockback throws a
     * player twenty yards, so he can be well outside the ten the debuff was applied in, and there
     * are never more than a dozen people on this map anyway.
     */
    void ClearHarshWinds()
    {
        for (auto const& itr : m_creature->GetMap()->GetPlayers())
            if (Player* player = itr.getSource())
                player->RemoveAurasByCasterSpell(SPELL_ARENA_TORNADO_HARSH_WINDS, m_creature->GetObjectGuid());
    }

    void UpdateAI(uint32 const diff) override
    {
        m_events.Update(diff);

        while (uint32 eventId = m_events.ExecuteEvent())
        {
            switch (eventId)
            {
                case TORNADO_EVENT_TOUCH:
                {
                    // match over (everybody frozen at the scoreboard) or not started: vanish quietly
                    BattleGround* bg = m_creature->GetMap()->IsBattleGround() ? static_cast<BattleGroundMap*>(m_creature->GetMap())->GetBG() : nullptr;
                    if (!bg || bg->GetStatus() != STATUS_IN_PROGRESS)
                    {
                        ClearHarshWinds();
                        m_creature->RemoveAurasDueToSpell(SPELL_ARENA_TORNADO_VISUAL);
                        m_creature->DespawnOrUnsummon(TORNADO_DISSOLVE_TIME);
                        break;
                    }

                    uint32 const damagePct = sWorld.getConfig(CONFIG_UINT32_ARENA_NAGRAND_TORNADO_DAMAGE_PCT);
                    SpellEntry const* harshWinds = sSpellMgr.GetSpellEntry(SPELL_ARENA_TORNADO_HARSH_WINDS);

                    std::list<Player*> players;
                    m_creature->GetAlivePlayerListInRange(m_creature, players, TORNADO_RADIUS);
                    for (Player* target : players)
                    {
                        if (target->IsArenaSpectator() || target->IsGameMaster())
                            continue;

                        /*
                         * Immunity has to be asked for by hand, because none of this is a real cast -
                         * the tornado deliberately stays out of combat, and the price of that is that
                         * nothing checks the target for it on the way in. Without this a paladin under
                         * Divine Shield was silenced, snared and damaged straight through the bubble,
                         * and a cyclone could take the killing blow off a player who was, on paper,
                         * immune to everything.
                         */
                        if (harshWinds && target->IsImmuneToSpell(harshWinds, false))
                            continue;

                        target->KnockBackFrom(m_creature, TORNADO_KNOCKBACK_HORIZONTAL, TORNADO_KNOCKBACK_VERTICAL);

                        /*
                         * Blizzard's own slow and silence, from Blizzard's own spell, with Blizzard's
                         * own numbers and its one second duration - and without its damage.
                         *
                         * AddAura builds a holder out of the APPLY_AURA effects only, so 25161's
                         * third effect, the flat 1961 School Damage, is not applied at all. That is
                         * exactly what is wanted: 1961 is a fixed number written for a level sixty
                         * player standing in a Silithus sandstorm, and this arena also runs brackets
                         * down to level ten, where it is not a hazard but an execution. The damage
                         * below is a share of the health bar instead and scales with the bracket.
                         *
                         * The spell id is in the 1.12 client's own Spell.dbc, so the debuff shows up
                         * with its proper icon and name without the client patch carrying anything.
                         */
                        target->AddAura(SPELL_ARENA_TORNADO_HARSH_WINDS, 0, m_creature);

                        // A share of the health bar, so it means the same thing in a level nineteen
                        // bracket as it does at sixty; 0 leaves a single point, which is no damage
                        // worth the name but still a hit, so stealth and crowd control still break.
                        // Self inflicted, so the tornado never enters combat and never reaches the
                        // damage column - the arena counts damage between the two sides, not weather.
                        if (!target->IsImmuneToDamage(SPELL_SCHOOL_MASK_NORMAL))
                        {
                            uint32 const damage = damagePct ? std::max(1u, uint32(target->GetMaxHealth() * damagePct / 100)) : 1u;
                            target->DealDamage(target, damage, nullptr, SPELL_DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                        }
                    }
                    m_events.ScheduleEvent(TORNADO_EVENT_TOUCH, TORNADO_TICK_INTERVAL);
                    break;
                }
                case TORNADO_EVENT_MOVE:
                {
                    MoveToRandomPoint(m_nextPoint);
                    break;
                }
                case TORNADO_EVENT_DESPAWN:
                {
                    /*
                     * It stops touching, it takes its debuffs back, and then it is played out rather
                     * than switched off - see TORNADO_DISSOLVE_TIME.
                     */
                    m_events.CancelEvent(TORNADO_EVENT_TOUCH);
                    m_events.CancelEvent(TORNADO_EVENT_MOVE);
                    ClearHarshWinds();
                    m_creature->RemoveAurasDueToSpell(SPELL_ARENA_TORNADO_VISUAL);
                    m_creature->DespawnOrUnsummon(TORNADO_DISSOLVE_TIME);
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
