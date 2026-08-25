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
 */

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "Opcodes.h"
#include "World.h"
#include "Player.h"
#include "Group.h"
#include "Chat.h"
#include "ObjectAccessor.h"
#include "Language.h"
#include "ObjectMgr.h"
#include "Util.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Map.h"
#include "Mail.h"
#include "MassMailMgr.h"
#include "InstanceData.h"
#include "MapManager.h"
#include "BattleGroundMgr.h"
#include "Arena.h"
#include <iomanip>
#include "ArenaRating.h"

bool ChatHandler::HandleHelpCommand(char* args)
{
    if (!*args)
    {
        ShowHelpForCommand(getCommandTable(), "help");
        ShowHelpForCommand(getCommandTable(), "");
    }
    else
    {
        if (!ShowHelpForCommand(getCommandTable(), args))
            SendSysMessage(LANG_NO_CMD);
    }

    return true;
}

bool ChatHandler::HandleCommandsCommand(char* /*args*/)
{
    ShowHelpForCommand(getCommandTable(), "");
    return true;
}

bool ChatHandler::HandleAuctionAllianceCommand(char* /*args*/)
{
    m_session->GetPlayer()->SetAuctionAccessMode(m_session->GetPlayer()->GetTeam() != ALLIANCE ? -1 : 0);
    m_session->SendAuctionHello(m_session->GetPlayer());
    return true;
}

bool ChatHandler::HandleAuctionHordeCommand(char* /*args*/)
{
    m_session->GetPlayer()->SetAuctionAccessMode(m_session->GetPlayer()->GetTeam() != HORDE ? -1 : 0);
    m_session->SendAuctionHello(m_session->GetPlayer());
    return true;
}

bool ChatHandler::HandleAuctionGoblinCommand(char* /*args*/)
{
    m_session->GetPlayer()->SetAuctionAccessMode(1);
    m_session->SendAuctionHello(m_session->GetPlayer());
    return true;
}

bool ChatHandler::HandleAuctionCommand(char* /*args*/)
{
    m_session->GetPlayer()->SetAuctionAccessMode(0);
    m_session->SendAuctionHello(m_session->GetPlayer());

    return true;
}

bool ChatHandler::HandleBankCommand(char* /*args*/)
{
    m_session->SendShowBank(m_session->GetPlayer()->GetObjectGuid());

    return true;
}

bool ChatHandler::HandleStableCommand(char* /*args*/)
{
    m_session->SendStablePet(m_session->GetPlayer()->GetObjectGuid());

    return true;
}

//Enable\Dissable GM Mode
bool ChatHandler::HandleGMCommand(char* args)
{
    if (!*args)
    {
        if (m_session->GetPlayer()->IsGameMaster())
            m_session->SendNotification(LANG_GM_ON);
        else
            m_session->SendNotification(LANG_GM_OFF);
        return true;
    }

    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }

    m_session->GetPlayer()->SetGameMaster(value, true);

    return true;
}

// Enables or disables hiding of the staff badge
bool ChatHandler::HandleGMChatCommand(char* args)
{
    if (!*args)
    {
        if (m_session->GetPlayer()->IsGMChat())
            m_session->SendNotification(LANG_GM_CHAT_ON);
        else
            m_session->SendNotification(LANG_GM_CHAT_OFF);
        return true;
    }

    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }

    m_session->GetPlayer()->SetGMChat(value, true);

    return true;
}

//Enable\Disable Invisible mode
bool ChatHandler::HandleGMVisibleCommand(char* args)
{
    if (!*args)
    {
        bool visible = GetSession()->GetPlayer()->IsGMVisible();
        uint32 visibilityLevel = visible ? 0 : GetSession()->GetPlayer()->GetGMInvisibilityLevel();
        PSendSysMessage(LANG_YOU_ARE, visible ? GetMangosString(LANG_VISIBLE) : GetMangosString(LANG_INVISIBLE), visibilityLevel);
        return true;
    }

    bool value;
    uint8 accessLevel = GetAccessLevel();
    uint32 visibilityLevel = accessLevel + 1;

    if (ExtractUInt32(&args, visibilityLevel))
        value = (visibilityLevel == 0); // Make visible if level = 0 only
    else if (ExtractOnOff(&args, value))
        visibilityLevel = accessLevel;

    if (visibilityLevel > accessLevel)
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }

    if (!value)
        m_session->GetPlayer()->SetGMInvisibilityLevel(visibilityLevel);

    m_session->GetPlayer()->SetGMVisible(value, true);

    return true;
}

