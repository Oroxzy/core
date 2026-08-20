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
 * Custom arena implementation for the 1.12 client (TBC / WotLK arena maps),
 * originally written by https://github.com/Oroxzy based on TrinityCore 3.3.5.
 * Requires the client patch from https://github.com/Oroxzy/VMaNGOSArenaPatch
 * and the world data in sql/arena/.
 */

#ifndef __ARENA_H
#define __ARENA_H

#include "Common.h"
#include "BattleGround.h"
#include "ArenaRating.h"
#include "Policies/Singleton.h"

#include <string>
#include <unordered_map>
#include <vector>

struct ItemPrototype;

// A fighter of a rated match, as he stood when the gates opened.
struct ArenaRatedParticipant
{
    ObjectGuid guid;
    Team team;
    uint32 rating;                                          // before the match
    uint32 mmr;                                             // before the match
};

// Sound ids used by the arenas. Most of them are TBC / WotLK sounds and are shipped with the client patch.
enum ArenaSounds
{
    SOUND_ARENA_KILL                        = 8213,
    SOUND_ARENA_LET_THE_GAMES_BEGIN         = 8280,
    SOUND_ARENA_DS_WATER_INCOMING           = 15355,
    // Blizzard never gave the Dalaran flush a sound: spell 37405 "Flush" uses visual kit 12036 with
    // SoundId 0 and its effect model "Arena Pipe Flush" has no animation events at all (checked against
    // the retail model). Same for the sewer door, whose model carries no $GO trigger although its display
    // entry names an open and a close sound. Both are played from here instead, with Blizzard's own ids.
    SOUND_ARENA_DS_WATER_FLUSH              = 15196,    // DalaranSewer_ArenaWaterFall_Close
    SOUND_ARENA_DS_DOOR_OPEN                = 15030,    // Dalaran_SewerDoor_01_Open (display 8308 sound slot 1)
    // Tol'Viron has the same trouble for the same reason: Blizzard's own display for ULDUM_DOOR_03
    // (MoP GameObjectDisplayInfo 11943) carries no sound at all, and the model has no $GO trigger, so
    // the gate is silent. This is the Uldum door family's own sound, played from the core like Dalaran's.
    SOUND_ARENA_TV_DOOR_OPEN                = 17790,    // Uldum_Door_01_Open
    SOUND_ARENA_PLAYER_LEFT                 = 17341,    // LFG denied
    SOUND_ARENA_MATCH_END                   = 17318,    // LFG dungeon ready
    SOUND_ARENA_MATCH_START                 = 17316,    // LFG rewards
    SOUND_ARENA_COUNTDOWN_TICK              = 25477,
    SOUND_ARENA_COUNTDOWN_FINISHED          = 25478,
    SOUND_ARENA_ORB_DENIED                  = 847,      // played by the arena orb on refused actions
    SOUND_ARENA_READY_CHECK                 = 8960,
};

// broadcast_text entries (sent through BattleGround::SendMessageToAll), see sql/arena/world_arena.sql
enum ArenaBroadcastTexts
{
    BCT_ARENA_START_ONE_MINUTE              = 800110,
    BCT_ARENA_START_HALF_MINUTE             = 800111,
    BCT_ARENA_START_FIFTEEN_SECONDS         = 800112,
    BCT_ARENA_HAS_BEGUN                     = 800113,
    BCT_ARENA_GOLD_TEAM_WINS                = 800114,   // alliance side
    BCT_ARENA_GREEN_TEAM_WINS               = 800115,   // horde side
    BCT_ARENA_DRAW                          = 800116,
    BCT_ARENA_WATCHER_HELLO                 = 800100,   // npc_text 800100
    BCT_ARENA_WATCHER_LEAVE_CONFIRM         = 800101,   // npc_text 800101
    BCT_ARENA_ORB_HELLO                     = 800102,   // npc_text 800102
};

