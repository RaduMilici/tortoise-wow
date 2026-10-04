#include "AzcCommands.h"
#include "AzcGenerator.h"
#include "AzcProgress.h"
#include "AzcProtocol.h"
#include "AzcRewards.h"
#include "Chat.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "WorldSession.h"
#include <cctype>
#include <sstream>

namespace Azc
{
    namespace
    {
        std::string Trim(char const* args)
        {
            std::string s = args ? args : "";
            s.erase(0, s.find_first_not_of(" \t\""));
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '"'))
                s.pop_back();
            return s;
        }

        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        // Zone by id, exact name, or unique name prefix; empty text = where the player stands.
        ZoneDef const* FindZoneArg(ChatHandler* handler, Definitions const& defs, std::string const& text)
        {
            if (text.empty())
            {
                Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
                ZoneDef const* zone = player ? defs.FindZone(player->GetZoneId()) : nullptr;
                if (!zone)
                    handler->SendSysMessage("This zone has no completion checklist.");
                return zone;
            }
            if (text.find_first_not_of("0123456789") == std::string::npos)
            {
                ZoneDef const* zone = defs.FindZone(uint32(std::strtoul(text.c_str(), nullptr, 10)));
                if (!zone)
                    handler->PSendSysMessage("No checklist for zone %s.", text.c_str());
                return zone;
            }
            std::string wanted = Lower(text);
            ZoneDef const* prefixMatch = nullptr;
            uint32 prefixCount = 0;
            for (auto const& pair : defs.zones)
            {
                std::string name = Lower(pair.second.name);
                if (name == wanted)
                    return &pair.second;
                if (name.compare(0, wanted.size(), wanted) == 0)
                {
                    prefixMatch = &pair.second;
                    ++prefixCount;
                }
            }
            if (prefixCount == 1)
                return prefixMatch;
            handler->PSendSysMessage(prefixCount ? "'%s' matches several zones; be more specific." : "No zone called '%s'.", text.c_str());
            return nullptr;
        }

        DefinitionsPtr RequireDefs(ChatHandler* handler)
        {
            DefinitionsPtr defs = GetDefinitions();
            if (!defs)
                handler->SendSysMessage("Azeroth Completion: the checklist is not generated (module disabled or still starting).");
            return defs;
        }

        Player* Self(ChatHandler* handler)
        {
            return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        }

        // Whose progress an admin command looks at: the selected player, else yourself.
        Player* Subject(ChatHandler* handler)
        {
            Player* self = Self(handler);
            if (!self)
                return nullptr;
            if (Player* target = ObjectAccessor::FindPlayer(self->GetSelectionGuid()))
                return target;
            return self;
        }

        void PrintZone(ChatHandler* handler, Player* player, PlayerState const& state, Definitions const& defs, ZoneDef const& zone)
        {
            ZoneEval ev = EvaluateZone(player, state, defs, zone, false);
            std::string upper = zone.name;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
            handler->PSendSysMessage("|cffffd100%s|r (level %u-%u)", upper.c_str(), zone.levelMin, zone.levelMax);
            for (uint8 c = 0; c < CAT_COUNT; ++c)
            {
                CategoryEval const& ce = ev.cats[c];
                if (!ce.visible)
                    continue;
                std::string bonus = ce.bonusTotal ? "  (+" + std::to_string(ce.bonusDone) + "/" + std::to_string(ce.bonusTotal) + " bonus)" : "";
                handler->PSendSysMessage("  %-18s %u / %u%s", CategoryTitle(Category(c)), ce.done, ce.total, bonus.c_str());
            }
            handler->PSendSysMessage("Completion: |cff33ff99%u%%|r%s", ev.percent,
                ev.earned ? (ev.allDone ? " - completed" : " - completed earlier; new objectives have appeared") : "");
        }

        void PrintMissing(ChatHandler* handler, Player* player, PlayerState const& state, Definitions const& defs, ZoneDef const& zone)
        {
            ZoneEval ev = EvaluateZone(player, state, defs, zone);
            handler->PSendSysMessage("|cffffd100Missing in %s:|r", zone.name.c_str());
            bool any = false;
            for (uint8 c = 0; c < CAT_COUNT; ++c)
            {
                CategoryEval const& ce = ev.cats[c];
                if (!ce.visible || ce.done >= ce.total)
                    continue;
                any = true;
                handler->PSendSysMessage("|cff33ff99%s|r", CategoryTitle(Category(c)));
                for (ObjectiveEval const& obj : ce.objectives)
                {
                    if (obj.done || obj.bonus)
                        continue;
                    std::string name = obj.hidden && GetConfig().hiddenInfo == 0 ? "???" : obj.name;
                    handler->PSendSysMessage("- %s", name.c_str());
                    if (c == CAT_STORYLINE)
                    {
                        StorylineEval const& se = ev.stories[obj.storyIndex];
                        if (QuestEval const* q = se.Find(se.nextQuest))
                            handler->PSendSysMessage("    %u/%u, next: %s", se.unitsDone, se.unitsTotal, QuestLine(*q).c_str());
                    }
                }
            }
            if (!any)
                handler->SendSysMessage("Nothing - this zone is complete.");
        }
    }

    bool HandleAcZone(ChatHandler* handler, char* args)
    {
        Player* player = Self(handler);
        DefinitionsPtr defs = RequireDefs(handler);
        if (!player || !defs)
            return true;
        ZoneDef const* zone = FindZoneArg(handler, *defs, Trim(args));
        if (!zone)
            return true;
        WithState(player, [&](PlayerState* state)
        {
            if (state)
                PrintZone(handler, player, *state, *defs, *zone);
            else
                handler->SendSysMessage("Azeroth Completion is not tracking this character.");
        });
        return true;
    }

    bool HandleAcMissing(ChatHandler* handler, char* args)
    {
        Player* player = Self(handler);
        DefinitionsPtr defs = RequireDefs(handler);
        if (!player || !defs)
            return true;
        ZoneDef const* zone = FindZoneArg(handler, *defs, Trim(args));
        if (!zone)
            return true;
        WithState(player, [&](PlayerState* state)
        {
            if (state)
                PrintMissing(handler, player, *state, *defs, *zone);
        });
        return true;
    }

    bool HandleAcProgress(ChatHandler* handler, char* /*args*/)
    {
        Player* player = Self(handler);
        DefinitionsPtr defs = RequireDefs(handler);
        if (!player || !defs)
            return true;
        WithState(player, [&](PlayerState* state)
        {
            if (!state)
                return;
            std::vector<std::pair<uint32, ZoneDef const*>> started;
            uint32 completed = 0;
            for (auto const& pair : defs->zones)
            {
                ZoneEval ev = EvaluateZone(player, *state, *defs, pair.second, false);
                completed += ev.earned ? 1 : 0;
                if (ev.percent > 0)
                    started.push_back({ ev.percent, &pair.second });
            }
            std::sort(started.begin(), started.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
            handler->PSendSysMessage("|cffffd100Azeroth Completion|r: %u of %u zones complete, %u started.", completed, uint32(defs->zones.size()), uint32(started.size()));
            for (size_t i = 0; i < started.size() && i < 15; ++i)
                handler->PSendSysMessage("  %3u%%  %s", started[i].first, started[i].second->name.c_str());
        });
        return true;
    }

    bool HandleAcSuggest(ChatHandler* handler, char* /*args*/)
    {
        Player* player = Self(handler);
        DefinitionsPtr defs = RequireDefs(handler);
        if (!player || !defs)
            return true;
        ZoneDef const* zone = FindZoneArg(handler, *defs, "");
        if (!zone)
            return true;
        WithState(player, [&](PlayerState* state)
        {
            if (!state)
                return;
            ZoneEval ev = EvaluateZone(player, *state, *defs, *zone);
            std::vector<Suggestion> list = BuildSuggestions(player, *state, *defs, ev);
            handler->SendSysMessage(list.empty() ? "Nothing to suggest here right now." : "|cffffd100Nearby:|r");
            for (Suggestion const& s : list)
            {
                if (s.distance >= 0.0f)
                    handler->PSendSysMessage("- %s  (%u yd)", s.text.c_str(), uint32(s.distance));
                else
                    handler->PSendSysMessage("- %s", s.text.c_str());
            }
        });
        return true;
    }

    bool HandleAcStatus(ChatHandler* handler, char* /*args*/)
    {
        DefinitionsPtr defs = GetDefinitions();
        Settings const& cfg = GetConfig();
        handler->PSendSysMessage("Azeroth Completion %s, protocol %u, %s.", MODULE_VERSION, AZCOMP_PROTOCOL, cfg.enabled ? "enabled" : "disabled");
        if (!defs)
        {
            handler->SendSysMessage("No checklist generated.");
            return true;
        }
        uint32 counts[CAT_COUNT] = {};
        for (auto const& pair : defs->zones)
        {
            counts[CAT_EXPLORATION] += uint32(pair.second.exploration.size());
            counts[CAT_STORYLINE] += uint32(pair.second.storylines.size());
            counts[CAT_RARE] += uint32(pair.second.rares.size());
            counts[CAT_ELITE] += uint32(pair.second.elites.size());
            counts[CAT_TRAVEL] += uint32(pair.second.travel.size());
        }
        handler->PSendSysMessage("Generation %u: %u zones, %u exploration, %u storylines (%u quests), %u rares, %u elites, %u flight paths.",
            defs->generation, uint32(defs->zones.size()), counts[0], counts[1], uint32(defs->quests.size()), counts[2], counts[3], counts[4]);
        for (std::string const& line : defs->globalLog)
            handler->PSendSysMessage("  %s", line.c_str());
        handler->PSendSysMessage("Tracked characters online: %u. Milestone reward rows: %u.", ProgressTrackedPlayers(), RewardRowCount());
        return true;
    }

    bool HandleAcInspect(ChatHandler* handler, char* args)
    {
        DefinitionsPtr defs = RequireDefs(handler);
        if (!defs)
            return true;
        ZoneDef const* zone = FindZoneArg(handler, *defs, Trim(args));
        if (!zone)
            return true;

        handler->PSendSysMessage("|cffffd100%s|r (zone %u, map %u, level %u-%u, definition v%u)", zone->name.c_str(), zone->zoneId, zone->mapId,
            zone->levelMin, zone->levelMax, zone->version);
        auto names = [](auto const& list, auto nameOf)
        {
            std::string out;
            for (auto const& item : list)
                out += (out.empty() ? "" : ", ") + nameOf(item);
            return out;
        };
        handler->PSendSysMessage("%u exploration: %s", uint32(zone->exploration.size()),
            names(zone->exploration, [](ExplorationObjective const& e) { return e.name + (e.bonus ? "*" : ""); }).c_str());
        handler->PSendSysMessage("%u storylines: %s", uint32(zone->storylines.size()),
            names(zone->storylines, [&](uint32 id) { Storyline const* s = defs->FindStoryline(id); return s->title + "[" + std::to_string(id) + "]" + (s->bonus ? "*" : ""); }).c_str());
        handler->PSendSysMessage("%u rares: %s", uint32(zone->rares.size()),
            names(zone->rares, [](CreatureObjective const& c) { return c.name + (c.bonus ? "*" : ""); }).c_str());
        handler->PSendSysMessage("%u elites: %s", uint32(zone->elites.size()),
            names(zone->elites, [](CreatureObjective const& c) { return c.name + "(" + std::to_string(c.importance) + ")" + (c.bonus ? "*" : ""); }).c_str());
        handler->PSendSysMessage("%u travel: %s", uint32(zone->travel.size()),
            names(zone->travel, [](TravelObjective const& t) { return t.name + (t.bonus ? "*" : ""); }).c_str());
        handler->SendSysMessage("(* = bonus, never required)");

        std::map<std::string, std::vector<std::string>> excluded;
        for (Exclusion const& e : zone->excluded)
            excluded[std::string(CategoryKey(e.cat)) + " " + e.reason].push_back(e.name);
        if (!excluded.empty())
            handler->SendSysMessage("Excluded:");
        for (auto const& e : excluded)
        {
            std::string sample;
            for (size_t i = 0; i < e.second.size() && i < 6; ++i)
                sample += (i ? ", " : "") + e.second[i];
            if (e.second.size() > 6)
                sample += ", ...";
            handler->PSendSysMessage("  %u %s: %s", uint32(e.second.size()), e.first.c_str(), sample.c_str());
        }
        return true;
    }

    bool HandleAcInspectStory(ChatHandler* handler, char* args)
    {
        DefinitionsPtr defs = RequireDefs(handler);
        if (!defs)
            return true;
        std::string text = Trim(args);
        Category cat = CAT_STORYLINE;
        uint32 id = 0;
        if (!ParseObjectiveId(text, cat, id))
            id = uint32(std::strtoul(text.c_str(), nullptr, 10));
        Storyline const* story = defs->FindStoryline(id);
        if (!story)
        {
            // also accept any quest id of the storyline
            if (QuestNode const* q = defs->FindQuest(id))
                story = defs->FindStoryline(q->storyline);
        }
        if (!story)
        {
            handler->PSendSysMessage("No storyline %s (give its id or any of its quest ids).", text.c_str());
            return true;
        }

        Player* subject = Subject(handler);
        handler->PSendSysMessage("|cffffd100%s|r [storyline:%u] zone %s, team %s, level %u-%u%s", story->title.c_str(), story->id,
            defs->AreaName(story->zoneId).c_str(), story->teamMask == TEAM_MASK_BOTH ? "both" : story->teamMask == TEAM_MASK_ALLIANCE ? "Alliance" : "Horde",
            story->levelMin, story->levelMax, story->bonus ? (" - bonus: " + story->bonusReason).c_str() : "");

        auto list = [](std::vector<uint32> const& v)
        {
            std::string out;
            for (uint32 x : v)
                out += (out.empty() ? "" : ",") + std::to_string(x);
            return out.empty() ? std::string("-") : out;
        };

        auto print = [&](StorylineEval const* se)
        {
            if (se)
                handler->PSendSysMessage("%s: %u/%u, next %u%s", subject->GetName(), se->unitsDone, se->unitsTotal, se->nextQuest, se->complete ? ", complete" : "");
            for (uint32 qid : story->quests)
            {
                QuestNode const* n = defs->FindQuest(qid);
                if (!n)
                    continue;
                std::string flags;
                if (n->optional) flags += " optional(" + n->optionalReason + ")";
                if (n->exclusiveGroup) flags += " excl" + std::to_string(n->exclusiveGroup);
                std::string st;
                if (se)
                    if (QuestEval const* q = se->Find(qid))
                        st = std::string(" [") + QuestStateName(q->state) + (q->reason.empty() ? "" : " " + q->reason) + (q->counted ? "" : " not-counted") + "]";
                handler->PSendSysMessage("  %u %s (lv %u, min %u) pre %s next %s giver %s%s%s", qid, n->title.c_str(), n->level, n->minLevel,
                    list(n->prev).c_str(), list(n->next).c_str(), n->giver.name.empty() ? "-" : n->giver.name.c_str(), flags.c_str(), st.c_str());
            }
        };

        if (subject)
        {
            WithState(subject, [&](PlayerState* state)
            {
                if (state)
                {
                    StorylineEval se = EvaluateStoryline(subject, *state, *defs, *story);
                    print(&se);
                }
                else
                    print(nullptr);
            });
        }
        else
            print(nullptr);
        return true;
    }

    bool HandleAcInspectObjective(ChatHandler* handler, char* args)
    {
        DefinitionsPtr defs = RequireDefs(handler);
        if (!defs)
            return true;
        std::string text = Trim(args);
        Category cat;
        uint32 key;
        if (!ParseObjectiveId(text, cat, key))
        {
            handler->SendSysMessage("Usage: .ac inspect-objective <exploration:area_20|storyline:65|rare:520|elite:448|travel:4>");
            return true;
        }
        if (cat == CAT_STORYLINE)
            return HandleAcInspectStory(handler, args);

        std::unordered_map<uint32, ObjectiveRef> const& map = cat == CAT_EXPLORATION ? defs->explorationByArea : cat == CAT_RARE ? defs->rareByEntry
            : cat == CAT_ELITE ? defs->eliteByEntry : defs->travelByNode;
        auto itr = map.find(key);
        ZoneDef const* zone = itr == map.end() ? nullptr : defs->FindZone(itr->second.zoneId);
        if (!zone)
        {
            // maybe it was excluded: say why
            for (auto const& pair : defs->zones)
                for (Exclusion const& e : pair.second.excluded)
                    if (e.cat == cat && e.key == key)
                    {
                        handler->PSendSysMessage("%s (%s) was excluded from %s: %s", text.c_str(), e.name.c_str(), pair.second.name.c_str(), e.reason.c_str());
                        return true;
                    }
            handler->PSendSysMessage("%s is not part of any checklist.", text.c_str());
            return true;
        }

        uint32 index = itr->second.index;
        switch (cat)
        {
            case CAT_EXPLORATION:
            {
                ExplorationObjective const& e = zone->exploration[index];
                handler->PSendSysMessage("%s: %s in %s, area level %d, explore flag %u, %u terrain cells%s%s", text.c_str(), e.name.c_str(), zone->name.c_str(),
                    e.areaLevel, e.exploreFlag, e.terrainCells, e.hasCenter ? "" : ", no centre (WMO only)", e.bonus ? ", bonus" : "");
                if (e.hasCenter)
                    handler->PSendSysMessage("  centre: map %u %.1f %.1f", e.center.map, e.center.x, e.center.y);
                break;
            }
            case CAT_RARE:
            case CAT_ELITE:
            {
                CreatureObjective const& c = (cat == CAT_RARE ? zone->rares : zone->elites)[index];
                std::string signals;
                for (std::string const& s : c.signals)
                    signals += (signals.empty() ? "" : ",") + s;
                handler->PSendSysMessage("%s: %s <%s> level %u-%u rank %u in %s, %u spawn(s), respawn %u-%us, score %u (%s)%s", text.c_str(), c.name.c_str(),
                    c.subname.c_str(), c.levelMin, c.levelMax, c.rank, zone->name.c_str(), c.spawnCount, c.respawnMin, c.respawnMax, c.importance,
                    signals.c_str(), c.bonus ? (", bonus: " + c.bonusReason).c_str() : "");
                for (Point const& p : c.spawns)
                    handler->PSendSysMessage("  spawn: map %u %.1f %.1f %.1f", p.map, p.x, p.y, p.z);
                break;
            }
            case CAT_TRAVEL:
            {
                TravelObjective const& t = zone->travel[index];
                handler->PSendSysMessage("%s: %s in %s (%s), map %u %.1f %.1f", text.c_str(), t.name.c_str(), zone->name.c_str(),
                    t.teamMask == TEAM_MASK_BOTH ? "both" : t.teamMask == TEAM_MASK_ALLIANCE ? "Alliance" : "Horde", t.position.map, t.position.x, t.position.y);
                break;
            }
            default:
                break;
        }

        if (Player* subject = Subject(handler))
        {
            WithState(subject, [&](PlayerState* state)
            {
                if (!state)
                    return;
                if (CompletionRecord const* rec = state->Find(cat, key))
                    handler->PSendSysMessage("  %s: completed (v%u, %s) at %s", subject->GetName(), rec->version, SourceName(rec->source),
                        TimeToTimestampStr(rec->at).c_str());
                else
                    handler->PSendSysMessage("  %s: not completed", subject->GetName());
            });
        }
        return true;
    }

    bool HandleAcRegenerate(ChatHandler* handler, char* /*args*/)
    {
        LoadConfig();
        LoadRewards();
        DefinitionsPtr old = GetDefinitions();
        std::shared_ptr<Definitions> fresh = GenerateDefinitions(old.get());
        SetDefinitions(fresh);
        ProgressOnDefinitionsChanged(old, fresh);
        handler->PSendSysMessage("Regenerated: generation %u, %u zones. Earned completions are kept; online players refresh on their next update.",
            fresh->generation, uint32(fresh->zones.size()));
        for (std::string const& line : fresh->globalLog)
            handler->PSendSysMessage("  %s", line.c_str());
        return true;
    }

    bool HandleAcReload(ChatHandler* handler, char* /*args*/)
    {
        LoadConfig();
        LoadRewards();
        handler->PSendSysMessage("Azeroth Completion config and %u reward rows reloaded. Use .ac regenerate to rebuild the checklist.", RewardRowCount());
        return true;
    }

    bool HandleAcReset(ChatHandler* handler, char* args)
    {
        std::istringstream in(Trim(args));
        std::string name, zoneText;
        in >> name;
        std::getline(in, zoneText);
        zoneText = Trim(zoneText.c_str());
        if (name.empty() || zoneText.empty())
        {
            handler->SendSysMessage("Usage: .ac reset <character> <zone name|zone id|all>");
            return true;
        }
        normalizePlayerName(name);
        ObjectGuid guid = sObjectMgr.GetPlayerGuidByName(name);
        if (!guid)
        {
            handler->PSendSysMessage("No character named %s.", name.c_str());
            return true;
        }
        uint32 zoneId = 0;
        std::string zoneName = "every zone";
        if (Lower(zoneText) != "all")
        {
            DefinitionsPtr defs = RequireDefs(handler);
            if (!defs)
                return true;
            ZoneDef const* zone = FindZoneArg(handler, *defs, zoneText);
            if (!zone)
                return true;
            zoneId = zone->zoneId;
            zoneName = zone->name;
        }
        ResetProgress(guid.GetCounter(), zoneId);
        handler->PSendSysMessage("Reset %s's Azeroth Completion progress in %s (kills, storyline records, milestone claims). "
            "Explored areas, finished quests and known flight paths count again automatically, without rewards.", name.c_str(), zoneName.c_str());
        return true;
    }

    std::vector<ChatCommand> GetAcCommands()
    {
        static ChatCommand subCommands[] =
        {
            { "zone",              SEC_PLAYER,        false, nullptr, "Completion of a zone: .ac zone [zone]", nullptr, 0, "", 0, &HandleAcZone },
            { "missing",           SEC_PLAYER,        false, nullptr, "What is missing in a zone: .ac missing [zone]", nullptr, 0, "", 0, &HandleAcMissing },
            { "progress",          SEC_PLAYER,        false, nullptr, "Your most complete zones", nullptr, 0, "", 0, &HandleAcProgress },
            { "suggest",           SEC_PLAYER,        false, nullptr, "Nearby objectives you can do now", nullptr, 0, "", 0, &HandleAcSuggest },
            { "status",            SEC_MODERATOR,     true,  nullptr, "Generator status and totals", nullptr, 0, "", 0, &HandleAcStatus },
            { "inspect",           SEC_MODERATOR,     true,  nullptr, "Generated checklist of a zone, with exclusions: .ac inspect <zone>", nullptr, 0, "", 0, &HandleAcInspect },
            { "inspect-story",     SEC_MODERATOR,     false, nullptr, "Quest graph of a storyline: .ac inspect-story <id>", nullptr, 0, "", 0, &HandleAcInspectStory },
            { "inspect-objective", SEC_MODERATOR,     false, nullptr, "One objective: .ac inspect-objective <rare:520>", nullptr, 0, "", 0, &HandleAcInspectObjective },
            { "regenerate",        SEC_ADMINISTRATOR, true,  nullptr, "Rebuild the checklist from world data", nullptr, 0, "", 0, &HandleAcRegenerate },
            { "reload",            SEC_ADMINISTRATOR, true,  nullptr, "Reload config and milestone rewards", nullptr, 0, "", 0, &HandleAcReload },
            { "reset",             SEC_ADMINISTRATOR, true,  nullptr, "Forget progress: .ac reset <character> <zone|all>", nullptr, 0, "", 0, &HandleAcReset },
            { "",                  SEC_PLAYER,        false, nullptr, "Completion of the zone you are in", nullptr, 0, "", 0, &HandleAcZone },
            { nullptr,             0,                 false, nullptr, "", nullptr, 0, "", 0, nullptr }
        };
        return { { "ac", SEC_PLAYER, true, nullptr, "Azeroth Completion", subCommands, 0, "", 0, nullptr } };
    }
}
