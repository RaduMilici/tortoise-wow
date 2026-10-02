-- Thrill of the Kill (61500)
-- Self buff: +1% damage done (all schools) per stack, 10 stacks max, 60 sec (durationIndex 3).
-- attributes   = SPELL_ATTR_CASTABLE_WHILE_MOUNTED
-- attributesEx = SPELL_ATTR_EX_NOT_BREAK_STEALTH | SPELL_ATTR_EX_NO_THREAT
-- attributesEx2 = SPELL_ATTR_EX2_NOT_NEED_SHAPESHIFT
DELETE FROM `spell_template` WHERE `entry` = 61500;
INSERT INTO `spell_template` (
    `entry`, `school`, `attributes`, `attributesEx`, `attributesEx2`,
    `castingTimeIndex`, `procChance`, `durationIndex`, `rangeIndex`, `stackAmount`, `equippedItemClass`,
    `effect1`, `effectDieSides1`, `effectBaseDice1`, `effectBasePoints1`,
    `effectBonusCoefficient1`, `effectBonusCoefficient2`, `effectBonusCoefficient3`,
    `effectImplicitTargetA1`, `effectApplyAuraName1`, `effectMiscValue1`,
    `spellIconId`, `name`, `description`, `auraDescription`,
    `dmgMultiplier1`, `dmgMultiplier2`, `dmgMultiplier3`
) VALUES (
    61500, 0, 16777216, 1056, 524288,
    1, 101, 3, 1, 10, -1,
    6, 1, 1, 0,
    -1, -1, -1,
    1, 79, 127,
    1662, 'Thrill of the Kill', 'The rush of a fresh kill sharpens every blow, increasing damage done by $s1% per stack for $d. Stacks up to $u times.', 'Each stack increases damage done by $s1%.',
    1, 1, 1
);