// mangos_string entries with printf arguments (sent through BattleGround::PSendMessageToAll)
enum ArenaMangosStrings
{
    LANG_ARENA_START_COUNTDOWN              = 11100,    // "%u seconds until the Arena battle begins!"
    LANG_ARENA_PLAYER_JOINED                = 11101,    // "%s joined the Arena for the %s!"
    LANG_ARENA_PLAYER_LEFT                  = 11102,    // "%s left the Arena."
    LANG_ARENA_TIME_LIMIT_REACHED           = 11103,    // "The time limit was reached! The team with the most damage done wins."
    LANG_ARENA_RATED_MATCH                  = 11104,    // "This match is rated."
    LANG_ARENA_RATING_RESULT                = 11105,    // "Arena rating %s: %u (%+d), matchmaking rating %u."
    LANG_ARENA_NOT_RATED                    = 11106,    // "This match is not rated: %s."
};

enum ArenaSpells
{
    SPELL_ARENA_PREPARATION                 = 32727,    // no power costs during the preparation, removed when the gates open
    // Retail arena teams are Gold and Green, not Alliance and Horde (a Horde player can end up on the
    // "alliance side" of a match). Blizzard's mapping (MoP WorldStateFrame.lua): faction 0 = Green,
    // faction 1 = Gold. The aura hangs the team banner on the player's back and goes on the moment he
    // enters, not when he reports ready - readiness is a flag of its own on ArenaScore. The watcher in
    // each start box wears the same banner, so a team sees at a glance which colour it is playing.
    //
    // The banner says two things at once: the colour is the arena side, the crest is the player's own
    // faction. All four combinations exist and the client patch already carries every model and
    // texture behind them - only this pick was missing, which is why everyone wore the lion.
    SPELL_ARENA_TEAM_GOLD                   = 32724,    // gold side,  alliance crest -> SPELLS\GoldArenaflag_spell
    SPELL_ARENA_TEAM_GREEN                  = 32725,    // green side, alliance crest -> SPELLS\GreenArenaflag_spell
    SPELL_ARENA_TEAM_GOLD_HORDE             = 35774,    // gold side,  horde crest    -> SPELLS\GoldHordeflag_spell
    SPELL_ARENA_TEAM_GREEN_HORDE            = 35775,    // green side, horde crest    -> SPELLS\GreenHordeflag_spell
    SPELL_ARENA_SHADOW_SIGHT                = 34709,
    SPELL_ARENA_DS_FLUSH                    = 37405,    // visual water flush cast by the water spouts
    SPELL_ARENA_RECENTLY_BANDAGED           = 11196,
    SPELL_ARENA_CALL_PET                    = 883,
    SPELL_ARENA_TORNADO_VISUAL              = 25160,    // used by the Nagrand tornado npc (script)
};

// world states used by the client patch (WorldStateFrame.lua)
enum ArenaWorldStates
{
    WORLD_STATE_ARENA_ALIVE_PLAYERS_RED     = 2575,     // horde side
    WORLD_STATE_ARENA_ALIVE_PLAYERS_BLUE    = 2576,     // alliance side
    WORLD_STATE_ARENA_TIME_SECONDS          = 8295,
    WORLD_STATE_ARENA_TIME_MINUTES          = 8296,
};

// battleground_events event1 ids of the arena maps (event2 is always 0)
enum ArenaEvents
{
    ARENA_EVENT_DS_WATERSPOUT_1             = 200,      // visual flush casters inside the pipes
    ARENA_EVENT_DS_WATERSPOUT_2             = 201,
    ARENA_EVENT_DS_WATERFALL_KICKER         = 202,      // knockback reference point below the waterfall
    ARENA_EVENT_DS_PIPE_KICKER_1            = 203,      // knockback reference points at the pipe ends
    ARENA_EVENT_DS_PIPE_KICKER_2            = 204,
    ARENA_EVENT_DS_DOODAD_SEWER01           = 209,      // rising water
    ARENA_EVENT_DS_DOODAD_WATERFALL         = 210,      // waterfall visual
    ARENA_EVENT_DS_DOODAD_WATERFALL_COLL    = 211,      // waterfall collision
    ARENA_EVENT_SHADOW_SIGHT                = 212,
    ARENA_EVENT_WATCHER_1                   = 214,      // ready check npc (one per starting room)
    ARENA_EVENT_WATCHER_2                   = 215,
};

