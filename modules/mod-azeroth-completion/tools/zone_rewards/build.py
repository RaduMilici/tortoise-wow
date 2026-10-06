#!/usr/bin/env python3
"""Generate the zone and region completion rewards from zones.py and regions.py.

Writes:
  data/sql/world/20261005120000_azeroth_completion_zone_rewards.sql   items, spells, pets, rewards
  data/sql/world/20261006120000_azeroth_completion_regions.sql        regions, their mounts and rewards
  data/client/zone_reward_spells.json    the spells of both for the client's Spell.dbc
                                         (~/Documents/WoW/make_client_patch.py packs them)
  <addon>/ZoneTitles.lua                 title names for the character sheet (--addon DIR)

Usage:
  python3 build.py [--addon PATH_TO/AzerothCompletion]

Every row is copied from an existing template row in the world DB and then changed, so the
SQL stays short and follows the server's own schema. Re-running the SQL replaces everything in
the id ranges below and nothing else.
"""
import argparse
import json
import os
import sys

from regions import REGIONS
from zones import ZONES

HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.dirname(os.path.dirname(HERE))
SQL_OUT = os.path.join(MODULE, "data/sql/world/20261005120000_azeroth_completion_zone_rewards.sql")
REGION_SQL_OUT = os.path.join(MODULE, "data/sql/world/20261006120000_azeroth_completion_regions.sql")
JSON_OUT = os.path.join(MODULE, "data/client/zone_reward_spells.json")

# Id ranges (zone index i = position in ZONES).
ITEM_BASE, ITEMS_PER_ZONE = 93500, 6        # 0 tabard, 1 shirt/cloak, 2 buff, 3 charm, 4 pet, 5 fun
CREATURE_BASE = 94000                       # one pet per zone
SPELL_BASE, SPELLS_PER_ZONE = 64000, 4      # 0 buff, 1 teleport, 2 pet summon, 3 transform
TITLE_BASE = 70                             # titles 70.. (the core's title ids are int8, so <= 127)
REWARD_ID_BASE = 100000                     # azcomp_milestone_reward ids owned by this file
BUFF_GROUP = 64000                          # spell_group: one zone buff at a time
MAX_ZONES = 50

# Regions (region id r): mount item REGION_ITEM_BASE + r, mount spell REGION_SPELL_BASE + r,
# azcomp_region_reward ids REGION_REWARD_BASE + r * 10 + n.
REGION_ITEM_BASE = 93800
REGION_SPELL_BASE = 64200
REGION_REWARD_BASE = 1000
MAX_REGION_ID = 49
REGION_TITLES = range(120, 128)

# Templates the rows are copied from.
T_CONSUMABLE_ITEM = 13452   # Elixir of the Mongoose: a plain usable consumable
T_PET_ITEM = 36500          # Sunfire Fox: Turtle's "add companion to collection" item
T_PET_CREATURE = 36500      # Sunfire Fox companion
T_BUFF_SPELL = 17538        # Elixir of the Mongoose buff
T_TELEPORT_SPELL = 8690     # Hearthstone (10 sec cast, teleport effect)
T_PET_SPELL = 36500         # Sunfire Fox summon (SPELL_EFFECT_SUMMON_CRITTER)
T_FUN_SPELL = 26157         # PX-238 Winter Wondervolt (transform aura)
T_MOUNT_ITEM = 83159        # Grim Totem Kodo: Turtle's "add mount to collection" item
T_MOUNT_SPELL = 50059       # Grim Totem Kodo mount (speed follows the Riding skill)

COUNT_FUN, COUNT_BUFF = 5, 5

