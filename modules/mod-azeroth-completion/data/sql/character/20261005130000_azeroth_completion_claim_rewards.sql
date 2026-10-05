-- Azeroth Completion: remember what each milestone claim actually granted.
-- NULL for claims made before this update (and for claims that granted nothing): the
-- journal then shows them as plain "Claimed" instead of today's reward list.

ALTER TABLE `azcomp_character_milestone`
  ADD COLUMN IF NOT EXISTS `reward_text` VARCHAR(1000) NULL DEFAULT NULL COMMENT 'description of everything granted',
  ADD COLUMN IF NOT EXISTS `reward_extra` VARCHAR(1000) NULL DEFAULT NULL COMMENT 'the non-item part of reward_text',
  ADD COLUMN IF NOT EXISTS `reward_items` VARCHAR(255) NULL DEFAULT NULL COMMENT 'granted items as id:count,id:count';
