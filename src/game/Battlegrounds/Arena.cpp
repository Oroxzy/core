/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
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
 * Custom arena implementation for the 1.12 client, see Arena.h.
 */

#include "Arena.h"
#include "BattleGroundMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Creature.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "World.h"
#include "Map.h"
#include "GridMap.h"
#include "Database/DatabaseEnv.h"
#include "ProgressBar.h"
#include "Utilities/Random.h"
#include "Policies/SingletonImp.h"

#include <sstream>
#include <algorithm>
#include <set>

INSTANTIATE_SINGLETON_1(ArenaMgr);

/*********************************************************/
/***                     ARENA MGR                     ***/
/*********************************************************/

void ArenaMgr::LoadFromDB()
{
    m_disabledSpells.clear();
    m_itemMinPatch.clear();

    if (!sWorld.getConfig(CONFIG_BOOL_ARENA_ENABLED))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Arenas are disabled (Arena.Enable = 0), skipping arena data.");
        return;
    }

    {
        //                                                               0        1      2      3      4
        std::unique_ptr<QueryResult> result(WorldDatabase.Query("SELECT `entry`, `1v1`, `2v2`, `3v3`, `5v5` FROM `disabled_arena_spells`"));
        uint32 count = 0;
        if (result)
        {
            BarGoLink bar(result->GetRowCount());
            do
            {
                bar.step();
                Field* fields = result->Fetch();

                uint32 spellId = fields[0].GetUInt32();
                if (!sSpellMgr.GetSpellEntry(spellId))
                {
                    sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Table `disabled_arena_spells` has nonexistent spell %u, skipped.", spellId);
                    continue;
                }

                DisabledSpell& data = m_disabledSpells[spellId];
                for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
                    data.disabledForType[i] = fields[1 + i].GetUInt8() != 0;
                ++count;
            }
            while (result->NextRow());
        }
        else
        {
            BarGoLink bar(1);
            bar.step();
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u disabled arena spells", count);
    }

    // The patch in which an item was introduced. item_template holds one row per patch in which the
    // item changed, so the lowest patch is the one we want. Items available since 1.2 (patch 0) are not stored.
    {
        std::unique_ptr<QueryResult> result(WorldDatabase.Query("SELECT `entry`, MIN(`patch`) FROM `item_template` GROUP BY `entry`"));
        uint32 count = 0;
        if (result)
        {
            BarGoLink bar(result->GetRowCount());
            do
            {
                bar.step();
                Field* fields = result->Fetch();
                if (uint8 patch = fields[1].GetUInt8())
                    m_itemMinPatch[fields[0].GetUInt32()] = patch;
                ++count;
            }
            while (result->NextRow());
        }
        else
        {
            BarGoLink bar(1);
            bar.step();
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded arena item patch data for %u items (" SIZEFMTD " added after patch 1.2)", count, m_itemMinPatch.size());
    }
}

bool ArenaMgr::IsSpellDisabled(uint32 spellId, ArenaType type) const
{
    if (type == ARENA_TYPE_NONE)
        return false;

    auto itr = m_disabledSpells.find(spellId);
    if (itr == m_disabledSpells.end())
        return false;

    return itr->second.disabledForType[GetArenaTypeIndex(type)];
}

uint8 ArenaMgr::GetItemMinPatch(uint32 itemId) const
{
    auto itr = m_itemMinPatch.find(itemId);
    return itr != m_itemMinPatch.end() ? itr->second : 0;
}

char const* ArenaMgr::GetPatchName(uint8 patch)
{
    switch (patch)
    {
        case 0:  return "Patch 1.2: Mysteries of Maraudon";
        case 1:  return "Patch 1.3: Ruins of the Dire Maul";
        case 2:  return "Patch 1.4: The Call to War";
        case 3:  return "Patch 1.5: Battlegrounds";
        case 4:  return "Patch 1.6: Assault on Blackwing Lair";
        case 5:  return "Patch 1.7: Rise of the Blood God";
        case 6:  return "Patch 1.8: Dragons of Nightmare";
        case 7:  return "Patch 1.9: The Gates of Ahn'Qiraj";
        case 8:  return "Patch 1.10: Storms of Azeroth";
        case 9:  return "Patch 1.11: Shadow of the Necropolis";
        case 10: return "Patch 1.12: Drums of War";
    }
    return "Unknown patch";
}

bool ArenaMgr::IsItemForbidden(ItemPrototype const* proto, ArenaType type, std::string* reason) const
{
    if (!proto)
        return false;

    uint32 const maxItemLevel = sWorld.getConfig(CONFIG_UINT32_ARENA_MAX_ITEM_LEVEL);
    uint32 const maxPatch = sWorld.getConfig(CONFIG_UINT32_ARENA_MAX_ITEM_PATCH);

    if (uint8 patch = GetItemMinPatch(proto->ItemId))
    {
        if (patch > maxPatch)
        {
            if (reason)
            {
                std::ostringstream ss;
                ss << "is from |cffff726f" << GetPatchName(patch) << "|r, allowed are items from |cff71d5ff" << GetPatchName(uint8(maxPatch)) << "|r and below.";
                *reason = ss.str();
            }
            return true;
        }
    }

    if (proto->ItemLevel > maxItemLevel)
    {
        if (reason)
        {
            std::ostringstream ss;
            ss << "has item level |cffff726f" << proto->ItemLevel << "|r, allowed is |cff71d5ff" << maxItemLevel << "|r.";
            *reason = ss.str();
        }
        return true;
    }

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        uint32 spellId = proto->Spells[i].SpellId;
        if (spellId && IsSpellDisabled(spellId, type))
        {
            if (reason)
            {
                std::ostringstream ss;
                ss << "is not allowed in " << GetArenaTypeName(type) << " arenas.";
                *reason = ss.str();
            }
            return true;
        }
    }

    return false;
}

/*********************************************************/
/***                       ARENA                       ***/
/*********************************************************/