# Buff presets: (effects [(aura, value, misc)], icon, tooltip, aura text)
# Values are whole numbers; the spell stores value - 1 with one die of one side.
BUFFS = {
    "health":        ([(133, 5, 0)], 1304, "Increases maximum health by 5%", "Maximum health increased by 5%."),
    "damage":        ([(79, 2, 127)], 1662, "Increases damage done by 2%", "Damage done increased by 2%."),
    "stats":         ([(137, 3, -1)], 332, "Increases all attributes by 3%", "All attributes increased by 3%."),
    "speed":         ([(31, 8, 0)], 47, "Increases run speed by 8%", "Run speed increased by 8%."),
    "crit":          ([(52, 1, 0), (57, 1, 0)], 1609, "Increases your chance to get a critical strike with attacks and spells by 1%", "Critical strike chance increased by 1%."),
    "hit":           ([(54, 1, 0), (55, 1, 0)], 1548, "Increases your chance to hit with attacks and spells by 1%", "Chance to hit increased by 1%."),
    "dodge":         ([(49, 1, 0)], 1613, "Increases your chance to dodge by 1%", "Dodge chance increased by 1%."),
    "armor":         ([(101, 10, 1)], 1301, "Increases armor by 10%", "Armor increased by 10%."),
    "healing":       ([(118, 10, 0)], 1305, "Increases healing received by 10%", "Healing received increased by 10%."),
    "regen":         ([(134, 10, 0)], 338, "Allows 10% of your mana regeneration to continue while casting", "10% of mana regeneration continues while casting."),
    "attack_power":  ([(166, 5, 0), (167, 5, 0)], 456, "Increases attack power and ranged attack power by 5%", "Attack power increased by 5%."),
    "damage_taken":  ([(87, -2, 127)], 685, "Reduces all damage taken by 2%", "Damage taken reduced by 2%."),
    "swim":          ([(82, 1, 0), (58, 40, 0)], 577, "Allows underwater breathing and increases swim speed by 40%", "Water breathing. Swim speed increased by 40%."),
    "resist_fire":   ([(22, 20, 4)], 16, "Increases Fire resistance by 20", "Fire resistance increased by 20."),
    "resist_nature": ([(22, 20, 8)], 198, "Increases Nature resistance by 20", "Nature resistance increased by 20."),
    "resist_frost":  ([(22, 20, 16)], 181, "Increases Frost resistance by 20", "Frost resistance increased by 20."),
    "resist_shadow": ([(22, 20, 32)], 89, "Increases Shadow resistance by 20", "Shadow resistance increased by 20."),
    "resist_arcane": ([(22, 20, 64)], 540, "Increases Arcane resistance by 20", "Arcane resistance increased by 20."),
}
TELEPORT_ICON, FUN_ICON = 1660, 976


def q(text):
    """SQL string literal. Apostrophes are doubled (the DB updater tracks quotes by parity)."""
    if ";" in text:
        sys.exit(f"';' is not allowed in texts (the DB updater splits on it): {text!r}")
    return "'" + text.replace("\\", "\\\\").replace("'", "''") + "'"


def sql_value(v):
    return q(v) if isinstance(v, str) else str(v)


class Zone:
    def __init__(self, index, z):
        self.i, self.z = index, z
        self.area, self.name = z["area"], z["name"]
        self.item = [ITEM_BASE + index * ITEMS_PER_ZONE + k for k in range(ITEMS_PER_ZONE)]
        self.creature = CREATURE_BASE + index
        self.spell = [SPELL_BASE + index * SPELLS_PER_ZONE + k for k in range(SPELLS_PER_ZONE)]
        self.title_id = TITLE_BASE + index


# --- spells -----------------------------------------------------------------------------------

def effect_fields(effects):
    """Columns for up to three effects; unused effects are cleared."""
    f = {}
    for n in range(1, 4):
        aura, value, misc = effects[n - 1] if n <= len(effects) else (0, 1, 0)
        used = n <= len(effects)
        f.update({
            f"effect{n}": 6 if used else 0,
            f"effectDieSides{n}": 1 if used else 0,
            f"effectBaseDice{n}": 1 if used else 0,
            f"effectBasePoints{n}": value - 1 if used else 0,
            f"effectDicePerLevel{n}": 0, f"effectRealPointsPerLevel{n}": 0,
            f"effectMechanic{n}": 0,
            f"effectImplicitTargetA{n}": 1 if used else 0, f"effectImplicitTargetB{n}": 0,
            f"effectRadiusIndex{n}": 0, f"effectApplyAuraName{n}": aura,
            f"effectAmplitude{n}": 0, f"effectMultipleValue{n}": 0, f"effectChainTarget{n}": 0,
            f"effectItemType{n}": 0, f"effectMiscValue{n}": misc, f"effectTriggerSpell{n}": 0,
            f"effectPointsPerComboPoint{n}": 0,
        })
    return f


