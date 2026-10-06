# Regions: groups of zones with a reward for completing every one of them. build.py turns this
# into the world SQL (azcomp_region, azcomp_region_zone, azcomp_region_reward) and the client
# spells of the region mounts.
#
# A region counts the zones of its list (scope "zones"), every zone of a map ("map", map id),
# or every zone with a checklist ("all"). Per character only the zones that have something for
# that character count, and a completed region is never taken away again.
#
# id is stable (character progress refers to it): append new regions, never renumber.
#
# mount = (name, mount creature entry, spell icon id, look item entry, flavour)
#         The creature gives the mount its model; the look item its inventory icon.
# title = (title id, name). Ids 120-122 (zone titles use 70-119, lore titles 123-127; the core caps titles at 127).


def R(id, name, description, scope, zones=(), map_id=0, icon="", money=0, mount=None, title=None):
    return dict(id=id, name=name, description=description, scope=scope, zones=list(zones), map_id=map_id,
                icon=icon, money=money, mount=mount, title=title)


GOLD = 10000

REGIONS = [
    R(1, "The Kingdom of Azeroth", "Elwynn's fields to the scorched Blasted Lands.", "zones",
      (12, 40, 44, 10, 33, 41, 8, 4), icon="Interface\\Icons\\INV_BannerPVP_02", money=25 * GOLD,
      mount=("Lion's Pride Charger", 14559, 1176, 18776, "Bred in the stables of Goldshire for those who rode every road of the kingdom.")),
    R(2, "Khaz Modan", "The dwarven mountains, from Dun Morogh to the Burning Steppes.", "zones",
      (1, 38, 11, 51, 3, 46), icon="Interface\\Icons\\INV_Hammer_Unique_Sulfuras", money=25 * GOLD,
      mount=("Khaz Modan Battle Ram", 14745, 1177, 19030, "Bred in the shadow of Ironforge. Stubborn, loyal and very loud.")),
    R(3, "Lordaeron", "The fallen northern kingdom, from Tirisfal to the plague-blighted east.", "zones",
      (85, 130, 267, 36, 45, 47, 28, 139, 5179, 4012), icon="Interface\\Icons\\Spell_Shadow_RaiseDead", money=25 * GOLD,
      mount=("Lordaeron Deathcharger", 80313, 1241, 81245, "It remembers when Lordaeron still stood.")),
    R(4, "Quel'Thalas", "The forests of the high elves.", "zones",
      (5225, 2040), icon="Interface\\Icons\\Spell_Holy_MindVision", money=25 * GOLD,
      mount=("Sunstrider Unicorn", 40045, 1176, 80459, "A gift of the quel'dorei to friends of Alah'Thalas.")),
    R(5, "The Isles", "The islands of the South Seas and beyond.", "zones",
      (408, 409, 5121, 5536, 5024), icon="Interface\\Icons\\INV_Misc_Shell_01", money=25 * GOLD,
      mount=("Islander's Sea Turtle", 17266, 2050, 23720, "Slow and steady. Somehow always gets there first.")),
    R(6, "Northern Kalimdor", "The ancient forests of the night elves, from Teldrassil to Mount Hyjal.", "zones",
      (141, 148, 331, 361, 618, 493, 16, 616), icon="Interface\\Icons\\Ability_Druid_Dreamstate", money=25 * GOLD,
      mount=("Grovewarden Stag", 50097, 2122, 50406, "Walks in the footsteps of Cenarius.")),
    R(7, "The Heart of Kalimdor", "Durotar and Mulgore, the Barrens and the lands around them.", "zones",
      (14, 215, 17, 406, 405, 400, 15), icon="Interface\\Icons\\INV_Misc_Head_Tauren_01", money=25 * GOLD,
      mount=("Crossroads War Kodo", 81102, 1684, 81237, "Has carried caravans across every mile of the Barrens.")),
    R(8, "Southern Kalimdor", "Feralas, Tanaris, Un'Goro and the sands of Silithus.", "zones",
      (357, 440, 490, 1377), icon="Interface\\Icons\\INV_Misc_Desecrated_PlateHelm", money=25 * GOLD,
      mount=("Un'Goro Ravasaur", 6507, 1584, 8586, "Hatched in the crater. Fears nothing, eats anything.")),
    R(9, "Eastern Kingdoms", "Every zone of the Eastern Kingdoms.", "map", map_id=0,
      icon="Interface\\Icons\\INV_Misc_Map_01", money=50 * GOLD, title=(120, "Pathfinder of the Eastern Kingdoms")),
    R(10, "Kalimdor", "Every zone of Kalimdor.", "map", map_id=1,
      icon="Interface\\Icons\\INV_Misc_Map02", money=50 * GOLD, title=(121, "Pathfinder of Kalimdor")),
    R(11, "Azeroth", "Every zone of the world.", "all",
      icon="Interface\\Icons\\INV_Misc_Orb_05", money=100 * GOLD, title=(122, "Master of Azeroth"),
      mount=("Azerothian Champion's Charger", 80160, 1179, 80692, "Ridden only by those who have seen all of Azeroth.")),
]
