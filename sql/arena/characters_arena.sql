-- ============================================================================
--  Custom arenas for the 1.12 client (character database)
--
--  Holds the arena rating of every character that has played a rated match. One row per
--  character and bracket; a character that never played rated has no row at all and counts
--  as Arena.Rated.StartRating / Arena.Rated.StartMatchmakerRating until he does.
--
--  Deliberately NOT a migration under sql/migrations: the core refuses to start when a
--  required migration is missing, and the arena is an optional feature - a server that takes
--  the code but not the arena should not be held hostage by this table.
--
--  Applied to the character database, not the world database.
-- ============================================================================

CREATE TABLE IF NOT EXISTS `character_arena_stats` (
    `guid`        INT UNSIGNED     NOT NULL                COMMENT 'characters.guid',
    `bracket`     TINYINT UNSIGNED NOT NULL                COMMENT '0 = 1v1, 1 = 2v2, 2 = 3v3, 3 = 5v5',
    `rating`      SMALLINT UNSIGNED NOT NULL DEFAULT 0     COMMENT 'the rating shown to the player',
    `mmr`         SMALLINT UNSIGNED NOT NULL DEFAULT 1500  COMMENT 'matchmaking rating, never shown in the ladder',
    `games`       INT UNSIGNED     NOT NULL DEFAULT 0,
    `wins`        INT UNSIGNED     NOT NULL DEFAULT 0,
    `best_rating` SMALLINT UNSIGNED NOT NULL DEFAULT 0     COMMENT 'highest rating ever reached in this bracket',
    `last_played` INT UNSIGNED     NOT NULL DEFAULT 0      COMMENT 'unix time of the last rated match',
    PRIMARY KEY (`guid`, `bracket`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='Arena rating per character and bracket';
