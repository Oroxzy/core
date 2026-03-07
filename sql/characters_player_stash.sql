-- ============================================
-- PlayerStash - characters DB
-- ============================================

DROP TABLE IF EXISTS `player_stash_gear`;
CREATE TABLE `player_stash_gear` (
  `char_guid` BIGINT(20) UNSIGNED NOT NULL,
  `temp_id` INT(10) UNSIGNED NOT NULL DEFAULT '0',
  `gossip_text` VARCHAR(100) NOT NULL DEFAULT '',
  `item_slot` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
  `item_entry` INT(10) UNSIGNED NOT NULL DEFAULT '0',
  `item_enchant` INT(10) UNSIGNED NOT NULL DEFAULT '0',
  `patch` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
  PRIMARY KEY (`char_guid`,`temp_id`,`item_slot`),
  KEY `idx_temp_id` (`temp_id`),
  KEY `idx_char_guid` (`char_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='PlayerStash gear sets';

DROP TABLE IF EXISTS `player_stash_talents`;
CREATE TABLE `player_stash_talents` (
  `char_guid` BIGINT(20) UNSIGNED NOT NULL,
  `temp_id` INT(10) UNSIGNED NOT NULL DEFAULT '0',
  `gossip_text` VARCHAR(100) NOT NULL DEFAULT '',
  `talent_id` INT(10) UNSIGNED NOT NULL DEFAULT '0',
  `rank` TINYINT(3) UNSIGNED NOT NULL DEFAULT '0',
  PRIMARY KEY (`char_guid`,`temp_id`,`talent_id`),
  KEY `idx_temp_id` (`temp_id`),
  KEY `idx_char_guid` (`char_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='PlayerStash talent sets';