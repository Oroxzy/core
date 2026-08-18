-- ============================================================================
--  Custom arenas for the 1.12 client (world database)
--
--  Server side counterpart of https://github.com/Oroxzy/VMaNGOSArenaPatch (client patch,
--  contains the TBC / WotLK arena maps, dbc entries, sounds and the scoreboard UI).
--
--  Requirements:
--   * Arena.Enable = 1 in mangosd.conf
--   * maps / vmaps / mmaps of the arena maps (see arena_data/ in the source tree)
--   * a client with the client patch (WorldSafeLocs.dbc must contain ids 929, 936, 939, 940,
--     1258, 1259, 1362, 1363, 1450, 1451 - the server reads its dbc files from the client;
--     the `arena_start_location` table of section 20 is the fallback if they are missing)
--
--  Sections:
--   1. world_safe_locs_facing      11. creature_template
--   2. area_template               12. creature_equip_template
--   3. map_template                13. creature
--   4. mangos_string               14. creature_battleground
--   5. battleground_template       15. battleground_events
--   6. gameobject_template         16. spell_template
--   7. gameobject                  17. game_tele
--   8. gameobject_battleground     18. creature_display_info_addon
--   9. broadcast_text              19. disabled_arena_spells
--  10. npc_text                    20. arena_start_location (dbc fallback)
--                                  21. The Tiger's Peak (maps, templates, gates, watchers, weather)
--
--  The file is idempotent (DELETE + INSERT per block) and can be re-applied after an update.
--  The Arena Orb (gameobject 187078) is placed by the admin (.gobj add 187078) and never touched here.
--  The gear rules changed through the orb's admin menu are runtime only - persist them in mangosd.conf.
--
--  Note: the Dalaran Sewers area triggers (5326, 5328-5331, 5343, 5344, 5347, 5348) used by
--  Arena::HandleAreaTrigger are not part of this file, the server needs `areatrigger_template`
--  rows for them (WotLK AreaTrigger.dbc data). Without them the pipe re-entry / out of bounds
--  handling of the Dalaran arena is simply inactive.
-- ============================================================================

-- 1. world_safe_locs_facing

    DELETE FROM `world_safe_locs_facing` WHERE `id` IN (929, 936, 939, 940, 1258, 1259, 1362, 1363);

    INSERT INTO `world_safe_locs_facing` (`id`, `orientation`) VALUES
        (929, -2.64879),
        (936, 0.492804),
        (939, -2.2602),
        (940, 0.881392),
        (1258, -1.45735),
        (1259, 1.68424),
        (1362, 3.15),
        (1363, 6.3);

-- 2. area_template

    DELETE FROM `area_template` WHERE `entry` IN (3702, 3698, 3968, 4378);

    INSERT INTO `area_template` (`entry`, `map_id`, `zone_id`, `explore_flag`, `flags`, `area_level`, `name`, `team`, `liquid_type`) VALUES
        (4378, 0, 4378, 0, 128, 60, 'Dalaran Sewers', 0, 0),
        (3968, 0, 3968, 0, 128, 60, 'Ruins of Lordaeron', 0, 0),
        (3698, 0, 3698, 0, 128, 60, 'Nagrand Arena', 0, 0),
        (3702, 0, 3702, 0, 128, 60, 'Blade\'s Edge Arena', 0, 0);