enum ArenaCreatures
{
    NPC_ARENA_TORNADO                       = 19922,
    NPC_ARENA_WATER_SPOUT                   = 28567,
    NPC_ARENA_ANNOUNCER                     = 600044,   // optional npc next to the arena orb, yells queue joins
    NPC_ARENA_WATCHER                       = 800100,   // fallback template; the spawns use 800110..800121,
                                                        // one entry per arena side so every arena gets its
                                                        // own pair of models. They are reached through the
                                                        // ARENA_EVENT_WATCHER_* guids, never by entry.
};

enum ArenaGameObjects
{
    GO_ARENA_ORB                            = 187078,   // queue / spectate / admin gossip
};

// Dalaran Sewers area triggers (client patch AreaTrigger.dbc / areatrigger_template)
enum ArenaAreaTriggers
{
    AT_ARENA_DS_PIPE_1                      = 5347,
    AT_ARENA_DS_PIPE_2                      = 5348,
    AT_ARENA_DS_UNDER_MAP_1                 = 5326,
    AT_ARENA_DS_UNDER_MAP_2                 = 5343,
    AT_ARENA_DS_UNDER_MAP_3                 = 5344,
    AT_ARENA_DS_OUTSIDE_1                   = 5328,
    AT_ARENA_DS_OUTSIDE_2                   = 5329,
    AT_ARENA_DS_OUTSIDE_3                   = 5330,
    AT_ARENA_DS_OUTSIDE_4                   = 5331,
};

enum ArenaTimers
{
    ARENA_WORLD_STATE_UPDATE_INTERVAL       = 1 * IN_MILLISECONDS,
    // TrinityCore never despawns the gates at all; ours go away so their collision stops blocking
    // the start box. Three seconds cut the door's own opening animation off half way, so wait five.
    ARENA_DOORS_DESPAWN_DELAY               = 5 * IN_MILLISECONDS,
    ARENA_SHADOW_SIGHT_SPAWN_DELAY          = 90,       // seconds after the gates opened
    ARENA_TP_ZONE_ID                        = 4600,     // area_template / AreaTable zone of The Tiger's Peak (weather)
    // the remaining arena zones, taken from Map.dbc - needed to send weather into an arena instance
    ARENA_NA_ZONE_ID                        = 3698,     // Nagrand Arena
    ARENA_BE_ZONE_ID                        = 3702,     // Blade's Edge Arena
    ARENA_RL_ZONE_ID                        = 3968,     // Ruins of Lordaeron
    ARENA_DS_ZONE_ID                        = 4378,     // Dalaran Sewers
    ARENA_TV_ZONE_ID                        = 4601,     // Tol'Viron Arena
    ARENA_SHORT_BUFF_DURATION               = 30 * IN_MILLISECONDS,     // buffs with less remaining time are removed on start
    ARENA_COOLDOWN_RESET_MAX_DURATION       = 10 * MINUTE * IN_MILLISECONDS,   // only shorter cooldowns are reset (unless configured)
    ARENA_TIME_TO_AUTOREMOVE_ABORTED        = 15 * IN_MILLISECONDS,     // players are removed this fast after an aborted match

    ARENA_NA_FIRST_TORNADO_DELAY            = 60 * IN_MILLISECONDS,
    ARENA_NA_TORNADO_INTERVAL_MIN           = 120 * IN_MILLISECONDS,
    ARENA_NA_TORNADO_INTERVAL_MAX           = 180 * IN_MILLISECONDS,
    ARENA_NA_TORNADO_RETRY_DELAY            = 2 * IN_MILLISECONDS,

