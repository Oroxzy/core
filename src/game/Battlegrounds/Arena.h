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
#include "Policies/Singleton.h"

#include <string>
#include <unordered_map>

struct ItemPrototype;

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
};

enum ArenaSpells
{
    SPELL_ARENA_PREPARATION                 = 32727,    // no power costs during the preparation, removed when the gates open
    // Retail arena teams are Gold and Green, not Alliance and Horde (a Horde player can end up on the
    // "alliance side" of a match). Blizzard's mapping (MoP WorldStateFrame.lua): faction 0 = Green,
    // faction 1 = Gold. The auras carry the team banner on the player's back and double as the "ready"
    // flag during the preparation.
    SPELL_ARENA_TEAM_GOLD                   = 32724,    // alliance side, visual 8378/8379 -> SPELLS\GoldArenaflag_spell
    SPELL_ARENA_TEAM_GREEN                  = 32725,    // horde side, visual 8380/8381 -> SPELLS\GreenArenaflag_spell
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
    NPC_ARENA_WATCHER                       = 800100,
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
};

class ArenaScore : public BattleGroundScore
{
    public:
        ArenaScore() : damageDone(0), healingDone(0) {}
        virtual ~ArenaScore() {}

        uint32 damageDone;
        uint32 healingDone;
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

        // Item level / patch / disabled item spell checks. If reason is given it receives a chat-ready explanation.
        bool IsItemForbidden(ItemPrototype const* proto, ArenaType type, std::string* reason = nullptr) const;

        static char const* GetPatchName(uint8 patch);

    private:
        struct DisabledSpell
        {
            bool disabledForType[ARENA_TYPES_COUNT] = { false, false, false, false };
        };
        std::unordered_map<uint32, DisabledSpell> m_disabledSpells;
        std::unordered_map<uint32, uint8> m_itemMinPatch;
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
        bool IsTolvironArena() const { return GetArenaMapType() == ARENA_MAP_TOLVIRON; }

        uint32 GetTeamDamageDone(Team team) const;
        uint32 GetRemainingTime() const;                    // ms until the time limit is reached
        static bool IsPlayerReady(Player const* player);
        static void ApplyTeamAura(Player* player);          // team marker aura, also the "ready" flag during preparation
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
        void ResetPlayerForFight(Player* player);           // on gate opening: short buffs, cooldowns, repair, health and power
        void RestorePlayer(Player* player, bool participant);   // on leave / end (visitors keep their buffs)
        static void ResetArenaCooldowns(Player* player);
        bool m_leaverIsParticipant;                         // set by RemovePlayerAtLeave for RemovePlayer (the base erased the player list already)
        bool m_spectatorsRemoved;                           // RemoveSpectators ran after the end

        // Nagrand Arena
        bool SummonTornado();

        // Dalaran Sewers
        void SetWaterActive(uint8 event1, bool active);
        void DoWaterFlush();
        void DoWaterfallKick();
        void KickFromPipes();

        uint32 m_worldStateTimer;
        uint32 m_matchTimer;                                // ms since the gates opened
        uint32 m_doorsDespawnTimer;                         // ms until the doors are removed after opening, 0 = done
        uint32 m_lastCountdownSecond;
        bool   m_playersReady;
        bool   m_timeLimitReached;

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
