# Custom Arenas (1.12 client)

Server side of the arena patch originally published at https://github.com/Oroxzy/core/tree/TheArena,
ported to the current core and cleaned up. Five arenas - Nagrand Arena, Blade's Edge Arena, Ruins of
Lordaeron, Dalaran Sewers (TBC / WotLK) and The Tiger's Peak (Mists of Pandaria, maps 624-627,
battleground ids 20-23) - can be played in 1v1, 2v2, 3v3 and 5v5, solo or as group, cross faction.

## Setup

1. Client: every player needs the client patch (`Patch-3.MPQ`) from
   https://github.com/Oroxzy/VMaNGOSArenaPatch (maps, dbc entries, sounds, scoreboard UI).
2. Server data: copy the content of `arena_data/` (maps, vmaps, mmaps of the arena maps) into the
   server data folder (`maps/`, `vmaps/`, `mmaps/`). The dbc folder must be extracted from a client
   that has the patch installed (WorldSafeLocs.dbc must contain the arena start locations
   929, 936, 939, 940, 1258, 1259, 1362, 1363, 1450, 1451). If the dbc lacks them, the core falls
   back to the `arena_start_location` table (section 20/21 of the SQL).
3. World database: apply `sql/arena/world_arena.sql`.
4. `mangosd.conf`: set `Arena.Enable = 1` (see the ARENA SETTINGS block for the other options).
5. Place an Arena Orb (`.gobj add 187078`) and optionally an Arena Announcer (`.npc add 600044`)
   next to it. The orb offers queueing (solo / group), leaving queues, spectating and, for
   administrators, the gear rules.

## Rules implemented in the core

* Both factions queue together (`BG_QUEUE_MIXED`); teams are balanced when the match is created,
  cross faction and same faction matches are possible. Everybody understands each other in the arena.
* Entering the arena strips all buffs, resets cooldowns (below 10 minutes unless
  `Arena.ResetAllCooldowns` is set), repairs gear, unequips forbidden items (item level, content
  patch, items with spells listed in `disabled_arena_spells`) and applies Arena Preparation.
* When the gates open: short buffs (< 30 s remaining) are removed, cooldowns reset again, health and
  power refilled. During the match armor can not be swapped (`Arena.AllowItemSwap`,
  `Arena.AllowTrinketSwap`).
* Ready check: talk to the Arena Watcher npc in the starting room; when everybody is ready the gates
  open after `Arena.ReadyStartDelaySeconds`, otherwise after one minute.
* Dead players can not be resurrected or reclaim their corpse; after releasing they watch the rest of
  the match as invisible spectators. A team wins when the other team has no alive players left, or,
  after `Arena.TimeLimitMinutes`, by damage done.
* Kills inside arenas give no honor, leaving gives no deserter debuff.
* Damage done and healing done are tracked and shown on the scoreboard (client patch).
* Nagrand: tornadoes; Dalaran Sewers: pipe flush after the start, periodic waterfall with knockback.
* Tiger's Peak: teams start in the west/east gate alcoves behind the original MoP gates
  (GO 212921, converted PA_Shadowpan_Arenagate_01), watcher npcs in both alcoves, Shadow Sight
  spawns on the two round platforms (layout as in the original MoP arena).

## Tables added / used

`disabled_arena_spells` (entry, 1v1, 2v2, 3v3, 5v5, description) - spells and item on-use spells that
are not allowed per arena size. Everything else lives in the standard world tables (see the section
list at the top of `world_arena.sql`).