Arena::Arena()
{
    m_startDelayTimes[BG_STARTING_EVENT_FIRST]  = BG_START_DELAY_1M;
    m_startDelayTimes[BG_STARTING_EVENT_SECOND] = BG_START_DELAY_30S;
    m_startDelayTimes[BG_STARTING_EVENT_THIRD]  = BG_START_DELAY_15S;
    m_startDelayTimes[BG_STARTING_EVENT_FOURTH] = BG_START_DELAY_NONE;

    m_startMessageIds[BG_STARTING_EVENT_FIRST]  = BCT_ARENA_START_ONE_MINUTE;
    m_startMessageIds[BG_STARTING_EVENT_SECOND] = BCT_ARENA_START_HALF_MINUTE;
    m_startMessageIds[BG_STARTING_EVENT_THIRD]  = BCT_ARENA_START_FIFTEEN_SECONDS;
    m_startMessageIds[BG_STARTING_EVENT_FOURTH] = BCT_ARENA_HAS_BEGUN;

    m_worldStateTimer = 0;
    m_matchTimer = 0;
    m_doorsDespawnTimer = 0;
    m_lastCountdownSecond = 0;
    m_playersReady = false;
    m_timeLimitReached = false;
    m_preparationExtended = false;
    m_tornadoTimer[0] = 0;
    m_tornadoTimer[1] = 0;
    m_waterfallTimer = 0;
    m_waterfallKnockbackTimer = 0;
    m_pipeKnockbackTimer = 0;
    m_pipeRecheckTimer = 0;
    m_pipeKnockbackCount = 0;
    m_waterfallState = WATERFALL_OFF;
    m_underMapCheckTimer = 0;
    m_leaverIsParticipant = false;
    m_spectatorsRemoved = false;
}

Arena::~Arena()
{
}

void Arena::Reset()
{
    BattleGround::Reset();

    // ready check npcs are spawned during the preparation
    m_activeEvents[ARENA_EVENT_WATCHER_1] = 0;
    m_activeEvents[ARENA_EVENT_WATCHER_2] = 0;

    if (IsDalaranArena())
    {
        // water spouts / knockback reference points and the water doodads are always spawned,
        // the water itself is toggled with DoorOpen / DoorClose (see SetWaterActive)
        for (uint8 event1 = ARENA_EVENT_DS_WATERSPOUT_1; event1 <= ARENA_EVENT_DS_PIPE_KICKER_2; ++event1)
            m_activeEvents[event1] = 0;
        for (uint8 event1 = ARENA_EVENT_DS_DOODAD_SEWER01; event1 <= ARENA_EVENT_DS_DOODAD_WATERFALL_COLL; ++event1)
            m_activeEvents[event1] = 0;
    }

    // ARENA_EVENT_SHADOW_SIGHT is spawned ARENA_SHADOW_SIGHT_SPAWN_DELAY seconds after the gates opened.
    // It must be marked as inactive explicitly: SpawnEvent() reads m_activeEvents through operator[],
    // and a default-constructed entry (0) would look like "already spawned" for event2 = 0.
    m_activeEvents[ARENA_EVENT_SHADOW_SIGHT] = BG_EVENT_NONE;

    m_worldStateTimer = 0;
    m_matchTimer = 0;
    m_doorsDespawnTimer = 0;
    m_lastCountdownSecond = 0;
    m_playersReady = false;
    m_timeLimitReached = false;
    m_preparationExtended = false;
    m_tornadoTimer[0] = 0;
    m_tornadoTimer[1] = 0;
    m_waterfallTimer = 0;
    m_waterfallKnockbackTimer = 0;
    m_pipeKnockbackTimer = 0;
    m_pipeRecheckTimer = 0;
    m_pipeKnockbackCount = 0;
    m_waterfallState = WATERFALL_OFF;
    m_underMapCheckTimer = 0;
    m_leaverIsParticipant = false;
    m_spectatorsRemoved = false;
}

/*********************************************************/
/***                      UPDATE                       ***/
/*********************************************************/

void Arena::Update(uint32 diff)
{
    // visitors are removed shortly before the participants - or right away when the last participant is
    // gone (the base class deletes an empty arena on the spot and the map would unload under their feet
    // without the status slot ever being cleared)
    bool const aboutToBeDeleted = !GetPlayersSize() && !GetInvitedCount(HORDE) && !GetInvitedCount(ALLIANCE);
    if (!m_spectatorsRemoved && ((GetStatus() == STATUS_WAIT_LEAVE && GetEndTime() <= diff) || aboutToBeDeleted))
    {
        m_spectatorsRemoved = true;
        RemoveSpectators();
    }

    if (GetStatus() == STATUS_WAIT_JOIN)
        UpdatePreparation(diff);

    if (GetStatus() == STATUS_IN_PROGRESS)
    {
        m_matchTimer += diff;

        // doors are removed a few seconds after they opened
        if (m_doorsDespawnTimer)
        {
            if (m_doorsDespawnTimer <= diff)
            {
                m_doorsDespawnTimer = 0;
                StartingEventDespawnDoors();
            }
            else
                m_doorsDespawnTimer -= diff;
        }

        if (IsNagrandArena())
            UpdateNagrand(diff);
        else if (IsDalaranArena())
            UpdateDalaran(diff);

        // rescue players that fell below the arena floor (bad vmaps / knockback accidents)
        if (m_underMapCheckTimer <= diff)
        {
            m_underMapCheckTimer = ARENA_UNDER_MAP_CHECK_INTERVAL;
            CheckPlayersUnderMap();
        }
        else
            m_underMapCheckTimer -= diff;

        if (!m_timeLimitReached && m_matchTimer >= sWorld.getConfig(CONFIG_UINT32_ARENA_TIME_LIMIT_MINUTES) * MINUTE * IN_MILLISECONDS)
        {
            m_timeLimitReached = true;
            PSendMessageToAll(LANG_ARENA_TIME_LIMIT_REACHED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr);
            CheckWinConditions();
        }
    }

    // world states (alive players, remaining time) once per second during preparation and fight
    if (GetStatus() == STATUS_WAIT_JOIN || GetStatus() == STATUS_IN_PROGRESS)
    {
        if (m_worldStateTimer <= diff)
        {
            m_worldStateTimer = ARENA_WORLD_STATE_UPDATE_INTERVAL;
            UpdateWorldStates();
            if (GetStatus() == STATUS_IN_PROGRESS)
                CheckWinConditions();
        }
        else
            m_worldStateTimer -= diff;
    }

    // must be the last call, the base class may delete the battleground
    BattleGround::Update(diff);
}

