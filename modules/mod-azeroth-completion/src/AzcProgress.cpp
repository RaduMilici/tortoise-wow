#include "AzcProgress.h"
#include "AzcProtocol.h"
#include "AzcRewards.h"
#include "AzerothCompletion.h"
#include "Creature.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include <cmath>
#include <mutex>

namespace Azc
{
    namespace
    {
        std::recursive_mutex storeMutex;
        std::unordered_map<uint32, std::unique_ptr<PlayerState>> states;
        std::vector<uint32> lastChangedZones;      // zones whose definition version changed in the last regeneration

        uint64 Mix(uint64 h, uint64 v)
        {
            h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }

        bool IsExplored(Player* player, uint32 flag)
        {
            uint32 offset = flag / 32;
            if (offset >= PLAYER_EXPLORED_ZONES_SIZE)
                return false;
            return (player->GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + offset) & (1u << (flag % 32))) != 0;
        }

        uint64 ExploredSignature(Player* player)
        {
            uint64 h = 0;
            for (uint32 i = 0; i < PLAYER_EXPLORED_ZONES_SIZE; ++i)
                h = Mix(h, player->GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + i));
            return h;
        }

        uint64 TaxiSignature(Player* player, Definitions const& defs)
        {
            uint64 h = 0;
            for (auto const& pair : defs.travelByNode)
                if (player->GetTaxi().IsTaximaskNodeKnown(pair.first))
                    h = Mix(h, pair.first);
            return h;
        }

        uint64 QuestSignature(Player* player)
        {
            uint64 h = 0;
            for (auto const& pair : player->getQuestStatusMap())
                h = Mix(h, (uint64(pair.first) << 8) | (uint64(pair.second.m_status) << 1) | (pair.second.m_rewarded ? 1 : 0));
            return h;
        }

        bool IsTracked(Player* player)
        {
            if (!player || !player->GetSession())
                return false;
            if (GetConfig().skipBots && sScriptMgr.IsBotManaged(player))
                return false;
            return true;
        }

        PlayerState* FindState(Player* player)
        {
            auto itr = states.find(player->GetGUIDLow());
            return itr == states.end() ? nullptr : itr->second.get();
        }

        void Record(PlayerState& state, Category cat, uint32 key, uint32 zoneId, uint32 version, uint8 source)
        {
            CompletionRecord& rec = state.records[MakeObjectiveKey(cat, key)];
            rec.at = time(nullptr);
            rec.version = version;
            rec.source = source;
            rec.zoneId = zoneId;
            CharacterDatabase.PExecute("INSERT IGNORE INTO `azcomp_character_objective` (`guid`, `category`, `objective`, `zone_id`, `completed_at`, `definition_version`, `source`) "
                "VALUES (%u, %u, %u, %u, " UI64FMTD ", %u, %u)", state.guid, uint32(cat), key, zoneId, uint64(rec.at), version, uint32(source));
        }

        std::string ChatPrefix() { return "|cff33ff99[Azeroth Completion]|r "; }

        Event MakeEvent(char const* type, ZoneDef const& zone, ZoneEval const& eval)
        {
            Event e;
            e.type = type;
            e.fields.push_back({ "z", std::to_string(zone.zoneId) });
            e.fields.push_back({ "zn", zone.name });
            e.fields.push_back({ "zp", std::to_string(eval.percent) });
            return e;
        }

        void AddCategory(Event& e, ZoneEval const& eval, Category cat)
        {
            e.fields.push_back({ "c", CategoryEventName(cat) });
            e.fields.push_back({ "cd", std::to_string(eval.cats[cat].done) });
            e.fields.push_back({ "ct", std::to_string(eval.cats[cat].total) });
        }

        std::string DisplayName(ObjectiveEval const& obj, Category cat)
        {
            if (obj.hidden && GetConfig().hiddenInfo == 0 && (cat == CAT_RARE || cat == CAT_ELITE))
                return "???";
            return obj.name;
        }

        Event MakeObjectiveEvent(ZoneDef const& zone, ZoneEval const& eval, Category cat, uint32 key, std::string const& name, bool bonus)
        {
            Event e = MakeEvent("OBJECTIVE_COMPLETED", zone, eval);
            AddCategory(e, eval, cat);
            e.fields.push_back({ "id", ObjectiveId(cat, key) });
            e.fields.push_back({ "n", name });
            if (bonus)
                e.fields.push_back({ "b", "1" });

            char const* verb = "";
            switch (cat)
            {
                case CAT_EXPLORATION: verb = "Explored "; break;
                case CAT_STORYLINE:   verb = "Storyline complete: "; break;
                case CAT_RARE:        verb = "Rare slain: "; break;
                case CAT_ELITE:       verb = "Elite defeated: "; break;
                case CAT_TRAVEL:      verb = "Flight path: "; break;
                default: break;
            }
            e.chat = ChatPrefix() + zone.name + ": " + verb + name + (bonus ? " (bonus)" : "") + " - " + CategoryTitle(cat) + " " +
                std::to_string(eval.cats[cat].done) + "/" + std::to_string(eval.cats[cat].total) + " (" + std::to_string(eval.percent) + "%)";
            return e;
        }

        uint8 DefaultSource(Category cat)
        {
            switch (cat)
            {
                case CAT_EXPLORATION: return SOURCE_EXPLORE;
                case CAT_STORYLINE:   return SOURCE_QUEST;
                case CAT_TRAVEL:      return SOURCE_TAXI;
                default:              return SOURCE_KILL;
            }
        }

        // Records whatever the character has achieved in the zone and raises the events that follow.
        void SyncZone(Player* player, PlayerState& state, Definitions const& defs, ZoneDef const& zone, bool silent, std::vector<Event>& events, bool allowRewards = true)
        {
            Settings const& cfg = GetConfig();
            ZoneEval eval = EvaluateZone(player, state, defs, zone, true);
            bool first = !state.baselined.count(zone.zoneId);
            bool announce = !silent && !first;

            // 1. objectives completed by live character data (exploration, flight paths, quests)
            for (uint8 c = 0; c < CAT_COUNT; ++c)
            {
                Category cat = Category(c);
                for (ObjectiveEval const& obj : eval.cats[cat].objectives)
                {
                    if (!obj.done || state.Find(cat, obj.key))
                        continue;
                    Record(state, cat, obj.key, zone.zoneId, zone.version, silent ? SOURCE_RETRO : DefaultSource(cat));
                    if (!silent)
                        events.push_back(MakeObjectiveEvent(zone, eval, cat, obj.key, obj.name, obj.bonus));
                }
            }

            // 2. storyline progress
            for (StorylineEval const& se : eval.stories)
            {
                uint32 id = se.story->id;
                auto prev = state.storyUnits.find(id);
                if (announce && prev != state.storyUnits.end() && se.unitsDone > prev->second && !se.complete)
                {
                    Event e = MakeEvent("STORYLINE_PROGRESS", zone, eval);
                    e.fields.push_back({ "id", ObjectiveId(CAT_STORYLINE, id) });
                    e.fields.push_back({ "n", se.story->title });
                    e.fields.push_back({ "qd", std::to_string(se.unitsDone) });
                    e.fields.push_back({ "qt", std::to_string(se.unitsTotal) });
                    std::string next;
                    if (QuestEval const* nq = se.Find(se.nextQuest))
                    {
                        e.fields.push_back({ "nx", std::to_string(nq->node->id) });
                        e.fields.push_back({ "nxn", nq->node->title });
                        e.fields.push_back({ "nxs", QuestStateName(nq->state) });
                        if (!nq->node->giver.name.empty())
                            e.fields.push_back({ "nxg", nq->node->giver.name });
                        next = " - next: " + QuestLine(*nq);
                    }
                    e.chat = ChatPrefix() + se.story->title + " " + std::to_string(se.unitsDone) + "/" + std::to_string(se.unitsTotal) + next;
                    events.push_back(e);
                }
                state.storyUnits[id] = se.unitsDone;
            }

            // 3. categories
            for (uint8 c = 0; c < CAT_COUNT; ++c)
            {
                CategoryEval const& ce = eval.cats[c];
                if (!ce.visible || ce.done < ce.total)
                    continue;
                if (state.catComplete.insert({ zone.zoneId, c }).second && announce)
                {
                    Event e = MakeEvent("CATEGORY_COMPLETED", zone, eval);
                    AddCategory(e, eval, Category(c));
                    e.chat = ChatPrefix() + zone.name + ": " + CategoryTitle(Category(c)) + " complete! (" + std::to_string(eval.percent) + "%)";
                    events.push_back(e);
                }
            }

            // 4. milestones (claims are kept apart from the percentage and never repeat)
            for (uint32 m : cfg.milestones)
            {
                if (eval.percent < m || state.milestones.count({ zone.zoneId, m }))
                    continue;
                state.milestones.insert({ zone.zoneId, m });
                bool grant = allowRewards && cfg.rewardsEnabled && (!silent || cfg.rewardsRetroactive);
                std::string reward = grant ? GrantMilestoneRewards(player, zone, m) : std::string();
                CharacterDatabase.PExecute("INSERT IGNORE INTO `azcomp_character_milestone` (`guid`, `zone_id`, `percent`, `claimed_at`, `rewarded`, `definition_version`) "
                    "VALUES (%u, %u, %u, " UI64FMTD ", %u, %u)", state.guid, zone.zoneId, m, uint64(time(nullptr)), grant ? 1u : 0u, zone.version);
                if (!silent || !reward.empty())
                {
                    Event e = MakeEvent("MILESTONE_REACHED", zone, eval);
                    e.fields.push_back({ "m", std::to_string(m) });
                    if (!reward.empty())
                        e.fields.push_back({ "rw", reward });
                    e.chat = ChatPrefix() + zone.name + " " + std::to_string(m) + "% milestone reached!" + (reward.empty() ? "" : " Reward: " + reward);
                    events.push_back(e);
                }
            }

            // 5. zone completion: earned once, kept forever
            if (eval.allDone && !state.zonesEarned.count(zone.zoneId))
            {
                time_t now = time(nullptr);
                state.zonesEarned[zone.zoneId] = { now, zone.version };
                CharacterDatabase.PExecute("INSERT IGNORE INTO `azcomp_character_zone` (`guid`, `zone_id`, `completed_at`, `definition_version`) VALUES (%u, %u, " UI64FMTD ", %u)",
                    state.guid, zone.zoneId, uint64(now), zone.version);
                if (!silent)
                {
                    Event e = MakeEvent("ZONE_COMPLETED", zone, eval);
                    e.fields.push_back({ "ver", std::to_string(zone.version) });
                    e.chat = ChatPrefix() + "|cffffd100" + zone.name + " is 100% complete!|r";
                    events.push_back(e);
                }
            }

            // 6. newly obtainable quests, for the zone the character is in
            if (zone.zoneId == player->GetZoneId())
            {
                std::set<uint32> available;
                for (StorylineEval const& se : eval.stories)
                    for (QuestEval const& q : se.quests)
                        if (q.counted && q.state == QUEST_STATE_AVAILABLE)
                            available.insert(q.node->id);

                // The first look at a zone only records what is open; later looks announce the difference.
                auto known = state.availableQuests.find(zone.zoneId);
                std::set<uint32> previous = known == state.availableQuests.end() ? std::set<uint32>() : known->second;
                if (announce && known != state.availableQuests.end())
                {
                    for (uint32 id : available)
                    {
                        if (previous.count(id))
                            continue;
                        QuestNode const* node = defs.FindQuest(id);
                        Storyline const* story = node ? defs.FindStoryline(node->storyline) : nullptr;
                        if (!node || !story)
                            continue;
                        Event e = MakeEvent("QUEST_BECAME_AVAILABLE", zone, eval);
                        e.fields.push_back({ "q", std::to_string(id) });
                        e.fields.push_back({ "qn", node->title });
                        e.fields.push_back({ "id", ObjectiveId(CAT_STORYLINE, story->id) });
                        e.fields.push_back({ "n", story->title });
                        if (!node->giver.name.empty())
                            e.fields.push_back({ "g", node->giver.name });
                        if (node->giver.areaId)
                            e.fields.push_back({ "ga", defs.AreaName(node->giver.areaId) });
                        QuestEval qe;
                        qe.node = node;
                        qe.state = QUEST_STATE_AVAILABLE;
                        e.chat = ChatPrefix() + "New quest available: " + QuestLine(qe);
                        events.push_back(e);
                    }
                }
                state.availableQuests[zone.zoneId] = available;
            }

            state.baselined.insert(zone.zoneId);
        }

        void SyncAll(Player* player, PlayerState& state, Definitions const& defs, bool silent, std::vector<Event>& events, bool allowRewards = true)
        {
            for (auto const& pair : defs.zones)
                SyncZone(player, state, defs, pair.second, silent, events, allowRewards);
        }

        void FinishReset(PlayerState& state)
        {
            if (!state.resync)
                return;
            // (zone 0, percent 0) is a pending-reset marker, never a real claim.
            CharacterDatabase.PExecute("DELETE FROM `azcomp_character_milestone` WHERE `guid` = %u AND `zone_id` = 0 AND `percent` = 0", state.guid);
            state.resync = false;
        }

        void LoadState(PlayerState& state)
        {
            if (std::unique_ptr<QueryResult> result{ CharacterDatabase.PQuery(
                "SELECT `category`, `objective`, `zone_id`, `completed_at`, `definition_version`, `source` FROM `azcomp_character_objective` WHERE `guid` = %u", state.guid) })
            {
                do
                {
                    Field* f = result->Fetch();
                    if (f[0].GetUInt8() >= CAT_COUNT)
                        continue;
                    CompletionRecord& rec = state.records[MakeObjectiveKey(Category(f[0].GetUInt8()), f[1].GetUInt32())];
                    rec.zoneId = f[2].GetUInt32();
                    rec.at = time_t(f[3].GetUInt64());
                    rec.version = f[4].GetUInt32();
                    rec.source = f[5].GetUInt8();
                } while (result->NextRow());
            }
            if (std::unique_ptr<QueryResult> result{ CharacterDatabase.PQuery(
                "SELECT `zone_id`, `completed_at`, `definition_version` FROM `azcomp_character_zone` WHERE `guid` = %u", state.guid) })
            {
                do
                {
                    Field* f = result->Fetch();
                    state.zonesEarned[f[0].GetUInt32()] = { time_t(f[1].GetUInt64()), f[2].GetUInt32() };
                } while (result->NextRow());
            }
            if (std::unique_ptr<QueryResult> result{ CharacterDatabase.PQuery(
                "SELECT `zone_id`, `percent` FROM `azcomp_character_milestone` WHERE `guid` = %u", state.guid) })
            {
                do
                {
                    Field* f = result->Fetch();
                    if (!f[0].GetUInt32() && !f[1].GetUInt32())
                        state.resync = true;
                    else
                        state.milestones.insert({ f[0].GetUInt32(), f[1].GetUInt32() });
                } while (result->NextRow());
            }
        }
    }

    char const* SourceName(uint8 source)
    {
        switch (source)
        {
            case SOURCE_RETRO:   return "retroactive";
            case SOURCE_EXPLORE: return "explore";
            case SOURCE_KILL:    return "kill";
            case SOURCE_QUEST:   return "quest";
            case SOURCE_TAXI:    return "taxi";
            case SOURCE_ADMIN:   return "admin";
            default:             return "unknown";
        }
    }

    char const* QuestStateName(QuestState state)
    {
        switch (state)
        {
            case QUEST_STATE_DONE:           return "done";
            case QUEST_STATE_ACTIVE:         return "active";
            case QUEST_STATE_AVAILABLE:      return "available";
            case QUEST_STATE_BLOCKED:        return "blocked";
            case QUEST_STATE_NOT_APPLICABLE: return "na";
            default:                         return "unknown";
        }
    }

    uint8 PlayerTeamMask(Player const* player)
    {
        return player->GetTeam() == ALLIANCE ? TEAM_MASK_ALLIANCE : TEAM_MASK_HORDE;
    }

    QuestEval EvaluateQuest(Player* player, QuestNode const& node)
    {
        QuestEval e;
        e.node = &node;
        Quest const* quest = sObjectMgr.GetQuestTemplate(node.id);
        if (!quest)
        {
            e.state = QUEST_STATE_NOT_APPLICABLE;
            e.reason = "QUEST_DISABLED";
            e.reasons.push_back(e.reason);
            return e;
        }
        if (player->GetQuestRewardStatus(node.id))
        {
            e.state = QUEST_STATE_DONE;
            return e;
        }
        if (player->IsCurrentQuest(node.id))
        {
            e.state = QUEST_STATE_ACTIVE;
            return e;
        }

        // Every failed requirement, permanent ones first so the primary reason explains the most.
        bool permanent = false;
        auto fail = [&](char const* reason, bool isPermanent)
        {
            e.reasons.push_back(reason);
            permanent = permanent || isPermanent;
        };
        if (!quest->IsActive())
            fail("QUEST_DISABLED", true);
        if (!player->SatisfyQuestRace(quest, false))
            fail(TeamMaskOfQuest(node) & PlayerTeamMask(player) ? "WRONG_RACE" : "WRONG_FACTION", true);
        if (!player->SatisfyQuestClass(quest, false))
            fail("WRONG_CLASS", true);
        if (!player->SatisfyQuestChallenges(quest, false))
            fail("CHALLENGE_RESTRICTED", true);
        if (!player->SatisfyQuestExclusiveGroup(quest, false))
        {
            // An active alternative can still be abandoned. Only a rewarded
            // alternative permanently removes this branch from completion.
            bool rewarded = false;
            auto bounds = sObjectMgr.GetExclusiveQuestGroupsMapBounds(quest->GetExclusiveGroup());
            for (auto itr = bounds.first; itr != bounds.second; ++itr)
                if (itr->second != node.id && player->GetQuestRewardStatus(itr->second))
                    rewarded = true;
            fail(rewarded ? "EXCLUSIVE_BRANCH" : "EXCLUSIVE_QUEST_ACTIVE", rewarded);
        }
        if (!player->SatisfyQuestNextChain(quest, false))
        {
            bool rewarded = player->GetQuestRewardStatus(quest->GetNextQuestInChain());
            fail(rewarded ? "EXCLUSIVE_BRANCH" : "NEXT_CHAIN_ACTIVE", rewarded);
        }
        if (quest->GetMaxLevel() && quest->GetMaxLevel() < player->GetLevel())
            fail("LEVEL_TOO_HIGH", true);
        if (!player->SatisfyQuestPreviousQuest(quest, false))
            fail("MISSING_PREREQUISITE", false);
        if (!player->SatisfyQuestPrevChain(quest, false))
            fail("PREREQUISITE_ACTIVE", false);
        if (!player->SatisfyQuestLevel(quest, false))
            fail("LEVEL_TOO_LOW", false);
        if (!player->SatisfyQuestSkill(quest, false))
            fail("WRONG_PROFESSION", false);
        if (!player->SatisfyQuestReputation(quest, false))
            fail("REPUTATION", false);
        if (!player->SatisfyQuestCondition(quest, false))
            fail("CONDITION", false);
        if (!player->SatisfyQuestTimed(quest, false))
            fail("TIMED_QUEST_ACTIVE", false);

        for (int32 p : quest->prevQuests)
            if (p > 0 && !player->GetQuestRewardStatus(uint32(p)))
                e.missingPrereqs.push_back(uint32(p));

        if (e.reasons.empty())
        {
            if (player->CanTakeQuest(quest, false))
                e.state = QUEST_STATE_AVAILABLE;
            else
            {
                e.state = QUEST_STATE_BLOCKED;
                e.reasons.push_back(node.seasonal ? "SEASONAL" : "UNAVAILABLE");
            }
        }
        else
            e.state = permanent ? QUEST_STATE_NOT_APPLICABLE : QUEST_STATE_BLOCKED;

        if (!e.reasons.empty())
            e.reason = e.reasons.front();
        return e;
    }

    StorylineEval EvaluateStoryline(Player* player, PlayerState const& state, Definitions const& defs, Storyline const& story)
    {
        StorylineEval se;
        se.story = &story;
        std::unordered_map<uint32, size_t> index;
        for (uint32 id : story.quests)
        {
            QuestNode const* node = defs.FindQuest(id);
            if (!node)
                continue;
            index[id] = se.quests.size();
            se.quests.push_back(EvaluateQuest(player, *node));
        }

        // Reachability in prerequisite order: a quest gated behind something this character can
        // never do is not applicable, so it can never hold the storyline (or zone) at 99%.
        std::unordered_map<uint32, bool> reachable;
        for (QuestEval& q : se.quests)
        {
            bool ok;
            if (q.state == QUEST_STATE_DONE || q.state == QUEST_STATE_ACTIVE || q.state == QUEST_STATE_AVAILABLE)
                ok = true;
            else if (q.state == QUEST_STATE_NOT_APPLICABLE)
                ok = false;
            else if (q.node->prev.empty())
                ok = true;
            else
            {
                ok = false;
                for (uint32 p : q.node->prev)
                {
                    auto itr = index.find(p);
                    if (itr == index.end())
                    {
                        ok = true;      // prerequisite outside the storyline: the core decides, trust it
                        break;
                    }
                    QuestEval const& pe = se.quests[itr->second];
                    if (pe.state == QUEST_STATE_DONE || reachable[p])
                    {
                        ok = true;
                        break;
                    }
                }
                if (!ok)
                {
                    q.state = QUEST_STATE_NOT_APPLICABLE;
                    q.reasons.insert(q.reasons.begin(), "PREREQUISITE_UNOBTAINABLE");
                    q.reason = q.reasons.front();
                }
            }
            reachable[q.node->id] = ok;
            // A bonus storyline never counts towards the zone, so its own progress may include
            // the optional quests it is made of.
            q.counted = (!q.node->optional || story.bonus) && ok;
        }

        std::map<int32, std::pair<bool, bool>> groups;     // exclusive group -> (counted, done)
        for (QuestEval const& q : se.quests)
        {
            if (!q.counted)
                continue;
            bool done = q.state == QUEST_STATE_DONE;
            if (q.node->exclusiveGroup > 0)
            {
                auto& g = groups[q.node->exclusiveGroup];
                g.first = true;
                g.second = g.second || done;
            }
            else
            {
                ++se.unitsTotal;
                se.unitsDone += done ? 1 : 0;
            }
        }
        for (auto const& g : groups)
        {
            ++se.unitsTotal;
            se.unitsDone += g.second.second ? 1 : 0;
        }
        se.questsDone = se.unitsDone;
        se.questsTotal = se.unitsTotal;

        CompletionRecord const* rec = state.Find(CAT_STORYLINE, story.id);
        se.applicable = se.unitsTotal > 0 || rec;
        se.complete = rec || (se.unitsTotal > 0 && se.unitsDone == se.unitsTotal);
        se.completedAt = rec ? rec->at : 0;

        // Next: what the character is on, else what they can pick up, else what is in the way.
        for (QuestState wanted : { QUEST_STATE_ACTIVE, QUEST_STATE_AVAILABLE, QUEST_STATE_BLOCKED })
        {
            for (QuestEval const& q : se.quests)
            {
                if (!q.counted || q.state != wanted)
                    continue;
                // in a one-of group already decided, the others no longer matter
                if (q.node->exclusiveGroup > 0 && groups[q.node->exclusiveGroup].second)
                    continue;
                se.nextQuest = q.node->id;
                break;
            }
            if (se.nextQuest)
                break;
        }
        for (QuestEval const& q : se.quests)
            if (q.counted && q.state == QUEST_STATE_BLOCKED)
                se.blocked.push_back(q.node->id);
        return se;
    }

    ZoneEval EvaluateZone(Player* player, PlayerState const& state, Definitions const& defs, ZoneDef const& zone, bool withQuestDetail)
    {
        Settings const& cfg = GetConfig();
        ZoneEval ev;
        ev.zone = &zone;
        uint8 team = PlayerTeamMask(player);

        auto add = [&](Category cat, ObjectiveEval obj)
        {
            CategoryEval& ce = ev.cats[cat];
            CompletionRecord const* rec = state.Find(cat, obj.key);
            if (rec)
            {
                obj.done = true;
                obj.at = rec->at;
            }
            if (obj.bonus)
            {
                ++ce.bonusTotal;
                ce.bonusDone += obj.done ? 1 : 0;
            }
            else
            {
                ++ce.total;
                ce.done += obj.done ? 1 : 0;
            }
            ce.objectives.push_back(std::move(obj));
        };

        for (uint32 i = 0; i < zone.exploration.size(); ++i)
        {
            ExplorationObjective const& e = zone.exploration[i];
            ObjectiveEval obj;
            obj.cat = CAT_EXPLORATION;
            obj.key = e.areaId;
            obj.index = i;
            obj.name = e.name;
            obj.bonus = e.bonus;
            obj.done = IsExplored(player, e.exploreFlag);
            add(CAT_EXPLORATION, obj);
        }

        for (uint32 i = 0; i < zone.storylines.size(); ++i)
        {
            Storyline const* story = defs.FindStoryline(zone.storylines[i]);
            if (!story)
                continue;
            CompletionRecord const* rec = state.Find(CAT_STORYLINE, story->id);
            StorylineEval se = EvaluateStoryline(player, state, defs, *story);
            if (!se.applicable && !rec)
                continue;   // nothing in it this character can do (other faction, class, ...)
            ObjectiveEval obj;
            obj.cat = CAT_STORYLINE;
            obj.key = story->id;
            obj.index = i;
            obj.name = story->title;
            obj.bonus = story->bonus;
            obj.done = se.complete;
            obj.storyIndex = int32(ev.stories.size());
            ev.stories.push_back(std::move(se));
            add(CAT_STORYLINE, obj);
        }
        (void)withQuestDetail;

        auto addCreatures = [&](Category cat, std::vector<CreatureObjective> const& list)
        {
            for (uint32 i = 0; i < list.size(); ++i)
            {
                CreatureObjective const& c = list[i];
                if (!(c.attackableBy & team) && !state.Find(cat, c.entry))
                    continue;
                ObjectiveEval obj;
                obj.cat = cat;
                obj.key = c.entry;
                obj.index = i;
                obj.name = c.name;
                obj.bonus = c.bonus;
                add(cat, obj);
                ObjectiveEval& added = ev.cats[cat].objectives.back();
                added.hidden = !added.done && cfg.hiddenInfo < 2;
            }
        };
        addCreatures(CAT_RARE, zone.rares);
        addCreatures(CAT_ELITE, zone.elites);

        for (uint32 i = 0; i < zone.travel.size(); ++i)
        {
            TravelObjective const& t = zone.travel[i];
            if (!(t.teamMask & team) && !state.Find(CAT_TRAVEL, t.nodeId))
                continue;
            ObjectiveEval obj;
            obj.cat = CAT_TRAVEL;
            obj.key = t.nodeId;
            obj.index = i;
            obj.name = t.name;
            obj.done = player->GetTaxi().IsTaximaskNodeKnown(t.nodeId);
            add(CAT_TRAVEL, obj);
        }

        // Weights: hidden (empty) categories give theirs to the others proportionally.
        uint32 weightSum = 0;
        for (uint8 c = 0; c < CAT_COUNT; ++c)
        {
            CategoryEval& ce = ev.cats[c];
            ce.visible = ce.total > 0;
            ce.percent = ce.total ? ce.done * 100 / ce.total : 0;
            if (ce.visible)
                weightSum += cfg.weights[c];
        }
        bool anyVisible = false;
        bool allDone = true;
        double score = 0.0;
        for (uint8 c = 0; c < CAT_COUNT; ++c)
        {
            CategoryEval& ce = ev.cats[c];
            if (!ce.visible)
                continue;
            anyVisible = true;
            allDone = allDone && ce.done >= ce.total;
            if (weightSum)
            {
                ce.weight = uint32(std::lround(100.0 * cfg.weights[c] / weightSum));
                score += double(cfg.weights[c]) / weightSum * ce.done / ce.total;
            }
        }
        if (anyVisible && !weightSum)   // every visible category weighted 0: plain average
        {
            uint32 visible = 0;
            for (uint8 c = 0; c < CAT_COUNT; ++c)
                if (ev.cats[c].visible)
                {
                    ++visible;
                    score += double(ev.cats[c].done) / ev.cats[c].total;
                }
            score /= visible;
        }

        ev.allDone = anyVisible && allDone;
        ev.percent = ev.allDone ? 100 : std::min<uint32>(99, uint32(std::floor(score * 100.0 + 1e-9)));

        auto earned = state.zonesEarned.find(zone.zoneId);
        if (earned != state.zonesEarned.end())
        {
            ev.earned = true;
            ev.earnedAt = earned->second.first;
            ev.earnedVersion = earned->second.second;
            for (uint8 c = 0; c < CAT_COUNT; ++c)
                ev.newSinceEarned += ev.cats[c].total - ev.cats[c].done;
        }
        return ev;
    }

    void WithState(Player* player, std::function<void(PlayerState*)> const& fn)
    {
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        fn(player ? FindState(player) : nullptr);
    }

    void ProgressOnLogin(Player* player)
    {
        if (!GetConfig().enabled || !IsTracked(player))
            return;

        auto state = std::make_unique<PlayerState>();
        state->guid = player->GetGUIDLow();
        LoadState(*state);

        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        PlayerState& s = *state;
        states[s.guid] = std::move(state);

        DefinitionsPtr defs = GetDefinitions();
        if (!defs)
            return;

        size_t before = s.records.size();
        std::vector<Event> events;
        SyncAll(player, s, *defs, true, events, !s.resync);   // retroactive: explored areas, rewarded quests, known flight paths
        FinishReset(s);
        s.generation = defs->generation;
        s.exploredSig = ExploredSignature(player);
        s.taxiSig = TaxiSignature(player, *defs);
        s.questSig = QuestSignature(player);
        s.lastLevel = player->GetLevel();
        s.lastZone = player->GetZoneId();

        size_t recognised = s.records.size() - before;
        if (recognised && GetConfig().chatNotify)
        {
            Event e;
            e.type = "RETROACTIVE";
            e.fields.push_back({ "count", std::to_string(recognised) });
            e.chat = ChatPrefix() + std::to_string(recognised) + " objective(s) recognised from your past adventures. Type .ac to see your progress.";
            events.insert(events.begin(), e);
        }
        DeliverEvents(player, s, events);
    }

    void ProgressOnLogout(Player* player)
    {
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        states.erase(player->GetGUIDLow());
    }

    void ProgressOnDelete(uint32 guidLow)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(storeMutex);
            states.erase(guidLow);
        }
        CharacterDatabase.PExecute("DELETE FROM `azcomp_character_objective` WHERE `guid` = %u", guidLow);
        CharacterDatabase.PExecute("DELETE FROM `azcomp_character_zone` WHERE `guid` = %u", guidLow);
        CharacterDatabase.PExecute("DELETE FROM `azcomp_character_milestone` WHERE `guid` = %u", guidLow);
    }

    void ProgressOnUpdate(Player* player, uint32 diff)
    {
        Settings const& cfg = GetConfig();
        if (!cfg.enabled)
            return;

        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        PlayerState* state = FindState(player);
        if (!state)
            return;
        state->tickTimer += diff;
        if (state->tickTimer < cfg.syncIntervalMs)
            return;
        state->forceTimer += state->tickTimer;
        state->tickTimer = 0;

        if (!player->IsInWorld())
            return;
        DefinitionsPtr defs = GetDefinitions();
        if (!defs)
            return;

        std::vector<Event> events;
        std::set<uint32> zones;

        if (state->resync && state->generation == defs->generation)
        {
            SyncAll(player, *state, *defs, true, events, false);
            FinishReset(*state);
        }
        if (state->generation != defs->generation)
        {
            // New definitions: rebuild silently (objectives that appeared and are already
            // satisfied count as retroactive), then tell the addon to refresh.
            state->baselined.clear();
            state->catComplete.clear();
            state->storyUnits.clear();
            state->availableQuests.clear();
            SyncAll(player, *state, *defs, true, events, !state->resync);
            FinishReset(*state);
            state->generation = defs->generation;
            Event e;
            e.type = "DEFINITION_UPDATED";
            e.fields.push_back({ "gen", std::to_string(defs->generation) });
            std::string changed;
            for (uint32 z : lastChangedZones)
                if (ZoneDef const* zd = defs->FindZone(z))
                    changed += (changed.empty() ? "" : ",") + std::to_string(z) + ":" + std::to_string(zd->version);
            e.fields.push_back({ "zones", changed });
            events.push_back(e);
        }

        uint64 explored = ExploredSignature(player);
        if (explored != state->exploredSig)
        {
            state->exploredSig = explored;
            for (auto const& pair : defs->explorationByArea)
            {
                ZoneDef const* zone = defs->FindZone(pair.second.zoneId);
                if (zone && !state->Find(CAT_EXPLORATION, pair.first) && IsExplored(player, zone->exploration[pair.second.index].exploreFlag))
                    zones.insert(pair.second.zoneId);
            }
        }

        uint64 taxi = TaxiSignature(player, *defs);
        if (taxi != state->taxiSig)
        {
            state->taxiSig = taxi;
            for (auto const& pair : defs->travelByNode)
                if (!state->Find(CAT_TRAVEL, pair.first) && player->GetTaxi().IsTaximaskNodeKnown(pair.first))
                    zones.insert(pair.second.zoneId);
        }

        uint64 quests = QuestSignature(player);
        bool levelChanged = player->GetLevel() != state->lastLevel;
        if (quests != state->questSig || levelChanged)
        {
            // Any quest state or level change can move storylines anywhere in the world.
            state->questSig = quests;
            state->lastLevel = player->GetLevel();
            for (auto const& pair : defs->zones)
                if (!pair.second.storylines.empty())
                    zones.insert(pair.first);
        }

        uint32 zoneId = player->GetZoneId();
        if (zoneId != state->lastZone || state->forceTimer >= 30000)
        {
            state->lastZone = zoneId;
            state->forceTimer = 0;
            zones.insert(zoneId);
        }

        for (uint32 z : zones)
            if (ZoneDef const* zone = defs->FindZone(z))
                SyncZone(player, *state, *defs, *zone, false, events);

        DeliverEvents(player, *state, events);
    }

    void ProgressOnKill(Player* player, Creature* creature)
    {
        if (!GetConfig().enabled || !creature)
            return;
        DefinitionsPtr defs = GetDefinitions();
        if (!defs)
            return;

        uint32 entry = creature->GetEntry();
        Category cat;
        ObjectiveRef ref;
        if (auto itr = defs->rareByEntry.find(entry); itr != defs->rareByEntry.end())
            cat = CAT_RARE, ref = itr->second;
        else if (auto itr2 = defs->eliteByEntry.find(entry); itr2 != defs->eliteByEntry.end())
            cat = CAT_ELITE, ref = itr2->second;
        else
            return;

        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        PlayerState* state = FindState(player);
        if (!state || state->Find(cat, entry))
            return;
        ZoneDef const* zone = defs->FindZone(ref.zoneId);
        if (!zone)
            return;
        CreatureObjective const& obj = (cat == CAT_RARE ? zone->rares : zone->elites)[ref.index];

        std::vector<Event> events;
        // A kill can arrive before the update that rebuilds reset progress.
        // Restore existing achievements without rewards before processing it.
        if (state->resync)
        {
            SyncAll(player, *state, *defs, true, events, false);
            FinishReset(*state);
        }
        Record(*state, cat, entry, zone->zoneId, zone->version, SOURCE_KILL);
        ZoneEval eval = EvaluateZone(player, *state, *defs, *zone, false);
        events.push_back(MakeObjectiveEvent(*zone, eval, cat, entry, obj.name, obj.bonus));
        SyncZone(player, *state, *defs, *zone, false, events);
        DeliverEvents(player, *state, events);
    }

    void ProgressOnZoneChange(Player* player)
    {
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        if (PlayerState* state = FindState(player))
            state->tickTimer = GetConfig().syncIntervalMs;     // sync on the next update
    }

    void ProgressOnDefinitionsChanged(DefinitionsPtr const& oldDefs, DefinitionsPtr const& newDefs)
    {
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        lastChangedZones.clear();
        if (!newDefs)
            return;
        for (auto const& pair : newDefs->zones)
        {
            ZoneDef const* old = oldDefs ? oldDefs->FindZone(pair.first) : nullptr;
            if (!old || old->version != pair.second.version)
                lastChangedZones.push_back(pair.first);
        }
        // Players pick the new generation up on their next update.
    }

    uint32 ProgressTrackedPlayers()
    {
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        return uint32(states.size());
    }

    void ResetProgress(uint32 guidLow, uint32 zoneId)
    {
        // Keep deletes ordered with inserts made by map-thread progress updates.
        std::lock_guard<std::recursive_mutex> lock(storeMutex);
        if (zoneId)
        {
            CharacterDatabase.PExecute("DELETE FROM `azcomp_character_objective` WHERE `guid` = %u AND `zone_id` = %u", guidLow, zoneId);
            CharacterDatabase.PExecute("DELETE FROM `azcomp_character_zone` WHERE `guid` = %u AND `zone_id` = %u", guidLow, zoneId);
            CharacterDatabase.PExecute("DELETE FROM `azcomp_character_milestone` WHERE `guid` = %u AND `zone_id` = %u", guidLow, zoneId);
        }
        else
        {
            CharacterDatabase.PExecute("DELETE FROM `azcomp_character_objective` WHERE `guid` = %u", guidLow);
            CharacterDatabase.PExecute("DELETE FROM `azcomp_character_zone` WHERE `guid` = %u", guidLow);
            CharacterDatabase.PExecute("DELETE FROM `azcomp_character_milestone` WHERE `guid` = %u", guidLow);
        }

        // Persist this for offline characters and logouts before the next update.
        CharacterDatabase.PExecute("INSERT IGNORE INTO `azcomp_character_milestone` (`guid`, `zone_id`, `percent`, `claimed_at`, `rewarded`, `definition_version`) "
            "VALUES (%u, 0, 0, 0, 0, 0)", guidLow);

        auto itr = states.find(guidLow);
        if (itr == states.end())
            return;
        PlayerState& s = *itr->second;
        for (auto r = s.records.begin(); r != s.records.end();)
            r = (!zoneId || r->second.zoneId == zoneId) ? s.records.erase(r) : std::next(r);
        for (auto m = s.milestones.begin(); m != s.milestones.end();)
            m = (!zoneId || m->first == zoneId) ? s.milestones.erase(m) : std::next(m);
        for (auto c = s.catComplete.begin(); c != s.catComplete.end();)
            c = (!zoneId || c->first == zoneId) ? s.catComplete.erase(c) : std::next(c);
        if (zoneId)
        {
            s.zonesEarned.erase(zoneId);
            s.baselined.erase(zoneId);
        }
        else
        {
            s.zonesEarned.clear();
            s.baselined.clear();
        }
        // Retroactive data (explored areas, quests, flight paths) is picked up again silently.
        s.resync = true;
    }
}

namespace AzerothCompletion
{
    uint32 GetZonePercent(Player* player, uint32 zoneId)
    {
        Azc::DefinitionsPtr defs = Azc::GetDefinitions();
        Azc::ZoneDef const* zone = defs ? defs->FindZone(zoneId) : nullptr;
        if (!zone)
            return 0;
        uint32 percent = 0;
        Azc::WithState(player, [&](Azc::PlayerState* state)
        {
            if (state)
                percent = Azc::EvaluateZone(player, *state, *defs, *zone, false).percent;
        });
        return percent;
    }
}
