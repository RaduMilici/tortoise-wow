# mod-azeroth-completion

Turns every eligible outdoor zone into a completion checklist generated from the server's own
world data, tracks it per character, and explains it to a future addon over a versioned protocol
([PROTOCOL.md](PROTOCOL.md)).

```
WESTFALL (level 9-18)
  Exploration        8 / 16
  Storylines         3 / 9
  Rare Hunts         2 / 6
  Elite Encounters   1 / 4
  Travel             1 / 1
Completion: 43%
```

The server knows what is missing, why, where it is, how each quest chain works and what the
character can do next. The addon is meant to be a presentation layer.

## Categories

| Category | Source | Completed by |
|---|---|---|
| Exploration | `area_template` subzones of the zone, sized and located from the server's `.map` terrain files | the normal exploration bit (`PLAYER_EXPLORED_ZONES`) |
| Storylines | quest graphs from `PrevQuestId`, `NextQuestId`, `NextQuestInChain`, `ExclusiveGroup` | every quest of the chain that this character can legitimately do is rewarded |
| Rare Hunts | rare / rare-elite creatures spawned in the zone | normal kill credit (group members in range count; no killing blow needed) |
| Elite Encounters | named open-world elites, scored (see below) | normal kill credit |
| Travel | taxi nodes of the zone usable by the character's faction | knowing the flight path (existing ones count retroactively) |

Default weights 30 / 30 / 15 / 15 / 10. A category with nothing to do (for this character) is
hidden and its weight is shared out among the others. The zone shows 100% only when every
required objective is done; until then it is capped at 99%.

## How the generator decides

The generator runs at startup (and on `.ac regenerate`) and is deliberately conservative: anything
it cannot show to be meaningful, reachable, available and completable is either dropped or kept as
a **bonus** objective that never counts. Every decision is logged and visible with `.ac inspect`.

**Exploration** — subzones whose discovery the game rewards (area level > 0), present in the
terrain or defined by a building/cave model, at least `MinTerrainCells` big. Dropped: internal or
test names, areas sharing another area's explore bit, areas named like their parent, unused and
inaccessible areas. Each objective gets a centre point (the terrain cell nearest the area's
centroid) for map pins.

**Storylines** — connected quest groups (2-40 quests) filed under the zone. Individual quests are
optional (never required) when they are: disabled, repeatable, seasonal or event-linked,
breadcrumbs (a quest leading to a follow-up that does not need it), gated by a condition,
reputation, profession or hardcore flag, only available while another quest is active,
item-started, or missing a spawned giver or ender — and so is anything that can only be reached
through optional quests. Per character, a quest also stops counting when it can never be done:
other faction, race or class, a mutually exclusive branch already chosen, out-levelled
(`MaxLevel`), or its prerequisites are unobtainable. A one-of exclusive group counts once. A chain
left with nothing reliable becomes a bonus storyline.

**Rare Hunts** — rank rare / rare-elite with a permanent spawn in the zone; several spawns are one
objective. Event-only spawns, creatures not attackable by default, phased creatures: bonus.
Triggers, guards, critters and creatures friendly to both factions: dropped. A rare friendly to
one faction only counts for the other.

**Elite Encounters** — rank elite (or world boss) with at most `MaxSpawns` permanent spawns, not a
service NPC, scoring at least `MinScore`: +2 single spawn, +3 quest target (objective or quest-item
drop or quest giver), +2 unique name, +1 title, +3 world boss, +1 scripted, +1 three or more levels
above the zone. World bosses are bonus unless configured otherwise. Rare elites belong only to Rare
Hunts.

**Travel** — nodes that are part of the flight network, with a flight master for at least one
faction. Enemy-only nodes simply do not apply to that character.

### Overrides

`azcomp_override` (world DB) fixes what the generator gets wrong, without code:

