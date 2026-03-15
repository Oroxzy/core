-- ============================================================
-- Buff Machine NPC template
-- ============================================================
DELETE FROM `creature_template` WHERE `entry` = 80000;

INSERT INTO `creature_template`
(
`entry`, `patch`, `name`, `subname`, `level_min`, `level_max`, `faction`, `npc_flags`, `gossip_menu_id`,
`display_id1`, `display_id2`, `display_id3`, `display_id4`,
`display_scale1`, `display_scale2`, `display_scale3`, `display_scale4`,
`display_probability1`, `display_probability2`, `display_probability3`, `display_probability4`,
`display_total_probability`, `mount_display_id`,
`speed_walk`, `speed_run`, `detection_range`, `call_for_help_range`, `leash_range`,
`type`, `pet_family`, `rank`, `unit_class`,
`xp_multiplier`, `health_multiplier`, `mana_multiplier`, `armor_multiplier`, `damage_multiplier`, `damage_variance`,
`damage_school`, `base_attack_time`, `ranged_attack_time`,
`holy_res`, `fire_res`, `nature_res`, `frost_res`, `shadow_res`, `arcane_res`,
`trainer_type`, `trainer_spell`, `trainer_class`, `trainer_race`,
`loot_id`, `pickpocket_loot_id`, `skinning_loot_id`,
`gold_min`, `gold_max`,
`spell_id1`, `spell_id2`, `spell_id3`, `spell_id4`,
`spell_list_id`, `pet_spell_list_id`, `spawn_spell_id`,
`auras`, `ai_name`, `movement_type`, `inhabit_type`,
`civilian`, `racial_leader`,
`equipment_id`, `trainer_id`, `vendor_id`,
`mechanic_immune_mask`, `school_immune_mask`, `immunity_flags`,
`static_flags1`, `static_flags2`, `flags_extra`,
`script_name`
)
VALUES
(
80000, 0, 'Super Buffer Supreme XXL', '', 1, 1, 35, 0, 0,
11686, 0, 0, 0,
1.7, 0, 0, 0,
0, 0, 0, 0,
0, 0,
0.91, 1.14286, 16, 5, 0,
10, 0, 0, 1,
1, 1, 1, 1, 1, 0.14,
0, 2000, 2200,
0, 0, 0, 0, 0, 0,
0, 0, 0, 0,
0, 0, 0,
0, 0,
0, 0, 0, 0,
0, 0, 0,
'31951 20370', '', 0, 3,
0, 0,
3, 0, 0,
0, 0, 0,
0, 0, 0,
'npc_buff_machine'
);

-- ============================================================
-- Buff Machine NPC spawn
-- ============================================================
DELETE FROM `creature` WHERE `guid` = 2530704;
DELETE FROM `creature` WHERE `id` = 80000;

INSERT INTO `creature`
(
`guid`, `id`, `id2`, `id3`, `id4`, `id5`, `map`,
`position_x`, `position_y`, `position_z`, `orientation`,
`spawntimesecsmin`, `spawntimesecsmax`,
`wander_distance`, `health_percent`, `mana_percent`,
`movement_type`, `spawn_flags`, `visibility_mod`,
`patch_min`, `patch_max`
)
VALUES
(
2530704, 80000, 0, 0, 0, 0, 1,
-11839.4, -4770.6, 6.97894, 0.0565052,
25, 25,
0, 100, 0,
0, 0, 0,
0, 10
);

-- ============================================================
-- Buff Machine GameObject template
-- ============================================================
DELETE FROM `gameobject_template` WHERE `entry` = 900067;

INSERT INTO `gameobject_template`
(
`entry`, `patch`, `type`, `displayId`, `name`, `icon`, `faction`, `flags`, `size`,
`data0`, `data1`, `data2`, `data3`, `data4`, `data5`, `data6`, `data7`, `data8`, `data9`, `data10`,
`data11`, `data12`, `data13`, `data14`, `data15`, `data16`, `data17`, `data18`, `data19`, `data20`,
`data21`, `data22`, `data23`,
`mingold`, `maxgold`, `script_name`
)
VALUES
(
900067, 0, 5, 6871, 'Super Buffer Supreme XXL', '', 0, 0, 1,
1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
0, 0, 0,
0, 0, ''
);

-- ============================================================
-- Buff Machine GameObject spawn
-- ============================================================
DELETE FROM `gameobject` WHERE `guid` = 3999038;

INSERT INTO `gameobject`
(
`guid`, `id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`,
`rotation0`, `rotation1`, `rotation2`, `rotation3`,
`spawntimesecsmin`, `spawntimesecsmax`, `animprogress`, `state`,
`spawn_flags`, `visibility_mod`, `patch_min`, `patch_max`
)
VALUES
(
3999038, 900067, 1, -11839.4, -4770.6, 6.15086, 0.0565052,
0, 0, 0.0282488, 0.999601,
25, 25, 100, 1,
0, 0, 0, 10
);