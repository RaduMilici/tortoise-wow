# Azeroth Completion addon protocol

`AZCOMP_PROTOCOL = 1`

The server is the source of truth. Everything an addon needs to draw the zone overview, category
progress, missing objectives, storyline graphs, rare information, locations, rewards, new
completions and history comes from the messages below; the addon never has to query or reconstruct
world data.

## Transport

Addon messages with the prefix `AZC`.

**Client → server**: `SendAddonMessage("AZC", "<reqId> <COMMAND> [args...]", "GUILD")`.
Any distribution works ("GUILD", "PARTY", "RAID", "BATTLEGROUND"): the server consumes the message
before it is relayed, and a player without a guild or group can still use `"GUILD"`.
`reqId` is a number from 1 to 99999 chosen by the addon; the reply carries it back.

**Server → client**: messages arrive as `CHAT_MSG_ADDON` with prefix `AZC` (distribution `GUILD`,
sender = the player). Body:

```
R<reqId> <seq>/<total> <chunk>      response to a request
E<eventSeq> <seq>/<total> <chunk>   pushed event
```

Concatenate the chunks of one `R<reqId>` (or `E<n>`) in `seq` order; once all `total` chunks have arrived (they may arrive in any order) the
payload is complete. Chunks are at most `chunk` bytes (see `PROTO`), split on UTF-8 boundaries,
and never end on a space (the client trims trailing spaces from chat text).

## Payload

A payload is a list of records separated by `;`. A record is a type followed by `^key=value`
fields:

```
ZONE^id=40^n=Westfall^pct=43;CAT^c=exploration^d=8^tot=13
```

Values are escaped: `%`, `^`, `;`, `=`, `|`, spaces and control characters become `%XX` (hex). Decode
with `string.gsub(v, "%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)` after
splitting. Lists are comma-separated numbers (`pre=65,132`). Booleans are `0`/`1`. Coordinates are
world coordinates (`m` map id, `x`, `y`) with one decimal; convert to map percentages on the client.
Unknown record types and keys must be ignored (the server may add fields without bumping the
protocol version).

Common keys: `id` objective id, `n` name, `d` done, `at` completion time (unix seconds),
`b` bonus (never required), `hid` hidden (see below), `z` zone id.

### Objective ids

```
exploration:area_<areaId>     exploration:area_20
storyline:<rootQuestId>       storyline:65
rare:<creatureEntry>          rare:520
elite:<creatureEntry>         elite:448
travel:<taxiNodeId>           travel:4
```

### Hidden information

`hid=1` on an unkilled rare or elite means "this is a secret": the addon decides whether to draw
`□ ???` or `□ Brack`. When the server runs with `AzerothCompletion.HiddenInfo = 0` it already
replaces the name with `???` and leaves out coordinates and loot; with `2` nothing is hidden.

## Requests

| Command | Args | Reply records |
|---|---|---|
| `HELLO` | `<clientProtocol>` | `PROTO`. Also switches events on for this session. Send it once after login. |
| `GET_PROTOCOL_VERSION` | | `PROTO` |
| `GET_ZONE_STATE` | `[zoneId] [full]` | `ZONE`, 5× `CAT`, `OBJ` for every objective (omit with `full=0`), `MS` per milestone |
| `GET_CATEGORY` | `<zoneId> <category>` | `ZONE`, `CAT`, `OBJ`… |
| `GET_OBJECTIVE_DETAIL` | `<objectiveId>` | `ZONE`, `CAT`, `OBJ`, `DONE`, plus per category: storyline → `STORY`, `EXG`, `QUEST`, `QOBJ`; rare/elite → `CRE`, `SPAWN`, `AREA`, `QREF`, `LOOT`; exploration → `AREA` |
| `GET_MISSING` | `[zoneId]` | `ZONE`, then per visible category `CAT` + `MISS`… (+ `BLOCK` under storylines) |
| `GET_STORYLINE` | `<id or storyline:id>` | `STORY`, `EXG`…, `QUEST`…, `QOBJ`… |
| `GET_CURRENT_PROGRESS` | `[all]` | `CUR`, `ZSUM` per zone (zones with progress only, unless `all=1`) |
| `GET_SUGGESTIONS` | `[zoneId]` | `ZONE`, `SUG`… |
| `GET_HISTORY` | `[limit]` (default 20, max 100) | `HIST`… newest first, `ZDONE`…, `RDONE`… |
| `GET_REGIONS` | | per region in display order: `REGION`, then `RZ` per zone |
| `SEARCH` | `<text>` (2+ letters, may contain spaces) | `HIT`… (max 40, then `MORE`), or `NOHIT` |

`zoneId` 0 or omitted = the zone the character is in. `category` is `exploration`, `storylines`,
`rares`, `elites` or `travel`.