COMMON_SPELL = {"nameSubtext": "", "category": 0, "recoveryTime": 0, "categoryRecoveryTime": 0,
                "spellFamilyName": 0, "spellFamilyFlags": 0, "maxLevel": 0, "baseLevel": 0,
                "spellLevel": 0, "activeIconId": 0}


def spells_for(zone):
    z = zone.z
    item_name, _, aura_name, preset = z["buff"]
    effects, icon, tooltip, aura_text = BUFFS[preset]
    charm_name, _, _, place = z["charm"]
    pet_name = z["pet"][0]
    fun_name, _, form, form_text = z["fun"]
    out = []

    buff = dict(COMMON_SPELL, entry=zone.spell[0], name=aura_name, spellIconId=icon,
                description=f"{tooltip} for $d. Only one zone feast can be active at a time.",
                auraDescription=aura_text, durationIndex=30, castingTimeIndex=1,
                procFlags=0, procChance=101, stackAmount=0)
    buff.update(effect_fields(effects))
    out.append((T_BUFF_SPELL, buff))

    where = place if zone.name in place else f"{place} in {zone.name}"
    out.append((T_TELEPORT_SPELL, dict(COMMON_SPELL, entry=zone.spell[1], name=charm_name, spellIconId=TELEPORT_ICON,
                description=f"Teleports the caster to {where}.", auraDescription="",
                effectImplicitTargetB1=17)))

    out.append((T_PET_SPELL, dict(COMMON_SPELL, entry=zone.spell[2], name=pet_name,
                description=f"Summons and dismisses {pet_name}, a companion from {zone.name}.",
                auraDescription="", effectMiscValue1=zone.creature)))

    out.append((T_FUN_SPELL, dict(COMMON_SPELL, entry=zone.spell[3], name=fun_name, spellIconId=FUN_ICON,
                description=f"Turns you into {form_text} for $d.", auraDescription=f"Disguised as {form_text}.",
                durationIndex=6, castingTimeIndex=1, effectImplicitTargetA1=1, effectApplyAuraName1=56,
                effectMiscValue1=form)))
    return out


# --- items ------------------------------------------------------------------------------------

def clear_item(spells=True):
    f = {"flags": 0, "buy_count": 1, "buy_price": 0, "sell_price": 0, "allowable_class": -1,
         "allowable_race": -1, "item_level": 1, "required_level": 0, "required_skill": 0,
         "required_skill_rank": 0, "required_spell": 0, "required_honor_rank": 0,
         "required_city_rank": 0, "required_reputation_faction": 0, "required_reputation_rank": 0,
         "bonding": 1, "page_text": 0, "start_quest": 0, "lock_id": 0, "random_property": 0,
         "set_id": 0, "area_bound": 0, "map_bound": 0, "duration": 0, "disenchant_id": 0,
         "extra_flags": 0, "other_team_entry": 0, "script_name": "", "food_type": 0,
         "holy_res": 0, "fire_res": 0, "nature_res": 0, "frost_res": 0, "shadow_res": 0, "arcane_res": 0}
    for n in range(1, 11):
        f[f"stat_type{n}"] = 0
        f[f"stat_value{n}"] = 0
    if spells:
        for n in range(1, 6):
            f.update({f"spellid_{n}": 0, f"spelltrigger_{n}": 0, f"spellcharges_{n}": 0,
                      f"spellppmrate_{n}": 0, f"spellcooldown_{n}": -1, f"spellcategory_{n}": 0,
                      f"spellcategorycooldown_{n}": -1})
    return f


def use_spell(spell):
    return {"spellid_1": spell, "spelltrigger_1": 0, "spellcharges_1": -1, "spellcooldown_1": -1,
            "spellcategory_1": 0, "spellcategorycooldown_1": -1}