void Arena::UpdatePreparation(uint32 diff)
{
    // nothing to do before the first player entered (base class starts the timers then)
    if (!(m_events & BG_STARTING_EVENT_1))
        return;

    // The watchers are spawned with the map rather than by a call of ours, so this waits for them to
    // appear. It runs every tick on purpose: two creatures and an aura check each is nothing, and it
    // puts the colours back should they ever fall off.
    ApplyWatcherTeamColours();

    // ready check: as soon as everybody told the arena watcher that he is ready, the gates open early
    if (!m_playersReady && AreAllPlayersReady())
    {
        m_playersReady = true;
        int32 const readyDelay = int32(sWorld.getConfig(CONFIG_UINT32_ARENA_READY_START_DELAY_SECONDS) * IN_MILLISECONDS);
        if (GetStartDelayTime() > readyDelay)
        {
            SetStartDelayTime(readyDelay);
            m_events |= (BG_STARTING_EVENT_2 | BG_STARTING_EVENT_3);    // skip the 30 / 15 seconds warnings
        }
    }

    // countdown during the last 10 seconds
    int32 const delay = GetStartDelayTime();
    if (delay > 0 && delay <= 10 * int32(IN_MILLISECONDS))
    {
        uint32 const second = uint32((delay + IN_MILLISECONDS - 1) / IN_MILLISECONDS);
        if (second != m_lastCountdownSecond)
        {
            m_lastCountdownSecond = second;
            PlaySoundToAll(SOUND_ARENA_COUNTDOWN_TICK);
            PSendMessageToAll(LANG_ARENA_START_COUNTDOWN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, second);
        }
    }
}

void Arena::UpdateNagrand(uint32 diff)
{
    for (uint8 i = 0; i < 2; ++i)
    {
        if (m_tornadoTimer[i] <= diff)
            m_tornadoTimer[i] = SummonTornado() ? urand(ARENA_NA_TORNADO_INTERVAL_MIN, ARENA_NA_TORNADO_INTERVAL_MAX) : uint32(ARENA_NA_TORNADO_RETRY_DELAY);
        else
            m_tornadoTimer[i] -= diff;
    }
}

void Arena::UpdateDalaran(uint32 diff)
{
    // waterfall cycle: warning (sound + water visual) -> collision + knockbacks -> off -> pause
    if (m_waterfallTimer <= diff)
    {
        switch (m_waterfallState)
        {
            case WATERFALL_OFF:
                PlaySoundToAll(SOUND_ARENA_DS_WATER_INCOMING);
                SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL, true);
                SetWaterActive(ARENA_EVENT_DS_DOODAD_SEWER01, true);
                m_waterfallState = WATERFALL_WARNING;
                m_waterfallTimer = ARENA_DS_WATERFALL_WARNING_DURATION;
                break;
            case WATERFALL_WARNING:
                SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL_COLL, true);
                m_waterfallState = WATERFALL_ON;
                m_waterfallTimer = ARENA_DS_WATERFALL_DURATION;
                m_waterfallKnockbackTimer = 0;
                break;
            case WATERFALL_ON:
                SetWaterActive(ARENA_EVENT_DS_DOODAD_SEWER01, false);
                SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL, false);
                SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL_COLL, false);
                m_waterfallState = WATERFALL_OFF;
                m_waterfallTimer = urand(ARENA_DS_WATERFALL_INTERVAL_MIN, ARENA_DS_WATERFALL_INTERVAL_MAX);
                break;
        }
    }
    else
        m_waterfallTimer -= diff;

    if (m_waterfallState == WATERFALL_ON)
    {
        if (m_waterfallKnockbackTimer <= diff)
        {
            DoWaterfallKick();
            m_waterfallKnockbackTimer = ARENA_DS_WATERFALL_KNOCKBACK_INTERVAL;
        }
        else
            m_waterfallKnockbackTimer -= diff;
    }

    // players are flushed out of the starting pipes after the gates opened
    if (m_pipeKnockbackCount < ARENA_DS_PIPE_KNOCKBACK_COUNT)
    {
        if (m_pipeKnockbackTimer <= diff)
        {
            KickFromPipes();
            DoWaterFlush();
            ++m_pipeKnockbackCount;
            m_pipeKnockbackTimer = ARENA_DS_PIPE_KNOCKBACK_INTERVAL;
        }
        else
            m_pipeKnockbackTimer -= diff;
    }
    // afterwards: anybody who climbed back into a pipe is flushed out again
    // (server side replacement for the WotLK area triggers 5347 / 5348 which have no 1.12 data)
    else if (m_pipeRecheckTimer <= diff)
    {
        m_pipeRecheckTimer = ARENA_DS_PIPE_RECHECK_INTERVAL;

        bool playerInPipe = false;
        for (uint8 spoutEvent = ARENA_EVENT_DS_WATERSPOUT_1; spoutEvent <= ARENA_EVENT_DS_WATERSPOUT_2 && !playerInPipe; ++spoutEvent)
        {
            Creature* spout = GetBgMap()->GetCreature(GetSingleCreatureGuid(spoutEvent, 0));
            if (!spout)
                continue;

            for (const auto& itr : m_players)
            {
                Player* player = sObjectMgr.GetPlayer(itr.first);
                if (!player || !player->IsAlive() || player->IsArenaSpectator() || !player->IsInMap(spout))
                    continue;

                // the pipes are the only place that high above the arena floor (floor z ~3-7, pipes z ~14)
                if (player->GetPositionZ() > 12.0f && spout->GetDistance(player) < 25.0f)
                {
                    playerInPipe = true;
                    break;
                }
            }
        }

        if (playerInPipe)
        {
            KickFromPipes();
            DoWaterFlush();
        }
    }
    else
        m_pipeRecheckTimer -= diff;
}

