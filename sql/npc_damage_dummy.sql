-- =========================================
-- Heal Dummy (Entry 60002)
-- =========================================
DELETE FROM `creature_template` WHERE `entry`=60002 AND `patch`=0;

INSERT INTO `creature_template`
(`entry`, `patch`, `name`, `subname`, `level_min`, `level_max`, `faction`, `npc_flags`, `gossip_menu_id`,
 `display_id1`, `display_id2`, `display_id3`, `display_id4`,
 `display_scale1`, `display_scale2`, `display_scale3`, `display_scale4`,
 `display_probability1`, `display_probability2`, `display_probability3`, `display_probability4`, `display_total_probability`,
 `mount_display_id`, `speed_walk`, `speed_run`, `detection_range`, `call_for_help_range`, `leash_range`,
 `type`, `pet_family`, `rank`, `unit_class`,
 `xp_multiplier`, `health_multiplier`, `mana_multiplier`, `armor_multiplier`,
 `damage_multiplier`, `damage_variance`, `damage_school`,
 `base_attack_time`, `ranged_attack_time`,
 `holy_res`, `fire_res`, `nature_res`, `frost_res`, `shadow_res`, `arcane_res`,
 `trainer_type`, `trainer_spell`, `trainer_class`, `trainer_race`,
 `loot_id`, `pickpocket_loot_id`, `skinning_loot_id`, `gold_min`, `gold_max`,
 `spell_id1`, `spell_id2`, `spell_id3`, `spell_id4`,
 `spell_list_id`, `pet_spell_list_id`, `spawn_spell_id`, `auras`,
 `ai_name`, `movement_type`, `inhabit_type`, `civilian`, `racial_leader`,
 `equipment_id`, `trainer_id`, `vendor_id`,
 `mechanic_immune_mask`, `school_immune_mask`, `immunity_flags`,
 `static_flags1`, `static_flags2`, `flags_extra`, `script_name`)
VALUES
(60002, 0,
 'Heal Dummy', 'Training Dummy',
 63, 63,
 914, 0, 0,
 16174, 0, 0, 0,
 1, 0, 0, 0,
 1, 0, 0, 0, 1,
 0,
 1, 1.14286,
 20, 5, 0,
 6, 0, 0, 1,
 0, 5000, 1, 1,
 0.14, 0, 0,
 2000, 2000,
 0, 0, 0, 0, 0, 0,
 0, 0, 0, 0,
 0, 0, 0, 0, 0,
 0, 0, 0, 0,
 0, 0, 0, '',
 '', 0, 3, 0, 0,
 0, 0, 0,
 3167436667, 0, 0,
 0, 0, 0,
 'npc_heal_dummy');


-- =========================================
-- Damage Dummy (Entry 60003)
-- =========================================
DELETE FROM `creature_template` WHERE `entry`=60003 AND `patch`=0;

INSERT INTO `creature_template`
(`entry`, `patch`, `name`, `subname`, `level_min`, `level_max`, `faction`, `npc_flags`, `gossip_menu_id`,
 `display_id1`, `display_id2`, `display_id3`, `display_id4`,
 `display_scale1`, `display_scale2`, `display_scale3`, `display_scale4`,
 `display_probability1`, `display_probability2`, `display_probability3`, `display_probability4`, `display_total_probability`,
 `mount_display_id`, `speed_walk`, `speed_run`, `detection_range`, `call_for_help_range`, `leash_range`,
 `type`, `pet_family`, `rank`, `unit_class`,
 `xp_multiplier`, `health_multiplier`, `mana_multiplier`, `armor_multiplier`,
 `damage_multiplier`, `damage_variance`, `damage_school`,
 `base_attack_time`, `ranged_attack_time`,
 `holy_res`, `fire_res`, `nature_res`, `frost_res`, `shadow_res`, `arcane_res`,
 `trainer_type`, `trainer_spell`, `trainer_class`, `trainer_race`,
 `loot_id`, `pickpocket_loot_id`, `skinning_loot_id`, `gold_min`, `gold_max`,
 `spell_id1`, `spell_id2`, `spell_id3`, `spell_id4`,
 `spell_list_id`, `pet_spell_list_id`, `spawn_spell_id`, `auras`,
 `ai_name`, `movement_type`, `inhabit_type`, `civilian`, `racial_leader`,
 `equipment_id`, `trainer_id`, `vendor_id`,
 `mechanic_immune_mask`, `school_immune_mask`, `immunity_flags`,
 `static_flags1`, `static_flags2`, `flags_extra`, `script_name`)
VALUES
(60003, 0,
 'Damage Dummy', 'Training Dummy',
 63, 63,
 914, 0, 0,
 16174, 0, 0, 0,
 1, 0, 0, 0,
 1, 0, 0, 0, 1,
 0,
 1, 1.14286,
 20, 5, 0,
 6, 0, 0, 1,
 0, 5000, 1, 1,
 0.14, 0, 0,
 2000, 2000,
 0, 0, 0, 0, 0, 0,
 0, 0, 0, 0,
 0, 0, 0, 0, 0,
 0, 0, 0, 0,
 0, 0, 0, '',
 '', 0, 3, 0, 0,
 0, 0, 0,
 3167436667, 0, 0,
 0, 0, 0,
 'npc_damage_dummy');
