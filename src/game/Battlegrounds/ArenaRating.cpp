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
 */

#include "ArenaRating.h"
#include "ObjectMgr.h"
#include "World.h"
#include "Log.h"
#include "Database/DatabaseEnv.h"
#include "ProgressBar.h"
#include "Policies/SingletonImp.h"

#include <algorithm>
#include <cmath>

INSTANTIATE_SINGLETON_1(ArenaRatingMgr);

/*********************************************************/
/***                      STORAGE                      ***/
/*********************************************************/

void ArenaRatingMgr::LoadFromDB()
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_ratings.clear();
    m_available = false;

    /*
     * Deliberately NOT skipped when Arena.Enable is off. The ratings are only ever kept in this
     * cache, and a match writes the whole row back with REPLACE - so a server that started with the
     * arena disabled, then had it switched on, would hand every returning player a fresh row at the
     * starting values and overwrite what he had. Loading always costs one small query.
     *
     * The table is optional though (sql/arena/characters_arena.sql is applied by hand), so ask
     * before reading rather than letting a missing table look like a database fault.
     */
    {
        std::unique_ptr<QueryResult> exists(CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `information_schema`.`tables` WHERE `table_schema` = DATABASE() AND `table_name` = 'character_arena_stats'"));
        if (!exists || !exists->Fetch()[0].GetUInt32())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Table `character_arena_stats` is missing, arena ratings are disabled");
            return;
        }
    }

    m_available = true;

    //                                                                   0       1          2        3      4       5        6              7
    std::unique_ptr<QueryResult> result(CharacterDatabase.Query("SELECT `guid`, `bracket`, `rating`, `mmr`, `games`, `wins`, `best_rating`, `last_played` FROM `character_arena_stats`"));
    uint32 count = 0;
    if (result)
    {
        BarGoLink bar(result->GetRowCount());
        do
        {
            bar.step();
            Field* fields = result->Fetch();

            uint32 const guidLow = fields[0].GetUInt32();
            uint8 const bracket = fields[1].GetUInt8();
            ArenaType const type = GetArenaTypeByIndex(bracket);
            if (type == ARENA_TYPE_NONE)
            {
                sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Table `character_arena_stats` has unknown bracket %u for character %u, skipped.", bracket, guidLow);
                continue;
            }

            ArenaRatingEntry entry;
            entry.rating = fields[2].GetUInt32();
            entry.mmr = fields[3].GetUInt32();
            entry.games = fields[4].GetUInt32();
            entry.wins = fields[5].GetUInt32();
            entry.bestRating = fields[6].GetUInt32();
            entry.lastPlayed = fields[7].GetUInt32();

            m_ratings[MakeKey(ObjectGuid(HIGHGUID_PLAYER, guidLow), type)] = entry;
            ++count;
        }
        while (result->NextRow());
    }
    else
    {
        BarGoLink bar(1);
        bar.step();
    }

    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u arena ratings", count);
}

ArenaRatingEntry ArenaRatingMgr::Get(ObjectGuid guid, ArenaType type) const
{
    if (type != ARENA_TYPE_NONE)
    {
        std::lock_guard<std::mutex> guard(m_lock);
        auto itr = m_ratings.find(MakeKey(guid, type));
        if (itr != m_ratings.end())
            return itr->second;
    }

    // never played this bracket: he starts where the configuration says everybody starts
    ArenaRatingEntry entry;
    entry.rating = GetStartRating();
    entry.mmr = GetStartMatchmakerRating();
    return entry;
}

bool ArenaRatingMgr::HasPlayed(ObjectGuid guid, ArenaType type) const
{
    if (type == ARENA_TYPE_NONE)
        return false;

    /*
     * ASKED OF THE GAMES, not of whether a row exists.
     *
     * Those were the same thing until Set() - the .arena setrating and setmmr override - started
     * creating a row for a character who had never entered that bracket. From then on he counted
     * as having played it: the ladder listed him, he was given a rank, and the arena window
     * offered him a rating he had never earned. Any administrator who set a starting value on a
     * fresh character put him on the ladder by doing it.
     */
    std::lock_guard<std::mutex> guard(m_lock);
    auto const itr = m_ratings.find(MakeKey(guid, type));
    return itr != m_ratings.end() && itr->second.games > 0;
}

uint32 ArenaRatingMgr::GetAverageMatchmakerRating(std::vector<ObjectGuid> const& guids, ArenaType type) const
{
    if (guids.empty())
        return GetStartMatchmakerRating();

    uint64 sum = 0;
    for (ObjectGuid const& guid : guids)
        sum += GetMatchmakerRating(guid, type);

    return uint32(sum / guids.size());
}