def items_for(zone):
    """(template entry, look entry or None, fields) for each of the zone's items."""
    z, n = zone.z, zone.name
    out = []
    tabard_name, tabard_look = z["tabard"]
    out.append((tabard_look, None, dict(clear_item(), entry=zone.item[0], name=tabard_name,
               description=f"Worn by those who know every corner of {n}.", quality=3, max_count=1, stackable=1)))
    wear_name, wear_look = z["wear"]
    out.append((wear_look, None, dict(clear_item(), entry=zone.item[1], name=wear_name,
               description=f"A keepsake from {n}.", quality=2, max_count=1, stackable=1)))
    buff_name, buff_look, _, _ = z["buff"]
    out.append((T_CONSUMABLE_ITEM, buff_look, dict(clear_item(), **use_spell(zone.spell[0]), entry=zone.item[2],
               name=buff_name, description=f"A taste of {n}.", quality=1, max_count=0, stackable=20)))
    charm_name, charm_look, _, _ = z["charm"]
    out.append((T_CONSUMABLE_ITEM, charm_look, dict(clear_item(), **use_spell(zone.spell[1]), entry=zone.item[3],
               name=charm_name, description="Crumbles after one use.", quality=2, max_count=0, stackable=1)))
    pet_name, _, _, pet_look, flavour = z["pet"]
    pet = clear_item(spells=False)
    pet.update(entry=zone.item[4], name=pet_name, description=flavour, quality=3, max_count=1, stackable=1)
    out.append((T_PET_ITEM, pet_look, pet))
    fun_name, fun_look, _, _ = z["fun"]
    out.append((T_CONSUMABLE_ITEM, fun_look, dict(clear_item(), **use_spell(zone.spell[3]), entry=zone.item[5],
               name=fun_name, description=f"A bit of {n} fun. Lasts 10 minutes.", quality=1, max_count=0, stackable=20)))
    return out


# --- output -----------------------------------------------------------------------------------

def copy_row(lines, table, tmp, template, fields, extra=None):
    """Copy a template row into tmp, change it, move it into table."""
    sets = [f"`{k}` = {sql_value(v)}" for k, v in fields.items()]
    if extra:
        sets += extra
    lines.append(f"INSERT INTO `{tmp}` SELECT * FROM `{table}` WHERE `entry` = {template};")
    lines.append(f"UPDATE `{tmp}` SET {', '.join(sets)};")
    lines.append(f"INSERT INTO `{table}` SELECT * FROM `{tmp}`;")
    lines.append(f"DELETE FROM `{tmp}`;")