    ARENA_DS_FIRST_WATERFALL_DELAY          = 20 * IN_MILLISECONDS,
    ARENA_DS_WATERFALL_INTERVAL_MIN         = 30 * IN_MILLISECONDS,
    ARENA_DS_WATERFALL_INTERVAL_MAX         = 60 * IN_MILLISECONDS,
    ARENA_DS_WATERFALL_WARNING_DURATION     = 7500,
    ARENA_DS_WATERFALL_DURATION             = 30 * IN_MILLISECONDS,
    ARENA_DS_WATERFALL_KNOCKBACK_INTERVAL   = 1500,
    ARENA_DS_PIPE_KNOCKBACK_FIRST_DELAY     = 10 * IN_MILLISECONDS,
    ARENA_DS_PIPE_KNOCKBACK_INTERVAL        = 3 * IN_MILLISECONDS,
    ARENA_DS_PIPE_KNOCKBACK_COUNT           = 2,
    ARENA_DS_PIPE_RECHECK_INTERVAL          = 2 * IN_MILLISECONDS,  // server side replacement for the pipe area triggers
    ARENA_UNDER_MAP_CHECK_INTERVAL          = 3 * IN_MILLISECONDS,
    // Extra time on top of the invite accept window when the countdown is held open for a replacement:
    // he still has to accept and load in before the gates open.
    ARENA_REFILL_LOAD_TIME                  = 10 * IN_MILLISECONDS,
};

class ArenaScore : public BattleGroundScore
{
    public:
        ArenaScore() : damageDone(0), healingDone(0), ready(false), newRating(0), ratingChange(0), team(TEAM_NONE) {}
        virtual ~ArenaScore() {}

        uint32 damageDone;
        uint32 healingDone;
        // Which side he fought on. Kept here rather than asked of the battleground, because a player
        // who left has no team there any more - and his row outlives him on the scoreboard.
        Team team;
        // Shown as two more scoreboard columns. The rating is filled when the player enters, so the
        // column means something during the match too, and overwritten at the end of a rated one.
        // Both are always sent: the 1.12 client takes its columns from WorldStateUI.dbc by map id,
        // not from the packet, and the client patch defines four of them for every arena map.
        uint32 newRating;
        int32 ratingChange;
        // Told the arena watcher he is ready. This used to be read off the team aura, which forced the
        // aura to wait for the ready check - the players stood in their box without their colours until
        // they spoke to the watcher. The two are separate now: the colours go on when you enter.
        bool ready;
};

/*
 * Holds the world data of the arena system:
 *  - `disabled_arena_spells`: spells (incl. item on-use spells) that can not be used per arena size
 *  - the patch in which each item was introduced (`item_template`.`patch`), used for the Arena.MaxItemPatch restriction
 */
class ArenaMgr
{
    public:
        void LoadFromDB();

        bool IsSpellDisabled(uint32 spellId, ArenaType type) const;
        uint8 GetItemMinPatch(uint32 itemId) const;
        // 0 when this enchantment may stay, otherwise the forbidden spell that applied it
        uint32 GetForbiddenTempEnchantSpell(uint32 enchantId, ArenaType type) const;

        // Item level / patch / disabled item spell checks. If reason is given it receives a chat-ready explanation.
        bool IsItemForbidden(ItemPrototype const* proto, ArenaType type, std::string* reason = nullptr) const;
        // Resistance a player carries in on his gear, against the Arena.MaxResistance.* caps.
        // Read from the items, not from his current stats - buffs at the orb would distort it and are
        // stripped on entering anyway.
        static bool HasExcessResistance(Player const* player, std::string* reason = nullptr);

        static char const* GetPatchName(uint8 patch);

        struct DisabledSpell
        {
            bool disabledForType[ARENA_TYPES_COUNT] = { false, false, false, false };
        };

        // The whole ban list, for the admin panel and the .arena commands.
        std::unordered_map<uint32, DisabledSpell> const& GetDisabledSpells() const { return m_disabledSpells; }
        // The item a banned spell belongs to, 0 for a spell a player casts himself. Most of the ban
        // list is items - the table holds their on-use spell, because that is what has to be refused.
        uint32 GetItemForSpell(uint32 spellId) const;
        // Every spell an item can put on the arena floor: its on-use spells.
        static void GetItemSpells(ItemPrototype const* proto, std::vector<uint32>& out);
        // Rebuilds the banned spell -> item map. Called after the ban list changes.
        void BuildSpellItemMap();
        // Bans or unbans a spell for the given brackets and writes the row through to the world
        // database, so a change survives a restart the way one made in the table would. Passing four
        // times false removes the row. Returns false only if the spell does not exist.
        bool SetSpellDisabled(uint32 spellId, bool const perType[ARENA_TYPES_COUNT]);