void Arena::CheckPlayersUnderMap()
{
    // Fallback floor heights per arena: slightly below the lowest legal fight spot.
    // Players that fall through the WMO floor come to rest on the ADT terrain below it
    // (Dalaran has a flat terrain plane at z = 0 under the whole arena), so the threshold
    // must sit ABOVE that resting height or they would never be rescued.
    float minZ;
    switch (GetArenaMapType())
    {
        case ARENA_MAP_NAGRAND:     minZ = 10.0f;  break;   // floor / start rooms ~12.1
        case ARENA_MAP_BLADES_EDGE: minZ = -5.0f;  break;   // lower fight floor 1.0-4.5 (navmesh), terrain 0.5 directly under it:
                                                            // a z threshold cannot separate fallen players from fighters, only a real void fall is caught
        case ARENA_MAP_LORDAERON:   minZ = 30.0f;  break;   // floor ~32.5
        case ARENA_MAP_DALARAN:     minZ = 1.0f;   break;   // floor ~3.2, water channel ~2.8, terrain plane at 0
        case ARENA_MAP_TIGERS_PEAK: minZ = 370.0f; break;   // plateau terrain ~380.7, platforms ~381.5
        case ARENA_MAP_TOLVIRON:    minZ = 14.0f;  break;   // fight floor ~24.4 (Blizzard's own teleport targets sit at 24.41)
        default:                    return;
    }

    for (const auto& itr : m_players)
    {
        Player* player = sObjectMgr.GetPlayer(itr.first);
        if (!player || !player->IsAlive() || player->IsArenaSpectator() || player->GetMap() != GetBgMap())
            continue;

        if (player->GetPositionZ() < minZ)
        {
            float x, y, z, o;
            GetTeamStartLoc(player->GetBGTeam(), x, y, z, o);
            player->NearTeleportTo(x, y, z, o);
        }
    }
}

/*********************************************************/
/***                  STARTING EVENTS                  ***/
/*********************************************************/

void Arena::StartingEventCloseDoors()
{
    // Dalaran: the water is running during the preparation, purely visual
    if (IsDalaranArena())
    {
        SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL, true);
        SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL_COLL, true);
        SetWaterActive(ARENA_EVENT_DS_DOODAD_SEWER01, true);
    }

    ApplyArenaWeather();

    UpdateWorldStates();
}

void Arena::StartMatchNow()
{
    if (GetStatus() != STATUS_WAIT_JOIN)
        return;

    // the last second is left standing so the normal countdown path runs once and opens the doors,
    // plays the sound and spawns what it always spawns - nothing here duplicates that work
    m_playersReady = true;
    m_events |= (BG_STARTING_EVENT_1 | BG_STARTING_EVENT_2 | BG_STARTING_EVENT_3);
    if (GetStartDelayTime() > int32(IN_MILLISECONDS))
        SetStartDelayTime(int32(IN_MILLISECONDS));
}

uint32 Arena::GetArenaZoneId() const
{
    switch (GetArenaMapType())
    {
        case ARENA_MAP_NAGRAND:     return ARENA_NA_ZONE_ID;
        case ARENA_MAP_BLADES_EDGE: return ARENA_BE_ZONE_ID;
        case ARENA_MAP_LORDAERON:   return ARENA_RL_ZONE_ID;
        case ARENA_MAP_DALARAN:     return ARENA_DS_ZONE_ID;
        case ARENA_MAP_TIGERS_PEAK: return ARENA_TP_ZONE_ID;
        case ARENA_MAP_TOLVIRON:    return ARENA_TV_ZONE_ID;
    }
    return 0;
}

uint8 Arena::GetSuitableWeather(WeatherType* out, uint8 max) const
{
    // WEATHER_TYPE_STORM is the sandstorm, so it belongs to Uldum and nowhere else, and snow belongs on
    // the mountain rather than in a desert or a sewer. Fine weather is always an option.
    static WeatherType const desert[]    = { WEATHER_TYPE_FINE, WEATHER_TYPE_STORM };
    static WeatherType const mountain[]  = { WEATHER_TYPE_FINE, WEATHER_TYPE_SNOW };
    static WeatherType const temperate[] = { WEATHER_TYPE_FINE, WEATHER_TYPE_RAIN };
    static WeatherType const indoors[]   = { WEATHER_TYPE_FINE };

    WeatherType const* kinds;
    uint8 count;
    switch (GetArenaMapType())
    {
        case ARENA_MAP_TOLVIRON:    kinds = desert;    count = 2; break;   // Uldum
        case ARENA_MAP_TIGERS_PEAK: kinds = mountain;  count = 2; break;   // snowy peak
        case ARENA_MAP_DALARAN:     kinds = indoors;   count = 1; break;   // underground, weather is not visible
        default:                    kinds = temperate; count = 2; break;   // Nagrand, Blade's Edge, Lordaeron
    }

    if (count > max)
        count = max;
    for (uint8 i = 0; i < count; ++i)
        out[i] = kinds[i];
    return count;
}

void Arena::SetArenaWeather(WeatherType type, float grade)
{
    // permanently, so the world's own weather regeneration cannot clear it mid match
    if (uint32 const zoneId = GetArenaZoneId())
        GetBgMap()->SetWeather(zoneId, type, grade, true);
}

void Arena::ApplyArenaWeather()
{
    if (!GetArenaZoneId())
        return;

    // Tiger's Peak sits on a snowy mountain and keeps its snow whether the option is on or not.
    if (!sWorld.getConfig(CONFIG_BOOL_ARENA_RANDOM_WEATHER))
    {
        if (IsTigersPeakArena())
            SetArenaWeather(WEATHER_TYPE_SNOW, 0.5f);
        return;
    }

    WeatherType kinds[4];
    uint8 const count = GetSuitableWeather(kinds, 4);
    WeatherType const type = kinds[urand(0, count - 1)];
    // fine weather carries no grade; the others get a light to heavy roll
    SetArenaWeather(type, (type == WEATHER_TYPE_FINE) ? 0.0f : frand(0.3f, 0.9f));
}