def build_sql(zones):
    last_item = ITEM_BASE + MAX_ZONES * ITEMS_PER_ZONE - 1
    last_spell = SPELL_BASE + MAX_ZONES * SPELLS_PER_ZONE - 1
    last_creature = CREATURE_BASE + MAX_ZONES - 1
    L = [
        "-- Azeroth Completion: themed rewards for every zone.",
        "-- GENERATED by tools/zone_rewards/build.py from tools/zone_rewards/zones.py. Do not edit by hand.",
        "--",
        f"-- Owns: items {ITEM_BASE}-{last_item}, creatures {CREATURE_BASE}-{last_creature},",
        f"-- spells {SPELL_BASE}-{last_spell} (also in the client patch), spell group {BUFF_GROUP},",
        f"-- azcomp_milestone_reward ids {REWARD_ID_BASE}-{REWARD_ID_BASE + 999}, titles {TITLE_BASE}-{TITLE_BASE + MAX_ZONES - 1}.",
        "--",
        "-- Per zone: 25% fun consumable, 50% teleport charm, 75% zone buff and a shirt or cloak,",
        "-- 100% tabard, companion pet and title. These add to the generic (zone 0) rewards.",
        "",
        f"DELETE FROM `item_template` WHERE `entry` BETWEEN {ITEM_BASE} AND {last_item};",
        f"DELETE FROM `creature_template` WHERE `entry` BETWEEN {CREATURE_BASE} AND {last_creature};",
        f"DELETE FROM `spell_template` WHERE `entry` BETWEEN {SPELL_BASE} AND {last_spell};",
        f"DELETE FROM `spell_target_position` WHERE `id` BETWEEN {SPELL_BASE} AND {last_spell};",
        f"DELETE FROM `collection_pet` WHERE `itemId` BETWEEN {ITEM_BASE} AND {last_item};",
        f"DELETE FROM `spell_group` WHERE `group_id` = {BUFF_GROUP};",
        f"DELETE FROM `spell_group_stack_rules` WHERE `group_id` = {BUFF_GROUP};",
        f"DELETE FROM `azcomp_milestone_reward` WHERE `id` BETWEEN {REWARD_ID_BASE} AND {REWARD_ID_BASE + 999};",
        "",
        "DROP TEMPORARY TABLE IF EXISTS `azc_tmp_item`;",
        "DROP TEMPORARY TABLE IF EXISTS `azc_tmp_spell`;",
        "DROP TEMPORARY TABLE IF EXISTS `azc_tmp_creature`;",
        "CREATE TEMPORARY TABLE `azc_tmp_item` LIKE `item_template`;",
        "CREATE TEMPORARY TABLE `azc_tmp_spell` LIKE `spell_template`;",
        "CREATE TEMPORARY TABLE `azc_tmp_creature` LIKE `creature_template`;",
        "",
        f"INSERT INTO `spell_group_stack_rules` (`group_id`, `stack_rule`) VALUES ({BUFF_GROUP}, 1);",
    ]
    rewards = []
    for zone in zones:
        z = zone.z
        L += ["", f"-- {zone.name} (zone {zone.area}): title {zone.title_id} \"{z['title']}\""]
        for template, look, fields in items_for(zone):
            if look:    # borrow only the icon
                extra = [f"`display_id` = (SELECT `display_id` FROM `item_template` WHERE `entry` = {look})"]
            else:       # a copy of a wearable: keep its look, make it a keepsake rather than gear
                # (some Turtle tabards are stored as class 0; make every copy plain armor)
                extra = ["`class` = 4", "`subclass` = IF(`inventory_type` = 16, 1, 0)",
                         "`armor` = LEAST(`armor`, 15)", "`block` = 0"]
            copy_row(L, "item_template", "azc_tmp_item", template, fields, extra)
        for template, fields in spells_for(zone):
            copy_row(L, "spell_template", "azc_tmp_spell", template, fields)
        pet_name, model, scale, _, _ = z["pet"]
        copy_row(L, "creature_template", "azc_tmp_creature", T_PET_CREATURE,
                 {"entry": zone.creature, "name": pet_name, "subname": "", "scale": scale,
                  "display_id2": 0, "display_id3": 0, "display_id4": 0},
                 [f"`display_id1` = (SELECT `display_id1` FROM `creature_template` WHERE `entry` = {model})"])
        L.append(f"INSERT INTO `collection_pet` (`itemId`, `spellId`) VALUES ({zone.item[4]}, {zone.spell[2]});")
        L.append("INSERT INTO `spell_target_position` (`id`, `target_map`, `target_position_x`, `target_position_y`, "
                 "`target_position_z`, `target_orientation`) SELECT "
                 f"{zone.spell[1]}, `map`, `position_x`, `position_y`, `position_z`, `orientation` "
                 f"FROM `game_tele` WHERE `name` = {q(z['charm'][2])} LIMIT 1;")
        L.append(f"INSERT INTO `spell_group` (`group_id`, `group_spell_id`, `spell_id`) VALUES ({BUFF_GROUP}, {zone.i}, {zone.spell[0]});")
        # Retirement stops new awards, but existing items and learned companions still
        # need their definitions (and client spell/title names) after regeneration.
        if z["retired"]:
            continue
        rid = REWARD_ID_BASE + zone.i * 10
        rows = [(25, "ITEM", zone.item[5], COUNT_FUN, f"zone set: {z['fun'][0]}"),
                (50, "ITEM", zone.item[3], 1, f"zone set: {z['charm'][0]}"),
                (75, "ITEM", zone.item[2], COUNT_BUFF, f"zone set: {z['buff'][0]}"),
                (75, "ITEM", zone.item[1], 1, f"zone set: {z['wear'][0]}"),
                (100, "ITEM", zone.item[0], 1, f"zone set: {z['tabard'][0]}"),
                (100, "ITEM", zone.item[4], 1, f"zone set: {z['pet'][0]}"),
                (100, "TITLE", zone.title_id, 0, z["title"])]
        for n, (pct, kind, v1, v2, text) in enumerate(rows):
            rewards.append(f"({rid + n}, {zone.area}, {pct}, '{kind}', {v1}, {v2}, {q(text)})")
    if rewards:
        L += ["", "INSERT INTO `azcomp_milestone_reward` (`id`, `zone_id`, `percent`, `reward_type`, `value1`, `value2`, `text`) VALUES",
              ",\n".join(rewards) + ";"]
    L += ["",
          "DROP TEMPORARY TABLE `azc_tmp_item`;",
          "DROP TEMPORARY TABLE `azc_tmp_spell`;",
          "DROP TEMPORARY TABLE `azc_tmp_creature`;", ""]
    return "\n".join(L)