Errors come back as `ERR^code=...^msg=...`. Codes: `NOT_READY`, `NOT_TRACKED`, `RATE_LIMITED`, `QUERY_TOO_SHORT`,
`UNKNOWN_REQUEST`, `UNKNOWN_ZONE`, `UNKNOWN_CATEGORY`, `BAD_OBJECTIVE_ID`, `UNKNOWN_OBJECTIVE`,
`NOT_APPLICABLE`, `UNKNOWN_STORYLINE`, `CLIENT_TOO_OLD`. A reply cut at the size cap ends with a
`TRUNC` record.

## Records

### PROTO
`v` protocol version, `min` oldest supported client protocol, `srv` module version, `chunk` chunk
size, `hid` hidden-info mode (0/1/2), `w` category weights (exploration, storylines, rares, elites,
travel), `ms` milestone percents, `gen` definition generation.

### ZONE
`id`, `n`, `map`, `lmin`/`lmax` level range, `pct` completion (0-100, never 100 unless every
required objective is done), `done` all required objectives done, `ver` definition version,
`cur` the character is here. When the zone was completed at some point: `earned=1`, `eat` when,
`ever` definition version at that time, `new` required objectives added since (an earned
completion is never revoked; `pct` may drop below 100 when later versions add content).

### CAT
`c` key, `t` title, `d` done, `tot` total, `pct`, `w` effective weight in percent (weights of
hidden categories are redistributed), `vis` (0 = nothing to do here, hide it), `bd`/`bt` bonus
done/total.

### OBJ / MISS
Common: `id`, `c`, `n`, `d`, `z`, `at`, `b`, `hid`, `br` (why it is bonus).

- exploration: `a` area id, `p` parent area, `lv` discovery level, `m`/`x`/`y` area centre, `h` hint
- storylines: `qd`/`qt` quests done/total, `lmin`/`lmax`, `f` faction (`A`, `H`, `B`oth), next quest
  `nx` id, `nxn` title, `nxs` state, `why` reason it is blocked; giver of the next quest (or the
  ender when it is in progress) as `g_*` / `e_*` (see NPC fields); when blocked `ml` required level
  and `mp` missing prerequisite quest ids
- rares / elites: `lmin`/`lmax`, `sc` spawn count; unless stripped: `e` entry, `r` rank
  (`rare`, `rare_elite`, `elite`, `boss`), `imp` encounter importance, `m`/`x`/`y`/`a`/`an` first
  spawn and its area, `h` hint
- travel: `node`, `f`, `m`/`x`/`y`, `a`/`an`

### STORY
`id`, `oid` objective id, `n` title, `sum` summary, `z`, `f`, `lmin`/`lmax`, `qd`/`qt`, `d`,
`app` applies to this character, `nx` next quest, `q` all quests in prerequisite order,
`roots`, `term` terminal quests, `brn` quests where the story branches, `blk` blocked quests,
`at`, `b`/`br`.

### EXG
Mutually exclusive branch: `s` storyline, `g` group, `q` member quests. Only one of them can be
done; doing one makes the others `na` with `EXCLUSIVE_BRANCH`, and the group counts once.

### QUEST
`id`, `n`, `s` storyline, `lv`, `ml` minimum level, `xl` maximum level, `st` state, `cnt` counts
towards the storyline for this character, `why` primary reason + `whys` all reasons when not
obtainable, `opt`/`optr` optional and why (breadcrumb, repeatable, seasonal, item_started,
giver_not_spawned, ...), `bc` breadcrumb, `rep` repeatable, `ex` exclusive group, `cls` class mask,
`rac` race mask, `f` faction, `sk`/`skv` profession skill, `pre` prerequisites (any one unlocks it),
`pa` quests that must be in the log at the same time, `nx` follow-ups, `mp` missing prerequisites,
giver `g_*`, ender `e_*`, `sum` summary.

States: `done`, `active` (in the quest log), `available` (can be picked up now), `blocked`
(possible later), `na` (never for this character; not counted).

Reasons: `LEVEL_TOO_LOW`, `LEVEL_TOO_HIGH`, `MISSING_PREREQUISITE`, `PREREQUISITE_ACTIVE`,
`PREREQUISITE_UNOBTAINABLE`, `WRONG_FACTION`, `WRONG_RACE`, `WRONG_CLASS`, `WRONG_PROFESSION`,
`EXCLUSIVE_BRANCH`, `QUEST_DISABLED`, `SEASONAL`, `REPUTATION`, `CONDITION`,
`CHALLENGE_RESTRICTED`, `TIMED_QUEST_ACTIVE`, `UNAVAILABLE`.

### QOBJ
`q` quest id, `t` objective text. One per objective line.

### NPC fields (prefix `g_` giver, `e_` ender)
`k` kind (`npc`, `object`, `item`, `auto`), `id` entry, `n` name, `m`/`x`/`y` spawn, `a`/`an` area.

