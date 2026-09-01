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

#ifndef __ARENA_RATING_H
#define __ARENA_RATING_H

/*
 * Arena rating, per player and per bracket. There are no arena teams here.
 *
 * Retail went the same way in the end: patch 5.4.0 removed arena teams altogether and left
 * exactly these two numbers per character and bracket. It also fits this arena better than a
 * team rating ever could - both factions wait in one mixed queue and the two sides are put
 * together when the match is created (BattleGroundQueue::FillArenaSelectionPools), so there is
 * no team object that outlives a match and could carry a rating.
 *
 * Two numbers, because one would not do the job of either:
 *  - `mmr` is honest Elo. It only ever asks who is the better player and it decides who meets
 *    whom in the queue. It is never shown in a ladder.
 *  - `rating` is what the player sees. It starts low and climbs towards his mmr (the gain below
 *    1300 is doubled for that), so a new character walks up his ladder instead of appearing at
 *    the top of it. Once the two meet, they move together.
 * Start them at the same value and they stay identical forever - that is why the defaults do not.
 *
 * The arithmetic is TrinityCore 3.3.5 (src/server/game/Battlegrounds/ArenaTeam.cpp), which is
 * as close to Blizzard's as anyone has got. Only the team half is dropped: TrinityCore already
 * computes each member's rating change from his OWN rating against the opponent TEAM's mmr, so
 * the per player part carries over unchanged.
 */

#include "Common.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include "Policies/Singleton.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Arena.Rated.Mode
enum ArenaRatedMode
{
    ARENA_RATED_OFF     = 0,                                // nothing is rated, no rating is ever written
    ARENA_RATED_PREMADE = 1,                                // 1v1 always; 2v2/3v3/5v5 only when both sides queued as a full group
    ARENA_RATED_ALL     = 2,                                // every full match counts, however the sides were assembled
};

struct ArenaRatingEntry
{
    uint32 rating = 0;
    uint32 mmr = 0;
    uint32 games = 0;
    uint32 wins = 0;
    uint32 bestRating = 0;
    uint32 lastPlayed = 0;                                  // unix time
};

// One row of a ladder listing, already sorted.
struct ArenaLadderRow
{
    ObjectGuid guid;
    std::string name;
    uint32 rating;
    uint32 games;
    uint32 wins;
};

class ArenaRatingMgr
{
    public:
        void LoadFromDB();

        // The rating of a character. A character who never played a rated match in this bracket has
        // no row at all and answers with the configured starting values.
        ArenaRatingEntry Get(ObjectGuid guid, ArenaType type) const;
        uint32 GetRating(ObjectGuid guid, ArenaType type) const { return Get(guid, type).rating; }
        uint32 GetMatchmakerRating(ObjectGuid guid, ArenaType type) const { return Get(guid, type).mmr; }
        // Average matchmaking rating of a group of characters, which is what the queue matches on.
        uint32 GetAverageMatchmakerRating(std::vector<ObjectGuid> const& guids, ArenaType type) const;
        // True when this character has a row in this bracket, i.e. has played it at least once.
        bool HasPlayed(ObjectGuid guid, ArenaType type) const;

        // Books a finished match and answers with the character's new numbers. Writes through to the character database
        // immediately: an arena result that survives only in memory is lost in the next crash, and
        // the rating is the one thing players will not forgive us for losing.
        ArenaRatingEntry Apply(ObjectGuid guid, ArenaType type, int32 ratingChange, int32 mmrChange, bool won);
        // Administrative override (.arena setrating), also used to hand a character back to the start.
        void Set(ObjectGuid guid, ArenaType type, uint32 rating, uint32 mmr);
        // Drops every rating of this bracket, or of all brackets when type is ARENA_TYPE_NONE.
        uint32 Reset(ArenaType type);
        // Forgets one character in every bracket, cache and table. For character deletion: guids are
        // handed out again, and the next owner must not inherit a ladder position.
        void Remove(ObjectGuid guid);

        // The top players of a bracket, best first. Characters with fewer than minGames are left out.
        void GetLadder(ArenaType type, uint32 maxRows, uint32 minGames, std::vector<ArenaLadderRow>& out) const;


        /*
         * The same thing for every bracket at once.
         *
         * GetRank walks the whole rating map, and the arena window wants all four - so asking it
         * four times walked the map four times for one refresh, and that window refreshes on
         * open, on every queue change and on every tab switch. One walk answers all of them.
         *
         * rank[i] and total[i] are indexed the way GetArenaTypeIndex indexes brackets.
         */
        void GetRanks(ObjectGuid guid, uint32* rank, uint32* total) const;

        /*
         * The maths, TrinityCore 3.3.5 ArenaTeam.cpp. All of it is driven by one number: how likely
         * the winner was to win. Beating somebody far above you moves the rating a lot, beating
         * somebody far below you moves it barely at all.
         */
        static float GetChanceAgainst(uint32 ownRating, uint32 opponentRating);
        // Change of the visible rating: own rating against the opponent side's mmr.
        static int32 GetRatingMod(uint32 ownRating, uint32 opponentMmr, bool won);
        // Change of the hidden mmr: own side's mmr against the opponent side's mmr. Every member of a
        // side gets the same one, so being carried does not wreck the mmr of the one carrying.
        static int32 GetMatchmakerRatingMod(uint32 ownMmr, uint32 opponentMmr, bool won);

        static uint32 GetStartRating();
        static uint32 GetStartMatchmakerRating();
        // Is there a rating system at all: the table has to be there and the configuration has to
        // want one. False turns every part of this off, down to the menu entry at the arena orb.
        bool IsRatingEnabled() const;
        // Whether matches of this size are rated. The premade question can only be answered by the
        // match itself and lives in Arena::DetermineRated.
        bool IsRatedBracket(ArenaType type) const { return type != ARENA_TYPE_NONE && IsRatingEnabled(); }
        // False when `character_arena_stats` does not exist - nothing is read, nothing is written.
        bool IsAvailable() const { return m_available; }

    private:
        // guid counter and bracket in one key - a character has a row only for the brackets he played
        static uint64 MakeKey(ObjectGuid guid, ArenaType type)
        {
            return (uint64(guid.GetCounter()) << 8) | uint64(GetArenaTypeIndex(type));
        }
        void Save(ObjectGuid guid, ArenaType type, ArenaRatingEntry const& entry) const;

        bool m_available = false;                           // the table exists, set by LoadFromDB
        mutable std::mutex m_lock;                          // read from map threads (arena end, gossip) and the world thread (queue)
        std::unordered_map<uint64, ArenaRatingEntry> m_ratings;
};

#define sArenaRatingMgr MaNGOS::Singleton<ArenaRatingMgr>::Instance()

#endif