# --- regions ----------------------------------------------------------------------------------

REGION_SCHEMA = [
    "CREATE TABLE IF NOT EXISTS `azcomp_region` (",
    "  `id` INT UNSIGNED NOT NULL COMMENT 'stable: character progress refers to it',",
    "  `name` VARCHAR(100) NOT NULL,",
    "  `description` VARCHAR(255) NOT NULL DEFAULT '',",
    "  `scope` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 the zones in azcomp_region_zone, 1 every zone of map_id, 2 every zone',",
    "  `map_id` INT UNSIGNED NOT NULL DEFAULT 0,",
    "  `sort_order` INT NOT NULL DEFAULT 0,",
    "  `icon` VARCHAR(100) NOT NULL DEFAULT '' COMMENT 'texture path shown by the addon',",
    "  PRIMARY KEY (`id`)",
    ") ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COMMENT='Azeroth Completion regions';",
    "",
    "CREATE TABLE IF NOT EXISTS `azcomp_region_zone` (",
    "  `region_id` INT UNSIGNED NOT NULL,",
    "  `zone_id` INT UNSIGNED NOT NULL,",
    "  PRIMARY KEY (`region_id`, `zone_id`)",
    ") ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COMMENT='Azeroth Completion: zones of a region (scope 0)';",
    "",
    "CREATE TABLE IF NOT EXISTS `azcomp_region_reward` (",
    "  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,",
    "  `region_id` INT UNSIGNED NOT NULL,",
    "  `reward_type` VARCHAR(16) NOT NULL COMMENT 'as azcomp_milestone_reward; MONEY_PER_LEVEL uses the highest zone level',",
    "  `value1` INT NOT NULL DEFAULT 0,",
    "  `value2` INT NOT NULL DEFAULT 0,",
    "  `text` VARCHAR(255) NOT NULL DEFAULT '',",
    "  PRIMARY KEY (`id`),",
    "  KEY `idx_region` (`region_id`)",
    ") ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COMMENT='Azeroth Completion: reward for completing a region';",
]
SCOPES = {"zones": 0, "map": 1, "all": 2}


def region_spells(r):
    if not r["mount"]:
        return []
    name, creature, icon, _, _ = r["mount"]
    return [(T_MOUNT_SPELL, dict(COMMON_SPELL, entry=REGION_SPELL_BASE + r["id"], name=name, spellIconId=icon,
             description=f"Summons and dismisses {name}, a reward for completing {r['name']}. "
                         "How fast it runs depends on your Riding skill.",
             auraDescription="Mounted.", effectMiscValue1=creature))]


