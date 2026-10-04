-- Azeroth Completion: admin overrides and milestone rewards.

CREATE TABLE IF NOT EXISTS `azcomp_override` (
  `category` VARCHAR(16) NOT NULL COMMENT 'exploration, storyline, rare, elite, travel',
  `objective` INT UNSIGNED NOT NULL COMMENT 'area id (a zone''s own id excludes the whole zone), storyline root quest id, creature entry, taxi node id',
  `mode` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 hint only, 1 exclude, 2 force mandatory, 3 bonus (never required)',
  `hint` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'shown to players by the addon',
  `comment` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`category`, `objective`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COMMENT='Azeroth Completion generator overrides';

CREATE TABLE IF NOT EXISTS `azcomp_milestone_reward` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `zone_id` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = every zone; rows for a specific zone replace the generic ones at that percent',
  `percent` TINYINT UNSIGNED NOT NULL COMMENT 'must also be listed in AzerothCompletion.Rewards.Milestones',
  `reward_type` VARCHAR(16) NOT NULL COMMENT 'XP, XP_PCT, MONEY, MONEY_PER_LEVEL, REPUTATION, ITEM, SPELL, HOOK',
  `value1` INT NOT NULL DEFAULT 0 COMMENT 'XP amount | % of level bar | copper | copper x zone max level | faction id | item id | spell id',
  `value2` INT NOT NULL DEFAULT 0 COMMENT 'REPUTATION: amount; ITEM: count',
  `text` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'HOOK: hook name; SPELL: description shown to the player',
  PRIMARY KEY (`id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COMMENT='Azeroth Completion milestone rewards';

-- Default rewards for every zone. Edit freely; ".ac reload" applies changes.
DELETE FROM `azcomp_milestone_reward` WHERE `zone_id` = 0 AND `text` LIKE 'default:%';
INSERT INTO `azcomp_milestone_reward` (`zone_id`, `percent`, `reward_type`, `value1`, `value2`, `text`) VALUES
(0,  25, 'XP_PCT',           5, 0, 'default: 5% of a level'),
(0,  50, 'XP_PCT',          10, 0, 'default: 10% of a level'),
(0,  50, 'MONEY_PER_LEVEL', 20, 0, 'default: 20c per zone level'),
(0,  75, 'XP_PCT',          15, 0, 'default: 15% of a level'),
(0, 100, 'XP_PCT',          25, 0, 'default: 25% of a level'),
(0, 100, 'MONEY_PER_LEVEL', 100, 0, 'default: 1s per zone level');