void Arena::StartingEventOpenDoors()
{
    OpenDoorEvent(BG_EVENT_DOOR);
    m_doorsDespawnTimer = ARENA_DOORS_DESPAWN_DELAY;

    PlaySoundToAll(SOUND_ARENA_LET_THE_GAMES_BEGIN);

    // The four TBC arenas get their gate sound from the door model's own $GO trigger. The Dalaran
    // sewer door and the Tol'Viron gate have none, so theirs is played from the doors themselves -
    // they are visible, so the sound stays positional like the others.
    uint32 const doorSound = IsDalaranArena()  ? SOUND_ARENA_DS_DOOR_OPEN
                           : IsTolvironArena() ? SOUND_ARENA_TV_DOOR_OPEN : 0;
    if (doorSound)
        for (auto const& guid : m_eventObjects[MAKE_PAIR32(BG_EVENT_DOOR, 0)].gameobjects)
            if (GameObject* door = GetBgMap()->GetGameObject(guid))
                door->PlayDistanceSound(doorSound);

    // the ready check npcs leave, the shadow sight orbs come later
    SpawnEvent(ARENA_EVENT_WATCHER_1, 0, false, true);
    SpawnEvent(ARENA_EVENT_WATCHER_2, 0, false, true);
    SpawnEvent(ARENA_EVENT_SHADOW_SIGHT, 0, true, false, ARENA_SHADOW_SIGHT_SPAWN_DELAY);

    // a team did not show up: no fight (except in .debug bg mode) - and no repair / cooldown reset either
    if ((!GetPlayersCountByTeam(ALLIANCE) || !GetPlayersCountByTeam(HORDE)) && !sBattleGroundMgr.isTesting())
    {
        EndBattleGround(TEAM_NONE);
        return;
    }

    for (const auto& itr : m_players)
        if (Player* player = sObjectMgr.GetPlayer(itr.first))
            ResetPlayerForFight(player);

    if (IsDalaranArena())
    {
        SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL, false);
        SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL_COLL, false);
        SetWaterActive(ARENA_EVENT_DS_DOODAD_SEWER01, false);
        m_waterfallState = WATERFALL_OFF;
        m_waterfallTimer = ARENA_DS_FIRST_WATERFALL_DELAY;
        m_waterfallKnockbackTimer = 0;
        m_pipeKnockbackCount = 0;
        m_pipeKnockbackTimer = ARENA_DS_PIPE_KNOCKBACK_FIRST_DELAY;
    }
    else if (IsNagrandArena())
    {
        m_tornadoTimer[0] = ARENA_NA_FIRST_TORNADO_DELAY;
        m_tornadoTimer[1] = urand(ARENA_NA_TORNADO_INTERVAL_MIN, ARENA_NA_TORNADO_INTERVAL_MAX);
    }

    m_matchTimer = 0;
    m_timeLimitReached = false;
}

/*********************************************************/
/***                      PLAYERS                      ***/
/*********************************************************/

void Arena::AddPlayer(Player* player)
{
    BattleGround::AddPlayer(player);

    m_playerScores[player->GetObjectGuid()] = new ArenaScore;

    // (the other arena queues were already left in HandleBattleFieldPortOpcode - world thread; the map
    // thread we run in here must not touch the queue containers of other maps)

    PrepareArenaPlayer(player);

    // late arrival (invited during preparation, ported after the gates opened): no preparation aura,
    // he joins the fight combat-ready instead - the aura would otherwise stay on for the whole match
    if (GetStatus() == STATUS_IN_PROGRESS)
        ResetPlayerForFight(player);
    else
        player->AddAura(SPELL_ARENA_PREPARATION);

    // The colours belong to the player from the moment he stands in his box, not from the moment he
    // reports ready - waiting for the ready check left both teams standing there unmarked. Visitors
    // watching through the orb belong to no team and stay unmarked.
    if (!player->IsArenaSpectator())
        ApplyTeamAura(player);

    PSendMessageToAll(LANG_ARENA_PLAYER_JOINED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, player->GetName(), player->GetBGTeam() == HORDE ? "Green Team" : "Gold Team");

    UpdateWorldStates();
}

void Arena::RemovePlayerAtLeave(ObjectGuid guid, bool transport, bool sendPacket)
{
    // the base class erases the player from m_players before it calls RemovePlayer - remember whether
    // this was a fighter or only a visitor of the match
    m_leaverIsParticipant = m_players.find(guid) != m_players.end();
    BattleGround::RemovePlayerAtLeave(guid, transport, sendPacket);
    m_leaverIsParticipant = false;
}

void Arena::RemovePlayer(Player* player, ObjectGuid /*guid*/)
{
    // spectators (visitors and dead participants) leave silently, fighters are announced
    bool const announce = GetStatus() < STATUS_WAIT_LEAVE && (!player || !player->IsArenaSpectator());
    if (announce)
    {
        PlaySoundToAll(SOUND_ARENA_PLAYER_LEFT);
        if (player)
            PSendMessageToAll(LANG_ARENA_PLAYER_LEFT, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, player->GetName());
    }

    if (player)
        RestorePlayer(player, m_leaverIsParticipant);

    // Somebody walked out before the gates opened, so the match is short of players. The queue can
    // send a replacement - BattleGround::RemovePlayerAtLeave puts the instance back among the invite
    // candidates right after this - but only while more of the countdown is left than an invite needs
    // to be accepted (BattleGroundQueue::FillPlayersToBg). With a one minute preparation that window
    // shuts after thirty seconds, and anybody leaving later left the others watching the clock run
    // down to a no-show draw. So the countdown is held open long enough for one replacement to arrive.
    //
    // Once per match. Otherwise two friends could keep a lobby open indefinitely by taking turns
    // leaving and rejoining.
    if (GetStatus() == STATUS_WAIT_JOIN && GetPlayersSize() < GetMaxPlayers() && !m_preparationExtended)
    {
        int32 const needed = int32(BattleGroundMgr::GetInviteAcceptWaitTime(GetTypeID())) + ARENA_REFILL_LOAD_TIME;
        if (GetStartDelayTime() < needed)
        {
            SetStartDelayTime(needed);
            m_lastCountdownSecond = 0;                      // let the last ten seconds be announced again
            m_preparationExtended = true;
        }
    }

    // the leaving player is already removed from the player list here
    if (GetStatus() == STATUS_IN_PROGRESS)
        CheckWinConditions();
    UpdateWorldStates();
}

void Arena::PrepareArenaPlayer(Player* player)
{
    ArenaType const type = GetArenaType();

    // no gm / cheat states inside the arena
    if (player->IsGameMaster())
        player->SetGameMaster(false);
    if (!player->IsGMVisible())
        player->SetGMVisible(true);
    player->SetCheatGod(false);

    player->UnequipForbiddenArenaItems(type);
    player->Unmount();

    // fresh start: no buffs (the preparation aura is applied by AddPlayer). The free repair and the
    // cooldown reset happen in ResetPlayerForFight when the gates open - entering and leaving during
    // the preparation must not hand out anything (free repairs / cooldown resets on demand).
    player->RemoveAllAurasOnDeath();

    if (Pet* pet = player->GetPet())
    {
        pet->RemoveAllAurasOnDeath();
        pet->SetHealth(pet->GetMaxHealth());
        pet->SetPower(POWER_MANA, pet->GetMaxPower(POWER_MANA));
        if (pet->GetPetType() == HUNTER_PET)
            pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(POWER_HAPPINESS));
    }
}