ArenaRatingEntry ArenaRatingMgr::Apply(ObjectGuid guid, ArenaType type, int32 ratingChange, int32 mmrChange, bool won)
{
    // without the table there is nowhere to put this, and writing into the cache alone would be a
    // rating that quietly disappears on the next restart
    if (type == ARENA_TYPE_NONE || !m_available)
        return Get(guid, type);

    ArenaRatingEntry entry;
    {
        std::lock_guard<std::mutex> guard(m_lock);

        uint64 const key = MakeKey(guid, type);
        auto itr = m_ratings.find(key);
        if (itr == m_ratings.end())
        {
            ArenaRatingEntry fresh;
            fresh.rating = GetStartRating();
            fresh.mmr = GetStartMatchmakerRating();
            fresh.bestRating = fresh.rating;   // a first match must not book a "best" below where it began
            itr = m_ratings.emplace(key, fresh).first;
        }

        // neither number may go below zero, and both are stored as SMALLINT UNSIGNED
        entry = itr->second;
        entry.rating = uint32(std::max(0, std::min(int32(0xFFFF), int32(entry.rating) + ratingChange)));
        entry.mmr = uint32(std::max(0, std::min(int32(0xFFFF), int32(entry.mmr) + mmrChange)));
        entry.games += 1;
        if (won)
            entry.wins += 1;
        entry.bestRating = std::max(entry.bestRating, entry.rating);
        entry.lastPlayed = uint32(time(nullptr));

        itr->second = entry;
    }

    Save(guid, type, entry);
    return entry;
}

void ArenaRatingMgr::Set(ObjectGuid guid, ArenaType type, uint32 rating, uint32 mmr)
{
    if (type == ARENA_TYPE_NONE || !m_available)
        return;

    ArenaRatingEntry entry;
    {
        std::lock_guard<std::mutex> guard(m_lock);

        uint64 const key = MakeKey(guid, type);
        auto itr = m_ratings.find(key);
        if (itr == m_ratings.end())
            itr = m_ratings.emplace(key, ArenaRatingEntry()).first;

        itr->second.rating = std::min(rating, uint32(0xFFFF));
        itr->second.mmr = std::min(mmr, uint32(0xFFFF));
        itr->second.bestRating = std::max(itr->second.bestRating, itr->second.rating);
        entry = itr->second;
    }

    Save(guid, type, entry);
}

void ArenaRatingMgr::Remove(ObjectGuid guid)
{
    if (!m_available)
        return;

    {
        std::lock_guard<std::mutex> guard(m_lock);
        for (uint8 index = 0; index < ARENA_TYPES_COUNT; ++index)
            m_ratings.erase(MakeKey(guid, GetArenaTypeByIndex(index)));
    }

    CharacterDatabase.PExecute("DELETE FROM `character_arena_stats` WHERE `guid` = %u", guid.GetCounter());
}

uint32 ArenaRatingMgr::Reset(ArenaType type)
{
    if (!m_available)
        return 0;

    uint32 removed = 0;
    {
        std::lock_guard<std::mutex> guard(m_lock);
        if (type == ARENA_TYPE_NONE)
        {
            removed = uint32(m_ratings.size());
            m_ratings.clear();
        }
        else
        {
            uint8 const bracket = GetArenaTypeIndex(type);
            for (auto itr = m_ratings.begin(); itr != m_ratings.end();)
            {
                if ((itr->first & 0xFF) == bracket)
                {
                    itr = m_ratings.erase(itr);
                    ++removed;
                }
                else
                    ++itr;
            }
        }
    }

    if (type == ARENA_TYPE_NONE)
        CharacterDatabase.Execute("DELETE FROM `character_arena_stats`");
    else
        CharacterDatabase.PExecute("DELETE FROM `character_arena_stats` WHERE `bracket` = %u", GetArenaTypeIndex(type));

    return removed;
}

void ArenaRatingMgr::GetRanks(ObjectGuid guid, uint32* rank, uint32* total) const
{
    uint32 own[ARENA_TYPES_COUNT];
    uint32 above[ARENA_TYPES_COUNT];
    bool played[ARENA_TYPES_COUNT];

    for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
    {
        rank[i] = 0;
        total[i] = 0;
        own[i] = 0;
        above[i] = 0;
        played[i] = false;
    }

    std::lock_guard<std::mutex> guard(m_lock);

    // his own rating in each bracket first, so the single pass below has something to compare to
    for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
    {
        // the same "played means games, not a row" rule as HasPlayed: a row written by the
        // .arena setrating override is not a bracket he has entered
        auto const itr = m_ratings.find(MakeKey(guid, GetArenaTypeByIndex(i)));
        if (itr != m_ratings.end() && itr->second.games > 0)
        {
            own[i] = itr->second.rating;
            played[i] = true;
        }
    }

    // MakeKey packs the bracket into the low byte, so one pass sorts every row into its bracket
    for (auto const& itr : m_ratings)
    {
        uint8 const index = uint8(itr.first & 0xFF);
        if (index >= ARENA_TYPES_COUNT)
            continue;

        // a set-but-never-played row is not part of the ladder, so it must not swell its size
        // either - "rank 7 of 40" where nine of the forty never fought is a number nobody can check
        if (!itr.second.games)
            continue;

        ++total[index];
        if (played[index] && itr.second.rating > own[index])
            ++above[index];
    }

    for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
        if (played[i])
            rank[i] = above[i] + 1;
}

