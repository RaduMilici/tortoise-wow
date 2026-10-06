# Lore & Secrets: rewards for how many lore objects (books, plaques, monuments) a character has
# found, and how many of them were secrets. build.py turns this into the world SQL
# (azcomp_lore_reward and the reward items) and the client spells of the companions.
#
# A lore object counts when the character walks up to it. Secrets are the objects far from any
# town (the generator decides, AzerothCompletion.Lore.SecretDistance). Each count pays once per
# character, even across ".ac reset".
#
# Counts may be changed freely; ids are fixed by position, so append new tiers at the end.
#
# pet   = (name, model creature entry, scale, look item entry, flavour)
# title = (title id, name)  ids 123-127 (regions use 120-122; the core caps titles at 127)
# wear  = (name, look item entry)  a tabard copied from the look item

GOLD = 10000


def T(kind, count, money=0, pet=None, title=None, wear=None):
    return dict(kind=kind, count=count, money=money, pet=pet, title=title, wear=wear)


LORE_TIERS = [
    T("lore", 10, money=10 * GOLD,
      pet=("Inkwing Raven", 7605, 0.8, 3406, "Reads over your shoulder. Disapproves of your handwriting.")),
    T("lore", 25, title=(123, "Lorekeeper")),
    T("lore", 50, money=25 * GOLD, wear=("Tabard of the Chronicler", 81289)),
    T("lore", 100, money=50 * GOLD, title=(124, "Keeper of Histories")),
    T("secret", 3, title=(125, "Seeker of Secrets")),
    T("secret", 10, money=25 * GOLD,
      pet=("Curious Mana Wyrm", 61775, 0.5, 12363, "Drawn to forgotten places and the people who find them.")),
]

# Server notices and signposts that are readable but are not lore. Overrides use the object
# entry (azcomp_override category 'lore', mode 1 = exclude).
LORE_EXCLUDE = [
    (1000063, "server notice"),
    (2010955, "Shellcoin information panel"),
    (2010956, "Shellcoin information panel"),
    (2011109, "Shell Co relocation notice"),
]
