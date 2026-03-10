-- Combined custom GameObjects for new DB schema
-- Uses current columns:
-- gameobject_template(..., script_name)
-- gameobject(..., spawn_flags, visibility_mod)

DELETE FROM `gameobject` WHERE `guid` IN (3998849, 3998850, 3998851, 4001802, 4001803);
DELETE FROM `gameobject_template` WHERE `entry` IN (600010, 600011, 600012, 600016, 70005);

INSERT INTO `gameobject_template`
(`entry`, `patch`, `type`, `displayId`, `name`, `icon`, `faction`, `flags`, `size`,
 `data0`, `data1`, `data2`, `data3`, `data4`, `data5`, `data6`, `data7`, `data8`,
 `data9`, `data10`, `data11`, `data12`, `data13`, `data14`, `data15`, `data16`,
 `data17`, `data18`, `data19`, `data20`, `data21`, `data22`, `data23`,
 `mingold`, `maxgold`, `script_name`)
VALUES
(600010, 0, 2, 356,  'Finkle''s Enchant-O-Matic',      '', 35, 0, 1.3,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0, 'npc_enchanter'),

(600011, 0, 2, 259,  'Your Stash',                     '', 0,  0, 1.5,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0, 'player_stash'),

(600012, 0, 5, 266,  'Yellow Stash aura',              '', 0,  0, 3.0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0, ''),

(600016, 0, 2, 2091, 'Finkle''s Property Manipulator', '', 35, 0, 1.0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0, 'npc_property_manager'),

(70005,  0, 2, 2373, 'The Poisoneer',                  '', 35, 0, 0.8,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0, 'npc_poisoneer');

INSERT INTO `gameobject`
(`guid`, `id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`,
 `rotation0`, `rotation1`, `rotation2`, `rotation3`,
 `spawntimesecsmin`, `spawntimesecsmax`, `animprogress`, `state`,
 `spawn_flags`, `visibility_mod`, `patch_min`, `patch_max`)
VALUES
(3998849, 600010, 1, -11849.3, -4760.48, 6.02144, 1.3116,
 0, 0, 0.609795, 0.792559,
 25, 25, 100, 1, 0, 0, 0, 10),

(3998850, 600011, 1, -11847.7, -4757.90, 6.02144, 1.3116,
 0, 0, 0.609795, 0.792559,
 25, 25, 100, 1, 0, 0, 0, 10),

(3998851, 600012, 1, -11847.7, -4757.90, 6.55, 1.3116,
 0, 0, 0.609795, 0.792559,
 25, 25, 100, 1, 0, 0, 0, 10),

(4001802, 600016, 1, -11860.1, -4758.05, 6.15487, 6.24971,
 0, 0, 0.0167349, -0.99986,
 25, 25, 100, 1, 0, 0, 0, 10),

(4001803, 70005, 1, -11855.0, -4758.40, 6.05, 1.45,
 0, 0, 0.663135, 0.748499,
 25, 25, 100, 1, 0, 0, 0, 10);
