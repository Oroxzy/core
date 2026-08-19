# Custom Arenas (1.12 client)

Server side of the arena patch originally published at https://github.com/Oroxzy/core/tree/TheArena,
ported to the current core and cleaned up. Six arenas - Nagrand Arena, Blade's Edge Arena, Ruins of
Lordaeron, Dalaran Sewers (TBC / WotLK), The Tiger's Peak (Mists of Pandaria, maps 624-627,
battleground ids 20-23) and Tol'Viron Arena (Mists of Pandaria, maps 628-631, battleground ids 24-27,
area 4601) - can be played in 1v1, 2v2, 3v3 and 5v5, solo or as group, cross faction.

The client patch, the prebuilt server data and the map conversion tool chain live in their own
repositories: [`VMaNGOS_Arena_1.12.1_Client_source`](https://github.com/Oroxzy/VMaNGOS_Arena_1.12.1_Client_source)
(patch sources, maps/vmaps/mmaps, scoreboard addon), the optional texture-only
[`VMaNGOS_Arena_1.12.1_HD_Textures`](https://github.com/Oroxzy/VMaNGOS_Arena_1.12.1_HD_Textures)
and [`VMaNGOS_MapPort`](https://github.com/Oroxzy/VMaNGOS_MapPort), the converter that produced
Tol'Viron.

## Setup

1. Client: every player needs the client patch (`Patch-3.MPQ`) from
   https://github.com/Oroxzy/VMaNGOSArenaPatch (maps, dbc entries, sounds, scoreboard columns).
   The `ArenaTeamColors` addon in the same repository is optional and per player: it paints the
   scoreboard rows in the Gold and Green team colours instead of red and blue. The scoreboard UI
   itself can not be shipped inside the patch - 1.12 refuses modified FrameXML, even from an MPQ.
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
  open after `Arena.ReadyStartDelaySeconds`, otherwise after one minute. Being ready is a flag of its
  own - it used to be read back off the team aura, which meant a player wore no colours until he had
  reported in.
* Team colours: gold and green go on the moment a player enters the arena, and the watcher in each
  start box wears the same banner, so a team sees at a glance which colour it plays. Which watcher
  takes which colour is decided by the start location he stands closer to, so it needs no table.
* Dead players can not be resurrected or reclaim their corpse; after releasing they watch the rest of
  the match as invisible spectators. A team wins when the other team has no alive players left, or,
  after `Arena.TimeLimitMinutes`, by damage done.
* Kills inside arenas give no honor. Losing a match costs nothing either, but *leaving* an unfinished
  one does: participants get the Deserter debuff for `Arena.LeaveLockoutMinutes` (default 10, 0 turns
  it off), including on logout and on teleporting out.
* Damage done and healing done are tracked and shown on the scoreboard (client patch). The teams are
  Gold and Green as on retail, not Alliance and Horde - the team travels in the bonus honor field so
  the scoreboard tells same faction matches apart (see the ArenaTeamColors addon in the client patch).
* Nagrand: tornadoes; Dalaran Sewers: pipe flush after the start, periodic waterfall with knockback.
  The flush and the sewer gates are given their sound from here (15196 / 15030): Blizzard's own spell
  and door model carry no sound trigger at all, unlike the gates of the other four arenas.
* Tiger's Peak: teams start in the west/east gate alcoves behind the original MoP gates
  (GO 212921, retail display 8600 = PA_ShadowpanDoor, size 1.45449), watcher npcs in both alcoves,
  Shadow Sight spawns on the two round platforms (layout as in the original MoP arena). It snows.
* Tol'Viron: gates 213196/213197 (client patch display 8601 = Uldum_Door_01, open/close sounds
  26879/26878), Shadow Sight in the two side alcoves, watcher npcs in both start boxes. Gate and orb
  positions are Blizzard's own, taken from TDB 1200.26021 map 980; the rear gates that seal the start
  boxes are doodads of the arena building itself and need no spawn. Uldum's desert ambience.
* Arena watchers: each arena has its own pair, entries 800110..800121, two per map and never the same
  two in different arenas - Nagrand a brute and an ogre mage, Blade's Edge two red fel orcs, Lordaeron
  a deathguard and an armoured skeleton, Dalaran two Kirin Tor casters, The Tiger's Peak two pandaren
  (the second one dark furred, since the client holds only one adult pandaren model), Tol'Viron two
  tol'vir in red and obsidian. Whoever came without a weapon of his own carries a two hander, staff or
  polearm from `creature_equip_template`. Display ids 30001..30012 and models 9001..9010 come from the
  client patch; 800100 stays as the plain fallback template. The core never addresses them by entry,
  only through the `ARENA_EVENT_WATCHER_*` spawn events, so the split costs nothing.
* Weather: `Arena.RandomWeather` rolls the weather per match, but only from what suits the place -
  the sandstorm belongs to Uldum, snow to The Tiger's Peak, rain to the three outdoor arenas, and
  the Dalaran sewers stay clear because there is no sky to see it in. With the option off The
  Tiger's Peak still snows permanently, as before. An admin entry at the arena watcher switches the
  weather of the running match; the orb's admin menu can pin the arena to one map for testing.

## Tables added / used

`disabled_arena_spells` (entry, 1v1, 2v2, 3v3, 5v5, description) - spells and item on-use spells that
are not allowed per arena size. Everything else lives in the standard world tables (see the section
list at the top of `world_arena.sql`).