def build_region_sql():
    ids = [r["id"] for r in REGIONS]
    id_list = ", ".join(str(i) for i in ids)
    last_item = REGION_ITEM_BASE + MAX_REGION_ID
    last_spell = REGION_SPELL_BASE + MAX_REGION_ID
    L = [
        "-- Azeroth Completion: regions (groups of zones) and their completion rewards.",
        "-- GENERATED by tools/zone_rewards/build.py from tools/zone_rewards/regions.py. Do not edit by hand.",
        "--",
        f"-- Owns: regions {id_list}, items {REGION_ITEM_BASE}-{last_item}, spells {REGION_SPELL_BASE}-{last_spell}",
        f"-- (also in the client patch), azcomp_region_reward ids {REGION_REWARD_BASE}-{REGION_REWARD_BASE + MAX_REGION_ID * 10 + 9}.",
        "",
    ] + REGION_SCHEMA + [
        "",
        f"DELETE FROM `azcomp_region` WHERE `id` IN ({id_list});",
        f"DELETE FROM `azcomp_region_zone` WHERE `region_id` IN ({id_list});",
        f"DELETE FROM `azcomp_region_reward` WHERE `id` BETWEEN {REGION_REWARD_BASE} AND {REGION_REWARD_BASE + MAX_REGION_ID * 10 + 9};",
        f"DELETE FROM `item_template` WHERE `entry` BETWEEN {REGION_ITEM_BASE} AND {last_item};",
        f"DELETE FROM `spell_template` WHERE `entry` BETWEEN {REGION_SPELL_BASE} AND {last_spell};",
        f"DELETE FROM `collection_mount` WHERE `itemId` BETWEEN {REGION_ITEM_BASE} AND {last_item};",
        "",
        "DROP TEMPORARY TABLE IF EXISTS `azc_tmp_item`;",
        "DROP TEMPORARY TABLE IF EXISTS `azc_tmp_spell`;",
        "CREATE TEMPORARY TABLE `azc_tmp_item` LIKE `item_template`;",
        "CREATE TEMPORARY TABLE `azc_tmp_spell` LIKE `spell_template`;",
    ]
    regions, zones, rewards = [], [], []
    for order, r in enumerate(REGIONS):
        rid = r["id"]
        regions.append(f"({rid}, {q(r['name'])}, {q(r['description'])}, {SCOPES[r['scope']]}, {r['map_id']}, {order}, {q(r['icon'])})")
        zones += [f"({rid}, {z})" for z in r["zones"]]
        rows = []
        if r["money"]:
            rows.append(("MONEY", r["money"], 0, "region"))
        if r["mount"]:
            name, _, _, look, flavour = r["mount"]
            item = REGION_ITEM_BASE + rid
            L += ["", f"-- {r['name']}: {name}"]
            fields = dict(clear_item(spells=False), entry=item, name=name, description=flavour,
                          quality=4 if r["scope"] != "zones" else 3, max_count=1, stackable=1)
            copy_row(L, "item_template", "azc_tmp_item", T_MOUNT_ITEM, fields,
                     [f"`display_id` = (SELECT `display_id` FROM `item_template` WHERE `entry` = {look})"])
            for template, spell in region_spells(r):
                copy_row(L, "spell_template", "azc_tmp_spell", template, spell)
            L.append(f"INSERT INTO `collection_mount` (`itemId`, `spellId`) VALUES ({item}, {REGION_SPELL_BASE + rid});")
            rows.append(("ITEM", item, 1, "region mount"))
        if r["title"]:
            rows.append(("TITLE", r["title"][0], 0, r["title"][1]))
        for n, (kind, v1, v2, text) in enumerate(rows):
            rewards.append(f"({REGION_REWARD_BASE + rid * 10 + n}, {rid}, '{kind}', {v1}, {v2}, {q(text)})")
    L += ["",
          "INSERT INTO `azcomp_region` (`id`, `name`, `description`, `scope`, `map_id`, `sort_order`, `icon`) VALUES",
          ",\n".join(regions) + ";",
          "INSERT INTO `azcomp_region_zone` (`region_id`, `zone_id`) VALUES",
          ",\n".join(zones) + ";",
          "INSERT INTO `azcomp_region_reward` (`id`, `region_id`, `reward_type`, `value1`, `value2`, `text`) VALUES",
          ",\n".join(rewards) + ";",
          "",
          "DROP TEMPORARY TABLE `azc_tmp_item`;",
          "DROP TEMPORARY TABLE `azc_tmp_spell`;", ""]
    return "\n".join(L)


def check_regions(zones):
    ids, titles = set(), set(z.title_id for z in zones)
    known_areas = set(z.area for z in zones)
    for r in REGIONS:
        if not 1 <= r["id"] <= MAX_REGION_ID or r["id"] in ids:
            sys.exit(f"region {r['name']}: duplicate or out-of-range id {r['id']}")
        ids.add(r["id"])
        if r["scope"] not in SCOPES:
            sys.exit(f"region {r['name']}: unknown scope {r['scope']}")
        if (r["scope"] == "zones") != bool(r["zones"]):
            sys.exit(f"region {r['name']}: scope 'zones' needs a zone list, and only it")
        for z in r["zones"]:
            if z not in known_areas:
                print(f"note: region {r['name']} lists zone {z}, which has no zone set in zones.py")
        if r["title"]:
            if r["title"][0] not in REGION_TITLES or r["title"][0] in titles:
                sys.exit(f"region {r['name']}: title id {r['title'][0]} is taken or outside {REGION_TITLES}")
            titles.add(r["title"][0])