bool ChatHandler::HandleSetViewCommand(char* /*args*/)
{
    if (Unit* unit = GetSelectedUnit())
        m_session->GetPlayer()->GetCamera().SetView(unit);
    else
    {
        PSendSysMessage(LANG_SELECT_CHAR_OR_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    return true;
}

// Display the list of GMs
bool ChatHandler::HandleGMListFullCommand(char* /*args*/)
{
    // Get the accounts with GM Level >0
    std::unique_ptr<QueryResult> result = LoginDatabase.PQuery("SELECT `username`, `account_access`.`gmlevel` FROM `account`, `account_access` "
        "WHERE `account_access`.`id` = `account`.`id` AND `account_access`.`gmlevel` > 0 AND `RealmID`=%u", realmID);
    if (result)
    {
        SendSysMessage(LANG_GMLIST);
        SendSysMessage("========================");
        SendSysMessage(LANG_GMLIST_HEADER);
        SendSysMessage("========================");

        // Circle through them. Display username and GM level
        do
        {
            Field* fields = result->Fetch();
            PSendSysMessage("|%15s|%6s|", fields[0].GetString(), fields[1].GetString());
        } while (result->NextRow());

        PSendSysMessage("========================");
    }
    else
        PSendSysMessage(LANG_GMLIST_EMPTY);
    return true;
}

bool ChatHandler::HandleGMListIngameCommand(char* /*args*/)
{
    std::vector< std::pair<std::string, bool> > names;

    {
        HashMapHolder<Player>::ReadGuard g(HashMapHolder<Player>::GetLock());
        HashMapHolder<Player>::MapType &m = sObjectAccessor.GetPlayers();
        for (const auto& itr : m)
        {
            Player* player = itr.second;
            AccountTypes itr_sec = player->GetSession()->GetSecurity();
            if ((player->IsGameMaster() || (itr_sec > SEC_PLAYER && itr_sec <= (AccountTypes)sWorld.getConfig(CONFIG_UINT32_GM_LEVEL_IN_GM_LIST))) &&
                (!m_session || player->IsVisibleGloballyFor(m_session->GetPlayer())))
                names.push_back(std::make_pair<std::string, bool>(GetNameLink(player), player->IsAcceptWhispers()));
        }
    }

    if (!names.empty())
    {
        SendSysMessage(LANG_GMS_ON_SRV);

        char const* accepts = GetMangosString(LANG_GM_ACCEPTS_WHISPER);
        char const* not_accept = GetMangosString(LANG_GM_NO_WHISPER);
        for (const auto& name : names)
            PSendSysMessage("%s - %s", name.first.c_str(), name.second ? accepts : not_accept);
    }
    else
        SendSysMessage(LANG_GMS_NOT_LOGGED);

    return true;
}

bool RegisterPlayerToBG(WorldSession* sess, BattleGroundTypeId bgid)
{
    Player* pPlayer = sess->GetPlayer();
    if (!pPlayer->GetBGAccessByLevel(bgid))
        return false;
    if (pPlayer->InBattleGround())
        return false;
    pPlayer->SetBattleGroundEntryPoint(pPlayer->GetMapId(), pPlayer->GetPositionX(), pPlayer->GetPositionY(), pPlayer->GetPositionZ(), pPlayer->GetOrientation());
    sess->SendBattleGroundList(pPlayer->GetObjectGuid(), bgid);
    return true;
}

bool ChatHandler::HandleGoWarsongCommand(char * args)
{
    return RegisterPlayerToBG(m_session, BattleGroundTypeId(BATTLEGROUND_WS));
}
bool ChatHandler::HandleGoArathiCommand(char *args)
{
    return RegisterPlayerToBG(m_session, BattleGroundTypeId(BATTLEGROUND_AB));
}
bool ChatHandler::HandleGoAlteracCommand(char *args)
{
    return RegisterPlayerToBG(m_session, BattleGroundTypeId(BATTLEGROUND_AV));
}

/** \brief GM command level 3 - Create a guild.
 *
 * This command allows a GM (level 3) to create a guild.
 *
 * The "args" parameter contains the name of the guild leader
 * and then the name of the guild.
 *
 */
bool ChatHandler::HandleGuildCreateCommand(char* args)
{
    // guildmaster name optional
    char* guildMasterStr = ExtractOptNotLastArg(&args);

    Player* target;
    if (!ExtractPlayerTarget(&guildMasterStr, &target))
        return false;

    char* guildStr = ExtractQuotedArg(&args);
    if (!guildStr)
        return false;

    std::string guildname = guildStr;

    if (target->GetGuildId())
    {
        SendSysMessage(LANG_PLAYER_IN_GUILD);
        return true;
    }

    Guild* guild = new Guild;
    if (!guild->Create(target, guildname))
    {
        delete guild;
        SendSysMessage(LANG_GUILD_NOT_CREATED);
        SetSentErrorMessage(true);
        return false;
    }

    sGuildMgr.AddGuild(guild);
    return true;
}

bool ChatHandler::HandleGuildInviteCommand(char *args)
{
    // player name optional
    char* nameStr = ExtractOptNotLastArg(&args);

    // if not guild name only (in "") then player name
    ObjectGuid target_guid;
    if (!ExtractPlayerTarget(&nameStr, nullptr, &target_guid))
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    char* guildStr = ExtractQuotedArg(&args);
    if (!guildStr)
        return false;

    std::string glName = guildStr;
    Guild* targetGuild = sGuildMgr.GetGuildByName(glName);
    if (!targetGuild)
    {
        SendSysMessage(LANG_GUILD_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    // player's guild membership checked in AddMember before add
    auto status = targetGuild->AddMember(target_guid, targetGuild->GetLowestRank());

    if (status != GuildAddStatus::OK)
    {
        std::string error;

        switch (status) // bad
        {
            case GuildAddStatus::ALREADY_IN_GUILD:
                error = "Player is already in a guild.";
                break;
            case GuildAddStatus::GUILD_FULL:
                error = "The target guild is full.";
                break;
            case GuildAddStatus::PLAYER_DATA_ERROR:
                error = "Player data appears to be corrupt - tell an administrator.";
                break;
            case GuildAddStatus::UNKNOWN_PLAYER:
                error = "Unable to find target player.";
                break;
            default:
                error = "Unhandled guild invite error.";
        }

        SendSysMessage(error.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("Player added to %s.", glName.c_str());
    return true;
}

bool ChatHandler::HandleGuildUninviteCommand(char *args)
{
    Player* target;
    ObjectGuid target_guid;
    if (!ExtractPlayerTarget(&args, &target, &target_guid))
        return false;

    uint32 glId = target ? target->GetGuildId() : Player::GetGuildIdFromDB(target_guid);

    if (!glId)
        return false;

    Guild* targetGuild = sGuildMgr.GetGuildById(glId);
    if (!targetGuild)
    {
        SendSysMessage(LANG_GUILD_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    if (targetGuild->DelMember(target_guid))
    {
        targetGuild->Disband();
        delete targetGuild;
    }

    return true;
}

bool ChatHandler::HandleGuildRankCommand(char *args)
{
    char* nameStr = ExtractOptNotLastArg(&args);

    Player* target;
    ObjectGuid target_guid;
    std::string target_name;
    if (!ExtractPlayerTarget(&nameStr, &target, &target_guid, &target_name))
        return false;

    uint32 glId = target ? target->GetGuildId() : Player::GetGuildIdFromDB(target_guid);

    if (!glId)
    {
        SendSysMessage(LANG_GUILD_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    Guild* targetGuild = sGuildMgr.GetGuildById(glId);
    if (!targetGuild)
        return false;

    uint32 newrank;
    if (!ExtractUInt32(&args, newrank))
        return false;

    if (newrank > targetGuild->GetLowestRank())
        return false;

    MemberSlot* slot = targetGuild->GetMemberSlot(target_guid);
    if (!slot)
        return false;

    slot->ChangeRank(newrank);
    return true;
}

bool ChatHandler::HandleGuildDeleteCommand(char* args)
{
    if (!*args)
        return false;

    char* guildStr = ExtractQuotedArg(&args);
    if (!guildStr)
        return false;

    std::string gld = guildStr;

    Guild* targetGuild = sGuildMgr.GetGuildByName(gld);

    if (!targetGuild)
    {
        SendSysMessage(LANG_GUILD_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    targetGuild->Disband();
    delete targetGuild;

    return true;
}

/**
 * Renames a guild if a guild could be found with the specified name.
 * Usage: .guild rename "name" "new name"
 * It is not possible to rename a guild to a name that is already in use.
 */
bool ChatHandler::HandleGuildRenameCommand(char* args)
{
    if (!args || !*args)
        return false;

    char* currentName = ExtractQuotedArg(&args);
    if (!currentName)
        return false;

    std::string current(currentName);

    char* newName = ExtractQuotedArg(&args);
    if (!newName)
        return false;

    std::string newn(newName);

    Guild* target = sGuildMgr.GetGuildByName(currentName);
    if (!target)
    {
        SendSysMessage(LANG_GUILD_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    if (Guild* existing = sGuildMgr.GetGuildByName(newn))
    {
        PSendSysMessage("A guild with the name '%s' already exists", newName);
        SetSentErrorMessage(true);
        return false;
    }

    target->Rename(newn);
    PSendSysMessage("Guild '%s' successfully renamed to '%s'. Players must relog to see the changes", currentName, newName);
    return true;
}

bool ChatHandler::HandleGuildShowLogCommand(char* args)
{
    if (!args || !*args)
        return false;

    char* guildName = ExtractQuotedArg(&args);
    if (!guildName)
        return false;

    Guild* target = sGuildMgr.GetGuildByName(guildName);
    if (!target)
    {
        SendSysMessage(LANG_GUILD_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    if (target->GetGuildEventLog().empty())
    {
        SendSysMessage("Guild log is empty.");
        return true;
    }

    time_t now = time(nullptr);
    SendSysMessage("Showing guild log:");
    for (auto const& itr : target->GetGuildEventLog())
    {
        time_t timeSinceEvent = now - itr.timestamp;
        PSendSysMessage("- Type: %s%s%s, Player1: %s, Player2: %s, Time: %s%s%s ago",
            m_session ? "|cff00ff00" : "",
            GuildEventLogTypeToString(itr.eventType),
            m_session ? "|r" : "",
            GetNameLink(itr.playerGuid1).c_str(),
            GetNameLink(itr.playerGuid2).c_str(),
            m_session ? "|cffffffff" : "",
            secsToTimeString(timeSinceEvent, true).c_str(),
            m_session ? "|r" : "");
    }
    return true;
}

bool ChatHandler::HandleInstanceBindingMode(char* args)
{
    Player* player = GetSession()->GetPlayer();

    if (strcmp(args, "on") == 0)
    {
        player->SetSmartInstanceBindingMode(true);
        PSendSysMessage("Smart rebinding has been enabled.");
        return true;
    }
    else if (strcmp(args, "off") == 0)
    {
        player->SetSmartInstanceBindingMode(false);
        PSendSysMessage("Smart rebinding has been disabled.");
        return true;
    }

    PSendSysMessage("[Error] Invalid arguments - only 'on' or 'off' are accepted");
    return false;
}

bool ChatHandler::HandleInstanceSwitchCommand(char* args)
{
    uint32 newInstanceId = 0;
    Player* target = GetSelectedPlayer();
    if (!target)
        return false;
    if (!ExtractUInt32(&args, newInstanceId))
        return false;
    uint32 oldInstance = target->GetInstanceId();
    PSendSysMessage("Instance switch from #%u to %u: %s", oldInstance, newInstanceId, target->SwitchInstance(newInstanceId) ? "OK" : "FAILED");
    target->SetAutoInstanceSwitch(false);
    return true;
}

bool ChatHandler::HandleInstanceContinentsCommand(char*)
{
    if (Player* target = GetSelectedPlayer())
        PSendSysMessage("Target: %s, map %u instance %u", target->GetName(), target->GetMapId(), target->GetInstanceId());

    for (int mapId = 0; mapId < 2; ++mapId)
    {
        PSendSysMessage("MAP %u", mapId);
        for (int i = 0; i < 20; ++i)
            if (Map* m = sMapMgr.FindMap(mapId, i))
            {
                Map::PlayerList const& players = m->GetPlayers();
                if (players.begin() == players.end())
                    continue;
                Map::PlayerList::const_iterator it = players.begin();
                int count = 0;
                while (it != players.end())
                {
                    ++count;
                    ++it;
                }
                PSendSysMessage("[Instance%2u] %u players, dist visible:%.1f activate:%.1f", i, count, m->GetVisibilityDistance(), m->GetGridActivationDistance());
            }
    }
    return true;
}

bool ChatHandler::HandleInstanceGetDataCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    if (!pPlayer)
        return false;
    Map* pMap = pPlayer->FindMap();
    if (!pMap)
        return false;
    InstanceData* pData = pMap->GetInstanceData();
    if (!pData)
        return false;
    uint32 index = 0;
    if (!ExtractUInt32(&args, index))
        return false;

    PSendSysMessage("Data[%u] = %u", index, pData->GetData(index));
    return true;
}

bool ChatHandler::HandleInstanceSetDataCommand(char* args)
{
    Player* pPlayer = GetSession()->GetPlayer();
    if (!pPlayer)
        return false;
    Map* pMap = pPlayer->FindMap();
    if (!pMap)
        return false;
    InstanceData* pData = pMap->GetInstanceData();
    if (!pData)
        return false;
    uint32 index = 0;
    if (!ExtractUInt32(&args, index))
        return false;
    uint32 value = 0;
    if (!ExtractUInt32(&args, value))
        return false;

    pData->SetData(index, value);

    PSendSysMessage("Data[%u] = %u", index, pData->GetData(index));
    return true;
}

bool ChatHandler::HandleInstancePerfInfosCommand(char* args)
{
    Player* player = GetSession()->GetPlayer();
    Map* map = player->FindMap();
    if (!map)
        return false;
    map->PrintInfos(*this);
    uint32 playersInClient = 0, gobjsInClient = 0, unitsInClient = 0, corpsesInClient = 0;
    for (const auto& itr : player->m_visibleGUIDs)
    {
        switch (itr.GetHigh())
        {
            case HIGHGUID_PLAYER: ++playersInClient; break;
            case HIGHGUID_GAMEOBJECT: ++gobjsInClient; break;
            case HIGHGUID_UNIT: ++unitsInClient; break;
            case HIGHGUID_CORPSE: ++corpsesInClient; break;
        }
    }
    PSendSysMessage("Units in client: %u player, %u gobj, %u crea, %u corpses", playersInClient, gobjsInClient, unitsInClient, corpsesInClient);
    return true;
}

bool ChatHandler::HandleInstanceListBindsCommand(char* /*args*/)
{
    Player* player = GetSelectedPlayer();
    if (!player) player = m_session->GetPlayer();
    uint32 counter = 0;

    Player::BoundInstancesMap &binds = player->GetBoundInstances();
    for (const auto& bind : binds)
    {
        DungeonPersistentState* state = bind.second.state;
        std::string timeleft;
        // permanent binds are raids, which reset globally per map since 1.9,
        // before that each raid instance has its own reset time
        if (!bind.second.perm || !DungeonResetScheduler::IsRaidResetSchedulingGlobal())
            timeleft = secsToTimeString(state->GetResetTime() - time(nullptr), true);
        else
            timeleft = secsToTimeString(sMapPersistentStateMgr.GetScheduler().GetResetTimeFor(bind.first) - time(nullptr));

        if (MapEntry const* entry = sMapStorage.LookupEntry<MapEntry>(bind.first))
        {
            PSendSysMessage("map: %d (%s) inst: %d perm: %s canReset: %s TTR: %s",
                            bind.first, entry->name, state->GetInstanceId(), bind.second.perm ? "yes" : "no",
                            state->CanReset() ? "yes" : "no", timeleft.c_str());
        }
        else
            PSendSysMessage("bound for a nonexistent map %u", bind.first);
        counter++;
    }

    PSendSysMessage("player binds: %d", counter);
    counter = 0;

    if (Group* group = player->GetGroup())
    {
        Group::BoundInstancesMap &binds = group->GetBoundInstances();
        for (const auto& bind : binds)
        {
            DungeonPersistentState* state = bind.second.state;
            std::string timeleft;
            if (!bind.second.perm || !DungeonResetScheduler::IsRaidResetSchedulingGlobal())
                timeleft = secsToTimeString(state->GetResetTime() - time(nullptr), true);
            else
                timeleft = secsToTimeString(sMapPersistentStateMgr.GetScheduler().GetResetTimeFor(bind.first) - time(nullptr));

            if (MapEntry const* entry = sMapStorage.LookupEntry<MapEntry>(bind.first))
            {
                PSendSysMessage("map: %d (%s) inst: %d perm: %s canReset: %s TTR: %s",
                                bind.first, entry->name, state->GetInstanceId(), bind.second.perm ? "yes" : "no",
                                state->CanReset() ? "yes" : "no", timeleft.c_str());
            }
            else
                PSendSysMessage("bound for a nonexistent map %u", bind.first);
            counter++;
        }
    }
    PSendSysMessage("group binds: %d", counter);

    return true;
}

void ChatHandler::HandleInstanceUnbindHelper(Player* player, bool got_map, uint32 mapid)
{
    if (!player || !player->IsInWorld())
        return;

    uint32 counter = 0;
    Player::BoundInstancesMap &binds = player->GetBoundInstances();
    for (Player::BoundInstancesMap::iterator itr = binds.begin(); itr != binds.end();)
    {
        if (got_map && mapid != itr->first)
        {
            ++itr;
            continue;
        }
        if (itr->first != player->GetMapId())
        {
            DungeonPersistentState* save = itr->second.state;
            std::string timeleft = secsToTimeString(save->GetResetTime() - time(nullptr), true);

            if (MapEntry const* entry = sMapStorage.LookupEntry<MapEntry>(itr->first))
            {
                player->PSendSysMessage("unbinding map: %d (%s) inst: %d perm: %s canReset: %s TTR: %s",
                    itr->first, entry->name, save->GetInstanceId(), itr->second.perm ? "yes" : "no",
                    save->CanReset() ? "yes" : "no", timeleft.c_str());
            }
            else
                player->PSendSysMessage("bound for a nonexistent map %u", itr->first);
            player->UnbindInstance(itr);
            counter++;
        }
        else
            ++itr;
    }
    player->PSendSysMessage("instances unbound: %d", counter);
}

bool ChatHandler::HandleInstanceUnbindCommand(char* args)
{
    if (!*args)
        return false;

    Player* player = GetSelectedPlayer();
    if (!player || GetAccessLevel() < SEC_BASIC_ADMIN)
        player = m_session->GetPlayer();

    uint32 mapid = 0;
    bool got_map = false;

    if (strncmp(args, "all", strlen(args)) != 0)
    {
        if (!isNumeric(args[0]))
            return false;

        got_map = true;
        mapid = atoi(args);
    }

    HandleInstanceUnbindHelper(player, got_map, mapid);

    return true;
}

bool ChatHandler::HandleInstanceGroupUnbindCommand(char* args)
{
    if (!*args)
        return false;

    Player* player = GetSelectedPlayer();
    if (!player || player->InBattleGround())
        return false;

    uint32 mapid = 0;
    bool got_map = false;

    if (strncmp(args, "all", strlen(args)) != 0)
    {
        if (!isNumeric(args[0]))
            return false;

        got_map = true;
        mapid = atoi(args);
    }

    Group* pGroup = player->GetGroup();
    if (!pGroup)
    {
        std::string nameLink = GetNameLink(player);
        PSendSysMessage(LANG_NOT_IN_GROUP, nameLink.c_str());
        SetSentErrorMessage(true);
        return false;
    }

    for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* pMember = itr->getSource())
        {
            if (!pMember->IsInWorld())
                continue;

            HandleInstanceUnbindHelper(pMember, got_map, mapid);
        }
    }

    pGroup->Disband();

    SendSysMessage("Group unbound. Disbanding.");
    return true;
}

bool ChatHandler::HandleInstanceStatsCommand(char* /*args*/)
{
    PSendSysMessage("instances loaded: %d", sMapMgr.GetNumInstances());
    PSendSysMessage("players in instances: %d", sMapMgr.GetNumPlayersInInstances());

    uint32 numSaves, numBoundPlayers, numBoundGroups;
    sMapPersistentStateMgr.GetStatistics(numSaves, numBoundPlayers, numBoundGroups);
    PSendSysMessage("instance saves: %d", numSaves);
    PSendSysMessage("players bound: %d", numBoundPlayers);
    PSendSysMessage("groups bound: %d", numBoundGroups);
    return true;
}

bool ChatHandler::HandleInstanceSaveDataCommand(char* /*args*/)
{
    Player* player = m_session->GetPlayer();

    Map* map = player->GetMap();

    InstanceData* iData = map->GetInstanceData();
    if (!iData)
    {
        PSendSysMessage("Map has no instance data.");
        SetSentErrorMessage(true);
        return false;
    }

    iData->SaveToDB();
    return true;
}

bool ChatHandler::HandleSendMailHelper(MailDraft& draft, char* args)
{
    // format: "subject text" "mail text"
    char* msgSubject = ExtractQuotedArg(&args);
    if (!msgSubject)
        return false;

    char* msgText = ExtractQuotedArg(&args);
    if (!msgText)
        return false;

    // msgSubject, msgText isn't NUL after prev. check
    draft.SetSubjectAndBody(msgSubject, msgText);

    return true;
}

bool ChatHandler::HandleSendMassMailCommand(char* args)
{
    // format: raceMask "subject text" "mail text"
    uint32 raceMask = 0;
    char const* name = nullptr;

    if (!ExtractRaceMask(&args, raceMask, &name))
        return false;

    // need dynamic object because it trasfered to mass mailer
    MailDraft* draft = new MailDraft;

    // fill mail
    if (!HandleSendMailHelper(*draft, args))
    {
        delete draft;
        return false;
    }

    // from console show nonexistent sender
    MailSender sender(MAIL_NORMAL, m_session ? m_session->GetPlayer()->GetObjectGuid().GetCounter() : 0, MAIL_STATIONERY_GM);

    sMassMailMgr.AddMassMailTask(draft, sender, raceMask);

    PSendSysMessage(LANG_MAIL_SENT, name);
    return true;
}

bool ChatHandler::HandleSendItemsHelper(MailDraft& draft, char* args)
{
    // format: "subject text" "mail text" item1[:count1] item2[:count2] ... item12[:count12]
    char* msgSubject = ExtractQuotedArg(&args);
    if (!msgSubject)
        return false;

    char* msgText = ExtractQuotedArg(&args);
    if (!msgText)
        return false;

    // extract items
    typedef std::pair<uint32, uint32> ItemPair;
    std::vector<ItemPair> items;

    // get from tail next item str
    while (char* itemStr = ExtractArg(&args))
    {
        // parse item str
        uint32 itemId = 0;
        uint32 itemCount = 1;
        if (sscanf(itemStr, "%u:%u", &itemId, &itemCount) != 2)
            if (sscanf(itemStr, "%u", &itemId) != 1)
                return false;

        if (!itemId)
        {
            PSendSysMessage(LANG_COMMAND_ITEMIDINVALID, itemId);
            SetSentErrorMessage(true);
            return false;
        }

        ItemPrototype const* item_proto = sObjectMgr.GetItemPrototype(itemId);
        if (!item_proto)
        {
            PSendSysMessage(LANG_COMMAND_ITEMIDINVALID, itemId);
            SetSentErrorMessage(true);
            return false;
        }

        if (itemCount < 1 || (item_proto->MaxCount > 0 && itemCount > uint32(item_proto->MaxCount)))
        {
            PSendSysMessage(LANG_COMMAND_INVALID_ITEM_COUNT, itemCount, itemId);
            SetSentErrorMessage(true);
            return false;
        }

        while (itemCount > item_proto->GetMaxStackSize())
        {
            items.push_back(ItemPair(itemId, item_proto->GetMaxStackSize()));
            itemCount -= item_proto->GetMaxStackSize();
        }

        items.push_back(ItemPair(itemId, itemCount));

        if (items.size() > MAX_MAIL_ITEMS)
        {
            PSendSysMessage(LANG_COMMAND_MAIL_ITEMS_LIMIT, MAX_MAIL_ITEMS);
            SetSentErrorMessage(true);
            return false;
        }
    }

    // fill mail
    draft.SetSubjectAndBody(msgSubject, msgText);

    for (const auto& itr : items)
    {
        if (Item* item = Item::CreateItem(itr.first, itr.second, m_session ? m_session->GetPlayer()->GetObjectGuid() : ObjectGuid()))
        {
            item->SaveToDB();                               // save for prevent lost at next mail load, if send fail then item will deleted
            draft.AddItem(item);
        }
    }

    return true;
}

bool ChatHandler::HandleSendItemsCommand(char* args)
{
    // format: name "subject text" "mail text" item1[:count1] item2[:count2] ... item12[:count12]
    Player* receiver;
    ObjectGuid receiver_guid;
    std::string receiver_name;
    if (!ExtractPlayerTarget(&args, &receiver, &receiver_guid, &receiver_name))
        return false;

    MailDraft draft;

    // fill mail
    if (!HandleSendItemsHelper(draft, args))
        return false;

    // from console show nonexistent sender
    MailSender sender(MAIL_NORMAL, m_session ? m_session->GetPlayer()->GetObjectGuid().GetCounter() : 0, MAIL_STATIONERY_GM);

    draft.SendMailTo(MailReceiver(receiver, receiver_guid), sender);

    std::string nameLink = playerLink(receiver_name);
    PSendSysMessage(LANG_MAIL_SENT, nameLink.c_str());
    return true;
}

bool ChatHandler::HandleSendMassItemsCommand(char* args)
{
    // format: racemask "subject text" "mail text" item1[:count1] item2[:count2] ... item12[:count12]

    uint32 raceMask = 0;
    char const* name = nullptr;

    if (!ExtractRaceMask(&args, raceMask, &name))
        return false;

    // need dynamic object because it trasfered to mass mailer
    MailDraft* draft = new MailDraft;


    // fill mail
    if (!HandleSendItemsHelper(*draft, args))
    {
        delete draft;
        return false;
    }

    // from console show nonexistent sender
    MailSender sender(MAIL_NORMAL, m_session ? m_session->GetPlayer()->GetObjectGuid().GetCounter() : 0, MAIL_STATIONERY_GM);

    sMassMailMgr.AddMassMailTask(draft, sender, raceMask);

    PSendSysMessage(LANG_MAIL_SENT, name);
    return true;
}

bool ChatHandler::HandleSendMoneyHelper(MailDraft& draft, char* args)
{
    // format: "subject text" "mail text" money

    char* msgSubject = ExtractQuotedArg(&args);
    if (!msgSubject)
        return false;

    char* msgText = ExtractQuotedArg(&args);
    if (!msgText)
        return false;

    uint32 money;
    if (!ExtractUInt32(&args, money))
        return false;

    if (money <= 0)
        return false;

    // msgSubject, msgText isn't NUL after prev. check
    draft.SetSubjectAndBody(msgSubject, msgText).SetMoney(money);

    return true;
}

bool ChatHandler::HandleSendMoneyCommand(char* args)
{
    // format: name "subject text" "mail text" money

    Player* receiver;
    ObjectGuid receiver_guid;
    std::string receiver_name;
    if (!ExtractPlayerTarget(&args, &receiver, &receiver_guid, &receiver_name))
        return false;

    MailDraft draft;

    // fill mail
    if (!HandleSendMoneyHelper(draft, args))
        return false;

    // from console show nonexistent sender
    MailSender sender(MAIL_NORMAL, m_session ? m_session->GetPlayer()->GetObjectGuid().GetCounter() : 0, MAIL_STATIONERY_GM);

    draft.SendMailTo(MailReceiver(receiver, receiver_guid), sender);

    std::string nameLink = playerLink(receiver_name);
    PSendSysMessage(LANG_MAIL_SENT, nameLink.c_str());
    return true;
}

bool ChatHandler::HandleSendMassMoneyCommand(char* args)
{
    // format: raceMask "subject text" "mail text" money

    uint32 raceMask = 0;
    char const* name = nullptr;

    if (!ExtractRaceMask(&args, raceMask, &name))
        return false;

    // need dynamic object because it trasfered to mass mailer
    MailDraft* draft = new MailDraft;

    // fill mail
    if (!HandleSendMoneyHelper(*draft, args))
    {
        delete draft;
        return false;
    }

    // from console show nonexistent sender
    MailSender sender(MAIL_NORMAL, m_session ? m_session->GetPlayer()->GetObjectGuid().GetCounter() : 0, MAIL_STATIONERY_GM);

    sMassMailMgr.AddMassMailTask(draft, sender, raceMask);

    PSendSysMessage(LANG_MAIL_SENT, name);
    return true;
}

// Send mail by command
bool ChatHandler::HandleSendMailCommand(char* args)
{
    // format: name "subject text" "mail text"
    Player* target;
    ObjectGuid target_guid;
    std::string target_name;
    if (!ExtractPlayerTarget(&args, &target, &target_guid, &target_name))
        return false;

    MailDraft draft;

    // fill draft
    if (!HandleSendMailHelper(draft, args))
        return false;

    // from console show nonexistent sender
    MailSender sender(MAIL_NORMAL, m_session ? m_session->GetPlayer()->GetObjectGuid().GetCounter() : 0, MAIL_STATIONERY_GM);

    draft.SendMailTo(MailReceiver(target, target_guid), sender);

    std::string nameLink = playerLink(target_name);
    PSendSysMessage(LANG_MAIL_SENT, nameLink.c_str());
    return true;
}

// Send a message to a player in game
bool ChatHandler::HandleSendMessageCommand(char* args)
{
    // Find the player
    Player* rPlayer;
    if (!ExtractPlayerTarget(&args, &rPlayer))
        return false;

    // message
    if (!*args)
        return false;

    WorldSession* rPlayerSession = rPlayer->GetSession();

    // Check that he is not logging out.
    if (rPlayerSession->IsLogingOut())
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    // Send the message
    // Use SendAreaTriggerMessage for fastest delivery.
    rPlayerSession->SendAreaTriggerMessage("%s", args);
    rPlayerSession->SendAreaTriggerMessage("|cffff0000[Message from administrator]:|r");

    //Confirmation message
    std::string nameLink = GetNameLink(rPlayer);
    PSendSysMessage(LANG_SENDMESSAGE, nameLink.c_str(), args);
    return true;
}

bool ChatHandler::HandlePoolUpdateCommand(char* args)
{
    Player* player = m_session->GetPlayer();

    MapPersistentState* mapState = player->GetMap()->GetPersistentState();
    SpawnedPoolData& spawns = mapState->GetSpawnedPoolData();

    // shared continent pools data expected too big for show
    uint32 pool_id = 0;
    if (!ExtractUint32KeyFromLink(&args, "Hpool", pool_id))
        return false;

    PoolTemplateData const& pool_template = sPoolMgr.GetPoolTemplate(pool_id);

    PSendSysMessage("Pool #%u: %u objects spawned [limit = %u]", pool_id, spawns.GetSpawnedObjects(pool_id), pool_template.MaxLimit);
    sPoolMgr.UpdatePool<GameObject>(*mapState, pool_id);
    sPoolMgr.UpdatePool<Creature>(*mapState, pool_id);
    return true;
}

bool ChatHandler::HandlePoolSpawnsCommand(char* args)
{
    Player* player = m_session->GetPlayer();

    MapPersistentState* mapState = player->GetMap()->GetPersistentState();

    // shared continent pools data expected too big for show
    uint32 pool_id = 0;
    if (!ExtractUint32KeyFromLink(&args, "Hpool", pool_id) && !mapState->GetMapEntry()->Instanceable())
    {
        PSendSysMessage(LANG_POOL_SPAWNS_NON_INSTANCE, mapState->GetMapEntry()->name, mapState->GetMapId());
        SetSentErrorMessage(false);
        return false;
    }

    SpawnedPoolData const& spawns = mapState->GetSpawnedPoolData();

    SpawnedPoolObjects const& crSpawns = spawns.GetSpawnedCreatures();
    for (const auto itr : crSpawns)
        if (!pool_id || pool_id == sPoolMgr.IsPartOfAPool<Creature>(itr))
            if (CreatureData const* data = sObjectMgr.GetCreatureData(itr))
                if (CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(data->creature_id[0]))
                    PSendSysMessage(LANG_CREATURE_LIST_CHAT, itr, PrepareStringNpcOrGoSpawnInformation<Creature>(itr).c_str(),
                                    itr, info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId);

    SpawnedPoolObjects const& goSpawns = spawns.GetSpawnedGameobjects();
    for (const auto itr : goSpawns)
        if (!pool_id || pool_id == sPoolMgr.IsPartOfAPool<GameObject>(itr))
            if (GameObjectData const* data = sObjectMgr.GetGOData(itr))
                if (GameObjectInfo const* info = sObjectMgr.GetGameObjectTemplate(data->id))
                    PSendSysMessage(LANG_GO_LIST_CHAT, itr, PrepareStringNpcOrGoSpawnInformation<GameObject>(itr).c_str(),
                                    itr, info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId);

    return true;
}

bool ChatHandler::HandlePoolInfoCommand(char* args)
{
    // id or [name] Shift-click form |color|Hpool:id|h[name]|h|r
    uint32 pool_id;
    if (!ExtractUint32KeyFromLink(&args, "Hpool", pool_id))
        return false;

    Player* player = m_session ? m_session->GetPlayer() : nullptr;

    MapPersistentState* mapState = player ? player->GetMap()->GetPersistentState() : nullptr;
    SpawnedPoolData const* spawns = mapState ? &mapState->GetSpawnedPoolData() : nullptr;

    std::string active_str = GetMangosString(LANG_ACTIVE);

    PoolTemplateData const& pool_template = sPoolMgr.GetPoolTemplate(pool_id);
    uint32 mother_pool_id = sPoolMgr.IsPartOfAPool<Pool>(pool_id);
    if (!mother_pool_id)
        PSendSysMessage(LANG_POOL_INFO_HEADER, pool_id, pool_template.IsAutoSpawn(), pool_template.MaxLimit);
    else
    {
        PoolTemplateData const& mother_template = sPoolMgr.GetPoolTemplate(mother_pool_id);
        if (m_session)
            PSendSysMessage(LANG_POOL_INFO_HEADER_CHAT, pool_id, mother_pool_id, mother_pool_id, mother_template.description.c_str(),
                            pool_template.IsAutoSpawn(), pool_template.MaxLimit);
        else
            PSendSysMessage(LANG_POOL_INFO_HEADER_CONSOLE, pool_id, mother_pool_id, mother_template.description.c_str(),
                            pool_template.IsAutoSpawn(), pool_template.MaxLimit);
    }

    PoolGroup<Creature> const& poolCreatures = sPoolMgr.GetPoolCreatures(pool_id);
    SpawnedPoolObjects const* crSpawns = spawns ? &spawns->GetSpawnedCreatures() : nullptr;

    PoolObjectList const& poolCreaturesEx = poolCreatures.GetExplicitlyChanced();
    if (!poolCreaturesEx.empty())
    {
        SendSysMessage(LANG_POOL_CHANCE_CREATURE_LIST_HEADER);
        for (const auto& itr : poolCreaturesEx)
        {
            if (CreatureData const* data = sObjectMgr.GetCreatureData(itr.guid))
            {
                if (CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(data->creature_id[0]))
                {
                    char const* active = crSpawns && crSpawns->find(itr.guid) != crSpawns->end() ? active_str.c_str() : "";
                    if (m_session)
                        PSendSysMessage(LANG_POOL_CHANCE_CREATURE_LIST_CHAT, itr.guid, PrepareStringNpcOrGoSpawnInformation<Creature>(itr.guid).c_str(),
                                        itr.guid, info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId, itr.chance, active);
                    else
                        PSendSysMessage(LANG_POOL_CHANCE_CREATURE_LIST_CONSOLE, itr.guid, PrepareStringNpcOrGoSpawnInformation<Creature>(itr.guid).c_str(),
                                        info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId, itr.chance, active);
                }
            }
        }
    }

    PoolObjectList const& poolCreaturesEq = poolCreatures.GetEqualChanced();
    if (!poolCreaturesEq.empty())
    {
        SendSysMessage(LANG_POOL_CREATURE_LIST_HEADER);
        for (const auto& itr : poolCreaturesEq)
        {
            if (CreatureData const* data = sObjectMgr.GetCreatureData(itr.guid))
            {
                if (CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(data->creature_id[0]))
                {
                    char const* active = crSpawns && crSpawns->find(itr.guid) != crSpawns->end() ? active_str.c_str() : "";
                    if (m_session)
                        PSendSysMessage(LANG_POOL_CREATURE_LIST_CHAT, itr.guid, PrepareStringNpcOrGoSpawnInformation<Creature>(itr.guid).c_str(),
                                        itr.guid, info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId, active);
                    else
                        PSendSysMessage(LANG_POOL_CREATURE_LIST_CONSOLE, itr.guid, PrepareStringNpcOrGoSpawnInformation<Creature>(itr.guid).c_str(),
                                        info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId, active);
                }
            }
        }
    }

    PoolGroup<GameObject> const& poolGameObjects = sPoolMgr.GetPoolGameObjects(pool_id);
    SpawnedPoolObjects const* goSpawns = spawns ? &spawns->GetSpawnedGameobjects() : nullptr;

    PoolObjectList const& poolGameObjectsEx = poolGameObjects.GetExplicitlyChanced();
    if (!poolGameObjectsEx.empty())
    {
        SendSysMessage(LANG_POOL_CHANCE_GO_LIST_HEADER);
        for (const auto& itr : poolGameObjectsEx)
        {
            if (GameObjectData const* data = sObjectMgr.GetGOData(itr.guid))
            {
                if (GameObjectInfo const* info = sObjectMgr.GetGameObjectTemplate(data->id))
                {
                    char const* active = goSpawns && goSpawns->find(itr.guid) != goSpawns->end() ? active_str.c_str() : "";
                    if (m_session)
                        PSendSysMessage(LANG_POOL_CHANCE_GO_LIST_CHAT, itr.guid, PrepareStringNpcOrGoSpawnInformation<GameObject>(itr.guid).c_str(),
                                        itr.guid, info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId, itr.chance, active);
                    else
                        PSendSysMessage(LANG_POOL_CHANCE_GO_LIST_CONSOLE, itr.guid, PrepareStringNpcOrGoSpawnInformation<GameObject>(itr.guid).c_str(),
                                        info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId, itr.chance, active);
                }
            }
        }
    }

    PoolObjectList const& poolGameObjectsEq = poolGameObjects.GetEqualChanced();
    if (!poolGameObjectsEq.empty())
    {
        SendSysMessage(LANG_POOL_GO_LIST_HEADER);
        for (const auto& itr : poolGameObjectsEq)
        {
            if (GameObjectData const* data = sObjectMgr.GetGOData(itr.guid))
            {
                if (GameObjectInfo const* info = sObjectMgr.GetGameObjectTemplate(data->id))
                {
                    char const* active = goSpawns && goSpawns->find(itr.guid) != goSpawns->end() ? active_str.c_str() : "";
                    if (m_session)
                        PSendSysMessage(LANG_POOL_GO_LIST_CHAT, itr.guid, PrepareStringNpcOrGoSpawnInformation<GameObject>(itr.guid).c_str(),
                                        itr.guid, info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId, active);
                    else
                        PSendSysMessage(LANG_POOL_GO_LIST_CONSOLE, itr.guid, PrepareStringNpcOrGoSpawnInformation<GameObject>(itr.guid).c_str(),
                                        info->name.c_str(), data->position.x, data->position.y, data->position.z, data->position.mapId, active);
                }
            }
        }
    }

    PoolGroup<Pool> const& poolPools = sPoolMgr.GetPoolPools(pool_id);
    SpawnedPoolPools const* poolSpawns = spawns ? &spawns->GetSpawnedPools() : nullptr;

    PoolObjectList const& poolPoolsEx = poolPools.GetExplicitlyChanced();
    if (!poolPoolsEx.empty())
    {
        SendSysMessage(LANG_POOL_CHANCE_POOL_LIST_HEADER);
        for (const auto& itr : poolPoolsEx)
        {
            PoolTemplateData const& itr_template = sPoolMgr.GetPoolTemplate(itr.guid);
            char const* active = poolSpawns && poolSpawns->find(itr.guid) != poolSpawns->end() ? active_str.c_str() : "";
            if (m_session)
                PSendSysMessage(LANG_POOL_CHANCE_POOL_LIST_CHAT, itr.guid,
                                itr.guid, itr_template.description.c_str(), itr_template.IsAutoSpawn() ? 1 : 0, itr_template.MaxLimit,
                                sPoolMgr.GetPoolCreatures(itr.guid).size(), sPoolMgr.GetPoolGameObjects(itr.guid).size(), sPoolMgr.GetPoolPools(itr.guid).size(),
                                itr.chance, active);
            else
                PSendSysMessage(LANG_POOL_CHANCE_POOL_LIST_CONSOLE, itr.guid,
                                itr_template.description.c_str(), itr_template.IsAutoSpawn() ? 1 : 0, itr_template.MaxLimit,
                                sPoolMgr.GetPoolCreatures(itr.guid).size(), sPoolMgr.GetPoolGameObjects(itr.guid).size(), sPoolMgr.GetPoolPools(itr.guid).size(),
                                itr.chance, active);
        }
    }

    PoolObjectList const& poolPoolsEq = poolPools.GetEqualChanced();
    if (!poolPoolsEq.empty())
    {
        SendSysMessage(LANG_POOL_POOL_LIST_HEADER);
        for (const auto& itr : poolPoolsEq)
        {
            PoolTemplateData const& itr_template = sPoolMgr.GetPoolTemplate(itr.guid);
            char const* active = poolSpawns && poolSpawns->find(itr.guid) != poolSpawns->end() ? active_str.c_str() : "";
            if (m_session)
                PSendSysMessage(LANG_POOL_POOL_LIST_CHAT, itr.guid,
                                itr.guid, itr_template.description.c_str(), itr_template.IsAutoSpawn() ? 1 : 0, itr_template.MaxLimit,
                                sPoolMgr.GetPoolCreatures(itr.guid).size(), sPoolMgr.GetPoolGameObjects(itr.guid).size(), sPoolMgr.GetPoolPools(itr.guid).size(),
                                active);
            else
                PSendSysMessage(LANG_POOL_POOL_LIST_CONSOLE, itr.guid,
                                itr_template.description.c_str(), itr_template.IsAutoSpawn() ? 1 : 0, itr_template.MaxLimit,
                                sPoolMgr.GetPoolCreatures(itr.guid).size(), sPoolMgr.GetPoolGameObjects(itr.guid).size(), sPoolMgr.GetPoolPools(itr.guid).size(),
                                active);
        }
    }
    return true;
}

void ChatHandler::ShowTriggerTargetListHelper(uint32 id, AreaTriggerTeleport const* at, bool subpart /*= false*/)
{
    if (m_session)
    {
        char dist_buf[50];
        if (!subpart)
        {
            float dist = m_session->GetPlayer()->GetDistance2d(at->destination);
            snprintf(dist_buf, 50, GetMangosString(LANG_TRIGGER_DIST), dist);
        }
        else
            dist_buf[0] = '\0';

        PSendSysMessage(LANG_TRIGGER_TARGET_LIST_CHAT,
                        subpart ? " -> " : "", id, id, at->destination.mapId, at->destination.x, at->destination.y, at->destination.z, dist_buf);
    }
    else
        PSendSysMessage(LANG_TRIGGER_TARGET_LIST_CONSOLE,
                        subpart ? " -> " : "", id, at->destination.mapId, at->destination.x, at->destination.y, at->destination.z);
}

void ChatHandler::ShowTriggerListHelper(AreaTriggerEntry const* atEntry)
{

    char const* tavern = sObjectMgr.IsTavernAreaTrigger(atEntry->id) ? GetMangosString(LANG_TRIGGER_TAVERN) : "";
    char const* quest = sObjectMgr.GetQuestForAreaTrigger(atEntry->id) ? GetMangosString(LANG_TRIGGER_QUEST) : "";

    if (m_session)
    {
        float dist = m_session->GetPlayer()->GetDistance2d(atEntry->x, atEntry->y);
        char dist_buf[50];
        snprintf(dist_buf, 50, GetMangosString(LANG_TRIGGER_DIST), dist);

        PSendSysMessage(LANG_TRIGGER_LIST_CHAT,
                        atEntry->id, atEntry->id, atEntry->map_id, atEntry->x, atEntry->y, atEntry->z, dist_buf, tavern, quest);
    }
    else
        PSendSysMessage(LANG_TRIGGER_LIST_CONSOLE,
                        atEntry->id, atEntry->map_id, atEntry->x, atEntry->y, atEntry->z, tavern, quest);

    if (AreaTriggerTeleport const* at = sObjectMgr.GetAreaTriggerTeleport(atEntry->id))
        ShowTriggerTargetListHelper(atEntry->id, at, true);
}

bool ChatHandler::HandleTriggerCommand(char* args)
{
    AreaTriggerEntry const* atEntry = nullptr;

    Player* player = m_session ? m_session->GetPlayer() : nullptr;

    // select by args
    if (*args)
    {
        uint32 atId;
        if (!ExtractUint32KeyFromLink(&args, "Hareatrigger", atId))
            return false;

        if (!atId)
            return false;

        atEntry = sObjectMgr.GetAreaTrigger(atId);

        if (!atEntry)
        {
            PSendSysMessage(LANG_COMMAND_GOAREATRNOTFOUND, atId);
            SetSentErrorMessage(true);
            return false;
        }
    }
    // find nearest
    else
    {
        if (!m_session)
            return false;

        float dist2 = MAP_SIZE * MAP_SIZE;

        Player* player = m_session->GetPlayer();

        // Search triggers
        for (auto const& itr : sObjectMgr.GetAreaTriggersMap())
        {
            AreaTriggerEntry const* atTestEntry = &itr.second;
            if (!atTestEntry)
                continue;

            if (atTestEntry->map_id != m_session->GetPlayer()->GetMapId())
                continue;

            float dx = atTestEntry->x - player->GetPositionX();
            float dy = atTestEntry->y - player->GetPositionY();

            float test_dist2 = dx * dx + dy * dy;

            if (test_dist2 >= dist2)
                continue;

            dist2 = test_dist2;
            atEntry = atTestEntry;
        }

        if (!atEntry)
        {
            SendSysMessage(LANG_COMMAND_NOTRIGGERFOUND);
            SetSentErrorMessage(true);
            return false;
        }
    }

    ShowTriggerListHelper(atEntry);

    int loc_idx = GetSessionDbLocaleIndex();

    AreaTriggerTeleport const* at = sObjectMgr.GetAreaTriggerTeleport(atEntry->id);
    if (at)
        PSendSysMessage(LANG_TRIGGER_REQ_LEVEL, at->requiredLevel);

    if (uint32 quest_id = sObjectMgr.GetQuestForAreaTrigger(atEntry->id))
    {
        SendSysMessage(LANG_TRIGGER_EXPLORE_QUEST);
        ShowQuestListHelper(quest_id, loc_idx, player);
    }

    return true;
}

bool ChatHandler::HandleTriggerActiveCommand(char* /*args*/)
{
    uint32 counter = 0;                                     // Counter for figure out that we found smth.

    Player* player = m_session->GetPlayer();

    // Search in AreaTable.dbc
    for (auto const& itr : sObjectMgr.GetAreaTriggersMap())
    {
        AreaTriggerEntry const* atEntry = &itr.second;
        if (!atEntry)
            continue;

        if (!IsPointInAreaTriggerZone(atEntry, player->GetMapId(), player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()))
            continue;

        ShowTriggerListHelper(atEntry);

        ++counter;
    }

    if (counter == 0)                                      // if counter == 0 then we found nth
        SendSysMessage(LANG_COMMAND_NOTRIGGERFOUND);

    return true;
}

bool ChatHandler::HandleTriggerNearCommand(char* args)
{
    float distance = (!*args) ? 10.0f : (float)atof(args);
    float dist2 =  distance * distance;
    uint32 counter = 0;                                     // Counter for figure out that we found smth.

    Player* player = m_session->GetPlayer();

    // Search triggers
    for (auto const& itr : sObjectMgr.GetAreaTriggersMap())
    {
        AreaTriggerEntry const* atEntry = &itr.second;
        if (!atEntry)
            continue;

        if (atEntry->map_id != m_session->GetPlayer()->GetMapId())
            continue;

        float dx = atEntry->x - player->GetPositionX();
        float dy = atEntry->y - player->GetPositionY();

        if (dx * dx + dy * dy > dist2)
            continue;

        ShowTriggerListHelper(atEntry);

        ++counter;
    }

    // Search trigger targets
    for (auto const& itr : sObjectMgr.GetAreaTriggersMap())
    {
        AreaTriggerEntry const* atEntry = &itr.second;
        if (!atEntry)
            continue;

        AreaTriggerTeleport const* at = sObjectMgr.GetAreaTriggerTeleport(atEntry->id);
        if (!at)
            continue;

        if (at->destination.mapId != m_session->GetPlayer()->GetMapId())
            continue;

        float dx = at->destination.x - player->GetPositionX();
        float dy = at->destination.y - player->GetPositionY();

        if (dx * dx + dy * dy > dist2)
            continue;

        ShowTriggerTargetListHelper(atEntry->id, at);

        ++counter;
    }

    if (counter == 0)                                      // if counter == 0 then we found nth
        SendSysMessage(LANG_COMMAND_NOTRIGGERFOUND);

    return true;
}

bool ChatHandler::HandleCinematicAddWpCommand(char *args)
{
    uint32 cinematic_id = 0;
    uint32 timer = 0;
    char comment[100];

    sscanf(args, "%u %u %s", &cinematic_id, &timer, comment);

    Player* me = m_session->GetPlayer();
    WorldDatabase.PExecute(
        "INSERT INTO `cinematic_waypoints` (`cinematic`, `timer`, `position_x`, `position_y`, `position_z`, `comment`) VALUES "
        "(%u, %u, %f, %f, %f, '%s')",
        cinematic_id, timer,
        ceil(me->GetPositionX()), ceil(me->GetPositionY()), ceil(me->GetPositionZ()), comment
    );

    PSendSysMessage("Added new waypoint at %u ms for cinematic %u.", timer, cinematic_id);
    sObjectMgr.LoadCinematicsWaypoints();
    return true;
}

bool ChatHandler::HandleCinematicGoTimeCommand(char *args)
{
    uint32 cinematic_id = 0;
    uint32 time = 0;

    sscanf(args, "%u %u", &cinematic_id, &time);

    Player* me = m_session->GetPlayer();
    Position const* tpPosition = sObjectMgr.GetCinematicPosition(cinematic_id, time);
    if (!tpPosition)
    {
        SendSysMessage("Cannot find location.");
        SetSentErrorMessage(true);
        return false;
    }

    if (me)
    {
        me->TeleportTo(me->GetMapId(), tpPosition->x, tpPosition->y, tpPosition->z, 0.0f);
        PSendSysMessage("You are now at position (%f %f %f)", tpPosition->x, tpPosition->y, tpPosition->z);
    }
    else
        PSendSysMessage("Position found (%f %f %f)", tpPosition->x, tpPosition->y, tpPosition->z);

    return true;
}

bool ChatHandler::HandleCinematicListWpCommand(char *args)
{
    // TODO
    // Donne une liste des WayPoints en fonction de la cinematique envoyee
    //
    // Exemple :
    // .cine listwp 41
    //
    return true;
}

// BG
#define COLOR_HORDE      "FF3300"
#define COLOR_ALLIANCE   "0066B3"
#define COLOR_BG         "D580FE"
#define COLOR_INFO       "FF9900"
#define COLOR_STATUS     "FECCBF"
#define DO_COLOR(a, b)   "|cff" a "" b "|r"

typedef std::map<ObjectGuid, BattleGroundPlayer> BattleGroundPlayerMap;
bool ChatHandler::HandleBGStatusCommand(char *args)
{
    Player* chr = m_session->GetPlayer();
    ASSERT(chr);
    SendSysMessage(DO_COLOR(COLOR_INFO, "-- Currently running BGs"));
    uint8 i = 0;
    uint8 uiAllianceCount, uiHordeCount;
    for (int8 bgTypeId = BATTLEGROUND_AB; bgTypeId >= BATTLEGROUND_AV; --bgTypeId)
    {
        for (BattleGroundSet::const_iterator it = sBattleGroundMgr.GetBattleGroundsBegin(BattleGroundTypeId(bgTypeId)); it != sBattleGroundMgr.GetBattleGroundsEnd(BattleGroundTypeId(bgTypeId)); ++it)
        {
            // Pas un "vrai" BG, mais un "modele" de BG.
            if (!it->first)
                continue;

            ++i;
            uiAllianceCount = 0;
            uiHordeCount    = 0;
            BattleGroundPlayerMap const& pPlayers = it->second->GetPlayers();
            std::string playerName;

            for (const auto& itr : pPlayers)
            {
                if (itr.second.playerTeam == HORDE)
                    uiHordeCount++;
                else
                    uiAllianceCount++;
                if (playerName.empty())
                    if (sObjectMgr.GetPlayerNameByGUID(itr.first, playerName))
                        playerName = playerLink(playerName);
            }

            std::string statusName;
            BattleGroundStatus status = it->second->GetStatus();
            switch (status)
            {
                case STATUS_WAIT_JOIN:
                    statusName = "WaitJoin";
                    break;
                case STATUS_IN_PROGRESS:
                    statusName = "InProgress";
                    break;
                case STATUS_WAIT_LEAVE:
                    statusName = "WaitLeave";
                    break;
            }

            PSendSysMessage(DO_COLOR(COLOR_BG, "[%s %2u]") " [%2u-%2u] "
                    DO_COLOR(COLOR_STATUS, "[%s]")
                    DO_COLOR(COLOR_ALLIANCE, "%2u") "vs" DO_COLOR(COLOR_HORDE, "%2u")
                    " Player:%s %s",
                    it->second->GetName(), it->first, it->second->GetMinLevel(), it->second->GetMaxLevel(), statusName.c_str(),
                    uiAllianceCount, uiHordeCount,
                    playerName.c_str(), secsToTimeString(it->second->GetStartTime() / 1000, true).c_str());
        }
    }
    if (!i)
        PSendSysMessage(DO_COLOR(COLOR_INFO, "(No battleground started)"));

    PSendSysMessage(DO_COLOR(COLOR_INFO, "-- Queues for your bracket"));
    i = 0;

    for (uint8 bgTypeId = BATTLEGROUND_AV; bgTypeId <= BATTLEGROUND_AB; ++bgTypeId)
    {
        ++i;
        uiAllianceCount = 0;
        uiHordeCount    = 0;

        BattleGroundQueueTypeId bgQueueTypeId = BattleGroundMgr::BgQueueTypeId(BattleGroundTypeId(bgTypeId));
        // Must be a reference (&), otherwise crash later on ...
        BattleGroundQueue& queue = sBattleGroundMgr.m_battleGroundQueues[bgQueueTypeId];
        for (const auto& itr : queue.m_queuedPlayers)
        {
            if (itr.second.groupInfo->groupTeam == HORDE)
                uiHordeCount++;
            else
                uiAllianceCount++;
        }

        BattleGround* bgTemplate = sBattleGroundMgr.GetBattleGroundTemplate(BattleGroundTypeId(bgTypeId));
        ASSERT(bgTemplate);

        PSendSysMessage(DO_COLOR(COLOR_BG, "[%s]" "   " DO_COLOR(COLOR_ALLIANCE, "[Alliance] : %2u") " - " DO_COLOR(COLOR_HORDE, "[Horde] : %2u")),
                        bgTemplate->GetName(), uiAllianceCount, uiHordeCount);
    }
    if (!i)
        PSendSysMessage(DO_COLOR(COLOR_INFO, "(No player queued)"));
    return true;
}

bool ChatHandler::HandleBGStartCommand(char *args)
{
    Player* chr = m_session->GetPlayer();
    ASSERT(chr);
    BattleGround* pBg = chr->GetBattleGround();
    if (!pBg)
    {
        SendSysMessage("Vous devez etre dans un champs de bataille pour utiliser cette commande.");
        SetSentErrorMessage(true);
        return false;
    }
    pBg->SetStartDelayTime(0);
    PSendSysMessage("BG commence [%s][%u]", pBg->GetName(), pBg->GetInstanceID());
    return true;
}

bool ChatHandler::HandleBGStopCommand(char *args)
{
    Player* chr = m_session->GetPlayer();
    ASSERT(chr);
    BattleGround* pBg = chr->GetBattleGround();
    if (!pBg)
    {
        SendSysMessage("You are not in a battleground");
        SetSentErrorMessage(true);
        return false;
    }
    pBg->StopBattleGround();
    PSendSysMessage("Battleground stopped [%s][%u]", pBg->GetName(), pBg->GetInstanceID());
    return true;
}

bool ChatHandler::HandleBGCustomCommand(char *args)
{
    Player* chr = m_session->GetPlayer();
    ASSERT(chr);
    BattleGround* pBg = chr->GetBattleGround();
    if (!pBg)
    {
        SendSysMessage("You are not in a battleground");
        SetSentErrorMessage(true);
        return false;
    }
    pBg->HandleCommand(chr, this, args);
    return true;

}

bool ChatHandler::HandleLinkGraveCommand(char* args)
{
    uint32 g_id;
    if (!ExtractUInt32(&args, g_id))
        return false;

    char* teamStr = ExtractLiteralArg(&args);

    Team g_team;
    if (!teamStr)
        g_team = TEAM_NONE;
    else if (strncmp(teamStr, "horde", strlen(teamStr)) == 0)
        g_team = HORDE;
    else if (strncmp(teamStr, "alliance", strlen(teamStr)) == 0)
        g_team = ALLIANCE;
    else
        return false;

    WorldSafeLocsEntry const* graveyard =  sWorldSafeLocsStore.LookupEntry(g_id);

    if (!graveyard)
    {
        PSendSysMessage(LANG_COMMAND_GRAVEYARDNOEXIST, g_id);
        SetSentErrorMessage(true);
        return false;
    }

    Player* player = m_session->GetPlayer();

    uint32 zoneId = player->GetZoneId();

    const auto *areaEntry = AreaEntry::GetById(zoneId);
    if (!areaEntry || !areaEntry->IsZone())
    {
        PSendSysMessage(LANG_COMMAND_GRAVEYARDWRONGZONE, g_id, zoneId);
        SetSentErrorMessage(true);
        return false;
    }

    if (sObjectMgr.AddGraveYardLink(g_id, zoneId, g_team))
        PSendSysMessage(LANG_COMMAND_GRAVEYARDLINKED, g_id, zoneId);
    else
        PSendSysMessage(LANG_COMMAND_GRAVEYARDALRLINKED, g_id, zoneId);

    return true;
}

/*
 * .arena rating [$playername] - the arena rating of a character, all four brackets.
 *
 * There is no interface for this in 1.12 outside the arena orb, and the orb only ever shows the
 * player his own numbers.
 */
bool ChatHandler::HandleArenaRatingCommand(char* args)
{
    Player* target;
    ObjectGuid targetGuid;
    std::string targetName;
    if (!ExtractPlayerTarget(&args, &target, &targetGuid, &targetName))
        return false;

    ObjectGuid const guid = target ? target->GetObjectGuid() : targetGuid;

    PSendSysMessage("Arena rating of %s:", targetName.c_str());
    for (uint8 index = 0; index < ARENA_TYPES_COUNT; ++index)
    {
        ArenaType const type = GetArenaTypeByIndex(index);
        ArenaRatingEntry const entry = sArenaRatingMgr.Get(guid, type);

        if (!sArenaRatingMgr.HasPlayed(guid, type))
        {
            PSendSysMessage("  %s: never played (would start at %u, mmr %u)", GetArenaTypeName(type), entry.rating, entry.mmr);
            continue;
        }

        PSendSysMessage("  %s: rating %u (best %u), mmr %u, %u played, %u won",
                        GetArenaTypeName(type), entry.rating, entry.bestRating, entry.mmr, entry.games, entry.wins);
    }

    return true;
}

/*
 * .arena setrating $bracket $rating [$mmr] - sets a character's rating, for testing and for
 * putting somebody back where he belongs after a mishap. Bracket is the team size: 1, 2, 3 or 5.
 */
bool ChatHandler::HandleArenaSetRatingCommand(char* args)
{
    if (!*args)
        return false;

    if (!sArenaRatingMgr.IsAvailable())
    {
        SendSysMessage("Table `character_arena_stats` is missing - apply sql/arena/characters_arena.sql.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 bracket = 0;
    uint32 rating = 0;
    if (!ExtractUInt32(&args, bracket) || !ExtractUInt32(&args, rating))
        return false;

    ArenaType const type = ArenaType(bracket);
    if (type != ARENA_TYPE_1V1 && type != ARENA_TYPE_2V2 && type != ARENA_TYPE_3V3 && type != ARENA_TYPE_5V5)
    {
        SendSysMessage("Bracket must be the team size: 1, 2, 3 or 5.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 mmr = 0;
    bool const mmrGiven = ExtractOptUInt32(&args, mmr, 0);

    Player* target;
    ObjectGuid targetGuid;
    std::string targetName;
    if (!ExtractPlayerTarget(&args, &target, &targetGuid, &targetName))
        return false;

    ObjectGuid const guid = target ? target->GetObjectGuid() : targetGuid;
    if (!mmrGiven || !mmr)
        mmr = sArenaRatingMgr.Get(guid, type).mmr;          // leave the matchmaking rating where it is

    sArenaRatingMgr.Set(guid, type, rating, mmr);
    PSendSysMessage("%s: %s rating set to %u, mmr %u.", targetName.c_str(), GetArenaTypeName(type), rating, mmr);
    return true;
}

// .arena resetratings [$bracket] - wipes a ladder, or all four of them
bool ChatHandler::HandleArenaResetRatingsCommand(char* args)
{
    ArenaType type = ARENA_TYPE_NONE;
    if (*args)
    {
        uint32 bracket = 0;
        if (!ExtractUInt32(&args, bracket))
            return false;

        type = ArenaType(bracket);
        if (type != ARENA_TYPE_1V1 && type != ARENA_TYPE_2V2 && type != ARENA_TYPE_3V3 && type != ARENA_TYPE_5V5)
        {
            SendSysMessage("Bracket must be the team size: 1, 2, 3 or 5.");
            SetSentErrorMessage(true);
            return false;
        }
    }

    uint32 const removed = sArenaRatingMgr.Reset(type);
    if (type == ARENA_TYPE_NONE)
        PSendSysMessage("All arena ratings dropped (%u rows).", removed);
    else
        PSendSysMessage("%s arena ratings dropped (%u rows).", GetArenaTypeName(type), removed);
    return true;
}

/*
 * .arena setmmr $bracket $mmr [$playername] - the matchmaking rating on its own.
 *
 * It decides who a player is matched against and is the number worth correcting after a rating was
 * moved by hand; .arena setrating needs the visible rating as well, which is not always what one
 * wants to touch.
 */
bool ChatHandler::HandleArenaSetMatchmakerRatingCommand(char* args)
{
    if (!*args)
        return false;

    if (!sArenaRatingMgr.IsAvailable())
    {
        SendSysMessage("Table `character_arena_stats` is missing - apply sql/arena/characters_arena.sql.");
        SetSentErrorMessage(true);
        return false;
    }

    uint32 bracket = 0;
    uint32 mmr = 0;
    if (!ExtractUInt32(&args, bracket) || !ExtractUInt32(&args, mmr))
        return false;

    ArenaType const type = ArenaType(bracket);
    if (type != ARENA_TYPE_1V1 && type != ARENA_TYPE_2V2 && type != ARENA_TYPE_3V3 && type != ARENA_TYPE_5V5)
    {
        SendSysMessage("Bracket must be the team size: 1, 2, 3 or 5.");
        SetSentErrorMessage(true);
        return false;
    }

    Player* target;
    ObjectGuid targetGuid;
    std::string targetName;
    if (!ExtractPlayerTarget(&args, &target, &targetGuid, &targetName))
        return false;

    ObjectGuid const guid = target ? target->GetObjectGuid() : targetGuid;
    ArenaRatingEntry const entry = sArenaRatingMgr.Get(guid, type);

    sArenaRatingMgr.Set(guid, type, entry.rating, mmr);
    PSendSysMessage("%s: %s matchmaking rating set to %u (rating stays %u).",
                    targetName.c_str(), GetArenaTypeName(type), mmr, entry.rating);
    return true;
}

/*********************************************************/
/***                 ARENA ADMIN PANEL                 ***/
/*********************************************************/

namespace
{

// A bracket list as an admin types it: "all", "none", or any mix of 1v1 / 2v2 / 3v3 / 5v5 (also
// written 1, 2, 3, 5), separated by commas or spaces.
bool ParseArenaBrackets(char const* text, bool out[ARENA_TYPES_COUNT])
{
    for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
        out[i] = false;

    if (!text || !*text)
    {
        for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
            out[i] = true;                              // no list given means every bracket
        return true;
    }

    std::string const list(text);
    if (list.find("all") != std::string::npos)
    {
        for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
            out[i] = true;
        return true;
    }
    if (list.find("none") != std::string::npos)
        return true;

    bool any = false;
    for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
    {
        ArenaType const type = GetArenaTypeByIndex(i);
        if (list.find(GetArenaTypeName(type)) != std::string::npos)
        {
            out[i] = true;
            any = true;
        }
    }

    if (!any)                                           // plain numbers: "1 2 3 5"
    {
        for (size_t pos = 0; pos < list.length(); ++pos)
        {
            switch (list[pos])
            {
                case '1': out[0] = true; any = true; break;
                case '2': out[1] = true; any = true; break;
                case '3': out[2] = true; any = true; break;
                case '5': out[3] = true; any = true; break;
                default: break;
            }
        }
    }

    return any;
}

// The settings the panel and .arena set may change while the server runs. Arena.Enable is not among
// them on purpose: the battleground templates are built once at startup.
struct ArenaBoolOption { char const* name; eConfigBoolValues config; };
struct ArenaUInt32Option { char const* name; eConfigUInt32Values config; uint32 min; uint32 max; };

ArenaBoolOption const s_arenaBoolOptions[] =
{
    { "AllowItemSwap",        CONFIG_BOOL_ARENA_ALLOW_ITEM_SWAP },
    { "AllowTrinketSwap",     CONFIG_BOOL_ARENA_ALLOW_TRINKET_SWAP },
    { "RandomWeather",        CONFIG_BOOL_ARENA_RANDOM_WEATHER },
    { "Spectate",             CONFIG_BOOL_ARENA_SPECTATE },
    { "ResetAllCooldowns",    CONFIG_BOOL_ARENA_RESET_ALL_COOLDOWNS },
    { "AnnounceQueue",        CONFIG_BOOL_ARENA_ANNOUNCE_QUEUE },
    { "LeaveQueuesOnLogout",  CONFIG_BOOL_ARENA_LEAVE_QUEUES_ON_LOGOUT },
    { "1v1.BlockHealerSpecs", CONFIG_BOOL_ARENA_1V1_BLOCK_HEALER_SPECS },
    { "AbsorbCountsAsHealing", CONFIG_BOOL_ARENA_ABSORB_COUNTS_AS_HEALING },
    { "NagrandTornado",       CONFIG_BOOL_ARENA_NAGRAND_TORNADO },
};

ArenaUInt32Option const s_arenaUInt32Options[] =
{
    { "MaxItemLevel",                CONFIG_UINT32_ARENA_MAX_ITEM_LEVEL,             1, 999 },
    { "MaxItemPatch",                CONFIG_UINT32_ARENA_MAX_ITEM_PATCH,             0, 10 },
    { "TimeLimitMinutes",            CONFIG_UINT32_ARENA_TIME_LIMIT_MINUTES,         1, 120 },
    { "ReadyStartDelaySeconds",      CONFIG_UINT32_ARENA_READY_START_DELAY_SECONDS,  3, 60 },
    { "InviteAcceptTimeSeconds",     CONFIG_UINT32_ARENA_INVITE_ACCEPT_TIME_SECONDS, 10, 80 },
    { "MinLevel",                    CONFIG_UINT32_ARENA_MIN_LEVEL,                  1, PLAYER_MAX_LEVEL },
    { "LeaveLockoutMinutes",         CONFIG_UINT32_ARENA_LEAVE_LOCKOUT_MINUTES,      0, 24 * 60 },
    { "MaxResistance.Fire",          CONFIG_UINT32_ARENA_MAX_RES_FIRE,               0, 10000 },
    { "MaxResistance.Nature",        CONFIG_UINT32_ARENA_MAX_RES_NATURE,             0, 10000 },
    { "MaxResistance.Frost",         CONFIG_UINT32_ARENA_MAX_RES_FROST,              0, 10000 },
    { "MaxResistance.Shadow",        CONFIG_UINT32_ARENA_MAX_RES_SHADOW,             0, 10000 },
    { "MaxResistance.Arcane",        CONFIG_UINT32_ARENA_MAX_RES_ARCANE,             0, 10000 },
    { "Rated.Mode",                  CONFIG_UINT32_ARENA_RATED_MODE,                 0, 2 },
    { "Rated.StartRating",           CONFIG_UINT32_ARENA_RATED_START_RATING,         0, 5000 },
    { "Rated.StartMatchmakerRating", CONFIG_UINT32_ARENA_RATED_START_MMR,            1, 5000 },
    { "Rated.WinModifierLow",        CONFIG_UINT32_ARENA_RATED_WIN_MODIFIER_LOW,     1, 1000 },
    { "Rated.WinModifier",           CONFIG_UINT32_ARENA_RATED_WIN_MODIFIER,         1, 1000 },
    { "Rated.LoseModifier",          CONFIG_UINT32_ARENA_RATED_LOSE_MODIFIER,        1, 1000 },
    { "Rated.MatchmakerModifier",    CONFIG_UINT32_ARENA_RATED_MMR_MODIFIER,         1, 1000 },
    { "Rated.DrawRatingLoss",        CONFIG_UINT32_ARENA_RATED_DRAW_LOSS,            0, 1000 },
    { "Rated.MaxRatingDifference",   CONFIG_UINT32_ARENA_RATED_MAX_MMR_DIFFERENCE,   0, 5000 },
    { "Rated.RatingDiscardMinutes",  CONFIG_UINT32_ARENA_RATED_MMR_DISCARD_MINUTES,  0, 60 },
    { "Rated.LadderMinGames",        CONFIG_UINT32_ARENA_RATED_LADDER_MIN_GAMES,     0, 1000 },
    { "NagrandTornado.DamagePercent", CONFIG_UINT32_ARENA_NAGRAND_TORNADO_DAMAGE_PCT,  0, 50 },
    { "Rated.MinLevel",              CONFIG_UINT32_ARENA_RATED_MIN_LEVEL,            1, PLAYER_MAX_LEVEL },
};

// How many ban list rows one panel request answers with. The table holds several hundred entries and
// every row is a chat packet of its own - the panel asks for the next page instead.
uint32 const ARENA_PANEL_PAGE = 50;

} // namespace

// .arena set [$option [$value]] - lists or changes a setting for as long as the server runs
bool ChatHandler::HandleArenaSetCommand(char* args)
{
    char* option = ExtractLiteralArg(&args);
    if (!option)
    {
        SendSysMessage("Arena settings (runtime only - put them in mangosd.conf to keep them):");
        for (auto const& entry : s_arenaBoolOptions)
            PSendSysMessage("  Arena.%s = %s", entry.name, sWorld.getConfig(entry.config) ? "1" : "0");
        for (auto const& entry : s_arenaUInt32Options)
            PSendSysMessage("  Arena.%s = %u", entry.name, sWorld.getConfig(entry.config));
        return true;
    }

    // "Arena.MaxItemLevel" and "MaxItemLevel" both work
    std::string name(option);
    if (name.compare(0, 6, "Arena.") == 0)
        name = name.substr(6);

    // Not a config value but the same kind of switch: which arena the queue picks. The orb's admin
    // menu can do this too; here it is reachable without walking to the orb.
    if (name == "Map")
    {
        char* pick = ExtractLiteralArg(&args);
        if (!pick)
        {
            ArenaMapType const forced = sBattleGroundMgr.GetForcedArenaMap();
            PSendSysMessage("Arena.Map = %s", forced < ARENA_MAPS_COUNT ? GetArenaMapName(forced) : "any");
            return true;
        }

        std::string wanted(pick);
        if (wanted == "any" || wanted == "all")
        {
            sBattleGroundMgr.SetForcedArenaMap(ArenaMapType(ARENA_MAPS_COUNT));
            SendSysMessage("The queue picks an arena as usual again.");
            return true;
        }

        uint32 index = ARENA_MAPS_COUNT;
        if (wanted.length() == 1 && wanted[0] >= '0' && wanted[0] <= '5')
            index = uint32(wanted[0] - '0');
        else
            for (uint32 i = 0; i < ARENA_MAPS_COUNT; ++i)
                if (strstr(GetArenaMapName(ArenaMapType(i)), pick))
                    index = i;

        if (index >= ARENA_MAPS_COUNT)
        {
            SendSysMessage("Arena.Map takes any, 0-5, or part of an arena's name:");
            for (uint32 i = 0; i < ARENA_MAPS_COUNT; ++i)
                PSendSysMessage("  %u = %s", i, GetArenaMapName(ArenaMapType(i)));
            SetSentErrorMessage(true);
            return false;
        }

        sBattleGroundMgr.SetForcedArenaMap(ArenaMapType(index));
        PSendSysMessage("Every arena match is played in %s until this is released again.", GetArenaMapName(ArenaMapType(index)));
        return true;
    }

    for (auto const& entry : s_arenaBoolOptions)
    {
        if (name != entry.name)
            continue;

        uint32 value = 0;
        if (!ExtractUInt32(&args, value))
        {
            PSendSysMessage("Arena.%s = %s", entry.name, sWorld.getConfig(entry.config) ? "1" : "0");
            return true;
        }

        sWorld.setConfig(entry.config, value != 0);
        PSendSysMessage("Arena.%s set to %s.", entry.name, value ? "1" : "0");
        return true;
    }

    for (auto const& entry : s_arenaUInt32Options)
    {
        if (name != entry.name)
            continue;

        uint32 value = 0;
        if (!ExtractUInt32(&args, value))
        {
            PSendSysMessage("Arena.%s = %u", entry.name, sWorld.getConfig(entry.config));
            return true;
        }

        if (value < entry.min || value > entry.max)
        {
            PSendSysMessage("Arena.%s takes %u to %u.", entry.name, entry.min, entry.max);
            SetSentErrorMessage(true);
            return false;
        }

        sWorld.setConfig(entry.config, value);
        PSendSysMessage("Arena.%s set to %u.", entry.name, value);
        return true;
    }

    PSendSysMessage("There is no arena setting called %s. Use .arena set without arguments for the list.", option);
    SetSentErrorMessage(true);
    return false;
}

/*
 * Reads either a spell id or "item <id>" / an item link. Items are what an admin actually wants to
 * ban most of the time; the table can only hold spells, so an item is banned through the spells it
 * casts. Returns false and explains itself if the item has no usable spell at all.
 */
bool ChatHandler::ExtractArenaBanTarget(char** args, std::vector<uint32>& spells, std::string& what)
{
    // A shift-clicked item is a link and never starts with the word "item", so an item counts as
    // named either way: by the keyword, or by the link an admin dropped into the line.
    uint32 itemId = 0;
    bool const byLink = **args == '|' && ExtractUint32KeyFromLink(args, "Hitem", itemId);
    char* first = byLink ? *args : ExtractLiteralArg(args, "item");
    if (first)
    {
        if (!byLink && !ExtractUint32KeyFromLink(args, "Hitem", itemId) && !ExtractUInt32(args, itemId))
            return false;

        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
        if (!proto)
        {
            PSendSysMessage("There is no item %u.", itemId);
            SetSentErrorMessage(true);
            return false;
        }

        ArenaMgr::GetItemSpells(proto, spells);
        if (spells.empty())
        {
            PSendSysMessage("|cffffffff|Hitem:%u:0:0:0|h[%s]|h|r has no spell on it - there is nothing to ban.",
                                     itemId, proto->Name1);
            SetSentErrorMessage(true);
            return false;
        }

        std::ostringstream ss;
        ss << "|cffffffff|Hitem:" << itemId << ":0:0:0|h[" << proto->Name1 << "]|h|r";
        what = ss.str();
        return true;
    }

    uint32 spellId = 0;
    if (!ExtractUint32KeyFromLink(args, "Hspell", spellId) && !ExtractUInt32(args, spellId))
        return false;

    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    if (!spell)
    {
        PSendSysMessage("There is no spell %u.", spellId);
        SetSentErrorMessage(true);
        return false;
    }

    spells.push_back(spellId);
    std::ostringstream ss;
    ss << "|cffffffff|Hspell:" << spellId << "|h[" << spell->SpellName[0] << "]|h|r";
    what = ss.str();
    return true;
}

// .arena ban <$spellId | item $itemId> [$brackets] - forbids it in the arena
bool ChatHandler::HandleArenaBanCommand(char* args)
{
    if (!*args)
        return false;

    std::vector<uint32> spells;
    std::string what;
    if (!ExtractArenaBanTarget(&args, spells, what))
        return false;

    bool brackets[ARENA_TYPES_COUNT];
    if (!ParseArenaBrackets(args, brackets))
    {
        SendSysMessage("Brackets: all, or any of 1v1 2v2 3v3 5v5.");
        SetSentErrorMessage(true);
        return false;
    }

    for (uint32 spellId : spells)
        sArenaMgr.SetSpellDisabled(spellId, brackets);

    std::ostringstream where;
    for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
        if (brackets[i])
            where << (where.str().empty() ? "" : ", ") << GetArenaTypeName(GetArenaTypeByIndex(i));

    if (where.str().empty())
        PSendSysMessage("%s is allowed in every arena again.", what.c_str());
    else
        PSendSysMessage("%s is banned in %s (%u spell%s).", what.c_str(), where.str().c_str(),
                        uint32(spells.size()), spells.size() == 1 ? "" : "s");
    return true;
}

// .arena unban <$spellId | item $itemId> - the same thing with an empty bracket list
bool ChatHandler::HandleArenaUnbanCommand(char* args)
{
    if (!*args)
        return false;

    std::vector<uint32> spells;
    std::string what;
    if (!ExtractArenaBanTarget(&args, spells, what))
        return false;

    bool const none[ARENA_TYPES_COUNT] = { false, false, false, false };
    for (uint32 spellId : spells)
        sArenaMgr.SetSpellDisabled(spellId, none);

    PSendSysMessage("%s is allowed in every arena again.", what.c_str());
    return true;
}

// .arena list - the matches that are running right now
bool ChatHandler::HandleArenaListCommand(char* /*args*/)
{
    uint32 shown = 0;
    for (uint32 bgTypeId = BATTLEGROUND_ARENA_FIRST; bgTypeId <= BATTLEGROUND_ARENA_LAST; ++bgTypeId)
    {
        for (BattleGroundSet::const_iterator itr = sBattleGroundMgr.GetBattleGroundsBegin(BattleGroundTypeId(bgTypeId));
             itr != sBattleGroundMgr.GetBattleGroundsEnd(BattleGroundTypeId(bgTypeId)); ++itr)
        {
            BattleGround* bg = itr->second;
            if (!itr->first || !bg->GetPlayersSize())   // 0 is the template, not a match
                continue;

            Arena* arena = static_cast<Arena*>(bg);
            std::ostringstream names;
            for (auto const& player : bg->GetPlayers())
            {
                std::string name;
                if (sObjectMgr.GetPlayerNameByGUID(player.first, name))
                    names << (names.str().empty() ? "" : ", ") << name;
            }

            PSendSysMessage("  #%u %s (%s%s) - %s", bg->GetInstanceID(), bg->GetName(),
                            GetArenaTypeName(arena->GetArenaType()), arena->IsRated() ? ", rated" : "",
                            names.str().c_str());
            ++shown;
        }
    }

    if (!shown)
        SendSysMessage("No arena match is running.");
    return true;
}

namespace
{

// Everybody watching a match without fighting in it. Visitors are known to the map, not to the
// battleground - the arena's own player list holds the fighters only.
// 1234567 -> "1.2M", 45678 -> "45.7k". A running match's damage column is read at a glance or not
// at all, and eight digits in a list of four players is not a glance.
static std::string ShortNumber(uint32 value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    if (value >= 1000000)
        out << (value / 1000000.0) << "M";
    else if (value >= 10000)
        out << (value / 1000.0) << "k";
    else
        out << value;
    return out.str();
}

void CollectArenaSpectators(BattleGround* bg, std::vector<Player*>& out)
{
    if (!bg->GetBgMap())
        return;

    Map::PlayerList const& players = bg->GetBgMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* player = itr->getSource();
        if (!player || !player->IsArenaSpectator())
            continue;
        if (bg->GetPlayers().find(player->GetObjectGuid()) != bg->GetPlayers().end())
            continue;                                   // a dead fighter is a ghost, not a visitor

        out.push_back(player);
    }
}

} // namespace

// .arena spectators - who is watching which match
bool ChatHandler::HandleArenaSpectatorsCommand(char* /*args*/)
{
    uint32 shown = 0;
    for (uint32 bgTypeId = BATTLEGROUND_ARENA_FIRST; bgTypeId <= BATTLEGROUND_ARENA_LAST; ++bgTypeId)
    {
        for (BattleGroundSet::const_iterator itr = sBattleGroundMgr.GetBattleGroundsBegin(BattleGroundTypeId(bgTypeId));
             itr != sBattleGroundMgr.GetBattleGroundsEnd(BattleGroundTypeId(bgTypeId)); ++itr)
        {
            if (!itr->first)
                continue;

            std::vector<Player*> spectators;
            CollectArenaSpectators(itr->second, spectators);
            if (spectators.empty())
                continue;

            std::ostringstream names;
            for (Player* player : spectators)
                names << (names.str().empty() ? "" : ", ") << player->GetName();

            PSendSysMessage("  #%u %s: %s", itr->second->GetInstanceID(), itr->second->GetName(), names.str().c_str());
            ++shown;
        }
    }

    if (!shown)
        SendSysMessage("Nobody is spectating.");
    return true;
}

/*
 * .arena kickspectators [$instanceId] - sends the watchers of one match, or of all of them, home.
 *
 * Safe from here: gamemaster commands run on the world thread, and the world thread does not run
 * while the maps update - which is the whole reason commands are deferred onto it.
 */
bool ChatHandler::HandleArenaKickSpectatorsCommand(char* args)
{
    uint32 instanceId = 0;
    ExtractOptUInt32(&args, instanceId, 0);

    uint32 kicked = 0;
    for (uint32 bgTypeId = BATTLEGROUND_ARENA_FIRST; bgTypeId <= BATTLEGROUND_ARENA_LAST; ++bgTypeId)
    {
        for (BattleGroundSet::const_iterator itr = sBattleGroundMgr.GetBattleGroundsBegin(BattleGroundTypeId(bgTypeId));
             itr != sBattleGroundMgr.GetBattleGroundsEnd(BattleGroundTypeId(bgTypeId)); ++itr)
        {
            if (!itr->first || (instanceId && itr->second->GetInstanceID() != instanceId))
                continue;

            std::vector<Player*> spectators;
            CollectArenaSpectators(itr->second, spectators);
            kicked += uint32(spectators.size());
            static_cast<Arena*>(itr->second)->RemoveSpectators();
        }
    }

    PSendSysMessage("%u spectator%s sent home.", kicked, kicked == 1 ? "" : "s");
    return true;
}

/*
 * .arena panel $what [$argument] - the same data, in one line per record, for the admin addon.
 *
 * The 1.12 client can not ask the server anything an addon could read, so the panel sends these
 * commands as chat and reads the answers back out of the system chat. Every line starts with
 * ARENA| so the addon can recognise its own traffic and keep it out of the chat window.
 */
/*
 * Everything the player's own arena window needs, in one answer.
 *
 * The 1.12 client has no arena interface of any kind - no JoinArena, no rating call, no queue
 * opcode - so a window that shows a rating and a queue has to be told all of it. This is the same
 * arrangement the admin panel already uses and has proved: a chat command in, machine readable
 * "ARENA|..." lines out, hidden from the chat frame by the AddOn that asked for them.
 *
 * SEC_PLAYER, and it only ever reports on the caller himself - there is nothing here that another
 * player's name could be substituted into.
 *
 *   ARENA|qb|<index>|<token>|<open>|<rating>|<best>|<games>|<wins>|<waiting>|<queued>
 *       one line per bracket, index 0..3 = 1v1 2v2 3v3 5v5
 *       token   the bracket in the player's own language ("2v2", "2c2", "2對2" ...)
 *       open    1 when this realm has that bracket set up at all
 *       queued  1 when he is standing in that queue right now
 *   ARENA|qi|<enabled>|<minLevel>|<ratedMinLevel>|<ratedMode>|<myLevel>|<inBg>
 *   ARENA|done|info
 */
bool ChatHandler::HandleArenaQueueInfoCommand(char* /*args*/)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    // all four ladder places in one walk of the rating map rather than one walk per bracket
    uint32 rankOf[ARENA_TYPES_COUNT];
    uint32 totalOf[ARENA_TYPES_COUNT];
    sArenaRatingMgr.GetRanks(player->GetObjectGuid(), rankOf, totalOf);

    /*
     * Worth knowing when this goes quiet: ChatHandler::ParseCommands drops every command from a
     * SEC_PLAYER session when PlayerCommands is off, before it ever reaches here. The default is on,
     * but a realm that turns it off turns the arena window off with it - there is no other way for a
     * 1.12 client to ask the server anything - and the symptom is a window that simply never fills.
     */

    for (uint8 index = 0; index < ARENA_TYPES_COUNT; ++index)
    {
        ArenaType const type = GetArenaTypeByIndex(index);

        // "open" means the realm has a battleground template for that bracket on any of its arenas
        BattleGround* bgTemplate = nullptr;
        for (uint8 map = 0; map < ARENA_MAPS_COUNT && !bgTemplate; ++map)
            bgTemplate = sBattleGroundMgr.GetBattleGroundTemplate(GetArenaBattleGroundTypeId(ArenaMapType(map), type));

        uint32 waiting = 0;
        bool queued = false;
        if (bgTemplate)
        {
            BattleGroundBracketId const bracketId = player->GetBattleGroundBracketIdFromLevel(bgTemplate->GetTypeID());
            for (uint8 map = 0; map < ARENA_MAPS_COUNT; ++map)
            {
                BattleGroundTypeId const bgTypeId = GetArenaBattleGroundTypeId(ArenaMapType(map), type);
                waiting += sBattleGroundMgr.GetArenaPlayersWaitingCount(bgTypeId, bracketId);
                if (player->InBattleGroundQueueForBattleGroundQueueType(BattleGroundMgr::BgQueueTypeId(bgTypeId)))
                    queued = true;
            }
        }

        ArenaRatingEntry const entry = sArenaRatingMgr.Get(player->GetObjectGuid(), type);

        uint32 const rank = rankOf[index];
        uint32 const total = totalOf[index];

        PSendSysMessage("ARENA|qb|%u|%s|%u|%u|%u|%u|%u|%u|%u|%u|%u", index,
                        ArenaMgr::BracketName(player, type),
                        bgTemplate ? 1 : 0,
                        entry.rating, entry.bestRating, entry.games, entry.wins,
                        waiting, queued ? 1 : 0, rank, total);
    }

    PSendSysMessage("ARENA|qi|%u|%u|%u|%u|%u|%u",
                    sWorld.getConfig(CONFIG_BOOL_ARENA_ENABLED) ? 1 : 0,
                    sWorld.getConfig(CONFIG_UINT32_ARENA_MIN_LEVEL),
                    sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_MIN_LEVEL),
                    sArenaRatingMgr.IsRatingEnabled() ? sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_MODE) : 0u,
                    player->GetLevel(),
                    player->InBattleGround() ? 1 : 0);

    PSendSysMessage("ARENA|done|info");
    return true;
}

/*
 * The bracket the player named, or ARENA_TYPES_COUNT when he named nothing usable.
 * Accepts the English tokens and the plain sizes, so "2v2", "2", "2c2" and "2vs2" all arrive.
 */
static uint8 ParseArenaBracketIndex(char const* text)
{
    if (!text || !*text)
        return ARENA_TYPES_COUNT;

    // the first digit decides: every bracket this server has is NvN with the same N twice
    while (*text && (*text < '0' || *text > '9'))
        ++text;

    switch (*text)
    {
        case '1': return 0;
        case '2': return 1;
        case '3': return 2;
        case '5': return 3;
    }
    return ARENA_TYPES_COUNT;
}

/*
 * ".arena join <1v1|2v2|3v3|5v5>" - what the arena window's Join button sends.
 *
 * Every gate the orb applies applies here too, because this IS the orb's code: the window is
 * another way in, not a second set of rules. A refusal arrives as the same notification the player
 * would have seen standing at the orb, in his own language, so the window has nothing to translate.
 */
bool ChatHandler::HandleArenaQueueJoinCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    uint8 const index = ParseArenaBracketIndex(args);
    if (index >= ARENA_TYPES_COUNT)
    {
        PSendSysMessage("ARENA|qerr|bracket");
        return true;
    }

    bool const joined = ArenaJoinQueueFromWindow(player, GetArenaTypeByIndex(index));
    PSendSysMessage("ARENA|q%s|%u", joined ? "joined" : "refused", index);
    return true;
}

// ".arena leave <bracket>" - the same button once he is in that queue
bool ChatHandler::HandleArenaQueueLeaveCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    uint8 const index = ParseArenaBracketIndex(args);
    if (index >= ARENA_TYPES_COUNT)
    {
        PSendSysMessage("ARENA|qerr|bracket");
        return true;
    }

    bool const left = ArenaLeaveQueueFromWindow(player, GetArenaTypeByIndex(index));
    PSendSysMessage("ARENA|q%s|%u", left ? "left" : "refused", index);
    return true;
}

/*
 * ".arena matches" - the running matches, for the arena window's spectate tab.
 *
 * Deliberately thinner than what the admin panel sends. The panel carries live damage and healing;
 * a player who is not watching yet has no business reading the scoreboard of a match in progress,
 * and handing it out would turn the window into a scouting tool. The names are in it because
 * anybody allowed to spectate can read them a second after arriving anyway.
 *
 * Spectators are not in bg->GetPlayers() - they stand on the map without being in the match - so
 * the name list below is the fighters and nothing else, with no filtering needed to keep it so.
 */
bool ChatHandler::HandleArenaMatchesCommand(char* /*args*/)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    // sent first and unconditionally, so the tab can say "switched off" rather than "none running"
    bool const mayWatch = sWorld.getConfig(CONFIG_BOOL_ARENA_SPECTATE);
    PSendSysMessage("ARENA|wcfg|%u", mayWatch ? 1 : 0);

    /*
     * And nothing else when watching is off.
     *
     * The list was going out regardless, so a player who could not spectate still learned every
     * running match and every name in it. That is precisely the scouting tool this command was
     * written to avoid being - it is why the live damage and healing the admin panel carries are
     * not in here - and leaving the roster in while refusing the door was inconsistent with it.
     */
    if (!mayWatch)
    {
        PSendSysMessage("ARENA|done|matches");
        return true;
    }

    for (uint32 bgTypeId = BATTLEGROUND_ARENA_FIRST; bgTypeId <= BATTLEGROUND_ARENA_LAST; ++bgTypeId)
    {
        for (BattleGroundSet::const_iterator itr = sBattleGroundMgr.GetBattleGroundsBegin(BattleGroundTypeId(bgTypeId));
             itr != sBattleGroundMgr.GetBattleGroundsEnd(BattleGroundTypeId(bgTypeId)); ++itr)
        {
            BattleGround* bg = itr->second;
            if (!itr->first || !bg->GetPlayersSize())
                continue;

            // only what can actually be watched: a match still at the gates has nothing to show and
            // SpectateArena would refuse it anyway
            if (bg->GetStatus() != STATUS_IN_PROGRESS)
                continue;

            Arena* arena = static_cast<Arena*>(bg);
            std::ostringstream names;
            for (auto const& member : bg->GetPlayers())
            {
                std::string name;
                if (sObjectMgr.GetPlayerNameByGUID(member.first, name))
                    names << (names.str().empty() ? "" : ", ") << name;
            }

            std::vector<Player*> spectators;
            CollectArenaSpectators(bg, spectators);

            PSendSysMessage("ARENA|w|%u|%s|%s|%u|%u|%s", bg->GetInstanceID(), bg->GetName(),
                            GetArenaTypeName(arena->GetArenaType()), arena->IsRated() ? 1 : 0,
                            uint32(spectators.size()), names.str().c_str());
        }
    }

    PSendSysMessage("ARENA|done|matches");
    return true;
}

// ".arena watch <instanceId>" - the spectate tab's button. The refusal, when there is one, arrives
// as the same notification the orb would have given, so the window has nothing to translate.
bool ChatHandler::HandleArenaWatchCommand(char* args)
{
    Player* player = m_session ? m_session->GetPlayer() : nullptr;
    if (!player)
        return false;

    uint32 instanceId = 0;
    if (!ExtractUInt32(&args, instanceId) || !instanceId)
    {
        PSendSysMessage("ARENA|werr|id");
        return true;
    }

    bool const watching = ArenaSpectateFromWindow(player, instanceId);
    PSendSysMessage("ARENA|w%s|%u", watching ? "ok" : "no", instanceId);
    return true;
}

bool ChatHandler::HandleArenaPanelCommand(char* args)
{
    char* what = ExtractLiteralArg(&args);
    if (!what)
        return false;

    std::string const query(what);

    /*
     * May this viewer change anything, or is he only looking?
     *
     * Reading the panel is a gamemaster right and every command behind its buttons is an
     * administrator one, so the two come apart and the addon has to be told which it is dealing with.
     *
     * Sent on EVERY request and not only with the settings, which is where it was and where it was
     * no use: the panel opens on its Matches tab, and a gamemaster who then goes to Bans or Ratings
     * never asked for the settings at all. His addon kept the optimistic default, let him press the
     * buttons, and the server refused them one layer down without a word - which is the exact
     * confusion this line exists to prevent.
     */
    PSendSysMessage("ARENA|rights|%u", GetAccessLevel() >= SEC_ADMINISTRATOR ? 1 : 0);

    if (query == "config")
    {
        for (auto const& entry : s_arenaBoolOptions)
            PSendSysMessage("ARENA|cfg|%s|%u|bool|", entry.name, sWorld.getConfig(entry.config) ? 1 : 0);

        for (auto const& entry : s_arenaUInt32Options)
        {
            // a number that means something gets its meaning sent along, the way the orb shows it
            std::string display;
            if (!strcmp(entry.name, "MaxItemPatch"))
                display = ArenaMgr::GetPatchName(uint8(sWorld.getConfig(entry.config)));
            else if (!strcmp(entry.name, "Rated.Mode"))
            {
                switch (sWorld.getConfig(entry.config))
                {
                    case ARENA_RATED_OFF:     display = "nothing is rated"; break;
                    case ARENA_RATED_PREMADE: display = "premade sides and every 1v1"; break;
                    case ARENA_RATED_ALL:     display = "every full match"; break;
                }
            }

            PSendSysMessage("ARENA|cfg|%s|%u|%u|%u|%s", entry.name, sWorld.getConfig(entry.config),
                            entry.min, entry.max, display.c_str());
        }

        // which arena the queue picks, and the six it can pick from
        ArenaMapType const forced = sBattleGroundMgr.GetForcedArenaMap();
        std::ostringstream maps;
        for (uint32 i = 0; i < ARENA_MAPS_COUNT; ++i)
            maps << "|" << GetArenaMapName(ArenaMapType(i));
        PSendSysMessage("ARENA|map|%u%s", uint32(forced), maps.str().c_str());

        PSendSysMessage("ARENA|done|config");
        return true;
    }

    if (query == "matches")
    {
        for (uint32 bgTypeId = BATTLEGROUND_ARENA_FIRST; bgTypeId <= BATTLEGROUND_ARENA_LAST; ++bgTypeId)
        {
            for (BattleGroundSet::const_iterator itr = sBattleGroundMgr.GetBattleGroundsBegin(BattleGroundTypeId(bgTypeId));
                 itr != sBattleGroundMgr.GetBattleGroundsEnd(BattleGroundTypeId(bgTypeId)); ++itr)
            {
                BattleGround* bg = itr->second;
                if (!itr->first || !bg->GetPlayersSize())
                    continue;

                Arena* arena = static_cast<Arena*>(bg);
                std::ostringstream names;
                for (auto const& player : bg->GetPlayers())
                {
                    std::string name;
                    if (sObjectMgr.GetPlayerNameByGUID(player.first, name))
                        names << (names.str().empty() ? "" : ", ") << name;
                }

                /*
                 * Damage and healing while the match is still running.
                 *
                 * They existed only on the end scoreboard until now, and that is precisely why the
                 * healing column could be blind to every heal over time for as long as it was: there
                 * was no way to watch the numbers move. A separate field rather than part of the name
                 * list, because the panel's "go to player" button reads the first name out of that one.
                 */
                std::ostringstream scores;
                for (auto itr = bg->GetPlayerScoresBegin(); itr != bg->GetPlayerScoresEnd(); ++itr)
                {
                    std::string name;
                    if (!sObjectMgr.GetPlayerNameByGUID(itr->first, name))
                        continue;

                    auto const* score = static_cast<ArenaScore const*>(itr->second);
                    scores << (scores.str().empty() ? "" : ", ") << name << " "
                           << ShortNumber(score->damageDone) << "/" << ShortNumber(score->healingDone);
                }

                std::vector<Player*> spectators;
                CollectArenaSpectators(bg, spectators);

                PSendSysMessage("ARENA|match|%u|%s|%s|%u|%u|%u|%s|%s", bg->GetInstanceID(), bg->GetName(),
                                GetArenaTypeName(arena->GetArenaType()), uint32(bg->GetStatus()),
                                arena->IsRated() ? 1 : 0, uint32(spectators.size()), names.str().c_str(),
                                scores.str().c_str());
            }
        }
        PSendSysMessage("ARENA|done|matches");
        return true;
    }

    if (query == "spells")
    {
        uint32 offset = 0;
        ExtractOptUInt32(&args, offset, 0);

        // the map has no order of its own; sorting keeps the pages stable between requests
        std::vector<uint32> ids;
        ids.reserve(sArenaMgr.GetDisabledSpells().size());
        for (auto const& itr : sArenaMgr.GetDisabledSpells())
            ids.push_back(itr.first);
        std::sort(ids.begin(), ids.end());

        uint32 sent = 0;
        for (uint32 i = offset; i < ids.size() && sent < ARENA_PANEL_PAGE; ++i, ++sent)
        {
            auto const& data = sArenaMgr.GetDisabledSpells().find(ids[i])->second;
            SpellEntry const* spell = sSpellMgr.GetSpellEntry(ids[i]);

            // most of the list is items - the row names the item it came from, if it is one
            uint32 const itemId = sArenaMgr.GetItemForSpell(ids[i]);
            ItemPrototype const* item = itemId ? sObjectMgr.GetItemPrototype(itemId) : nullptr;

            PSendSysMessage("ARENA|spell|%u|%u%u%u%u|%u|%s|%s", ids[i],
                            uint32(data.disabledForType[0]), uint32(data.disabledForType[1]),
                            uint32(data.disabledForType[2]), uint32(data.disabledForType[3]),
                            itemId, spell ? spell->SpellName[0].c_str() : "?", item ? item->Name1 : "");
        }

        uint32 const next = offset + sent;
        PSendSysMessage("ARENA|done|spells|%u|%u", next < ids.size() ? next : 0, uint32(ids.size()));
        return true;
    }

    if (query == "rating")
    {
        Player* target;
        ObjectGuid targetGuid;
        std::string targetName;
        if (!ExtractPlayerTarget(&args, &target, &targetGuid, &targetName))
            return false;

        ObjectGuid const guid = target ? target->GetObjectGuid() : targetGuid;
        for (uint8 index = 0; index < ARENA_TYPES_COUNT; ++index)
        {
            ArenaType const type = GetArenaTypeByIndex(index);
            ArenaRatingEntry const entry = sArenaRatingMgr.Get(guid, type);
            PSendSysMessage("ARENA|rating|%s|%s|%u|%u|%u|%u|%u|%u", targetName.c_str(), GetArenaTypeName(type),
                            entry.rating, entry.mmr, entry.games, entry.wins, entry.bestRating,
                            sArenaRatingMgr.HasPlayed(guid, type) ? 1 : 0);
        }
        PSendSysMessage("ARENA|done|rating");
        return true;
    }

    PSendSysMessage("ARENA|done|unknown");
    return true;
}

bool ChatHandler::HandleNearGraveCommand(char* args)
{
    Team g_team;

    size_t argslen = strlen(args);

    if (!*args)
        g_team = TEAM_NONE;
    else if (strncmp(args, "horde", argslen) == 0)
        g_team = HORDE;
    else if (strncmp(args, "alliance", argslen) == 0)
        g_team = ALLIANCE;
    else
        return false;

    Player* player = m_session->GetPlayer();
    uint32 zone_id = player->GetZoneId();

    WorldSafeLocsEntry const* graveyard = sObjectMgr.GetClosestGraveYard(
            player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetMapId(), g_team);

    if (graveyard)
    {
        uint32 g_id = graveyard->ID;

        GraveYardData const* data = sObjectMgr.FindGraveYardData(g_id, zone_id);
        if (!data)
        {
            PSendSysMessage(LANG_COMMAND_GRAVEYARDERROR, g_id);
            SetSentErrorMessage(true);
            return false;
        }

        g_team = data->team;

        std::string team_name = GetMangosString(LANG_COMMAND_GRAVEYARD_NOTEAM);

        if (g_team == 0)
            team_name = GetMangosString(LANG_COMMAND_GRAVEYARD_ANY);
        else if (g_team == HORDE)
            team_name = GetMangosString(LANG_COMMAND_GRAVEYARD_HORDE);
        else if (g_team == ALLIANCE)
            team_name = GetMangosString(LANG_COMMAND_GRAVEYARD_ALLIANCE);

        PSendSysMessage(LANG_COMMAND_GRAVEYARDNEAREST, g_id, team_name.c_str(), zone_id);
    }
    else
    {
        std::string team_name;

        if (g_team == 0)
            team_name = GetMangosString(LANG_COMMAND_GRAVEYARD_ANY);
        else if (g_team == HORDE)
            team_name = GetMangosString(LANG_COMMAND_GRAVEYARD_HORDE);
        else if (g_team == ALLIANCE)
            team_name = GetMangosString(LANG_COMMAND_GRAVEYARD_ALLIANCE);

        if (g_team == ~uint32(0))
            PSendSysMessage(LANG_COMMAND_ZONENOGRAVEYARDS, zone_id);
        else
            PSendSysMessage(LANG_COMMAND_ZONENOGRAFACTION, zone_id, team_name.c_str());
    }

    return true;
}
