-- Azeroth Completion: per-character progress.
-- Completion is permanent: rows are only removed by ".ac reset" or when the character is deleted.

CREATE TABLE IF NOT EXISTS `azcomp_character_objective` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `category` TINYINT UNSIGNED NOT NULL COMMENT '0 exploration, 1 storyline, 2 rare, 3 elite, 4 travel',
  `objective` INT UNSIGNED NOT NULL COMMENT 'area id, storyline root quest id, creature entry or taxi node id',
  `zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
  `completed_at` BIGINT UNSIGNED NOT NULL,
  `definition_version` INT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'zone definition version when completed',
  `source` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 retroactive, 2 explore, 3 kill, 4 quest, 5 taxi, 6 admin',
  PRIMARY KEY (`guid`, `category`, `objective`),
  KEY `idx_guid_zone` (`guid`, `zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Azeroth Completion objectives';

CREATE TABLE IF NOT EXISTS `azcomp_character_zone` (
  `guid` INT UNSIGNED NOT NULL,
  `zone_id` INT UNSIGNED NOT NULL,
  `completed_at` BIGINT UNSIGNED NOT NULL,
  `definition_version` INT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`guid`, `zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Azeroth Completion: zones completed (never revoked)';

CREATE TABLE IF NOT EXISTS `azcomp_character_milestone` (
  `guid` INT UNSIGNED NOT NULL,
  `zone_id` INT UNSIGNED NOT NULL,
  `percent` TINYINT UNSIGNED NOT NULL,
  `claimed_at` BIGINT UNSIGNED NOT NULL,
  `rewarded` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '0 = reached by retroactive progress, no reward given',
  `definition_version` INT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`guid`, `zone_id`, `percent`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Azeroth Completion: one-time milestone claims';

CREATE TABLE IF NOT EXISTS `azcomp_zone_definition` (
  `zone_id` INT UNSIGNED NOT NULL,
  `version` INT UNSIGNED NOT NULL DEFAULT 1,
  `content_hash` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `objective_count` INT UNSIGNED NOT NULL DEFAULT 0,
  `generated_at` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Azeroth Completion: generated checklist versions';