void Arena::ResetPlayerForFight(Player* player)
{
    if (player->IsArenaSpectator())
        return;

    // died during the preparation (Hellfire, a fall): the fight starts for everybody on their feet
    if (!player->IsAlive())
    {
        player->ResurrectPlayer(1.0f);
        player->SpawnCorpseBones();
    }

    player->RemoveAurasDueToSpell(SPELL_ARENA_PREPARATION);
    player->RemoveAurasDueToSpell(SPELL_ARENA_RECENTLY_BANDAGED);
    player->RemoveShortDurationBuffs(ARENA_SHORT_BUFF_DURATION);
    player->DurabilityRepairAll(false, 0.0f);
    ResetArenaCooldowns(player);
    player->SetHealthPercent(100.0f);
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    player->SetPower(POWER_ENERGY, player->GetMaxPower(POWER_ENERGY));
    player->SetPower(POWER_RAGE, 0);
    ApplyTeamAura(player);

    if (Pet* pet = player->GetPet())
    {
        pet->RemoveShortDurationBuffs(ARENA_SHORT_BUFF_DURATION);
        pet->RemoveAllCooldowns();
        pet->SetHealth(pet->GetMaxHealth());
        pet->SetPower(POWER_MANA, pet->GetMaxPower(POWER_MANA));
    }
}

void Arena::RestorePlayer(Player* player, bool participant)
{
    // The spectator state is deliberately NOT cleared here. This runs while he is still standing in
    // the arena and the far teleport has not happened yet, so restoring his visibility now makes him
    // appear in the middle of the map for the length of the handshake. BattleGroundMap::Remove clears
    // it at the one moment he has actually left.
    player->Unmount();
    player->CombatStopWithPets(true);

    // visitors keep their buffs (they never fought), fighters leave the arena clean
    if (!participant)
        return;

    player->RemoveAllAurasOnDeath();

    if (Pet* pet = player->GetPet())
    {
        if (pet->IsControlled())
        {
            pet->RemoveAllAurasOnDeath();
            pet->CombatStop();
            if (pet->IsAlive())
                pet->SetHealth(pet->GetMaxHealth());
        }
    }
}

void Arena::ResetArenaCooldowns(Player* player)
{
    if (sWorld.getConfig(CONFIG_BOOL_ARENA_RESET_ALL_COOLDOWNS))
        player->RemoveAllCooldowns();
    else
    {
        // like TBC: only cooldowns shorter than 10 minutes are reset
        player->RemoveSomeCooldown([](SpellEntry const& spell)
        {
            return spell.RecoveryTime < ARENA_COOLDOWN_RESET_MAX_DURATION && spell.CategoryRecoveryTime < ARENA_COOLDOWN_RESET_MAX_DURATION;
        });
    }
}

uint32 Arena::GetTeamBannerSpell(Team side, bool horde)
{
    if (side == HORDE)
        return horde ? SPELL_ARENA_TEAM_GREEN_HORDE : SPELL_ARENA_TEAM_GREEN;
    return horde ? SPELL_ARENA_TEAM_GOLD_HORDE : SPELL_ARENA_TEAM_GOLD;
}

void Arena::ApplyTeamAura(Player* player)
{
    // The colour is the arena side he was put on, the crest is the faction he actually plays - a
    // Horde player can end up on the gold side, and then he carries a gold banner with the Horde
    // crest. Blizzard has all four; only this pick was missing.
    uint32 const spellId = GetTeamBannerSpell(player->GetBGTeam(), player->GetTeam() == HORDE);
    if (!player->HasAura(spellId))
        player->AddAura(spellId);
}

bool Arena::IsPlayerReady(ObjectGuid guid) const
{
    auto const itr = m_playerScores.find(guid);
    return itr != m_playerScores.end() && static_cast<ArenaScore*>(itr->second)->ready;
}

bool Arena::SetPlayerReady(Player* player)
{
    // Reports whether it took. Without a score there is nothing to mark, and the gossip would
    // otherwise keep offering "I'm ready!" and replay the sound and the watcher's yell on every click.
    auto const itr = m_playerScores.find(player->GetObjectGuid());
    if (itr == m_playerScores.end())
        return false;

    static_cast<ArenaScore*>(itr->second)->ready = true;
    return true;
}

void Arena::ApplyWatcherTeamColours()
{
    // Which watcher belongs to which team is decided by where he stands rather than by his spawn
    // event, so this holds for every arena without a table of its own.
    float ax, ay, az, ao, hx, hy, hz, ho;
    GetTeamStartLoc(ALLIANCE, ax, ay, az, ao);
    GetTeamStartLoc(HORDE, hx, hy, hz, ho);
    for (uint8 const event : { ARENA_EVENT_WATCHER_1, ARENA_EVENT_WATCHER_2 })
    {
        for (ObjectGuid const& guid : m_eventObjects[MAKE_PAIR32(event, 0)].creatures)
        {
            Creature* watcher = GetBgMap()->GetCreature(guid);
            if (!watcher)
                continue;

            Team const side = watcher->GetDistance2d(hx, hy) < watcher->GetDistance2d(ax, ay) ? HORDE : ALLIANCE;

            // he carries the crest of the faction standing in his box, so the banner reads the same
            // as the one on their backs. A mixed side takes the majority, alliance on a tie.
            int32 balance = 0;
            for (const auto& itr : m_players)
                if (Player const* player = sObjectMgr.GetPlayer(itr.first))
                    if (player->GetBGTeam() == side)
                        balance += player->GetTeam() == HORDE ? 1 : -1;

            uint32 const spellId = GetTeamBannerSpell(side, balance > 0);
            if (watcher->HasAura(spellId))
                continue;

            // the side can still fill up while they gather, so an earlier guess is taken off again
            for (uint32 const other : { SPELL_ARENA_TEAM_GOLD, SPELL_ARENA_TEAM_GREEN,
                                        SPELL_ARENA_TEAM_GOLD_HORDE, SPELL_ARENA_TEAM_GREEN_HORDE })
                if (other != spellId)
                    watcher->RemoveAurasDueToSpell(other);

            watcher->AddAura(spellId);
        }
    }
}

bool Arena::AreAllPlayersReady() const
{
    if (GetPlayersSize() < GetMaxPlayers())
        return false;

    for (const auto& itr : m_players)
    {
        if (!IsPlayerReady(itr.first))
            return false;
    }
    return true;
}

