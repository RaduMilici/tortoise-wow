-- Azeroth Completion: lore count rewards claimed by each character. Kept across ".ac reset"
-- (found lore cannot be rebuilt from character data), so each count pays once.

CREATE TABLE IF NOT EXISTS `azcomp_character_lore` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `kind` TINYINT UNSIGNED NOT NULL COMMENT '0 lore objects found, 1 secrets found',
  `count` INT UNSIGNED NOT NULL,
  `claimed_at` BIGINT UNSIGNED NOT NULL,
  `rewarded` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `reward_text` VARCHAR(1000) NULL DEFAULT NULL COMMENT 'description of everything granted',
  `reward_extra` VARCHAR(1000) NULL DEFAULT NULL COMMENT 'the non-item part of reward_text',
  `reward_items` VARCHAR(255) NULL DEFAULT NULL COMMENT 'granted items as id:count,id:count',
  PRIMARY KEY (`guid`, `kind`, `count`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Azeroth Completion: lore count claims';