-- 3. map_template

    DELETE FROM `map_template` WHERE `entry` BETWEEN 556 AND 620 AND `map_type`=3;

    INSERT INTO `map_template` (`entry`, `patch`, `parent`, `map_type`, `linked_zone`, `player_limit`, `reset_delay`, `ghost_entrance_map`, `ghost_entrance_x`, `ghost_entrance_y`, `map_name`, `script_name`) VALUES
        (620, 0, 0, 3, 4378, 0, 0, -1, 0, 0, 'Dalaran Arena 5v5', ''),
        (619, 0, 0, 3, 4378, 0, 0, -1, 0, 0, 'Dalaran Arena 3v3', ''),
        (618, 0, 0, 3, 4378, 0, 0, -1, 0, 0, 'Dalaran Arena 2v2', ''),
        (617, 0, 0, 3, 4378, 0, 0, -1, 0, 0, 'Dalaran Arena 1v1', ''),
        (573, 0, 0, 3, 3968, 0, 0, -1, 0, 0, 'Ruins of Lordaeron 5v5', ''),
        (572, 0, 0, 3, 3968, 0, 0, -1, 0, 0, 'Ruins of Lordaeron 3v3', ''),
        (571, 0, 0, 3, 3968, 0, 0, -1, 0, 0, 'Ruins of Lordaeron 2v2', ''),
        (570, 0, 0, 3, 3968, 0, 0, -1, 0, 0, 'Ruins of Lordaeron 1v1', ''),
        (563, 0, 0, 3, 3702, 0, 0, -1, 0, 0, 'Blade\'s Edge Arena 5v5', ''),
        (562, 0, 0, 3, 3702, 0, 0, -1, 0, 0, 'Blade\'s Edge Arena 3v3', ''),
        (561, 0, 0, 3, 3702, 0, 0, -1, 0, 0, 'Blade\'s Edge Arena 2v2', ''),
        (560, 0, 0, 3, 3702, 0, 0, -1, 0, 0, 'Blade\'s Edge Arena 1v1', ''),
        (559, 0, 0, 3, 3698, 0, 0, -1, 0, 0, 'Nagrand Arena 5v5', ''),
        (558, 0, 0, 3, 3698, 0, 0, -1, 0, 0, 'Nagrand Arena 3v3', ''),
        (557, 0, 0, 3, 3698, 0, 0, -1, 0, 0, 'Nagrand Arena 2v2', ''),
        (556, 0, 0, 3, 3698, 0, 0, -1, 0, 0, 'Nagrand Arena 1v1', '');


-- 4. mangos_string (see ArenaMangosStrings in src/game/Battlegrounds/Arena.h)

    DELETE FROM `mangos_string` WHERE `entry` BETWEEN 11100 AND 11103;

    INSERT INTO `mangos_string` (`entry`, `content_default`, `content_loc1`, `content_loc2`, `content_loc3`, `content_loc4`, `content_loc5`, `content_loc6`, `content_loc7`, `content_loc8`) VALUES
        (11100, '%u seconds until the Arena battle begins!', NULL, 'Le combat commence dans %u secondes !', 'Noch %u Sekunden bis der Arenakampf beginnt!', NULL, NULL, NULL, NULL, NULL),
        (11101, '%s joined the Arena for the %s!', NULL, '%s a rejoint l\'arène pour l\'équipe %s !', '%s ist der Arena für das %s beigetreten!', NULL, NULL, NULL, NULL, NULL),
        (11102, '%s left the Arena.', NULL, '%s a quitté l\'arène.', '%s hat die Arena verlassen.', NULL, NULL, NULL, NULL, NULL),
        (11103, 'The time limit was reached! The team with the most damage done wins.', NULL, 'Temps écoulé ! L\'équipe avec le plus de dégâts gagne.', 'Das Zeitlimit ist erreicht! Das Team mit dem meisten Schaden gewinnt.', NULL, NULL, NULL, NULL, NULL);