def build_client(zones):
    spells = []
    for zone in zones:
        for template, fields in spells_for(zone):
            spells.append({"template": template, "fields": fields})
    for r in REGIONS:
        for template, fields in region_spells(r):
            spells.append({"template": template, "fields": fields})
    return {"comment": "Generated by mod-azeroth-completion/tools/zone_rewards/build.py. "
                       "make_client_patch.py adds these rows to the client's Spell.dbc.",
            "spells": spells}


def build_titles(zones):
    lines = ["-- Generated by mod-azeroth-completion/tools/zone_rewards/build.py. Do not edit by hand.",
             "-- Names of the zone completion titles. The character sheet's title list and the",
             "-- \"you have earned a title\" message look titles up as PVP_MEDAL<id>.", ""]
    for zone in zones:
        title = zone.z["title"].replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'PVP_MEDAL{zone.title_id} = "{title}"  -- {zone.name}')
    for r in REGIONS:
        if r["title"]:
            title = r["title"][1].replace("\\", "\\\\").replace('"', '\\"')
            lines.append(f'PVP_MEDAL{r["title"][0]} = "{title}"  -- region: {r["name"]}')
    return "\n".join(lines) + "\n"


def check(zones):
    if len(zones) > MAX_ZONES:
        sys.exit(f"{len(zones)} zones; the id ranges hold {MAX_ZONES}")
    seen = {}
    areas, indices = set(), set()
    for zone in zones:
        z = zone.z
        if zone.i < 0 or zone.i >= MAX_ZONES or zone.i in indices:
            sys.exit(f"{zone.name}: duplicate or out-of-range zone index {zone.i}")
        indices.add(zone.i)
        if not 1 <= zone.title_id <= 127:
            sys.exit("title ids must stay within 1-127")
        # A retired set may share its area with the set that replaced it: only one awards.
        if zone.area <= 0 or (not z["retired"] and zone.area in areas):
            sys.exit(f"{zone.name}: duplicate or invalid area id {zone.area}")
        if not z["retired"]:
            areas.add(zone.area)
        if z["buff"][3] not in BUFFS:
            sys.exit(f"{zone.name}: unknown buff preset {z['buff'][3]}")
        for key in ("title",):
            if z[key] in seen:
                sys.exit(f"{zone.name}: {key} {z[key]!r} already used by {seen[z[key]]}")
            seen[z[key]] = zone.name
        names = [z["tabard"][0], z["wear"][0], z["buff"][0], z["charm"][0], z["pet"][0], z["fun"][0]]
        for name in names:
            if len(name) > 64:
                sys.exit(f"{zone.name}: name too long: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--addon", help="AzerothCompletion addon folder to write ZoneTitles.lua into")
    args = parser.parse_args()

    zones = [Zone(i, z) for i, z in enumerate(ZONES)]
    check(zones)
    check_regions(zones)

    os.makedirs(os.path.dirname(JSON_OUT), exist_ok=True)
    with open(SQL_OUT, "w", newline="\n") as f:
        f.write(build_sql(zones))
    with open(REGION_SQL_OUT, "w", newline="\n") as f:
        f.write(build_region_sql())
    with open(JSON_OUT, "w", newline="\n") as f:
        json.dump(build_client(zones), f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"Wrote {SQL_OUT}")
    print(f"Wrote {REGION_SQL_OUT}")
    print(f"Wrote {JSON_OUT}")
    if args.addon:
        path = os.path.join(args.addon, "ZoneTitles.lua")
        with open(path, "w", newline="\n") as f:
            f.write(build_titles(zones))
        print(f"Wrote {path}")
    print(f"{len(zones)} zones, {len(REGIONS)} regions")


if __name__ == "__main__":
    main()