    private:
        std::unordered_map<uint32, DisabledSpell> m_disabledSpells;
        std::unordered_map<uint32, uint8> m_itemMinPatch;
        // banned spell -> the item it comes from, filled while the ban list is read
        std::unordered_map<uint32, uint32> m_spellItem;
        // enchantment id -> the forbidden spell that applies it, for the temporary enchants a player can
        // walk in with (weapon oils, sharpening and weight stones). Rogue poisons are never in here.
        std::unordered_map<uint32, uint32> m_tempEnchantSpells;
};

#define sArenaMgr MaNGOS::Singleton<ArenaMgr>::Instance()

class Arena : public BattleGround
{
    friend class BattleGroundMgr;

    public:
        Arena();
        ~Arena();

        void Update(uint32 diff) override;
        void Reset() override;
        bool SetupBattleGround() override { return true; }
        void StartingEventCloseDoors() override;
        void StartingEventOpenDoors() override;

        void AddPlayer(Player* player) override;
        void RemovePlayerAtLeave(ObjectGuid guid, bool transport, bool sendPacket) override;
        void RemovePlayer(Player* player, ObjectGuid guid) override;
        void HandleKillPlayer(Player* pVictim, Player* pKiller) override;
        // world states, sounds and messages go to everybody on the map (visitors are not in m_players)
        void SendPacketToAll(WorldPacket* packet) override;
        using BattleGround::SendPacketToAll;
        bool HandleAreaTrigger(Player* player, uint32 trigger) override;
        void EndBattleGround(Team winner) override;
        void UpdatePlayerScore(Player* source, uint32 type, uint32 value) override;
        void FillInitialWorldStates(WorldPacket& data, uint32& count) override;
        // no graveyards in arenas, dead players become spectators (see Player::SetArenaSpectator)
        WorldSafeLocsEntry const* GetClosestGraveYard(Player* /*player*/) override { return nullptr; }

        ArenaType GetArenaType() const { return GetArenaTypeForBattleGroundTypeId(GetTypeID()); }
        ArenaMapType GetArenaMapType() const { return GetArenaMapTypeForBattleGroundTypeId(GetTypeID()); }
        bool IsNagrandArena() const { return GetArenaMapType() == ARENA_MAP_NAGRAND; }
        bool IsDalaranArena() const { return GetArenaMapType() == ARENA_MAP_DALARAN; }
        bool IsTigersPeakArena() const { return GetArenaMapType() == ARENA_MAP_TIGERS_PEAK; }
        // Zone of this arena, so weather can be sent into the instance. 0 for an arena without one.
        uint32 GetArenaZoneId() const;
        // Weather kinds that suit this arena, most specific first. Used by the gamemaster's weather menu
        // so it cannot offer a sandstorm on a snowy peak. Returns how many were written.
        uint8 GetSuitableWeather(WeatherType* out, uint8 max) const;
        // Sets one weather for this running match, permanently so the regeneration cannot clear it.
        void SetArenaWeather(WeatherType type, float grade);
        bool IsTolvironArena() const { return GetArenaMapType() == ARENA_MAP_TOLVIRON; }

        // Whether this match counts for the rating. Decided once, when the gates open (see
        // DetermineRated) - not when the match is created, because until the last man has actually
        // arrived we do not know who is standing in the boxes.
        bool IsRated() const { return m_rated; }
        // Takes the match out of the rating again, for an end that is nobody's fault: a gamemaster
        // stopping it would otherwise be booked as a draw, with the loss that carries.
        void CancelRated() { m_rated = false; m_ratedRoster.clear(); }