void Arena::SendPacketToAll(WorldPacket* packet)
{
    // the template has no map; visitors are on the map but not in m_players, participants that are
    // being ported out are in m_players but no longer on the map - so send to the union of both
    if (!HasBgMap())
    {
        BattleGround::SendPacketToAll(packet);
        return;
    }

    BattleGroundMap* map = GetBgMap();
    std::set<ObjectGuid> sent;
    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        if (Player* player = itr->getSource())
        {
            player->GetSession()->SendPacket(packet);
            sent.insert(player->GetObjectGuid());
        }
    }

    for (const auto& itr : m_players)
        if (sent.find(itr.first) == sent.end())
            if (Player* player = sObjectMgr.GetPlayer(itr.first))
                player->GetSession()->SendPacket(packet);
}

void Arena::RemoveSpectators()
{
    Map::PlayerList const& players = GetBgMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* player = itr->getSource();
        if (!player || !player->IsArenaSpectator())
            continue;

        // participants are handled by the base class, visitors are only known to the map
        if (m_players.find(player->GetObjectGuid()) != m_players.end())
            continue;

        player->LeaveBattleground(true);
    }
}

/*********************************************************/
/***                   FIGHT / SCORES                  ***/
/*********************************************************/

void Arena::HandleKillPlayer(Player* pVictim, Player* pKiller)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    BattleGround::HandleKillPlayer(pVictim, pKiller);

    // no insignia in arenas
    pVictim->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE);

    // no self resurrection (Reincarnation, Twisting Nether ...): dead is dead in the arena
    pVictim->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);

    PlaySoundToAll(SOUND_ARENA_KILL);
    CheckWinConditions();
}

void Arena::UpdatePlayerScore(Player* source, uint32 type, uint32 value)
{
    BattleGroundScoreMap::iterator itr = m_playerScores.find(source->GetObjectGuid());
    if (itr == m_playerScores.end())
        return;

    switch (type)
    {
        case SCORE_DAMAGE_DONE:
            ((ArenaScore*)itr->second)->damageDone += value;
            break;
        case SCORE_HEALING_DONE:
            ((ArenaScore*)itr->second)->healingDone += value;
            break;
        default:
            BattleGround::UpdatePlayerScore(source, type, value);
            break;
    }
}

uint32 Arena::GetTeamDamageDone(Team team) const
{
    uint32 damage = 0;
    for (const auto& itr : m_players)
    {
        if (itr.second.playerTeam != team)
            continue;

        BattleGroundScoreMap::const_iterator score = m_playerScores.find(itr.first);
        if (score != m_playerScores.end())
            damage += ((ArenaScore*)score->second)->damageDone;
    }
    return damage;
}

uint32 Arena::GetRemainingTime() const
{
    uint32 const limit = sWorld.getConfig(CONFIG_UINT32_ARENA_TIME_LIMIT_MINUTES) * MINUTE * IN_MILLISECONDS;
    if (GetStatus() != STATUS_IN_PROGRESS)
        return limit;
    return m_matchTimer < limit ? limit - m_matchTimer : 0;
}

void Arena::CheckWinConditions()
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    uint32 const aliveAlliance = GetAlivePlayersCountByTeam(ALLIANCE);
    uint32 const aliveHorde = GetAlivePlayersCountByTeam(HORDE);

    // in .debug bg mode a match can run with one team only
    bool const testing = sBattleGroundMgr.isTesting();
    bool const allianceOut = !aliveAlliance && (!testing || GetPlayersCountByTeam(ALLIANCE));
    bool const hordeOut = !aliveHorde && (!testing || GetPlayersCountByTeam(HORDE));

    if (allianceOut && hordeOut)
        EndBattleGround(TEAM_NONE);
    else if (allianceOut)
        EndBattleGround(HORDE);
    else if (hordeOut)
        EndBattleGround(ALLIANCE);
    else if (m_timeLimitReached)
    {
        // time is up: the team with the most damage done wins
        uint32 const damageAlliance = GetTeamDamageDone(ALLIANCE);
        uint32 const damageHorde = GetTeamDamageDone(HORDE);
        if (damageAlliance > damageHorde)
            EndBattleGround(ALLIANCE);
        else if (damageHorde > damageAlliance)
            EndBattleGround(HORDE);
        else
            EndBattleGround(TEAM_NONE);
    }
}

void Arena::EndBattleGround(Team winner)
{
    if (GetStatus() == STATUS_WAIT_LEAVE)
        return;

    for (const auto& itr : m_players)
        if (Player* player = sObjectMgr.GetPlayer(itr.first))
            RestorePlayer(player, true);

    BattleGround::EndBattleGround(winner);

    if (winner == TEAM_NONE)
    {
        SendMessageToAll(BCT_ARENA_DRAW, CHAT_MSG_BG_SYSTEM_NEUTRAL);
        // no-show abort: remove everybody quickly; a real draw keeps the normal scoreboard time
        if (m_matchTimer < MINUTE * IN_MILLISECONDS)
            SetEndTime(ARENA_TIME_TO_AUTOREMOVE_ABORTED);
    }

    UpdateWorldStates();
}

/*********************************************************/
/***                    WORLD STATES                   ***/
/*********************************************************/

void Arena::UpdateWorldStates()
{
    UpdateWorldState(WORLD_STATE_ARENA_ALIVE_PLAYERS_RED, GetAlivePlayersCountByTeam(HORDE));
    UpdateWorldState(WORLD_STATE_ARENA_ALIVE_PLAYERS_BLUE, GetAlivePlayersCountByTeam(ALLIANCE));
    UpdateTimeWorldStates(GetRemainingTime());
}

void Arena::UpdateTimeWorldStates(uint32 remainingMs)
{
    uint32 const totalSeconds = remainingMs / IN_MILLISECONDS;
    UpdateWorldState(WORLD_STATE_ARENA_TIME_MINUTES, totalSeconds / MINUTE);
    UpdateWorldState(WORLD_STATE_ARENA_TIME_SECONDS, totalSeconds % MINUTE);
}

void Arena::FillInitialWorldStates(WorldPacket& data, uint32& count)
{
    uint32 const totalSeconds = GetRemainingTime() / IN_MILLISECONDS;
    FillInitialWorldState(data, count, WORLD_STATE_ARENA_ALIVE_PLAYERS_RED, GetAlivePlayersCountByTeam(HORDE));
    FillInitialWorldState(data, count, WORLD_STATE_ARENA_ALIVE_PLAYERS_BLUE, GetAlivePlayersCountByTeam(ALLIANCE));
    FillInitialWorldState(data, count, WORLD_STATE_ARENA_TIME_MINUTES, totalSeconds / MINUTE);
    FillInitialWorldState(data, count, WORLD_STATE_ARENA_TIME_SECONDS, totalSeconds % MINUTE);
}

