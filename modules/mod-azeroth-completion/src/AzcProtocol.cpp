#include "AzcProtocol.h"
#include "AzcRewards.h"
#include "Chat.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Timer.h"
#include "WorldSession.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace Azc
{
    namespace
    {
        constexpr size_t MAX_RESPONSE_BYTES = 48 * 1024;

        std::string LowerText(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        std::string Fmt(float value)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f", value);
            return buf;
        }

        // Splits on byte boundaries that are not inside a UTF-8 sequence.
        void SendFramed(Player* player, char kind, uint32 id, std::string const& payload)
        {
            size_t chunk = GetConfig().chunkSize;
            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos < payload.size())
            {
                size_t len = std::min(chunk, payload.size() - pos);
                while (len > 1 && pos + len < payload.size() && (static_cast<unsigned char>(payload[pos + len]) & 0xC0) == 0x80)
                    --len;
                parts.push_back(payload.substr(pos, len));
                pos += len;
            }
            if (parts.empty())
                parts.emplace_back();

            std::string head = std::string(1, kind) + std::to_string(id) + " ";
            for (size_t i = 0; i < parts.size(); ++i)
                player->SendAddonMessage(ADDON_PREFIX, head + std::to_string(i + 1) + "/" + std::to_string(parts.size()) + " " + parts[i]);
        }

        void SendResponse(Player* player, uint32 reqId, RecordWriter const& w)
        {
            std::string payload = w.Str();
            if (payload.size() > MAX_RESPONSE_BYTES)
            {
                size_t cut = payload.rfind(';', MAX_RESPONSE_BYTES);
                payload = payload.substr(0, cut == std::string::npos ? MAX_RESPONSE_BYTES : cut) + ";TRUNC";
            }
            SendFramed(player, 'R', reqId, payload);
        }

        void SendError(Player* player, uint32 reqId, char const* code, std::string const& msg)
        {
            RecordWriter w;
            w.Rec("ERR").Kv("code", code).Kv("msg", msg);
            SendResponse(player, reqId, w);
        }

        char const* TeamLetter(uint8 mask)
        {
            return mask == TEAM_MASK_ALLIANCE ? "A" : mask == TEAM_MASK_HORDE ? "H" : "B";
        }

        void WriteLocation(RecordWriter& w, char const* prefix, Point const& p, uint32 areaId, Definitions const& defs)
        {
            std::string pre(prefix);
            w.Kv((pre + "m").c_str(), p.map).Kv((pre + "x").c_str(), Fmt(p.x)).Kv((pre + "y").c_str(), Fmt(p.y));
            if (areaId)
                w.Kv((pre + "a").c_str(), areaId).Kv((pre + "an").c_str(), defs.AreaName(areaId));
        }

        void WriteNpc(RecordWriter& w, char const* prefix, NpcRef const& ref, Definitions const& defs)
        {
            if (ref.kind == STARTER_NONE)
                return;
            std::string pre(prefix);
            static char const* kinds[] = { "none", "npc", "object", "item", "auto" };
            w.Kv((pre + "k").c_str(), kinds[ref.kind]);
            if (ref.entry)
                w.Kv((pre + "id").c_str(), ref.entry);
            w.KvIf((pre + "n").c_str(), ref.name);
            if (ref.hasLocation)
                WriteLocation(w, prefix, ref.location, ref.areaId, defs);
        }

        bool StripHidden(ObjectiveEval const& obj)
        {
            return obj.hidden && GetConfig().hiddenInfo == 0;
        }

        void WriteZone(RecordWriter& w, Player* player, ZoneEval const& ev)
        {
            ZoneDef const& z = *ev.zone;
            w.Rec("ZONE").Kv("id", z.zoneId).Kv("n", z.name).Kv("map", z.mapId).Kv("lmin", z.levelMin).Kv("lmax", z.levelMax)
                .Kv("pct", ev.percent).Kv("done", ev.allDone).Kv("ver", z.version).Kv("cur", player->GetZoneId() == z.zoneId);
            if (ev.earned)
                w.Kv("earned", true).Kv("eat", uint64(ev.earnedAt)).Kv("ever", ev.earnedVersion).Kv("new", ev.newSinceEarned);
        }

        void WriteCategory(RecordWriter& w, ZoneEval const& ev, Category cat)
        {
            CategoryEval const& ce = ev.cats[cat];
            w.Rec("CAT").Kv("c", CategoryKey(cat)).Kv("t", CategoryTitle(cat)).Kv("d", ce.done).Kv("tot", ce.total)
                .Kv("pct", ce.percent).Kv("w", ce.weight).Kv("vis", ce.visible).Kv("bd", ce.bonusDone).Kv("bt", ce.bonusTotal);
        }

        // One objective as a summary line. `type` is OBJ or MISS.
        void WriteObjective(RecordWriter& w, char const* type, Definitions const& defs, ZoneEval const& ev, ObjectiveEval const& obj)
        {
            ZoneDef const& zone = *ev.zone;
            bool strip = StripHidden(obj);
            w.Rec(type).Kv("id", ObjectiveId(obj.cat, obj.key)).Kv("c", CategoryKey(obj.cat)).Kv("n", strip ? std::string("???") : obj.name)
                .Kv("d", obj.done).Kv("z", zone.zoneId);
            if (obj.at)
                w.Kv("at", uint64(obj.at));
            if (obj.bonus)
                w.Kv("b", true);
            if (obj.hidden)
                w.Kv("hid", true);

            switch (obj.cat)
            {
                case CAT_EXPLORATION:
                {
                    ExplorationObjective const& e = zone.exploration[obj.index];
                    w.Kv("a", e.areaId).Kv("p", e.parentAreaId).Kv("lv", e.areaLevel);
                    if (e.hasCenter)
                        WriteLocation(w, "", e.center, 0, defs);
                    w.KvIf("h", e.hint);
                    break;
                }
                case CAT_STORYLINE:
                {
                    StorylineEval const& se = ev.stories[obj.storyIndex];
                    Storyline const& s = *se.story;
                    w.Kv("qd", se.unitsDone).Kv("qt", se.unitsTotal).Kv("lmin", s.levelMin).Kv("lmax", s.levelMax).Kv("f", TeamLetter(s.teamMask));
                    if (!s.bonusReason.empty())
                        w.Kv("br", s.bonusReason);
                    if (QuestEval const* nq = se.Find(se.nextQuest))
                    {
                        w.Kv("nx", nq->node->id).Kv("nxn", nq->node->title).Kv("nxs", QuestStateName(nq->state)).KvIf("why", nq->reason);
                        if (nq->state == QUEST_STATE_ACTIVE)
                            WriteNpc(w, "e_", nq->node->ender, defs);
                        else
                            WriteNpc(w, "g_", nq->node->giver, defs);
                        if (nq->state == QUEST_STATE_BLOCKED)
                            w.Kv("ml", nq->node->minLevel).KvList("mp", nq->missingPrereqs);
                    }
                    break;
                }
                case CAT_RARE:
                case CAT_ELITE:
                {
                    CreatureObjective const& c = (obj.cat == CAT_RARE ? zone.rares : zone.elites)[obj.index];
                    w.Kv("lmin", c.levelMin).Kv("lmax", c.levelMax).Kv("sc", c.spawnCount);
                    if (!strip)
                    {
                        w.Kv("e", c.entry).Kv("r", c.rank == 2 ? "rare_elite" : c.rank == 4 ? "rare" : c.rank == 3 ? "boss" : "elite");
                        if (obj.cat == CAT_ELITE)
                            w.Kv("imp", c.importance);
                        if (!c.spawns.empty())
                            WriteLocation(w, "", c.spawns.front(), c.spawnAreas.empty() ? 0 : c.spawnAreas.front(), defs);
                        w.KvIf("h", c.hint);
                    }
                    else if (!c.spawnAreas.empty())
                        w.Kv("a", c.spawnAreas.front()).Kv("an", defs.AreaName(c.spawnAreas.front()));
                    if (!c.bonusReason.empty())
                        w.Kv("br", c.bonusReason);
                    break;
                }
                case CAT_TRAVEL:
                {
                    TravelObjective const& t = zone.travel[obj.index];
                    w.Kv("node", t.nodeId).Kv("f", TeamLetter(t.teamMask));
                    WriteLocation(w, "", t.position, t.areaId, defs);
                    break;
                }
                default:
                    break;
            }
        }

        void WriteQuest(RecordWriter& w, Definitions const& defs, QuestEval const& q)
        {
            QuestNode const& n = *q.node;
            w.Rec("QUEST").Kv("id", n.id).Kv("n", n.title).Kv("s", n.storyline).Kv("lv", n.level).Kv("ml", n.minLevel)
                .Kv("st", QuestStateName(q.state)).Kv("cnt", q.counted);
            if (n.maxLevel)
                w.Kv("xl", n.maxLevel);
            if (!q.reasons.empty())
            {
                std::string all;
                for (std::string const& r : q.reasons)
                    all += (all.empty() ? "" : ",") + r;
                w.Kv("why", q.reason).Kv("whys", all);
            }
            if (n.optional)
                w.Kv("opt", true).Kv("optr", n.optionalReason);
            if (n.breadcrumb)
                w.Kv("bc", true);
            if (n.repeatable)
                w.Kv("rep", true);
            if (n.exclusiveGroup)
                w.Kv("ex", n.exclusiveGroup);
            if (n.reqClasses)
                w.Kv("cls", n.reqClasses);
            if (n.reqRaces)
                w.Kv("rac", n.reqRaces).Kv("f", TeamLetter(TeamMaskOfQuest(n)));
            if (n.reqSkill)
                w.Kv("sk", n.reqSkill).Kv("skv", n.reqSkillValue);
            w.KvList("pre", n.prev).KvList("pa", n.prevActive).KvList("nx", n.next).KvList("mp", q.missingPrereqs);
            WriteNpc(w, "g_", n.giver, defs);
            WriteNpc(w, "e_", n.ender, defs);
            w.KvIf("sum", n.summary);
            for (std::string const& o : n.objectives)
                w.Rec("QOBJ").Kv("q", n.id).Kv("t", o);
        }

        void WriteStoryline(RecordWriter& w, Definitions const& defs, StorylineEval const& se, bool withQuests)
        {
            Storyline const& s = *se.story;
            w.Rec("STORY").Kv("id", s.id).Kv("oid", ObjectiveId(CAT_STORYLINE, s.id)).Kv("n", s.title).KvIf("sum", s.summary).Kv("z", s.zoneId)
                .Kv("f", TeamLetter(s.teamMask)).Kv("lmin", s.levelMin).Kv("lmax", s.levelMax).Kv("qd", se.unitsDone).Kv("qt", se.unitsTotal)
                .Kv("d", se.complete).Kv("app", se.applicable).Kv("nx", se.nextQuest)
                .KvList("q", s.quests).KvList("roots", s.roots).KvList("term", s.terminals).KvList("brn", s.branches).KvList("blk", se.blocked);
            if (se.completedAt)
                w.Kv("at", uint64(se.completedAt));
            if (s.bonus)
                w.Kv("b", true).Kv("br", s.bonusReason);
            for (auto const& g : s.exclusiveGroups)
                w.Rec("EXG").Kv("s", s.id).Kv("g", g.first).KvList("q", g.second);
            if (withQuests)
                for (QuestEval const& q : se.quests)
                    WriteQuest(w, defs, q);
        }

        ObjectiveEval const* FindObjective(ZoneEval const& ev, Category cat, uint32 key)
        {
            for (ObjectiveEval const& obj : ev.cats[cat].objectives)
                if (obj.key == key)
                    return &obj;
            return nullptr;
        }

        // Which zone an objective lives in.
        ZoneDef const* ZoneOfObjective(Definitions const& defs, Category cat, uint32 key)
        {
            std::unordered_map<uint32, ObjectiveRef> const* map = nullptr;
            switch (cat)
            {
                case CAT_EXPLORATION: map = &defs.explorationByArea; break;
                case CAT_RARE:        map = &defs.rareByEntry; break;
                case CAT_ELITE:       map = &defs.eliteByEntry; break;
                case CAT_TRAVEL:      map = &defs.travelByNode; break;
                case CAT_STORYLINE:
                {
                    Storyline const* s = defs.FindStoryline(key);
                    return s ? defs.FindZone(s->zoneId) : nullptr;
                }
                default: return nullptr;
            }
            auto itr = map->find(key);
            return itr == map->end() ? nullptr : defs.FindZone(itr->second.zoneId);
        }

        ZoneDef const* ResolveZone(Player* player, Definitions const& defs, std::string const& arg)
        {
            uint32 id = arg.empty() ? 0 : uint32(std::strtoul(arg.c_str(), nullptr, 10));
            return defs.FindZone(id ? id : player->GetZoneId());
        }

        void BuildDetail(RecordWriter& w, Player* player, PlayerState const& state, Definitions const& defs, Category cat, uint32 key)
        {
            ZoneDef const* zone = ZoneOfObjective(defs, cat, key);
            if (!zone)
            {
                w.Rec("ERR").Kv("code", "UNKNOWN_OBJECTIVE").Kv("msg", ObjectiveId(cat, key));
                return;
            }
            ZoneEval ev = EvaluateZone(player, state, defs, *zone);
            ObjectiveEval const* obj = FindObjective(ev, cat, key);
            if (!obj)
            {
                w.Rec("ERR").Kv("code", "NOT_APPLICABLE").Kv("msg", "This objective does not apply to this character.");
                return;
            }
            WriteZone(w, player, ev);
            WriteCategory(w, ev, cat);
            WriteObjective(w, "OBJ", defs, ev, *obj);
            if (CompletionRecord const* rec = state.Find(cat, key))
                w.Rec("DONE").Kv("id", ObjectiveId(cat, key)).Kv("at", uint64(rec->at)).Kv("ver", rec->version).Kv("src", SourceName(rec->source));

            bool strip = StripHidden(*obj);
            switch (cat)
            {
                case CAT_STORYLINE:
                    WriteStoryline(w, defs, ev.stories[obj->storyIndex], true);
                    break;
                case CAT_RARE:
                case CAT_ELITE:
                {
                    if (strip)
                        break;
                    CreatureObjective const& c = (cat == CAT_RARE ? zone->rares : zone->elites)[obj->index];
                    w.Rec("CRE").Kv("id", ObjectiveId(cat, key)).Kv("e", c.entry).KvIf("sub", c.subname).Kv("lmin", c.levelMin).Kv("lmax", c.levelMax)
                        .Kv("sc", c.spawnCount).KvList("areas", c.spawnAreas);
                    if (GetConfig().exposeRespawn && c.respawnMax)
                        w.Kv("rmin", c.respawnMin).Kv("rmax", c.respawnMax);
                    if (!c.signals.empty())
                    {
                        std::string sig;
                        for (std::string const& s : c.signals)
                            sig += (sig.empty() ? "" : ",") + s;
                        w.Kv("imp", c.importance).Kv("sig", sig);
                    }
                    for (Point const& p : c.spawns)
                    {
                        w.Rec("SPAWN").Kv("id", ObjectiveId(cat, key));
                        WriteLocation(w, "", p, 0, defs);
                    }
                    for (uint32 a : c.spawnAreas)
                        w.Rec("AREA").Kv("id", ObjectiveId(cat, key)).Kv("a", a).Kv("an", defs.AreaName(a));
                    for (uint32 q : c.relatedQuests)
                        if (Quest const* quest = sObjectMgr.GetQuestTemplate(q))
                            w.Rec("QREF").Kv("id", ObjectiveId(cat, key)).Kv("q", q).Kv("qn", quest->GetTitle())
                                .Kv("st", player->GetQuestRewardStatus(q) ? "done" : player->IsCurrentQuest(q) ? "active" : "none");
                    for (auto const& l : c.loot)
                        if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(l.first))
                            w.Rec("LOOT").Kv("id", ObjectiveId(cat, key)).Kv("item", l.first).Kv("n", proto->Name1).Kv("q", proto->Quality).Kv("ch", Fmt(l.second));
                    break;
                }
                case CAT_EXPLORATION:
                {
                    ExplorationObjective const& e = zone->exploration[obj->index];
                    w.Rec("AREA").Kv("id", ObjectiveId(cat, key)).Kv("a", e.areaId).Kv("an", e.name).Kv("p", e.parentAreaId)
                        .Kv("pn", defs.AreaName(e.parentAreaId)).Kv("cells", e.terrainCells);
                    break;
                }
                default:
                    break;
            }
        }

        void BuildMissing(RecordWriter& w, Player* player, PlayerState const& state, Definitions const& defs, ZoneDef const& zone)
        {
            ZoneEval ev = EvaluateZone(player, state, defs, zone);
            WriteZone(w, player, ev);
            for (uint8 c = 0; c < CAT_COUNT; ++c)
            {
                Category cat = Category(c);
                if (!ev.cats[cat].visible)
                    continue;
                WriteCategory(w, ev, cat);
                for (ObjectiveEval const& obj : ev.cats[cat].objectives)
                {
                    if (obj.done || obj.bonus)
                        continue;
                    WriteObjective(w, "MISS", defs, ev, obj);
                    if (cat == CAT_STORYLINE)
                    {
                        // explain how progression is blocked
                        StorylineEval const& se = ev.stories[obj.storyIndex];
                        for (uint32 b : se.blocked)
                            if (QuestEval const* q = se.Find(b))
                                w.Rec("BLOCK").Kv("s", se.story->id).Kv("q", b).Kv("qn", q->node->title).Kv("why", q->reason)
                                    .Kv("ml", q->node->minLevel).KvList("mp", q->missingPrereqs);
                    }
                }
            }
        }

        bool AllowRequest(PlayerState& state)
        {
            uint32 now = WorldTimer::getMSTime();
            while (!state.requestTimes.empty() && WorldTimer::getMSTimeDiff(state.requestTimes.front(), now) > 10000)
                state.requestTimes.pop_front();
            if (state.requestTimes.size() >= GetConfig().maxRequestsPer10s)
                return false;
            state.requestTimes.push_back(now);
            return true;
        }

        void Dispatch(Player* player, PlayerState& state, uint32 reqId, std::string const& cmd, std::vector<std::string> const& args)
        {
            DefinitionsPtr defsPtr = GetDefinitions();
            Settings const& cfg = GetConfig();
            auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
            RecordWriter w;

            if (cmd == "HELLO" || cmd == "GET_PROTOCOL_VERSION")
            {
                if (cmd == "HELLO")
                {
                    state.addonActive = true;
                    state.addonProtocol = uint32(std::strtoul(arg(0).c_str(), nullptr, 10));
                }
                std::vector<uint32> weights(cfg.weights.begin(), cfg.weights.end());
                w.Rec("PROTO").Kv("v", AZCOMP_PROTOCOL).Kv("min", AZCOMP_PROTOCOL_MIN_CLIENT).Kv("srv", MODULE_VERSION).Kv("chunk", cfg.chunkSize)
                    .Kv("hid", cfg.hiddenInfo).KvList("w", weights).KvList("ms", cfg.milestones).Kv("gen", defsPtr ? defsPtr->generation : 0u);
                if (state.addonProtocol && state.addonProtocol < AZCOMP_PROTOCOL_MIN_CLIENT)
                    w.Rec("ERR").Kv("code", "CLIENT_TOO_OLD").Kv("msg", "Please update the Azeroth Completion addon.");
                SendResponse(player, reqId, w);
                return;
            }

            if (!defsPtr)
            {
                SendError(player, reqId, "NOT_READY", "The checklist is still being generated.");
                return;
            }
            Definitions const& defs = *defsPtr;

            if (cmd == "GET_ZONE_STATE" || cmd == "GET_MISSING" || cmd == "GET_SUGGESTIONS" || cmd == "GET_CATEGORY")
            {
                ZoneDef const* zone = ResolveZone(player, defs, arg(0));
                if (!zone)
                {
                    SendError(player, reqId, "UNKNOWN_ZONE", "No checklist for this zone.");
                    return;
                }
                if (cmd == "GET_MISSING")
                    BuildMissing(w, player, state, defs, *zone);
                else
                {
                    ZoneEval ev = EvaluateZone(player, state, defs, *zone);
                    WriteZone(w, player, ev);
                    if (cmd == "GET_SUGGESTIONS")
                    {
                        for (Suggestion const& s : BuildSuggestions(player, state, defs, ev))
                        {
                            w.Rec("SUG").Kv("k", s.kind).Kv("id", s.id).Kv("t", s.text);
                            if (s.questId)
                                w.Kv("q", s.questId);
                            if (s.hasLocation)
                                w.Kv("m", s.location.map).Kv("x", Fmt(s.location.x)).Kv("y", Fmt(s.location.y));
                            if (s.distance >= 0.0f)
                                w.Kv("dist", uint32(s.distance));
                        }
                    }
                    else if (cmd == "GET_CATEGORY")
                    {
                        Category cat;
                        if (!ParseCategory(arg(1), cat))
                        {
                            SendError(player, reqId, "UNKNOWN_CATEGORY", arg(1));
                            return;
                        }
                        WriteCategory(w, ev, cat);
                        for (ObjectiveEval const& obj : ev.cats[cat].objectives)
                            WriteObjective(w, "OBJ", defs, ev, obj);
                    }
                    else
                    {
                        bool full = arg(1) != "0";
                        for (uint8 c = 0; c < CAT_COUNT; ++c)
                            WriteCategory(w, ev, Category(c));
                        if (full)
                            for (uint8 c = 0; c < CAT_COUNT; ++c)
                                for (ObjectiveEval const& obj : ev.cats[c].objectives)
                                    WriteObjective(w, "OBJ", defs, ev, obj);
                        for (uint32 m : cfg.milestones)
                            w.Rec("MS").Kv("m", m).Kv("got", state.milestones.count({ zone->zoneId, m }) != 0)
                                .KvIf("rw", DescribeMilestoneRewards(player, *zone, m));
                    }
                }
                SendResponse(player, reqId, w);
                return;
            }

            if (cmd == "GET_OBJECTIVE_DETAIL")
            {
                Category cat;
                uint32 key;
                if (!ParseObjectiveId(arg(0), cat, key))
                {
                    SendError(player, reqId, "BAD_OBJECTIVE_ID", arg(0));
                    return;
                }
                BuildDetail(w, player, state, defs, cat, key);
                SendResponse(player, reqId, w);
                return;
            }

            if (cmd == "GET_STORYLINE")
            {
                Category cat = CAT_STORYLINE;
                uint32 key = 0;
                if (!ParseObjectiveId(arg(0), cat, key))
                    key = uint32(std::strtoul(arg(0).c_str(), nullptr, 10));
                Storyline const* story = defs.FindStoryline(key);
                if (!story)
                {
                    SendError(player, reqId, "UNKNOWN_STORYLINE", arg(0));
                    return;
                }
                WriteStoryline(w, defs, EvaluateStoryline(player, state, defs, *story), true);
                SendResponse(player, reqId, w);
                return;
            }

            if (cmd == "GET_CURRENT_PROGRESS")
            {
                bool all = arg(0) == "1";
                uint32 zoneId = player->GetZoneId();
                w.Rec("CUR").Kv("z", zoneId).Kv("zn", defs.AreaName(zoneId)).Kv("a", player->GetAreaId()).Kv("an", defs.AreaName(player->GetAreaId()))
                    .Kv("tracked", defs.FindZone(zoneId) != nullptr);
                for (auto const& pair : defs.zones)
                {
                    ZoneEval ev = EvaluateZone(player, state, defs, pair.second, false);
                    uint32 done = 0, total = 0;
                    for (CategoryEval const& ce : ev.cats) { done += ce.done; total += ce.total; }
                    if (!all && !done && !ev.earned && pair.first != zoneId)
                        continue;
                    w.Rec("ZSUM").Kv("id", pair.first).Kv("n", pair.second.name).Kv("pct", ev.percent).Kv("d", done).Kv("tot", total)
                        .Kv("lmin", pair.second.levelMin).Kv("lmax", pair.second.levelMax).Kv("map", pair.second.mapId).Kv("earned", ev.earned);
                }
                SendResponse(player, reqId, w);
                return;
            }

            if (cmd == "SEARCH")
            {
                std::string query;
                for (std::string const& a : args)
                    query += (query.empty() ? "" : " ") + a;
                query = LowerText(query);
                if (query.size() < 2)
                {
                    SendError(player, reqId, "QUERY_TOO_SHORT", "Type at least two letters.");
                    return;
                }
                uint8 team = PlayerTeamMask(player);
                uint32 hits = 0;
                auto hit = [&](char const* kind, std::string const& id, std::string const& name, uint32 zoneId) -> bool
                {
                    if (hits >= 40 || LowerText(name).find(query) == std::string::npos)
                        return false;
                    ++hits;
                    w.Rec("HIT").Kv("k", kind).Kv("id", id).Kv("n", name).Kv("z", zoneId).Kv("zn", defs.AreaName(zoneId));
                    return true;
                };
                for (auto const& pair : defs.zones)
                {
                    ZoneDef const& zone = pair.second;
                    hit("zone", "zone:" + std::to_string(zone.zoneId), zone.name, zone.zoneId);
                    for (ExplorationObjective const& e : zone.exploration)
                        hit("exploration", ObjectiveId(CAT_EXPLORATION, e.areaId), e.name, zone.zoneId);
                    for (uint32 s : zone.storylines)
                    {
                        Storyline const* story = defs.FindStoryline(s);
                        if (!story || !(story->teamMask & team))
                            continue;
                        hit("storyline", ObjectiveId(CAT_STORYLINE, s), story->title, zone.zoneId);
                        std::set<std::string> seen;     // chains often repeat one title ("... II", "... III" aside)
                        for (uint32 q : story->quests)
                        {
                            QuestNode const* node = defs.FindQuest(q);
                            if (!node || !(TeamMaskOfQuest(*node) & team) || !seen.insert(node->title).second || node->title == story->title)
                                continue;
                            if (hit("quest", ObjectiveId(CAT_STORYLINE, s), node->title, zone.zoneId))
                                w.Kv("q", q);
                        }
                    }
                    // unkilled rares and elites stay secret unless the server reveals them
                    for (Category cat : { CAT_RARE, CAT_ELITE })
                        for (CreatureObjective const& c : cat == CAT_RARE ? zone.rares : zone.elites)
                            if ((c.attackableBy & team) && (cfg.hiddenInfo == 2 || state.Find(cat, c.entry)))
                                hit(cat == CAT_RARE ? "rare" : "elite", ObjectiveId(cat, c.entry), c.name, zone.zoneId);
                    for (TravelObjective const& t : zone.travel)
                        if (t.teamMask & team)
                            hit("travel", ObjectiveId(CAT_TRAVEL, t.nodeId), t.name, zone.zoneId);
                }
                if (hits >= 40)
                    w.Rec("MORE");
                if (!hits)
                    w.Rec("NOHIT").Kv("q", query);
                SendResponse(player, reqId, w);
                return;
            }

            if (cmd == "GET_HISTORY")
            {
                uint32 limit = std::min<uint32>(100, std::max<uint32>(1, uint32(std::strtoul(arg(0).empty() ? "20" : arg(0).c_str(), nullptr, 10))));
                std::vector<std::pair<uint64, CompletionRecord const*>> list;
                for (auto const& r : state.records)
                    list.emplace_back(r.first, &r.second);
                std::sort(list.begin(), list.end(), [](auto const& a, auto const& b) { return a.second->at > b.second->at; });
                for (size_t i = 0; i < list.size() && i < limit; ++i)
                {
                    Category cat = Category(list[i].first >> 32);
                    uint32 key = uint32(list[i].first & 0xFFFFFFFF);
                    std::string name;
                    if (ZoneDef const* zone = ZoneOfObjective(defs, cat, key))
                    {
                        switch (cat)
                        {
                            case CAT_EXPLORATION: name = zone->exploration[defs.explorationByArea.at(key).index].name; break;
                            case CAT_RARE:        name = zone->rares[defs.rareByEntry.at(key).index].name; break;
                            case CAT_ELITE:       name = zone->elites[defs.eliteByEntry.at(key).index].name; break;
                            case CAT_TRAVEL:      name = zone->travel[defs.travelByNode.at(key).index].name; break;
                            case CAT_STORYLINE:   name = defs.FindStoryline(key)->title; break;
                            default: break;
                        }
                    }
                    CompletionRecord const& rec = *list[i].second;
                    w.Rec("HIST").Kv("id", ObjectiveId(cat, key)).Kv("c", CategoryKey(cat)).Kv("n", name).Kv("z", rec.zoneId)
                        .Kv("zn", defs.AreaName(rec.zoneId)).Kv("at", uint64(rec.at)).Kv("src", SourceName(rec.source));
                }
                for (auto const& z : state.zonesEarned)
                    w.Rec("ZDONE").Kv("z", z.first).Kv("zn", defs.AreaName(z.first)).Kv("at", uint64(z.second.first)).Kv("ver", z.second.second);
                SendResponse(player, reqId, w);
                return;
            }

            SendError(player, reqId, "UNKNOWN_REQUEST", cmd);
        }
    }

    std::string Escape(std::string const& text)
    {
        static char const hex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(text.size());
        for (unsigned char c : text)
        {
            // Chat trims trailing spaces, including whole chunks of whitespace.
            if (c == '%' || c == '^' || c == ';' || c == '=' || c == '|' || c <= 0x20)
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 15];
            }
            else
                out += char(c);
        }
        return out;
    }

    RecordWriter& RecordWriter::Rec(char const* type)
    {
        if (!m_out.empty())
            m_out += ';';
        m_out += type;
        return *this;
    }

    RecordWriter& RecordWriter::Kv(char const* key, std::string const& value)
    {
        m_out += '^';
        m_out += key;
        m_out += '=';
        m_out += Escape(value);
        return *this;
    }

    RecordWriter& RecordWriter::Kv(char const* key, float value)
    {
        return Kv(key, Fmt(value));
    }

    RecordWriter& RecordWriter::KvList(char const* key, std::vector<uint32> const& values)
    {
        if (values.empty())
            return *this;
        std::string joined;
        for (uint32 v : values)
            joined += (joined.empty() ? "" : ",") + std::to_string(v);
        return Kv(key, joined);
    }

    std::string QuestLine(QuestEval const& q)
    {
        QuestNode const& n = *q.node;
        std::string line = n.title;
        if (q.state == QUEST_STATE_ACTIVE)
        {
            line += " (in progress)";
            if (!n.ender.name.empty())
                line += " - turn in to " + n.ender.name;
        }
        else if (!n.giver.name.empty())
            line += std::string(n.giver.kind == STARTER_ITEM ? " - starts from item " : " - starts at ") + n.giver.name;
        if (q.state == QUEST_STATE_BLOCKED)
        {
            if (q.reason == "LEVEL_TOO_LOW")
                line += " (requires level " + std::to_string(n.minLevel) + ")";
            else if (q.reason == "MISSING_PREREQUISITE" && !q.missingPrereqs.empty())
            {
                Quest const* pre = sObjectMgr.GetQuestTemplate(q.missingPrereqs.front());
                line += " (first complete " + (pre ? pre->GetTitle() : std::to_string(q.missingPrereqs.front())) + ")";
            }
            else if (!q.reason.empty())
                line += " (" + q.reason + ")";
        }
        return line;
    }

    std::vector<Suggestion> BuildSuggestions(Player* player, PlayerState const& state, Definitions const& defs, ZoneEval const& ev)
    {
        Settings const& cfg = GetConfig();
        ZoneDef const& zone = *ev.zone;
        std::vector<Suggestion> out;
        auto distanceTo = [&](Point const& p)
        {
            if (p.map != player->GetMapId())
                return -1.0f;
            float dx = p.x - player->GetPositionX();
            float dy = p.y - player->GetPositionY();
            return std::sqrt(dx * dx + dy * dy);
        };
        auto add = [&](std::string kind, std::string id, std::string text, bool hasLoc, Point const& loc, uint32 quest = 0)
        {
            Suggestion s;
            s.kind = std::move(kind);
            s.id = std::move(id);
            s.text = std::move(text);
            s.questId = quest;
            s.hasLocation = hasLoc;
            s.location = loc;
            s.distance = hasLoc ? distanceTo(loc) : -1.0f;
            out.push_back(std::move(s));
        };

        for (ObjectiveEval const& obj : ev.cats[CAT_EXPLORATION].objectives)
        {
            if (obj.done || obj.bonus)
                continue;
            ExplorationObjective const& e = zone.exploration[obj.index];
            add("explore", ObjectiveId(CAT_EXPLORATION, obj.key), "Explore " + e.name, e.hasCenter, e.center);
        }
        for (ObjectiveEval const& obj : ev.cats[CAT_STORYLINE].objectives)
        {
            if (obj.done || obj.bonus)
                continue;
            StorylineEval const& se = ev.stories[obj.storyIndex];
            QuestEval const* q = se.Find(se.nextQuest);
            // only what the character can do now: never point at locked or secret content
            if (!q || (q->state != QUEST_STATE_AVAILABLE && q->state != QUEST_STATE_ACTIVE))
                continue;
            NpcRef const& npc = q->state == QUEST_STATE_ACTIVE ? q->node->ender : q->node->giver;
            add("quest", ObjectiveId(CAT_STORYLINE, obj.key), "Quest: " + QuestLine(*q), npc.hasLocation, npc.location, q->node->id);
        }
        // Unkilled rares and elites are secrets unless the server is configured to reveal them.
        uint32 level = player->GetLevel();
        for (Category cat : { CAT_RARE, CAT_ELITE })
        {
            if (cfg.hiddenInfo < 2)
                break;
            for (ObjectiveEval const& obj : ev.cats[cat].objectives)
            {
                if (obj.done || obj.bonus)
                    continue;
                CreatureObjective const& c = (cat == CAT_RARE ? zone.rares : zone.elites)[obj.index];
                if (c.levelMin > level + 3)
                    continue;   // not yet
                std::string where = !c.spawnAreas.empty() ? " (" + defs.AreaName(c.spawnAreas.front()) + ")" : "";
                add(cat == CAT_RARE ? "rare" : "elite", ObjectiveId(cat, obj.key), (cat == CAT_RARE ? "Rare: " : "Elite: ") + c.name + where,
                    !c.spawns.empty(), c.spawns.empty() ? Point() : c.spawns.front());
            }
        }
        for (ObjectiveEval const& obj : ev.cats[CAT_TRAVEL].objectives)
        {
            if (obj.done)
                continue;
            TravelObjective const& t = zone.travel[obj.index];
            add("travel", ObjectiveId(CAT_TRAVEL, obj.key), "Flight path: " + t.name, true, t.position);
        }

        // Nearest first; things without a known spot after everything in range.
        std::stable_sort(out.begin(), out.end(), [&](Suggestion const& a, Suggestion const& b)
        {
            float da = a.distance < 0.0f ? 1e9f : a.distance;
            float db = b.distance < 0.0f ? 1e9f : b.distance;
            return da < db;
        });
        std::vector<Suggestion> result;
        for (Suggestion const& s : out)
        {
            if (result.size() >= cfg.suggestionCount)
                break;
            if (s.distance > cfg.suggestionRange && !result.empty() && s.kind != "quest")
                continue;
            result.push_back(s);
        }
        (void)state;
        return result;
    }

    void DeliverEvents(Player* player, PlayerState& state, std::vector<Event> const& events)
    {
        if (events.empty() || !player->GetSession())
            return;
        Settings const& cfg = GetConfig();
        for (Event const& e : events)
        {
            if (state.addonActive)
            {
                RecordWriter w;
                w.Rec("EV").Kv("t", e.type);
                for (auto const& f : e.fields)
                    w.Kv(f.first.c_str(), f.second);
                SendFramed(player, 'E', ++state.eventSeq, w.Str());
            }
            bool chat = cfg.chatNotify == 2 || (cfg.chatNotify == 1 && !state.addonActive);
            if (e.type == "QUEST_BECAME_AVAILABLE" && cfg.chatNotify != 2)
                chat = false;   // too chatty without the addon to group them
            if (chat && !e.chat.empty())
                ChatHandler(player->GetSession()).SendSysMessage(e.chat.c_str());
        }
    }

    bool HandleAddonMessage(Player* player, std::string const& msg)
    {
        static std::string const prefix = std::string(ADDON_PREFIX) + "\t";
        if (msg.compare(0, prefix.size(), prefix) != 0)
            return false;
        if (!GetConfig().enabled)
            return true;

        std::istringstream in(msg.substr(prefix.size()));
        std::string reqText, cmd;
        in >> reqText >> cmd;
        std::vector<std::string> args;
        for (std::string a; in >> a;)
            args.push_back(a);
        uint32 reqId = uint32(std::strtoul(reqText.c_str(), nullptr, 10)) % 100000;

        WithState(player, [&](PlayerState* state)
        {
            if (!state)
            {
                SendError(player, reqId, "NOT_TRACKED", "Azeroth Completion is not tracking this character.");
                return;
            }
            if (!AllowRequest(*state))
            {
                SendError(player, reqId, "RATE_LIMITED", "Too many requests; slow down.");
                return;
            }
            Dispatch(player, *state, reqId, cmd, args);
        });
        return true;
    }
}