void ArenaRatingMgr::GetLadder(ArenaType type, uint32 maxRows, uint32 minGames, std::vector<ArenaLadderRow>& out) const
{
    out.clear();
    if (type == ARENA_TYPE_NONE || !maxRows)
        return;

    uint8 const bracket = GetArenaTypeIndex(type);
    {
        std::lock_guard<std::mutex> guard(m_lock);
        out.reserve(m_ratings.size() / ARENA_TYPES_COUNT + 1);
        for (auto const& itr : m_ratings)
        {
            if ((itr.first & 0xFF) != bracket || itr.second.games < minGames)
                continue;

            ArenaLadderRow row;
            row.guid = ObjectGuid(HIGHGUID_PLAYER, uint32(itr.first >> 8));
            row.rating = itr.second.rating;
            row.games = itr.second.games;
            row.wins = itr.second.wins;
            out.push_back(row);
        }
    }

    // best first, and on equal rating the one who needed fewer games for it
    std::sort(out.begin(), out.end(), [](ArenaLadderRow const& a, ArenaLadderRow const& b)
    {
        if (a.rating != b.rating)
            return a.rating > b.rating;
        return a.games < b.games;
    });

    // Names last, so a table full of characters costs one name lookup per row shown instead of one
    // per row held. Deleted characters have no name any more and are dropped here, which is why the
    // list is cut to length only afterwards - cutting first would return a short ladder.
    std::vector<ArenaLadderRow> shown;
    shown.reserve(maxRows);
    for (auto& row : out)
    {
        if (!sObjectMgr.GetPlayerNameByGUID(row.guid, row.name))
            continue;

        shown.push_back(row);
        if (shown.size() >= maxRows)
            break;
    }
    out.swap(shown);
}

void ArenaRatingMgr::Save(ObjectGuid guid, ArenaType type, ArenaRatingEntry const& entry) const
{
    CharacterDatabase.PExecute("REPLACE INTO `character_arena_stats` (`guid`, `bracket`, `rating`, `mmr`, `games`, `wins`, `best_rating`, `last_played`) "
                               "VALUES (%u, %u, %u, %u, %u, %u, %u, %u)",
                               guid.GetCounter(), GetArenaTypeIndex(type), entry.rating, entry.mmr,
                               entry.games, entry.wins, entry.bestRating, entry.lastPlayed);
}

/*********************************************************/
/***                     THE MATHS                     ***/
/*********************************************************/

uint32 ArenaRatingMgr::GetStartRating()
{
    return sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_START_RATING);
}

uint32 ArenaRatingMgr::GetStartMatchmakerRating()
{
    return sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_START_MMR);
}

bool ArenaRatingMgr::IsRatingEnabled() const
{
    return m_available && sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_MODE) != ARENA_RATED_OFF;
}

float ArenaRatingMgr::GetChanceAgainst(uint32 ownRating, uint32 opponentRating)
{
    // The chance to beat somebody at that rating. Elo on a scale of 650: 650 points of difference
    // are ten to one, half that is about three to one.
    return 1.0f / (1.0f + std::exp(std::log(10.0f) * (float(opponentRating) - float(ownRating)) / 650.0f));
}

int32 ArenaRatingMgr::GetMatchmakerRatingMod(uint32 ownMmr, uint32 opponentMmr, bool won)
{
    float const chance = GetChanceAgainst(ownMmr, opponentMmr);
    float const mod = ((won ? 1.0f : 0.0f) - chance) * float(sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_MMR_MODIFIER));
    return int32(std::ceil(mod));
}

int32 ArenaRatingMgr::GetRatingMod(uint32 ownRating, uint32 opponentMmr, bool won)
{
    float const chance = GetChanceAgainst(ownRating, opponentMmr);

    if (!won)
        return int32(std::ceil(float(sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_LOSE_MODIFIER)) * -chance));

    float const low = float(sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_WIN_MODIFIER_LOW));
    float const normal = float(sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_WIN_MODIFIER));

    // Below 1300 the gain is inflated so a fresh character walks up to his real place instead of
    // grinding towards it: the full low step below 1000, fading into the normal one at 1300.
    // TrinityCore ends the fade at half of the low modifier, which is the same thing only as long
    // as nobody changes the defaults - here the two ends are the two configured values.
    float mod;
    if (ownRating < 1000)
        mod = low * (1.0f - chance);
    else if (ownRating < 1300)
        mod = (normal + (low - normal) * (1300.0f - float(ownRating)) / 300.0f) * (1.0f - chance);
    else
        mod = normal * (1.0f - chance);

    return int32(std::ceil(mod));
}