        uint32 GetTeamDamageDone(Team team) const;
        uint32 GetRemainingTime() const;                    // ms until the time limit is reached
        // Testing aid: ends the preparation immediately, whatever the countdown says. Same path the
        // ready check takes, only without waiting for anybody to be ready.
        void StartMatchNow();
        bool IsPlayerReady(ObjectGuid guid) const;
        bool SetPlayerReady(Player* player);            // false when he has no score to mark
        // colour by arena side, crest by the player's own faction
        static uint32 GetTeamBannerSpell(Team side, bool horde);
        static void ApplyTeamAura(Player* player);          // the team's colours, worn from the moment he enters
        // Puts the same colours on the two watchers, each taking the team whose start box he stands in.
        void ApplyWatcherTeamColours();
        bool AreAllPlayersReady() const;
        // Removes visitors (players that spectate through the arena orb and are not participants).
        void RemoveSpectators();

        void CheckWinConditions();

    private:
        void UpdatePreparation(uint32 diff);
        void UpdateNagrand(uint32 diff);
        void UpdateDalaran(uint32 diff);
        void UpdateWorldStates();
        void UpdateTimeWorldStates(uint32 remainingMs);

        // player handling
        void PrepareArenaPlayer(Player* player);            // on join: strip buffs, forbidden gear (the preparation aura is applied by AddPlayer)
        // Takes off the temporary weapon enchants the table forbids - they are applied outside and
        // would otherwise ride into the match on the weapon. Never touches rogue poisons.
        static void RemoveForbiddenTempEnchants(Player* player, ArenaType type);
        void ResetPlayerForFight(Player* player);           // on gate opening: short buffs, cooldowns, repair, health and power
        void RestorePlayer(Player* player, bool participant);   // on leave / end (visitors keep their buffs)
        static void ResetArenaCooldowns(Player* player);
        bool m_leaverIsParticipant;                         // set by RemovePlayerAtLeave for RemovePlayer (the base erased the player list already)
        // the leaver's scoreboard row, in flight between the two of them (see RemovePlayerAtLeave)
        ArenaScore* m_keptScore = nullptr;
        bool m_spectatorsRemoved;                           // RemoveSpectators ran after the end

        // rating (see ArenaRating.h)
        void DetermineRated();                              // at the gates: does this match count, and who is in it
        bool IsSidePremade(Team team) const;                // one party fills the whole side (a side of one always does)
        void ApplyRatedResult(Team winner);                 // books the result, fills the scoreboard columns, tells the players
        bool m_rated;                                       // stays set after the end - the scoreboard packet asks
        bool m_ratedSettled;                                // the result was booked, never do it twice
        // Who fought, and where he stood before the match. The roster is taken when the gates open and
        // kept, because the players are gone by the time we need it: leaving deletes the score row and
        // the entry in m_players, and reading the rating off the survivors would hand everybody who
        // walks out of a lost match a free pass.
        std::vector<ArenaRatedParticipant> m_ratedRoster;

        // Nagrand Arena
        bool SummonTornado();

        // Dalaran Sewers
        void SetWaterActive(uint8 event1, bool active);
        // Sets the weather for this match: the map's own permanent weather, or a roll from the kinds that
        // suit it when Arena.RandomWeather is on. It never snows in the desert and never storms on a peak.
        void ApplyArenaWeather();
        void DoWaterFlush();
        void DoWaterfallKick();
        void KickFromPipes();

        uint32 m_worldStateTimer;
        uint32 m_matchTimer;                                // ms since the gates opened
        uint32 m_doorsDespawnTimer;                         // ms until the doors are removed after opening, 0 = done
        uint32 m_lastCountdownSecond;
        bool   m_playersReady;
        bool   m_timeLimitReached;
        bool   m_preparationExtended;                       // the countdown was held open once for a replacement

        // Nagrand
        uint32 m_tornadoTimer[2];

        // Dalaran
        uint32 m_waterfallTimer;
        uint32 m_waterfallKnockbackTimer;
        uint32 m_pipeKnockbackTimer;
        uint32 m_pipeRecheckTimer;
        uint8  m_pipeKnockbackCount;
        enum WaterfallState { WATERFALL_OFF, WATERFALL_WARNING, WATERFALL_ON };
        WaterfallState m_waterfallState;

        uint32 m_underMapCheckTimer;
        // teleports players that fell below the arena floor back to their team start location
        void CheckPlayersUnderMap();
};

#endif
