#include "AzcGenerator.h"
#include "AzcTerrain.h"
#include "Creature.h"
#include "Database/DatabaseEnv.h"
#include "Database/DBCStores.h"
#include "Database/SQLStorages.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "World.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <numeric>
#include <queue>
#include <regex>
#include <sstream>

namespace Azc
{
    namespace
    {
        // Rows of azcomp_override.
        enum OverrideMode : uint8
        {
            OVERRIDE_NONE      = 0,
            OVERRIDE_EXCLUDE   = 1,     // drop the objective entirely
            OVERRIDE_MANDATORY = 2,     // keep and count it even if the generator was unsure
            OVERRIDE_BONUS     = 3      // keep it but never count it
        };

        struct Override
        {
            uint8 mode = OVERRIDE_NONE;
            std::string hint;
        };

        struct Spawn
        {
            uint32 guid = 0;
            Point pos;
            uint32 areaId = 0;
            uint32 zoneId = 0;
            uint32 respawnMin = 0;
            uint32 respawnMax = 0;
            bool event = false;
        };

        uint64 Fnv(uint64 hash, std::string const& text)
        {
            for (unsigned char c : text)
            {
                hash ^= c;
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        }

        // Internal, test and placeholder records.
        bool IsTechnicalName(std::string const& name)
        {
            if (name.empty())
                return true;
            static std::regex const bad(R"((unused|not used|delete|deprecated|\btest\b|testing|\*\*\*|programmer|\bgm\b|development|do not use|\bdnd\b|\[ph\]|placeholder|\(old\)|\bzz|\bxx))",
                std::regex::icase);
            return std::regex_search(name, bad) || name[0] == '[';
        }

        std::vector<std::string> Words(std::string const& name)
        {
            std::vector<std::string> words;
            std::string current;
            for (unsigned char c : Lower(name))
            {
                if (std::isalpha(c) || c == '\'')
                    current += char(c);
                else
                {
                    if (current.size() >= 3)
                        words.push_back(current);
                    current.clear();
                }
            }
            if (current.size() >= 3)
                words.push_back(current);
            return words;
        }

        // "The Defias Brotherhood IV" -> "The Defias Brotherhood"
        std::string StorylineTitle(std::string title)
        {
            static std::regex const suffix(R"(\s*(\((part\s*)?[0-9ivx]+\)|\bpart\s+[0-9ivx]+|\b[IVX]+|\s[0-9]+)\s*$)", std::regex::icase);
            std::string trimmed = std::regex_replace(title, suffix, "");
            return trimmed.empty() ? title : trimmed;
        }

        // Quest text placeholders: $B line break, $N/$C/$R name/class/race, $Gmale:female;
        std::string ExpandPlaceholders(std::string const& text)
        {
            std::string out;
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] != '$' || i + 1 >= text.size())
                {
                    out += text[i];
                    continue;
                }
                char code = char(std::toupper(static_cast<unsigned char>(text[i + 1])));
                if (code == 'B')
                    out += ' ', ++i;
                else if (code == 'N' || code == 'C' || code == 'R')
                    out += "adventurer", ++i;
                else if (code == 'G')
                {
                    size_t colon = text.find(':', i);
                    size_t semi = text.find(';', i);
                    if (colon == std::string::npos || semi == std::string::npos || colon > semi)
                        continue;
                    out += text.substr(i + 2, colon - i - 2);
                    i = semi;
                }
                else
                    out += text[i];
            }
            return out;
        }

        std::string FirstSentence(std::string const& raw, size_t maxLen = 180)
        {
            std::string out;
            for (char c : ExpandPlaceholders(raw))
            {
                out += (c == '\n' || c == '\r') ? ' ' : c;
                if ((c == '.' || c == '!' || c == '?') && out.size() > 20)
                    break;
            }
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            if (out.size() > maxLen)
                out = out.substr(0, maxLen - 3) + "...";
            return out;
        }

        bool IsSeasonalSort(int32 zoneOrSort)
        {
            switch (-zoneOrSort)
            {
                case QUEST_SORT_SEASONAL:
                case QUEST_SORT_DARKMOON_FAIRE:
                case QUEST_SORT_LUNAR_FESTIVAL:
                case QUEST_SORT_MIDSUMMER:
                case QUEST_SORT_BREWFEST:
                case QUEST_SORT_INVASION:
                case QUEST_SORT_AHN_QIRAJ_WAR:
                    return true;
                default:
                    return false;
            }
        }

        class Generator
        {
        public:
            explicit Generator(Definitions const* previous) : m_previous(previous), m_terrain(sWorld.GetDataPath()) {}

            std::shared_ptr<Definitions> Run();

        private:
            void Log(std::string const& line) { m_defs->globalLog.push_back(line); sLog.outString("[mod-azeroth-completion] %s", line.c_str()); }
            void Exclude(uint32 zoneId, Category cat, uint32 key, std::string const& name, std::string const& reason);

            void LoadOverrides();
            void LoadAreas();
            void ScanTerrain();
            void IndexSpawns();
            void LoadQuestRelations();
            uint32 ZoneOfPoint(uint32 map, float x, float y, uint32* areaOut = nullptr);
            uint32 TopZone(uint32 areaId) const;
            ZoneDef& Zone(uint32 zoneId);
            bool IsEligibleZone(uint32 zoneId) const;
            Override const* FindOverride(Category cat, uint32 key) const;

            void BuildExploration();
            void BuildTravel();
            void BuildCreatures();
            void BuildStorylines();
            void FillNpcRef(NpcRef& ref, StarterKind kind, uint32 entry, uint32 preferZone);
            void Finish();
            void BuildRegions();
            void AssignVersions();

            Definitions const* m_previous;
            std::shared_ptr<Definitions> m_defs;
            Settings m_cfg;
            TerrainAreas m_terrain;

            std::map<std::pair<uint8, uint32>, Override> m_overrides;
            std::unordered_map<uint32, AreaEntry const*> m_areas;
            std::map<std::pair<uint32, uint32>, uint32> m_areaByFlag;      // (map, flag) -> area id
            std::unordered_map<uint32, std::vector<std::pair<float, float>>> m_areaCells;
            std::unordered_map<uint32, std::vector<Spawn>> m_creatureSpawns;    // entry -> spawns on eligible maps
            std::unordered_map<uint32, std::vector<Spawn>> m_goSpawns;
            std::set<uint32> m_eventQuests;
            std::multimap<uint32, uint32> m_questStartCreature, m_questEndCreature, m_questStartGo, m_questEndGo, m_questStartItem;
            std::unordered_map<uint32, std::vector<uint32>> m_questsByCreatureTarget;   // creature -> quests needing it
            std::unordered_map<uint32, std::vector<uint32>> m_questsByItem;             // item -> quests needing it
            std::unordered_map<uint32, std::vector<uint32>> m_questLootByLootId;        // loot id -> quest items
            std::unordered_map<std::string, uint32> m_nameWordCount;
            std::unordered_map<uint32, std::vector<uint32>> m_questsStartedByCreature;  // creature -> quests it gives
            std::set<uint32> m_wmoAreas;
            uint32 m_maxPlayerLevel = 60;
        };

        void Generator::Exclude(uint32 zoneId, Category cat, uint32 key, std::string const& name, std::string const& reason)
        {
            if (zoneId && IsEligibleZone(zoneId))
                Zone(zoneId).excluded.push_back({ cat, key, name, reason });
        }

        Override const* Generator::FindOverride(Category cat, uint32 key) const
        {
            auto itr = m_overrides.find({ uint8(cat), key });
            return itr == m_overrides.end() ? nullptr : &itr->second;
        }

        void Generator::LoadOverrides()
        {
            std::unique_ptr<QueryResult> result(WorldDatabase.Query("SELECT `category`, `objective`, `mode`, `hint` FROM `azcomp_override`"));
            if (!result)
                return;
            do
            {
                Field* fields = result->Fetch();
                Category cat;
                if (!ParseCategory(fields[0].GetCppString(), cat))
                    continue;
                Override& o = m_overrides[{ uint8(cat), fields[1].GetUInt32() }];
                o.mode = fields[2].GetUInt8();
                o.hint = fields[3].GetCppString();
            } while (result->NextRow());
        }

        void Generator::LoadAreas()
        {
            for (auto itr = sAreaStorage.begin<AreaEntry>(); itr < sAreaStorage.end<AreaEntry>(); ++itr)
            {
                AreaEntry const* area = *itr;
                m_areas[area->Id] = area;
                // first match wins, as in AreaEntry::GetByAreaFlagAndMap
                m_areaByFlag.emplace(std::make_pair(area->MapId, area->ExploreFlag), area->Id);
                m_defs->areaNames[area->Id] = area->Name ? area->Name : "";
            }
            for (auto const& pair : m_areas)
                m_defs->zoneOfArea[pair.first] = TopZone(pair.first);
        }

        uint32 Generator::TopZone(uint32 areaId) const
        {
            for (int depth = 0; depth < 6; ++depth)
            {
                auto itr = m_areas.find(areaId);
                if (itr == m_areas.end() || itr->second->ZoneId == 0)
                    return areaId;
                areaId = itr->second->ZoneId;
            }
            return areaId;
        }

        uint32 Generator::ZoneOfPoint(uint32 map, float x, float y, uint32* areaOut)
        {
            uint16 flag = m_terrain.GetAreaFlag(map, x, y);
            auto itr = m_areaByFlag.find({ map, uint32(flag) });
            uint32 area = itr == m_areaByFlag.end() ? 0 : itr->second;
            if (areaOut)
                *areaOut = area;
            return area ? TopZone(area) : 0;
        }

        bool Generator::IsEligibleZone(uint32 zoneId) const
        {
            auto itr = m_areas.find(zoneId);
            if (itr == m_areas.end())
                return false;
            AreaEntry const* zone = itr->second;
            if (zone->ZoneId != 0 || !m_cfg.maps.count(zone->MapId))
                return false;
            if (IsTechnicalName(zone->Name ? zone->Name : ""))
                return false;
            if (Override const* o = FindOverride(CAT_EXPLORATION, zoneId))
                if (o->mode == OVERRIDE_EXCLUDE)    // excluding a zone's own area id removes the whole zone
                    return false;
            return true;
        }

        ZoneDef& Generator::Zone(uint32 zoneId)
        {
            ZoneDef& zone = m_defs->zones[zoneId];
            if (!zone.zoneId)
            {
                zone.zoneId = zoneId;
                AreaEntry const* area = m_areas[zoneId];
                zone.mapId = area->MapId;
                zone.name = area->Name ? area->Name : "";
            }
            return zone;
        }

        void Generator::ScanTerrain()
        {
            for (uint32 i = 0; i < sWMOAreaTableStore.GetNumRows(); ++i)
                if (WMOAreaTableEntry const* wmo = sWMOAreaTableStore.LookupEntry(i))
                    m_wmoAreas.insert(wmo->areaId);

            for (uint32 map : m_cfg.maps)
            {
                m_terrain.ForEachCell(map, [&](uint16 flag, float x, float y)
                {
                    auto itr = m_areaByFlag.find({ map, uint32(flag) });
                    if (itr != m_areaByFlag.end())
                        m_areaCells[itr->second].emplace_back(x, y);
                });
            }
        }

        void Generator::IndexSpawns()
        {
            std::set<uint32> eventCreatures, eventGos;
            if (std::unique_ptr<QueryResult> result{ WorldDatabase.Query("SELECT `guid` FROM `game_event_creature`") })
                do { eventCreatures.insert(result->Fetch()[0].GetUInt32()); } while (result->NextRow());
            if (std::unique_ptr<QueryResult> result{ WorldDatabase.Query("SELECT `guid` FROM `game_event_gameobject`") })
                do { eventGos.insert(result->Fetch()[0].GetUInt32()); } while (result->NextRow());
            if (std::unique_ptr<QueryResult> result{ WorldDatabase.Query("SELECT `quest` FROM `game_event_quest`") })
                do { m_eventQuests.insert(result->Fetch()[0].GetUInt32()); } while (result->NextRow());

            auto creatureWorker = [&](CreatureDataPair const& pair) -> bool
            {
                CreatureData const& data = pair.second;
                if (!m_cfg.maps.count(data.position.mapId) || (data.spawn_flags & SPAWN_FLAG_DISABLED))
                    return false;
                Spawn spawn;
                spawn.guid = pair.first;
                spawn.pos = { data.position.mapId, data.position.x, data.position.y, data.position.z };
                spawn.zoneId = ZoneOfPoint(data.position.mapId, data.position.x, data.position.y, &spawn.areaId);
                spawn.respawnMin = data.spawntimesecsmin;
                spawn.respawnMax = data.spawntimesecsmax;
                spawn.event = eventCreatures.count(pair.first) != 0;
                for (uint32 i = 0; i < MAX_CREATURE_IDS_PER_SPAWN && data.creature_id[i]; ++i)
                    m_creatureSpawns[data.creature_id[i]].push_back(spawn);
                return false;
            };
            sObjectMgr.DoCreatureData(creatureWorker);

            auto goWorker = [&](GameObjectDataPair const& pair) -> bool
            {
                GameObjectData const& data = pair.second;
                if (!m_cfg.maps.count(data.position.mapId) || (data.spawn_flags & SPAWN_FLAG_DISABLED))
                    return false;
                Spawn spawn;
                spawn.guid = pair.first;
                spawn.pos = { data.position.mapId, data.position.x, data.position.y, data.position.z };
                spawn.zoneId = ZoneOfPoint(data.position.mapId, data.position.x, data.position.y, &spawn.areaId);
                spawn.event = eventGos.count(pair.first) != 0;
                m_goSpawns[data.id].push_back(spawn);
                return false;
            };
            sObjectMgr.DoGOData(goWorker);
        }

        void Generator::LoadQuestRelations()
        {
            auto load = [](char const* sql, std::multimap<uint32, uint32>& target)
            {
                if (std::unique_ptr<QueryResult> result{ WorldDatabase.Query(sql) })
                {
                    do
                    {
                        Field* fields = result->Fetch();
                        target.emplace(fields[1].GetUInt32(), fields[0].GetUInt32());     // quest -> entry
                    } while (result->NextRow());
                }
            };
            load("SELECT `id`, `quest` FROM `creature_questrelation`", m_questStartCreature);
            load("SELECT `id`, `quest` FROM `creature_involvedrelation`", m_questEndCreature);
            load("SELECT `id`, `quest` FROM `gameobject_questrelation`", m_questStartGo);
            load("SELECT `id`, `quest` FROM `gameobject_involvedrelation`", m_questEndGo);
            load("SELECT `entry`, `start_quest` FROM `item_template` WHERE `start_quest` > 0", m_questStartItem);
            for (auto const& pair : m_questStartCreature)
                m_questsStartedByCreature[pair.second].push_back(pair.first);

            if (std::unique_ptr<QueryResult> result{ WorldDatabase.Query(
                "SELECT `entry`, `item` FROM `creature_loot_template` WHERE `ChanceOrQuestChance` < 0 AND `mincountOrRef` > 0") })
            {
                do
                {
                    Field* fields = result->Fetch();
                    m_questLootByLootId[fields[0].GetUInt32()].push_back(fields[1].GetUInt32());
                } while (result->NextRow());
            }

            for (auto const& pair : sObjectMgr.GetQuestTemplates())
            {
                Quest const* quest = pair.second.get();
                for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    if (quest->ReqCreatureOrGOId[i] > 0)
                        m_questsByCreatureTarget[uint32(quest->ReqCreatureOrGOId[i])].push_back(quest->GetQuestId());
                for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                    if (quest->ReqItemId[i])
                        m_questsByItem[quest->ReqItemId[i]].push_back(quest->GetQuestId());
            }
        }

        void Generator::BuildExploration()
        {
            uint32 included = 0;
            std::map<std::string, uint32> excluded;
            for (auto const& pair : m_areas)
            {
                AreaEntry const* area = pair.second;
                if (area->ZoneId == 0 || !m_cfg.maps.count(area->MapId))
                    continue;
                uint32 zoneId = TopZone(area->Id);
                if (!IsEligibleZone(zoneId))
                    continue;

                std::string name = area->Name ? area->Name : "";
                auto cellsItr = m_areaCells.find(area->Id);
                uint32 cells = cellsItr == m_areaCells.end() ? 0 : uint32(cellsItr->second.size());
                Override const* o = FindOverride(CAT_EXPLORATION, area->Id);

                std::string reason;
                auto flagOwner = m_areaByFlag.find({ area->MapId, area->ExploreFlag });
                if (IsTechnicalName(name))
                    reason = "technical_name";
                else if (area->ExploreFlag == 0 || area->ExploreFlag >= uint32(PLAYER_EXPLORED_ZONES_SIZE) * 32)
                    reason = "no_explore_flag";
                else if (flagOwner != m_areaByFlag.end() && flagOwner->second != area->Id)
                    reason = "duplicate_flag";
                else if (m_areas.count(area->ZoneId) && Lower(name) == Lower(m_areas[area->ZoneId]->Name ? m_areas[area->ZoneId]->Name : ""))
                    reason = "duplicate_of_parent";
                else if (area->AreaLevel <= 0)
                    reason = "no_exploration_reward";   // the game itself gives no discovery for it
                else if (cells < m_cfg.minExploreCells && !m_wmoAreas.count(area->Id))
                    // A WMO-defined area (cave, building) has few or no terrain cells but is real.
                    reason = cells == 0 ? "not_in_terrain" : "tiny_area";

                if (o && o->mode == OVERRIDE_EXCLUDE)
                    reason = "override_exclude";
                else if (o && (o->mode == OVERRIDE_MANDATORY || o->mode == OVERRIDE_BONUS))
                    reason.clear();

                if (!reason.empty())
                {
                    ++excluded[reason];
                    Exclude(zoneId, CAT_EXPLORATION, area->Id, name, reason);
                    continue;
                }

                ExplorationObjective obj;
                obj.areaId = area->Id;
                obj.parentAreaId = area->ZoneId;
                obj.exploreFlag = area->ExploreFlag;
                obj.name = name;
                obj.areaLevel = area->AreaLevel;
                obj.terrainCells = cells;
                if (cells)
                {
                    // The cell nearest the centroid: inside the area even when it is not convex.
                    auto const& list = cellsItr->second;
                    double sx = 0, sy = 0;
                    for (auto const& c : list) { sx += c.first; sy += c.second; }
                    sx /= list.size(); sy /= list.size();
                    auto best = std::min_element(list.begin(), list.end(), [&](auto const& a, auto const& b)
                    {
                        return (a.first - sx) * (a.first - sx) + (a.second - sy) * (a.second - sy) <
                               (b.first - sx) * (b.first - sx) + (b.second - sy) * (b.second - sy);
                    });
                    obj.hasCenter = true;
                    obj.center = { area->MapId, best->first, best->second, 0.0f };
                }
                if (o)
                {
                    obj.hint = o->hint;
                    obj.bonus = o->mode == OVERRIDE_BONUS;
                    if (obj.bonus)
                        obj.bonusReason = "override_bonus";
                }
                Zone(zoneId).exploration.push_back(obj);
                ++included;
            }

            std::ostringstream line;
            line << "Exploration: " << included << " areas";
            for (auto const& e : excluded)
                line << ", " << e.second << " " << e.first;
            Log(line.str());
        }

        void Generator::BuildTravel()
        {
            uint32 included = 0;
            std::map<std::string, uint32> excluded;
            for (uint32 id = 1; id < sObjectMgr.GetMaxTaxiNodeId(); ++id)
            {
                TaxiNodesEntry const* node = sObjectMgr.GetTaxiNodeEntry(id);
                if (!node || !m_cfg.maps.count(node->map_id))
                    continue;

                std::string name = node->name[0];
                uint32 areaId = 0;
                uint32 zoneId = ZoneOfPoint(node->map_id, node->x, node->y, &areaId);
                uint8 team = 0;
                if (node->MountCreatureID[1]) team |= TEAM_MASK_ALLIANCE;
                if (node->MountCreatureID[0]) team |= TEAM_MASK_HORDE;

                uint8 field = uint8((id - 1) / 32);
                uint32 submask = 1 << ((id - 1) % 32);
                bool inNetwork = field < sTaxiNodesMask.size() && (sTaxiNodesMask[field] & submask);

                std::string reason;
                if (IsTechnicalName(name))
                    reason = "technical_name";
                else if (!team)
                    reason = "unused_node";
                else if (!inNetwork)
                    reason = "not_in_network";
                else if (!zoneId || !IsEligibleZone(zoneId))
                    reason = "no_zone";

                Override const* o = FindOverride(CAT_TRAVEL, id);
                if (o && o->mode == OVERRIDE_EXCLUDE)
                    reason = "override_exclude";
                else if (o && o->mode != OVERRIDE_NONE && zoneId && IsEligibleZone(zoneId))
                    reason.clear();

                if (!reason.empty())
                {
                    ++excluded[reason];
                    Exclude(zoneId, CAT_TRAVEL, id, name, reason);
                    continue;
                }

                TravelObjective obj;
                obj.nodeId = id;
                obj.name = name;
                obj.teamMask = team;
                obj.position = { node->map_id, node->x, node->y, node->z };
                obj.areaId = areaId;
                obj.bonus = o && o->mode == OVERRIDE_BONUS;
                Zone(zoneId).travel.push_back(obj);
                ++included;
            }

            std::ostringstream line;
            line << "Travel: " << included << " flight paths";
            for (auto const& e : excluded)
                line << ", " << e.second << " " << e.first;
            Log(line.str());
        }

        void Generator::BuildCreatures()
        {
            FactionTemplateEntry const* alliance = sObjectMgr.GetFactionTemplateEntry(1);  // PLAYER, Human
            FactionTemplateEntry const* horde = sObjectMgr.GetFactionTemplateEntry(2);     // PLAYER, Orc

            for (auto const& pair : sObjectMgr.GetCreatureInfoMap())
                for (std::string const& word : Words(pair.second->name))
                    ++m_nameWordCount[word];

            // Zone level ceilings from exploration, for the "unusually high level" signal.
            std::unordered_map<uint32, int32> zoneMaxLevel;
            for (auto const& pair : m_areas)
                if (pair.second->ZoneId && pair.second->AreaLevel > 0)
                {
                    int32& level = zoneMaxLevel[TopZone(pair.first)];
                    level = std::max(level, pair.second->AreaLevel);
                }

            std::map<std::string, uint32> rareExcluded, eliteExcluded;
            uint32 rareCount = 0, eliteCount = 0;
            std::vector<CreatureObjective*> raresForLoot;

            for (auto const& pair : sObjectMgr.GetCreatureInfoMap())
            {
                CreatureInfo const* info = pair.second.get();
                bool isRare = info->rank == CREATURE_ELITE_RARE || info->rank == CREATURE_ELITE_RAREELITE;
                bool isElite = info->rank == CREATURE_ELITE_ELITE || info->rank == CREATURE_ELITE_WORLDBOSS;
                if (!isRare && !isElite)
                    continue;

                Category cat = isRare ? CAT_RARE : CAT_ELITE;
                auto& excluded = isRare ? rareExcluded : eliteExcluded;

                auto spawnItr = m_creatureSpawns.find(info->entry);
                if (spawnItr == m_creatureSpawns.end())
                    continue;   // summons, instance-only or unspawned: nothing to hunt in the open world

                std::vector<Spawn const*> permanent, eventOnly;
                for (Spawn const& s : spawnItr->second)
                    (s.event ? eventOnly : permanent).push_back(&s);
                std::vector<Spawn const*> const& used = permanent.empty() ? eventOnly : permanent;

                std::map<uint32, uint32> zoneVotes;
                for (Spawn const* s : used)
                    if (s->zoneId)
                        ++zoneVotes[s->zoneId];
                uint32 zoneId = 0, votes = 0;
                for (auto const& v : zoneVotes)
                    if (v.second > votes) { zoneId = v.first; votes = v.second; }

                if (!zoneId || !IsEligibleZone(zoneId))
                {
                    ++excluded["no_zone"];
                    continue;
                }

                uint8 attackable = 0;
                FactionTemplateEntry const* faction = sObjectMgr.GetFactionTemplateEntry(info->faction);
                if (!faction || !alliance || !faction->IsFriendlyTo(*alliance)) attackable |= TEAM_MASK_ALLIANCE;
                if (!faction || !horde || !faction->IsFriendlyTo(*horde)) attackable |= TEAM_MASK_HORDE;

                std::vector<uint32> related;
                if (auto q = m_questsByCreatureTarget.find(info->entry); q != m_questsByCreatureTarget.end())
                    related.insert(related.end(), q->second.begin(), q->second.end());
                if (info->loot_id)
                    if (auto loot = m_questLootByLootId.find(info->loot_id); loot != m_questLootByLootId.end())
                        for (uint32 item : loot->second)
                            if (auto q = m_questsByItem.find(item); q != m_questsByItem.end())
                                related.insert(related.end(), q->second.begin(), q->second.end());
                if (auto q = m_questsStartedByCreature.find(info->entry); q != m_questsStartedByCreature.end())
                    related.insert(related.end(), q->second.begin(), q->second.end());
                std::sort(related.begin(), related.end());
                related.erase(std::unique(related.begin(), related.end()), related.end());

                std::string reason, bonusReason;
                if (IsTechnicalName(info->name))
                    reason = "technical_name";
                else if (info->flags_extra & (CREATURE_FLAG_EXTRA_INVISIBLE | CREATURE_FLAG_EXTRA_GUARD))
                    reason = "trigger_or_guard";
                else if (info->type == CREATURE_TYPE_CRITTER)
                    reason = "critter";
                else if (!attackable)
                    reason = "friendly_to_all";
                else if (isElite && info->npc_flags)
                    reason = "service_npc";
                else if (isElite && permanent.size() > m_cfg.eliteMaxSpawns)
                    reason = "generic_elite";

                if (reason.empty())
                {
                    if (permanent.empty())
                        bonusReason = "event_only";
                    else if (info->unit_flags & (UNIT_FLAG_SPAWNING | UNIT_FLAG_IMMUNE_TO_PLAYER | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE_2))
                        bonusReason = "not_attackable_by_default";
                    else if (info->phase_quest_id)
                        bonusReason = "phased";
                    else if (info->rank == CREATURE_ELITE_WORLDBOSS && !m_cfg.worldBossesMandatory)
                        bonusReason = "world_boss";
                }

                uint32 score = 0;
                std::vector<std::string> signals;
                if (isElite && reason.empty())
                {
                    bool properName = false;
                    for (std::string const& word : Words(info->name))
                        if (m_nameWordCount[word] <= 2)
                            properName = true;
                    if (permanent.size() == 1) { score += 2; signals.push_back("unique_spawn"); }
                    if (!related.empty()) { score += 3; signals.push_back("quest_target"); }
                    if (properName) { score += 2; signals.push_back("unique_name"); }
                    if (!info->subname.empty()) { score += 1; signals.push_back("title"); }
                    if (info->rank == CREATURE_ELITE_WORLDBOSS) { score += 3; signals.push_back("world_boss"); }
                    if (info->script_id) { score += 1; signals.push_back("scripted"); }
                    auto zl = zoneMaxLevel.find(zoneId);
                    if (zl != zoneMaxLevel.end() && int32(info->level_max) >= zl->second + 3) { score += 1; signals.push_back("high_level"); }
                    if (score < m_cfg.eliteMinScore)
                        reason = "generic_elite";
                }

                Override const* o = FindOverride(cat, info->entry);
                if (o && o->mode == OVERRIDE_EXCLUDE)
                    reason = "override_exclude";
                else if (o && o->mode == OVERRIDE_MANDATORY)
                    reason.clear(), bonusReason.clear();
                else if (o && o->mode == OVERRIDE_BONUS)
                    reason.clear(), bonusReason = "override_bonus";

                if (!reason.empty())
                {
                    ++excluded[reason];
                    Exclude(zoneId, cat, info->entry, info->name, reason);
                    continue;
                }
                if (!bonusReason.empty())
                    ++excluded["bonus_" + bonusReason];

                CreatureObjective obj;
                obj.entry = info->entry;
                obj.name = info->name;
                obj.subname = info->subname;
                obj.levelMin = info->level_min;
                obj.levelMax = info->level_max;
                obj.rank = info->rank;
                obj.spawnCount = uint32(used.size());
                obj.attackableBy = attackable;
                obj.relatedQuests = related;
                obj.importance = score;
                obj.signals = signals;
                obj.bonus = !bonusReason.empty();
                obj.bonusReason = bonusReason;
                if (o)
                    obj.hint = o->hint;
                std::set<uint32> areas;
                obj.respawnMin = UINT32_MAX;
                for (Spawn const* s : used)
                {
                    if (obj.spawns.size() < 8)
                        obj.spawns.push_back(s->pos);
                    if (s->areaId)
                        areas.insert(s->areaId);
                    obj.respawnMin = std::min(obj.respawnMin, s->respawnMin);
                    obj.respawnMax = std::max(obj.respawnMax, s->respawnMax);
                }
                if (obj.respawnMin == UINT32_MAX)
                    obj.respawnMin = 0;
                obj.spawnAreas.assign(areas.begin(), areas.end());

                ZoneDef& zone = Zone(zoneId);
                if (isRare)
                {
                    zone.rares.push_back(obj);
                    ++rareCount;
                }
                else
                {
                    zone.elites.push_back(obj);
                    ++eliteCount;
                }
            }

            // Loot summaries for rares: the uncommon-or-better items they can drop.
            if (m_cfg.exposeLoot)
            {
                std::unordered_map<uint32, std::vector<CreatureObjective*>> byLootId;
                for (auto& zp : m_defs->zones)
                    for (CreatureObjective& rare : zp.second.rares)
                        if (CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(rare.entry))
                            if (info->loot_id)
                                byLootId[info->loot_id].push_back(&rare);
                if (!byLootId.empty())
                {
                    std::ostringstream ids;
                    bool first = true;
                    for (auto const& p : byLootId)
                    {
                        ids << (first ? "" : ",") << p.first;
                        first = false;
                    }
                    std::unique_ptr<QueryResult> result(WorldDatabase.PQuery(
                        "SELECT `entry`, `item`, `ChanceOrQuestChance` FROM `creature_loot_template` "
                        "WHERE `mincountOrRef` > 0 AND `ChanceOrQuestChance` > 0 AND `entry` IN (%s)", ids.str().c_str()));
                    if (result)
                    {
                        do
                        {
                            Field* fields = result->Fetch();
                            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(fields[1].GetUInt32());
                            if (!proto || proto->Quality < ITEM_QUALITY_UNCOMMON)
                                continue;
                            for (CreatureObjective* rare : byLootId[fields[0].GetUInt32()])
                                rare->loot.emplace_back(proto->ItemId, fields[2].GetFloat());
                        } while (result->NextRow());
                    }
                    for (auto& p : byLootId)
                        for (CreatureObjective* rare : p.second)
                        {
                            std::sort(rare->loot.begin(), rare->loot.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
                            if (rare->loot.size() > 5)
                                rare->loot.resize(5);
                        }
                }
            }

            std::ostringstream rl, el;
            rl << "Rare Hunts: " << rareCount << " rares";
            for (auto const& e : rareExcluded)
                rl << ", " << e.second << " " << e.first;
            el << "Elite Encounters: " << eliteCount << " elites";
            for (auto const& e : eliteExcluded)
                el << ", " << e.second << " " << e.first;
            Log(rl.str());
            Log(el.str());
        }

        void Generator::FillNpcRef(NpcRef& ref, StarterKind kind, uint32 entry, uint32 preferZone)
        {
            ref.kind = kind;
            ref.entry = entry;
            std::vector<Spawn> const* spawns = nullptr;
            if (kind == STARTER_CREATURE)
            {
                if (CreatureInfo const* info = sObjectMgr.GetCreatureTemplate(entry))
                    ref.name = info->name;
                auto itr = m_creatureSpawns.find(entry);
                if (itr != m_creatureSpawns.end())
                    spawns = &itr->second;
            }
            else if (kind == STARTER_GAMEOBJECT)
            {
                if (GameObjectInfo const* info = sObjectMgr.GetGameObjectInfo(entry))
                    ref.name = info->name;
                auto itr = m_goSpawns.find(entry);
                if (itr != m_goSpawns.end())
                    spawns = &itr->second;
            }
            else if (kind == STARTER_ITEM)
            {
                if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(entry))
                    ref.name = proto->Name1;
            }

            if (!spawns || spawns->empty())
                return;

            Spawn const* best = nullptr;
            for (Spawn const& s : *spawns)
            {
                if (s.event)
                    continue;
                if (!best || (s.zoneId == preferZone && best->zoneId != preferZone))
                    best = &s;
            }
            if (!best)
                return;
            ref.hasLocation = true;
            ref.location = best->pos;
            ref.zoneId = best->zoneId;
            ref.areaId = best->areaId;
        }

        void Generator::BuildStorylines()
        {
            auto const& templates = sObjectMgr.GetQuestTemplates();

            // Union-find over every quest relationship the core itself evaluates.
            std::unordered_map<uint32, uint32> parent;
            std::function<uint32(uint32)> find = [&](uint32 x) -> uint32
            {
                uint32 root = x;
                while (parent[root] != root)
                    root = parent[root];
                while (parent[x] != root)
                {
                    uint32 next = parent[x];
                    parent[x] = root;
                    x = next;
                }
                return root;
            };
            auto unite = [&](uint32 a, uint32 b)
            {
                if (!templates.count(a) || !templates.count(b))
                    return;
                parent[find(a)] = find(b);
            };
            for (auto const& pair : templates)
                parent[pair.first] = pair.first;

            std::map<int32, std::vector<uint32>> exclusive;
            for (auto const& pair : templates)
            {
                Quest const* q = pair.second.get();
                if (q->GetPrevQuestId())
                    unite(q->GetQuestId(), uint32(std::abs(q->GetPrevQuestId())));
                if (q->GetNextQuestId())
                    unite(q->GetQuestId(), uint32(std::abs(q->GetNextQuestId())));
                if (q->GetNextQuestInChain())
                    unite(q->GetQuestId(), q->GetNextQuestInChain());
                if (q->GetExclusiveGroup())
                    exclusive[q->GetExclusiveGroup()].push_back(q->GetQuestId());
            }
            for (auto const& group : exclusive)
                for (size_t i = 1; i < group.second.size(); ++i)
                    unite(group.second[0], group.second[i]);

            std::unordered_map<uint32, std::vector<uint32>> components;
            for (auto const& pair : templates)
                components[find(pair.first)].push_back(pair.first);

            std::map<std::string, uint32> excludedCounts;
            uint32 storylineCount = 0, bonusStorylines = 0, optionalQuests = 0;

            for (auto& comp : components)
            {
                std::vector<uint32>& ids = comp.second;
                if (ids.size() < m_cfg.minStorylineQuests)
                    continue;   // a lone quest is not a storyline
                std::sort(ids.begin(), ids.end());

                // Zone: where most of its quests are filed.
                std::map<uint32, uint32> votes;
                for (uint32 id : ids)
                {
                    int32 zos = templates.find(id)->second->GetZoneOrSort();
                    if (zos > 0)
                    {
                        uint32 zone = TopZone(uint32(zos));
                        if (IsEligibleZone(zone))
                            ++votes[zone];
                    }
                }
                uint32 zoneId = 0, best = 0;
                for (auto const& v : votes)
                    if (v.second > best) { zoneId = v.first; best = v.second; }

                Quest const* first = templates.find(ids[0])->second.get();
                if (!zoneId)
                {
                    ++excludedCounts["no_outdoor_zone"];
                    continue;
                }
                // A chain made only of placeholder quests ("[DEPRECATED] ...", "UNUSED ...") is not content.
                if (std::all_of(ids.begin(), ids.end(), [&](uint32 id) { return IsTechnicalName(templates.find(id)->second->GetTitle()); }))
                {
                    ++excludedCounts["technical_name"];
                    Exclude(zoneId, CAT_STORYLINE, ids[0], first->GetTitle(), "technical_name");
                    continue;
                }
                if (ids.size() > m_cfg.maxStorylineQuests)
                {
                    ++excludedCounts["oversized_chain"];
                    Exclude(zoneId, CAT_STORYLINE, ids[0], first->GetTitle(), "oversized_chain");
                    continue;
                }

                std::set<uint32> members(ids.begin(), ids.end());
                Storyline story;
                story.zoneId = zoneId;

                std::vector<QuestNode> nodes;
                for (uint32 id : ids)
                {
                    Quest const* q = templates.find(id)->second.get();
                    QuestNode node;
                    node.id = id;
                    node.title = q->GetTitle();
                    node.level = q->GetQuestLevel();
                    node.minLevel = q->GetMinLevel();
                    node.maxLevel = q->GetMaxLevel();
                    node.reqRaces = q->GetRequiredRaces();
                    node.reqClasses = q->GetRequiredClasses();
                    node.reqSkill = q->GetRequiredSkill();
                    node.reqSkillValue = q->GetRequiredSkillValue();
                    node.zoneOrSort = q->GetZoneOrSort();
                    node.repeatable = q->IsRepeatable() || q->HasSpecialFlag(QUEST_SPECIAL_FLAG_DAILY);
                    node.disabled = !q->IsActive();
                    node.seasonal = m_eventQuests.count(id) || IsSeasonalSort(q->GetZoneOrSort());
                    node.exclusiveGroup = q->GetExclusiveGroup();
                    node.nextInChain = members.count(q->GetNextQuestInChain()) ? q->GetNextQuestInChain() : 0;
                    for (int32 p : q->prevQuests)
                    {
                        uint32 pid = uint32(std::abs(p));
                        if (!members.count(pid))
                            continue;
                        (p > 0 ? node.prev : node.prevActive).push_back(pid);
                    }
                    node.summary = FirstSentence(q->GetObjectives());

                    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                    {
                        if (!q->ObjectiveText[i].empty())
                            node.objectives.push_back(q->ObjectiveText[i]);
                        else if (q->ReqCreatureOrGOId[i] > 0 && q->ReqCreatureOrGOCount[i])
                        {
                            CreatureInfo const* target = sObjectMgr.GetCreatureTemplate(uint32(q->ReqCreatureOrGOId[i]));
                            node.objectives.push_back("Slay " + std::to_string(q->ReqCreatureOrGOCount[i]) + " " + (target ? target->name : "creatures"));
                        }
                    }
                    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                        if (q->ReqItemId[i] && q->ReqItemCount[i])
                            if (ItemPrototype const* proto = sObjectMgr.GetItemPrototype(q->ReqItemId[i]))
                                node.objectives.push_back("Collect " + std::to_string(q->ReqItemCount[i]) + " " + proto->Name1);

                    // Giver: creature, then object, then item.
                    auto sc = m_questStartCreature.equal_range(id);
                    auto sg = m_questStartGo.equal_range(id);
                    auto si = m_questStartItem.equal_range(id);
                    if (sc.first != sc.second)
                    {
                        // prefer a giver that is actually spawned
                        uint32 pick = sc.first->second;
                        for (auto itr = sc.first; itr != sc.second; ++itr)
                            if (m_creatureSpawns.count(itr->second)) { pick = itr->second; break; }
                        FillNpcRef(node.giver, STARTER_CREATURE, pick, zoneId);
                    }
                    else if (sg.first != sg.second)
                        FillNpcRef(node.giver, STARTER_GAMEOBJECT, sg.first->second, zoneId);
                    else if (si.first != si.second)
                        FillNpcRef(node.giver, STARTER_ITEM, si.first->second, zoneId);
                    else if (q->HasQuestFlag(QUEST_FLAGS_AUTO_REWARDED) || q->GetSrcSpell())
                        node.giver.kind = STARTER_AUTO;

                    auto ec = m_questEndCreature.equal_range(id);
                    auto eg = m_questEndGo.equal_range(id);
                    if (ec.first != ec.second)
                    {
                        uint32 pick = ec.first->second;
                        for (auto itr = ec.first; itr != ec.second; ++itr)
                            if (m_creatureSpawns.count(itr->second)) { pick = itr->second; break; }
                        FillNpcRef(node.ender, STARTER_CREATURE, pick, zoneId);
                    }
                    else if (eg.first != eg.second)
                        FillNpcRef(node.ender, STARTER_GAMEOBJECT, eg.first->second, zoneId);
                    else if (q->HasQuestFlag(QUEST_FLAGS_AUTO_REWARDED))
                        node.ender.kind = STARTER_AUTO;

                    // Conservative: anything we cannot prove obtainable and completable never counts.
                    std::string why;
                    if (IsTechnicalName(node.title))                        why = "technical_name";
                    else if (node.disabled)                                 why = "disabled";
                    else if (node.seasonal)                                 why = "seasonal";
                    else if (node.repeatable)                               why = "repeatable";
                    else if (q->GetRequiredCondition())                     why = "condition";
                    else if (q->GetRequiredMinRepFaction() || q->GetRequiredMaxRepFaction()) why = "reputation";
                    else if (node.reqSkill)                                 why = "profession";
                    else if (q->HasSpecialFlag(QUEST_SPECIAL_FLAG_HARDCORE_ONLY) || q->HasSpecialFlag(QUEST_SPECIAL_FLAG_NOT_HARDCORE)) why = "challenge";
                    else if (!node.prevActive.empty())                      why = "concurrent_only";
                    else if (node.minLevel > m_maxPlayerLevel)              why = "level_unreachable";
                    else if (node.giver.kind == STARTER_ITEM)               why = "item_started";
                    else if (node.giver.kind == STARTER_NONE)               why = "no_giver";
                    else if (node.giver.kind != STARTER_AUTO && !node.giver.hasLocation) why = "giver_not_spawned";
                    else if (node.ender.kind == STARTER_NONE)               why = "no_ender";
                    else if (node.ender.kind != STARTER_AUTO && !node.ender.hasLocation) why = "ender_not_spawned";
                    if (!why.empty())
                    {
                        node.optional = true;
                        node.optionalReason = why;
                    }
                    nodes.push_back(std::move(node));
                }

                std::unordered_map<uint32, QuestNode*> byId;
                for (QuestNode& n : nodes)
                    byId[n.id] = &n;

                // Breadcrumbs: a quest pointing at a follow-up that does not need it.
                for (QuestNode& n : nodes)
                {
                    if (!n.nextInChain)
                        continue;
                    QuestNode* target = byId[n.nextInChain];
                    if (std::find(target->prev.begin(), target->prev.end(), n.id) == target->prev.end())
                    {
                        n.breadcrumb = true;
                        if (!n.optional)
                        {
                            n.optional = true;
                            n.optionalReason = "breadcrumb";
                        }
                    }
                }

                // A quest whose only prerequisites are optional (or missing) cannot be relied on.
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (QuestNode& n : nodes)
                    {
                        if (n.optional || n.prev.empty())
                            continue;
                        bool reachable = std::any_of(n.prev.begin(), n.prev.end(), [&](uint32 p) { return !byId[p]->optional; });
                        if (!reachable)
                        {
                            n.optional = true;
                            n.optionalReason = "prerequisite_optional";
                            changed = true;
                        }
                    }
                }

                // Follow-ups.
                for (QuestNode& n : nodes)
                {
                    for (uint32 p : n.prev)
                        byId[p]->next.push_back(n.id);
                    for (uint32 p : n.prevActive)
                        byId[p]->next.push_back(n.id);
                }
                for (QuestNode& n : nodes)
                    if (n.nextInChain)
                        n.next.push_back(n.nextInChain);
                for (QuestNode& n : nodes)
                {
                    std::sort(n.next.begin(), n.next.end());
                    n.next.erase(std::unique(n.next.begin(), n.next.end()), n.next.end());
                }

                // Topological order (prerequisites first), ties by level then id.
                std::unordered_map<uint32, uint32> indegree;
                std::unordered_map<uint32, std::vector<uint32>> edges;
                for (QuestNode& n : nodes)
                    indegree[n.id];
                for (QuestNode& n : nodes)
                    for (uint32 next : n.next)
                    {
                        edges[n.id].push_back(next);
                        ++indegree[next];
                    }
                auto later = [&](uint32 a, uint32 b)
                {
                    QuestNode const* na = byId[a];
                    QuestNode const* nb = byId[b];
                    if (na->minLevel != nb->minLevel)
                        return na->minLevel > nb->minLevel;
                    return a > b;
                };
                std::priority_queue<uint32, std::vector<uint32>, decltype(later)> ready(later);
                for (auto const& d : indegree)
                    if (d.second == 0)
                        ready.push(d.first);
                std::set<uint32> placed;
                while (!ready.empty())
                {
                    uint32 id = ready.top();
                    ready.pop();
                    story.quests.push_back(id);
                    placed.insert(id);
                    for (uint32 next : edges[id])
                        if (--indegree[next] == 0)
                            ready.push(next);
                }
                for (uint32 id : ids)      // cycles in broken data: append the rest
                    if (!placed.count(id))
                        story.quests.push_back(id);

                for (QuestNode& n : nodes)
                {
                    if (n.prev.empty() && n.prevActive.empty())
                        story.roots.push_back(n.id);
                    if (n.next.empty())
                        story.terminals.push_back(n.id);
                    if (n.next.size() > 1)
                        story.branches.push_back(n.id);
                    if (n.exclusiveGroup > 0)
                        story.exclusiveGroups[n.exclusiveGroup].push_back(n.id);
                }
                for (auto itr = story.exclusiveGroups.begin(); itr != story.exclusiveGroups.end();)
                    itr = itr->second.size() < 2 ? story.exclusiveGroups.erase(itr) : std::next(itr);
                if (story.roots.empty())
                    story.roots.push_back(story.quests.front());

                // Identity and title from the main root: the earliest mandatory starting quest.
                uint32 mainRoot = 0;
                for (uint32 id : story.quests)
                {
                    QuestNode const* n = byId[id];
                    if (n->prev.empty() && n->prevActive.empty() && !n->optional) { mainRoot = id; break; }
                }
                if (!mainRoot)
                    mainRoot = story.roots.front();
                story.id = *std::min_element(story.roots.begin(), story.roots.end());
                story.title = StorylineTitle(byId[mainRoot]->title);
                story.summary = byId[mainRoot]->summary;

                // Apply forced requirements before counting and classifying the storyline.
                Override const* o = FindOverride(CAT_STORYLINE, story.id);
                if (o && o->mode == OVERRIDE_MANDATORY)
                    for (QuestNode& n : nodes)
                        if (n.optional && !n.disabled)
                            n.optional = false, n.optionalReason.clear();

                uint8 team = 0;
                uint32 mandatory = 0;
                story.levelMin = UINT32_MAX;
                for (QuestNode const& n : nodes)
                {
                    team |= TeamMaskOfRaces(n.reqRaces);
                    if (n.optional)
                        continue;
                    ++mandatory;
                    uint32 lvl = n.level ? n.level : n.minLevel;
                    story.levelMin = std::min(story.levelMin, std::max<uint32>(1, n.minLevel ? n.minLevel : lvl));
                    story.levelMax = std::max(story.levelMax, lvl);
                }
                if (story.levelMin == UINT32_MAX)
                    story.levelMin = 0;
                story.teamMask = team ? team : TEAM_MASK_BOTH;

                if (o && o->mode == OVERRIDE_EXCLUDE)
                {
                    ++excludedCounts["override_exclude"];
                    Exclude(zoneId, CAT_STORYLINE, story.id, story.title, "override_exclude");
                    continue;
                }
                if (!mandatory || (o && o->mode == OVERRIDE_BONUS))
                {
                    story.bonus = true;
                    story.bonusReason = o && o->mode == OVERRIDE_BONUS ? "override_bonus" : "no_reliable_quests";
                    ++bonusStorylines;
                }

                for (QuestNode& n : nodes)
                {
                    if (n.optional)
                        ++optionalQuests;
                    n.storyline = story.id;
                    m_defs->quests[n.id] = n;
                }
                ZoneDef& zone = Zone(zoneId);
                zone.storylines.push_back(story.id);
                m_defs->storylines[story.id] = story;
                ++storylineCount;
            }

            std::ostringstream line;
            line << "Storylines: " << storylineCount << " (" << bonusStorylines << " bonus-only), "
                 << optionalQuests << " optional quests (breadcrumbs, repeatables, seasonal, broken)";
            for (auto const& e : excludedCounts)
                line << ", " << e.second << " " << e.first;
            Log(line.str());
        }

        void Generator::Finish()
        {
            uint32 dropped = 0;
            for (auto itr = m_defs->zones.begin(); itr != m_defs->zones.end();)
            {
                ZoneDef& zone = itr->second;

                auto mandatoryCount = [&]()
                {
                    uint32 n = 0;
                    for (auto const& e : zone.exploration) n += !e.bonus;
                    for (uint32 s : zone.storylines) n += !m_defs->storylines[s].bonus;
                    for (auto const& r : zone.rares) n += !r.bonus;
                    for (auto const& e : zone.elites) n += !e.bonus;
                    for (auto const& t : zone.travel) n += !t.bonus;
                    return n;
                };
                if (mandatoryCount() == 0)
                {
                    ++dropped;
                    itr = m_defs->zones.erase(itr);
                    continue;
                }

                // Level range: discovery levels of its areas, else its storylines.
                zone.levelMin = UINT32_MAX;
                for (auto const& e : zone.exploration)
                    if (e.areaLevel > 0)
                    {
                        zone.levelMin = std::min(zone.levelMin, uint32(e.areaLevel));
                        zone.levelMax = std::max(zone.levelMax, uint32(e.areaLevel));
                    }
                if (zone.levelMin == UINT32_MAX)
                {
                    for (uint32 s : zone.storylines)
                    {
                        Storyline const& story = m_defs->storylines[s];
                        if (story.levelMin)
                            zone.levelMin = std::min(zone.levelMin, story.levelMin);
                        zone.levelMax = std::max(zone.levelMax, story.levelMax);
                    }
                }
                if (zone.levelMin == UINT32_MAX)
                    zone.levelMin = 0;

                std::sort(zone.exploration.begin(), zone.exploration.end(), [](auto const& a, auto const& b) { return a.areaLevel != b.areaLevel ? a.areaLevel < b.areaLevel : a.name < b.name; });
                std::sort(zone.storylines.begin(), zone.storylines.end(), [&](uint32 a, uint32 b)
                {
                    Storyline const& sa = m_defs->storylines[a];
                    Storyline const& sb = m_defs->storylines[b];
                    return sa.levelMin != sb.levelMin ? sa.levelMin < sb.levelMin : a < b;
                });
                auto byLevel = [](auto const& a, auto const& b) { return a.levelMin != b.levelMin ? a.levelMin < b.levelMin : a.name < b.name; };
                std::sort(zone.rares.begin(), zone.rares.end(), byLevel);
                std::sort(zone.elites.begin(), zone.elites.end(), byLevel);
                std::sort(zone.travel.begin(), zone.travel.end(), [](auto const& a, auto const& b) { return a.name < b.name; });

                for (uint32 i = 0; i < zone.exploration.size(); ++i)
                    m_defs->explorationByArea[zone.exploration[i].areaId] = { zone.zoneId, i };
                for (uint32 i = 0; i < zone.rares.size(); ++i)
                    m_defs->rareByEntry[zone.rares[i].entry] = { zone.zoneId, i };
                for (uint32 i = 0; i < zone.elites.size(); ++i)
                    m_defs->eliteByEntry[zone.elites[i].entry] = { zone.zoneId, i };
                for (uint32 i = 0; i < zone.travel.size(); ++i)
                    m_defs->travelByNode[zone.travel[i].nodeId] = { zone.zoneId, i };

                if (m_cfg.logGeneratorDetail)
                {
                    std::map<std::string, uint32> excl;
                    for (Exclusion const& e : zone.excluded)
                        ++excl[std::string(CategoryKey(e.cat)) + ":" + e.reason];
                    std::ostringstream line;
                    line << zone.name << ": " << zone.exploration.size() << " exploration, " << zone.storylines.size() << " storylines, "
                         << zone.rares.size() << " rares, " << zone.elites.size() << " elites, " << zone.travel.size() << " travel";
                    for (auto const& e : excl)
                        line << "; excluded " << e.second << " " << e.first;
                    Log(line.str());
                }
                ++itr;
            }

            std::ostringstream line;
            line << "Zones: " << m_defs->zones.size() << " with a checklist";
            if (dropped)
                line << ", " << dropped << " without any reliable objective";
            Log(line.str());
        }

        void Generator::BuildRegions()
        {
            std::multimap<uint32, uint32> listed;
            if (std::unique_ptr<QueryResult> result{ WorldDatabase.Query("SELECT `region_id`, `zone_id` FROM `azcomp_region_zone`") })
            {
                do
                {
                    Field* fields = result->Fetch();
                    listed.emplace(fields[0].GetUInt32(), fields[1].GetUInt32());
                } while (result->NextRow());
            }

            std::unique_ptr<QueryResult> result(WorldDatabase.Query(
                "SELECT `id`, `name`, `description`, `scope`, `map_id`, `sort_order`, `icon` FROM `azcomp_region`"));
            if (!result)
                return;

            uint32 dropped = 0;
            std::vector<std::string> unknown;
            do
            {
                Field* fields = result->Fetch();
                RegionDef region;
                region.id = fields[0].GetUInt32();
                region.name = fields[1].GetCppString();
                region.description = fields[2].GetCppString();
                region.scope = fields[3].GetUInt8();
                region.mapId = fields[4].GetUInt32();
                region.sortOrder = fields[5].GetInt32();
                region.icon = fields[6].GetCppString();

                // Zones without a checklist are left out, so a region never waits on them.
                if (region.scope == REGION_SCOPE_ZONES)
                {
                    auto range = listed.equal_range(region.id);
                    for (auto itr = range.first; itr != range.second; ++itr)
                    {
                        if (m_defs->FindZone(itr->second))
                            region.zones.push_back(itr->second);
                        else
                            unknown.push_back(region.name + ":" + std::to_string(itr->second));
                    }
                }
                else
                {
                    for (auto const& pair : m_defs->zones)
                        if (region.scope == REGION_SCOPE_ALL || pair.second.mapId == region.mapId)
                            region.zones.push_back(pair.first);
                }
                if (region.zones.empty())
                {
                    ++dropped;
                    continue;
                }
                std::sort(region.zones.begin(), region.zones.end(), [&](uint32 a, uint32 b)
                {
                    ZoneDef const& za = m_defs->zones[a];
                    ZoneDef const& zb = m_defs->zones[b];
                    return za.levelMin != zb.levelMin ? za.levelMin < zb.levelMin : za.name < zb.name;
                });
                for (uint32 z : region.zones)
                    region.levelMax = std::max(region.levelMax, m_defs->zones[z].levelMax);
                m_defs->regions[region.id] = std::move(region);
            } while (result->NextRow());

            for (auto const& pair : m_defs->regions)
                m_defs->regionOrder.push_back(pair.first);
            std::sort(m_defs->regionOrder.begin(), m_defs->regionOrder.end(), [&](uint32 a, uint32 b)
            {
                RegionDef const& ra = m_defs->regions[a];
                RegionDef const& rb = m_defs->regions[b];
                return ra.sortOrder != rb.sortOrder ? ra.sortOrder < rb.sortOrder : a < b;
            });

            std::ostringstream line;
            line << "Regions: " << m_defs->regions.size();
            if (dropped)
                line << ", " << dropped << " without any zone";
            if (!unknown.empty())
            {
                line << "; zones without a checklist left out:";
                for (std::string const& u : unknown)
                    line << " " << u;
            }
            Log(line.str());
        }

        void Generator::AssignVersions()
        {
            // Content hash of everything that changes what a player must do.
            for (auto& pair : m_defs->zones)
            {
                ZoneDef& zone = pair.second;
                uint64 h = 1469598103934665603ULL;
                for (auto const& e : zone.exploration) h = Fnv(h, "e" + std::to_string(e.areaId) + (e.bonus ? "b" : ""));
                for (uint32 s : zone.storylines)
                {
                    Storyline const& story = m_defs->storylines[s];
                    h = Fnv(h, "s" + std::to_string(s) + (story.bonus ? "b" : ""));
                    for (uint32 q : story.quests)
                        h = Fnv(h, "q" + std::to_string(q) + (m_defs->quests[q].optional ? "o" : ""));
                }
                for (auto const& r : zone.rares) h = Fnv(h, "r" + std::to_string(r.entry) + (r.bonus ? "b" : ""));
                for (auto const& e : zone.elites) h = Fnv(h, "l" + std::to_string(e.entry) + (e.bonus ? "b" : ""));
                for (auto const& t : zone.travel) h = Fnv(h, "t" + std::to_string(t.nodeId) + (t.bonus ? "b" : ""));
                zone.contentHash = h;
            }

            std::unordered_map<uint32, std::pair<uint32, uint64>> stored;
            if (std::unique_ptr<QueryResult> result{ CharacterDatabase.Query("SELECT `zone_id`, `version`, `content_hash` FROM `azcomp_zone_definition`") })
            {
                do
                {
                    Field* fields = result->Fetch();
                    stored[fields[0].GetUInt32()] = { fields[1].GetUInt32(), fields[2].GetUInt64() };
                } while (result->NextRow());
            }

            uint32 changed = 0;
            for (auto& pair : m_defs->zones)
            {
                ZoneDef& zone = pair.second;
                auto itr = stored.find(zone.zoneId);
                if (itr != stored.end() && itr->second.second == zone.contentHash)
                {
                    zone.version = itr->second.first;
                    continue;
                }
                zone.version = itr == stored.end() ? 1 : itr->second.first + 1;
                ++changed;
                uint32 objectives = uint32(zone.exploration.size() + zone.storylines.size() + zone.rares.size() + zone.elites.size() + zone.travel.size());
                CharacterDatabase.PExecute("REPLACE INTO `azcomp_zone_definition` (`zone_id`, `version`, `content_hash`, `objective_count`, `generated_at`) "
                    "VALUES (%u, %u, " UI64FMTD ", %u, " UI64FMTD ")", zone.zoneId, zone.version, zone.contentHash, objectives, uint64(m_defs->generatedAt));
            }
            if (changed)
                Log("Definition versions: " + std::to_string(changed) + " zone(s) changed since the last generation");
        }

        std::shared_ptr<Definitions> Generator::Run()
        {
            m_cfg = GetConfig();
            m_defs = std::make_shared<Definitions>();
            m_defs->generatedAt = time(nullptr);
            m_defs->generation = m_previous ? m_previous->generation + 1 : 1;
            m_maxPlayerLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);

            uint32 start = WorldTimer::getMSTime();
            LoadOverrides();
            LoadAreas();
            ScanTerrain();
            IndexSpawns();
            LoadQuestRelations();
            if (!m_terrain.LoadedTiles() || m_areaCells.empty())
                Log("WARNING: no terrain .map files found under " + sWorld.GetDataPath() + "maps/; exploration areas and zones of spawns cannot be resolved");

            BuildExploration();
            BuildTravel();
            BuildCreatures();
            BuildStorylines();
            Finish();
            BuildRegions();
            AssignVersions();

            Log("Generated in " + std::to_string(WorldTimer::getMSTimeDiff(start, WorldTimer::getMSTime())) + " ms");
            return m_defs;
        }
    }

    std::shared_ptr<Definitions> GenerateDefinitions(Definitions const* previous)
    {
        Generator generator(previous);
        return generator.Run();
    }
}