-- 5. battleground_template

    DELETE FROM `battleground_template` WHERE `id` BETWEEN 4 AND 19;

    INSERT INTO `battleground_template` (`id`, `patch`, `min_players_per_team`, `max_players_per_team`, `min_level`, `max_level`, `alliance_win_spell`, `alliance_lose_spell`, `horde_win_spell`, `horde_lose_spell`, `alliance_start_location`, `horde_start_location`) VALUES
        (4, 0, 1, 1, 10, 60, 0, 0, 0, 0, 936, 929),
        (5, 0, 2, 2, 10, 60, 0, 0, 0, 0, 936, 929),
        (6, 0, 3, 3, 10, 60, 0, 0, 0, 0, 936, 929),
        (7, 0, 5, 5, 10, 60, 0, 0, 0, 0, 936, 929),
        (8, 0, 1, 1, 10, 60, 0, 0, 0, 0, 939, 940),
        (9, 0, 2, 2, 10, 60, 0, 0, 0, 0, 939, 940),
        (10, 0, 3, 3, 10, 60, 0, 0, 0, 0, 939, 940),
        (11, 0, 5, 5, 10, 60, 0, 0, 0, 0, 939, 940),
        (12, 0, 1, 1, 10, 60, 0, 0, 0, 0, 1258, 1259),
        (13, 0, 2, 2, 10, 60, 0, 0, 0, 0, 1258, 1259),
        (14, 0, 3, 3, 10, 60, 0, 0, 0, 0, 1258, 1259),
        (15, 0, 5, 5, 10, 60, 0, 0, 0, 0, 1258, 1259),
        (16, 0, 1, 1, 10, 60, 0, 0, 0, 0, 1362, 1363),
        (17, 0, 2, 2, 10, 60, 0, 0, 0, 0, 1362, 1363),
        (18, 0, 3, 3, 10, 60, 0, 0, 0, 0, 1362, 1363),
        (19, 0, 5, 5, 10, 60, 0, 0, 0, 0, 1362, 1363);

-- 6. gameobject_template

    DELETE FROM `gameobject_template` WHERE `entry` IN (184663, 184664, 185917, 185918, 183972, 183970, 183973, 183971, 183980, 183978, 183979, 183977, 192643, 192642, 194395, 191877, 191878, 187078);

    -- Shadow Sight
    -- only the columns this door actually needs; everything else keeps its default
    INSERT INTO `gameobject_template` (`entry`, `patch`, `type`, `displayId`, `name`, `faction`, `flags`, `size`) VALUES
        (213196, 0, 0, 8601, 'Doodad_Uldum_Door_01', 114, 36, 1),
        (213197, 0, 0, 8601, 'Doodad_Uldum_Door_01', 114, 36, 1);

    DELETE FROM `gameobject` WHERE `guid` IN (628003, 628004, 629003, 629004, 630003, 630004, 631003, 631004);
    INSERT INTO `gameobject` (`guid`, `id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `rotation0`, `rotation1`, `rotation2`, `rotation3`, `spawntimesecsmin`, `spawntimesecsmax`, `animprogress`, `state`, `visibility_mod`, `patch_min`, `patch_max`) VALUES
        (628003, 213197, 628, -10774.600, 431.238, 23.5428, 0, 0, 0, 0, 1, 86400, 86400, 100, 1, 0, 0, 10),
        (628004, 213196, 628, -10654.300, 428.305, 23.5428, 3.14159265, 0, 0, -1, 0, 86400, 86400, 100, 1, 0, 0, 10),
        (629003, 213197, 629, -10774.600, 431.238, 23.5428, 0, 0, 0, 0, 1, 86400, 86400, 100, 1, 0, 0, 10),
        (629004, 213196, 629, -10654.300, 428.305, 23.5428, 3.14159265, 0, 0, -1, 0, 86400, 86400, 100, 1, 0, 0, 10),
        (630003, 213197, 630, -10774.600, 431.238, 23.5428, 0, 0, 0, 0, 1, 86400, 86400, 100, 1, 0, 0, 10),
        (630004, 213196, 630, -10654.300, 428.305, 23.5428, 3.14159265, 0, 0, -1, 0, 86400, 86400, 100, 1, 0, 0, 10),
        (631003, 213197, 631, -10774.600, 431.238, 23.5428, 0, 0, 0, 0, 1, 86400, 86400, 100, 1, 0, 0, 10),
        (631004, 213196, 631, -10654.300, 428.305, 23.5428, 3.14159265, 0, 0, -1, 0, 86400, 86400, 100, 1, 0, 0, 10);

    DELETE FROM `gameobject_battleground` WHERE `guid` IN (628003, 628004, 629003, 629004, 630003, 630004, 631003, 631004);
    INSERT INTO `gameobject_battleground` (`guid`, `event1`, `event2`) VALUES
        (628003, 254, 0), (628004, 254, 0),
        (629003, 254, 0), (629004, 254, 0),
        (630003, 254, 0), (630004, 254, 0),
        (631003, 254, 0), (631004, 254, 0);