### CRE / SPAWN / AREA / QREF / LOOT (objective detail)
- `CRE`: `id`, `e`, `sub` title, `lmin`/`lmax`, `sc`, `areas`, `rmin`/`rmax` respawn seconds,
  `imp`/`sig` importance score and signals (elites)
- `SPAWN`: `id`, `m`/`x`/`y` (up to 8)
- `AREA`: `id`, `a`, `an`, (exploration) `p`/`pn` parent, `cells` size in 33-yard terrain cells
- `QREF`: `id`, `q`, `qn`, `st` (`done`, `active`, `none`)
- `LOOT`: `id`, `item`, `n`, `q` quality, `ch` drop chance %

### DONE
`id`, `at`, `ver` definition version when completed, `src` (`retroactive`, `explore`, `kill`,
`quest`, `taxi`, `admin`).

### BLOCK
Why a storyline cannot progress: `s`, `q`, `qn`, `why`, `ml`, `mp`.

### MS
Milestone: `m` percent, `got` claimed, `rw` reward description, `rx` the non-item part of `rw`,
`it` reward items as `id:count,id:count`. An open milestone describes today's rewards; a claimed
one describes what the claim granted, and claims recorded before rewards were kept have none.

### CUR / ZSUM
`CUR`: `z`, `zn`, `a`, `an`, `tracked`. `ZSUM`: `id`, `n`, `pct`, `d`, `tot`, `lmin`, `lmax`,
`map`, `earned`.

### SUG
Suggestion: `k` (`explore`, `quest`, `rare`, `elite`, `travel`), `id`, `t` display text, `q` quest,
`m`/`x`/`y`, `dist` yards. Only content the character can do now; rares and elites only when the
server reveals hidden information.

### HIT
Search result: `k` (`zone`, `exploration`, `storyline`, `quest`, `rare`, `elite`, `travel`), `id`
(objective id; `zone:<id>` for zones; for quests the id of their storyline), `n`, `z`, `zn`, and
`q` quest id for quests. Only content for the character's faction; unkilled rares and elites are
never returned unless the server reveals hidden information.

### HIST / ZDONE / RDONE
`HIST`: `id`, `c`, `n`, `z`, `zn`, `at`, `src`. `ZDONE`: `z`, `zn`, `at`, `ver`. `RDONE`: region
`r`, `rn`, `at`.

### REGION / RZ
A region is a group of zones with a reward for completing all of them.
`REGION`: `id`, `n`, `desc`, `icon` texture path, `d` zones completed, `tot` zones that count,
`done` every zone that counts is complete, `earned` the completion is recorded (never revoked),
`at` when, and the reward as in `MS`: `rw`, `rx`, `it` (what it granted once earned, else what
it would grant).
`RZ`: `r` region, `z`, `zn`, `pct`, `earned`, `app` (0 = nothing in this zone for the character,
so it does not count), `lmin`/`lmax`.

## Events

After `HELLO`, the server pushes `EV` records (frame `E<n>`):

| `t` | Fields |
|---|---|
| `OBJECTIVE_COMPLETED` | `c` (`EXPLORATION`, `STORYLINE`, `RARE`, `ELITE`, `TRAVEL`), `id`, `n`, `cd`/`ct` category progress, `b` |
| `CATEGORY_COMPLETED` | `c`, `cd`, `ct` |
| `MILESTONE_REACHED` | `m` percent, `rw` rewards granted, `it` items granted (`id:count,...`) |
| `ZONE_COMPLETED` | `ver` |
| `STORYLINE_PROGRESS` | `id`, `n`, `qd`, `qt`, `nx`, `nxn`, `nxs`, `nxg` |
| `QUEST_BECAME_AVAILABLE` | `q`, `qn`, `id`/`n` storyline, `g` giver, `ga` giver area |
| `DEFINITION_UPDATED` | `gen`, `zones` = `zoneId:version,...` changed; re-request what is on screen |
| `RETROACTIVE` | `count` objectives recognised from existing character data at login |
| `REGION_COMPLETED` | `r`, `rn` region name, `zc` zones, `rw`/`rx`/`it` rewards granted (no zone fields) |

Every zone event also has `z`, `zn` and `zp` (zone percent after the change). Example:

```
EV^t=OBJECTIVE_COMPLETED^z=40^zn=Westfall^zp=61^c=RARE^cd=3^ct=6^id=rare:520^n=Brack
```

## Limits

- Requests: 30 per 10 seconds per character (configurable); over that, `RATE_LIMITED`.
- Replies are capped at 48 KB. `GET_ZONE_STATE` of a large zone is typically 3-8 KB
  (15-40 chunks); ask for `full=0` and fetch categories on demand if that matters.
