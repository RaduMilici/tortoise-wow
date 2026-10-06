-- Azeroth Completion: regions completed by each character (never revoked, except by ".ac reset <character> all").

CREATE TABLE IF NOT EXISTS `azcomp_character_region` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `region_id` INT UNSIGNED NOT NULL COMMENT 'azcomp_region.id (world DB)',
  `completed_at` BIGINT UNSIGNED NOT NULL,
  `rewarded` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '0 = completed while progress was rebuilt after a reset, no reward given',
  `reward_text` VARCHAR(1000) NULL DEFAULT NULL COMMENT 'description of everything granted',
  `reward_extra` VARCHAR(1000) NULL DEFAULT NULL COMMENT 'the non-item part of reward_text',
  `reward_items` VARCHAR(255) NULL DEFAULT NULL COMMENT 'granted items as id:count,id:count',
  PRIMARY KEY (`guid`, `region_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Azeroth Completion: completed regions';