| `mode` | Effect |
|---|---|
| 0 | hint only (`hint` is shown by the addon) |
| 1 | exclude (an exploration override on a zone's own area id removes the whole zone) |
| 2 | force mandatory |
| 3 | bonus |

```sql
INSERT INTO azcomp_override (category, objective, mode, hint, comment) VALUES
('rare', 520, 0, 'Patrols the road south of the Dagger Hills.', ''),
('elite', 448, 3, '', 'Hogger: keep as a bonus, not a requirement');
```

Then `.ac regenerate`.

### Definition versions

Each zone's checklist has a content hash; when a regeneration changes it, the zone's version goes
up (`azcomp_zone_definition`, character DB). Completion records keep the version they were earned
under. A zone that was completed stays completed (`earned`) even if a later version adds objectives;
they appear as new objectives and the addon gets `DEFINITION_UPDATED`. Milestone rewards are never
granted twice.

## Progress

Per character, permanently, in the character DB:

- `azcomp_character_objective` — one row per `(guid, category, objective)` with time, definition
  version and source (retroactive, explore, kill, quest, taxi).
- `azcomp_character_zone` — zones completed.
- `azcomp_character_milestone` — milestone claims, separate from the percentage.

On login the module derives existing progress from explored areas, rewarded quests and known
flight paths (silently, one chat line with the count). Rare and elite kills from before the module
are not invented.

## Rewards

One-time milestones (`AzerothCompletion.Rewards.Milestones`, default 25/50/75/100) pay what
`azcomp_milestone_reward` (world DB) lists. `zone_id = 0` rows apply to every zone; rows for a
specific zone replace them at that percent.

| `reward_type` | `value1` | `value2` | `text` |
|---|---|---|---|
| `XP` | amount | | |
| `XP_PCT` | % of the current level bar | | |
| `MONEY` | copper | | |
| `MONEY_PER_LEVEL` | copper × zone max level | | |
| `REPUTATION` | faction id | amount | |
| `ITEM` | item id | count (mailed if bags are full) | |
| `SPELL` | spell id (cast on the player) | | description |
| `HOOK` | | | hook name (C++: `AzerothCompletion::RegisterRewardHook`, see `src/AzerothCompletion.h`) |

Milestones reached through retroactive progress are claimed without a reward unless
`Rewards.Retroactive = 1`.

## Commands

| Command | Who | |
|---|---|---|
| `.ac` / `.ac zone [zone]` | player | zone overview |
| `.ac missing [zone]` | player | what is missing, with the next quest of each storyline and where it starts |
| `.ac progress` | player | most complete zones |
| `.ac suggest` | player | nearby things to do now |
| `.ac status` | moderator | generator totals and log |
| `.ac inspect <zone>` | moderator | the zone's generated checklist and every exclusion with its reason |
| `.ac inspect-story <id>` | moderator | storyline graph (any of its quest ids works), with the selected player's state |
| `.ac inspect-objective <id>` | moderator | one objective (`rare:520`, `exploration:area_920`, ...) or why it was excluded |
| `.ac regenerate` | admin | rebuild from world data; online players refresh on their next update |
| `.ac reload` | admin | reload config and rewards |
| `.ac reset <character> <zone\|all>` | admin | forget progress; explored areas, quests and flight paths count again without rewards |

Zones can be given by id, name or a unique name prefix (`.ac missing west`).

## Configuration

`conf/mod-azeroth-completion.conf.dist` — copy to `modules/mod-azeroth-completion.conf`.
Chat notifications go to players without the addon by default (`ChatNotify`).

## Core changes this module relies on

Two small patches in this fork, both generic:

- `ChatHandler::FindCommand` prefers a command typed in full over an abbreviation of an earlier
  one; otherwise `.ac` would always open `.account`.
- `WorldSession::HandleTurtleAddonMessages` gives modules every addon message once, whatever the
  channel; before, a player without a guild or group could not reach a module at all.

## Implementation notes

- Terrain areas come from the `.map` files' 16×16 area grid only (512 bytes per tile, read once at
  generation) — no heights, vmaps or mmaps are loaded. Areas defined only inside buildings or caves
  have no centre point.
- Exploration, quests and flight paths are watched by cheap signatures every `SyncIntervalMs`;
  kills come from the kill-credit hook.
- Bots from bot modules are not tracked (`SkipBots`).