/*********************************************************/
/***                   NAGRAND ARENA                   ***/
/*********************************************************/

bool Arena::SummonTornado()
{
    // random point on a ring around the arena center, the tornado npc script moves it around afterwards
    float const angle = frand(0.0f, 2.0f * M_PI_F);
    float const distance = float(urand(5, 45));
    float const x = 4056.0f + distance * cos(angle);
    float const y = 2922.0f + distance * sin(angle);
    float z = 13.65f;
    float const groundZ = GetBgMap()->GetHeight(x, y, z, true, 10.0f);
    if (groundZ > INVALID_HEIGHT)
        z = groundZ + 0.05f;

    if (!GetBgMap()->SummonCreature(NPC_ARENA_TORNADO, x, y, z, angle - M_PI_F, TEMPSUMMON_MANUAL_DESPAWN, 0))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[Arena] Could not summon tornado on map %u instance %u.", GetMapId(), GetInstanceID());
        return false;
    }
    return true;
}

/*********************************************************/
/***                   DALARAN SEWERS                  ***/
/*********************************************************/

// The water doodads are doors: closed = water visible / solid, open = water gone.
void Arena::SetWaterActive(uint8 event1, bool active)
{
    BGObjects const& objects = m_eventObjects[MAKE_PAIR32(event1, 0)].gameobjects;
    for (const auto& guid : objects)
    {
        if (active)
            DoorClose(guid);
        else
            DoorOpen(guid);
    }
}

void Arena::DoWaterFlush()
{
    for (uint8 event1 = ARENA_EVENT_DS_WATERSPOUT_1; event1 <= ARENA_EVENT_DS_WATERSPOUT_2; ++event1)
        if (Creature* waterSpout = GetBgMap()->GetCreature(GetSingleCreatureGuid(event1, 0)))
        {
            waterSpout->CastSpell(waterSpout, SPELL_ARENA_DS_FLUSH, true);

            // The spell itself is mute (see SOUND_ARENA_DS_WATER_FLUSH). The spouts only wear the
            // invisible stalker model 11686 - they are ordinary units the client loads, which is why
            // their spell is visible - so the sound comes out of the pipe it belongs to instead of
            // being played flat to everybody.
            waterSpout->PlayDistanceSound(SOUND_ARENA_DS_WATER_FLUSH);
        }
}

void Arena::DoWaterfallKick()
{
    Creature* kicker = GetBgMap()->GetCreature(GetSingleCreatureGuid(ARENA_EVENT_DS_WATERFALL_KICKER, 0));
    if (!kicker)
        return;

    for (const auto& itr : m_players)
    {
        Player* player = sObjectMgr.GetPlayer(itr.first);
        if (!player || !player->IsAlive() || player->IsArenaSpectator() || !player->IsInMap(kicker))
            continue;

        if (kicker->GetDistance(player) < 5.0f)
        {
            player->KnockBackFrom(kicker, 5.0f, 20.0f);
        }
    }
}

// Players standing inside a starting pipe are pushed straight through the pipe into the arena.
// The direction is the line from the pipe kicker to the water spout of the same pipe, so nobody
// gets thrown against the pipe walls.
void Arena::KickFromPipes()
{
    for (uint8 event1 = ARENA_EVENT_DS_PIPE_KICKER_1; event1 <= ARENA_EVENT_DS_PIPE_KICKER_2; ++event1)
    {
        Creature* kicker = GetBgMap()->GetCreature(GetSingleCreatureGuid(event1, 0));
        if (!kicker)
            continue;

        Creature* target = nullptr;
        for (uint8 spoutEvent = ARENA_EVENT_DS_WATERSPOUT_1; spoutEvent <= ARENA_EVENT_DS_WATERSPOUT_2 && !target; ++spoutEvent)
            if (Creature* spout = GetBgMap()->GetCreature(GetSingleCreatureGuid(spoutEvent, 0)))
                if (spout->GetDistance(kicker) < 30.0f)
                    target = spout;

        if (!target)
            continue;

        float const angle = kicker->GetAngle(target);
        for (const auto& itr : m_players)
        {
            Player* player = sObjectMgr.GetPlayer(itr.first);
            if (!player || !player->IsAlive() || player->IsArenaSpectator() || !player->IsInMap(kicker))
                continue;

            float const distance = kicker->GetDistance(player);
            if (distance >= 53.0f)
                continue;

            float const speed = 20.0f + std::max(0.0f, 40.0f - distance);
            player->SetLaunched(true);
            player->KnockBack(angle, speed, 8.0f);
        }
    }
}

bool Arena::HandleAreaTrigger(Player* player, uint32 trigger)
{
    if (!IsDalaranArena())
        return false;

    switch (trigger)
    {
        // somebody went back into the pipes after the flush: kick him out again
        case AT_ARENA_DS_PIPE_1:
        case AT_ARENA_DS_PIPE_2:
            if (GetStatus() == STATUS_IN_PROGRESS && m_pipeKnockbackCount >= ARENA_DS_PIPE_KNOCKBACK_COUNT)
            {
                KickFromPipes();
                DoWaterFlush();
            }
            return true;
        // outside of the arena: back in
        case AT_ARENA_DS_OUTSIDE_1:
            player->NearTeleportTo(1290.44f, 744.96f, 3.16f, 1.6f);
            return true;
        case AT_ARENA_DS_OUTSIDE_2:
            player->NearTeleportTo(1292.6f, 837.07f, 3.161f, 4.7f);
            return true;
        case AT_ARENA_DS_OUTSIDE_3:
            player->NearTeleportTo(1250.68f, 790.86f, 3.16f, 0.0f);
            return true;
        case AT_ARENA_DS_OUTSIDE_4:
            player->NearTeleportTo(1332.50f, 790.9f, 3.16f, 3.14f);
            return true;
        case AT_ARENA_DS_UNDER_MAP_1:
        case AT_ARENA_DS_UNDER_MAP_2:
        case AT_ARENA_DS_UNDER_MAP_3:
            player->NearTeleportTo(1330.0f, 800.0f, 3.16f, player->GetOrientation());
            return true;
    }
    return false;
}
