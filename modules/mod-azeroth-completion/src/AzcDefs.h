#ifndef AZC_DEFS_H
#define AZC_DEFS_H

// Azeroth Completion: the generated world checklist.
//
// Everything in this header is built once by the generator (AzcGenerator.cpp) from world data
// and then only read. A regeneration builds a fresh Definitions and swaps the pointer, so
// readers holding the old shared_ptr keep a consistent snapshot.

#include "Common.h"
#include <array>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Azc
{
    constexpr uint32 AZCOMP_PROTOCOL = 1;
    constexpr uint32 AZCOMP_PROTOCOL_MIN_CLIENT = 1;
    constexpr char const* MODULE_VERSION = "1.0.0";
    constexpr char const* ADDON_PREFIX = "AZC";

    enum Category : uint8
    {
        CAT_EXPLORATION = 0,
        CAT_STORYLINE   = 1,
        CAT_RARE        = 2,
        CAT_ELITE       = 3,
        CAT_TRAVEL      = 4,
        CAT_COUNT
    };

    // "exploration", "storylines", ... - the category keys of the zone snapshot.
    char const* CategoryKey(Category cat);
    // "EXPLORATION", "STORYLINE", ... - the category names used in events.
    char const* CategoryEventName(Category cat);
    // "exploration", "storyline", ... - the prefix of an objective id ("rare:448").
    char const* ObjectivePrefix(Category cat);
    // Display name: "Exploration", "Storylines", "Rare Hunts", "Elite Encounters", "Travel".
    char const* CategoryTitle(Category cat);
    bool ParseCategory(std::string const& text, Category& out);

    // Team bits used for faction eligibility.
    enum TeamMask : uint8
    {
        TEAM_MASK_ALLIANCE = 0x1,
        TEAM_MASK_HORDE    = 0x2,
        TEAM_MASK_BOTH     = 0x3
    };

    struct Point
    {
        uint32 map = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // One generator decision worth logging ("Excluded: 2 generic elites").
    struct Exclusion
    {
        Category cat;
        uint32 key;
        std::string name;
        std::string reason;     // machine-ish reason key, e.g. "generic_elite", "event_only"
    };

    struct ExplorationObjective
    {
        uint32 areaId = 0;
        uint32 parentAreaId = 0;        // direct parent (the zone, or a subzone for nested areas)
        uint32 exploreFlag = 0;
        std::string name;
        int32 areaLevel = 0;
        bool hasCenter = false;
        Point center;
        uint32 terrainCells = 0;        // 33-yard cells of terrain carrying this area (0 = WMO only)
        std::string hint;               // from azcomp_override
        bool bonus = false;
        std::string bonusReason;
    };

    enum StarterKind : uint8
    {
        STARTER_NONE       = 0,
        STARTER_CREATURE   = 1,
        STARTER_GAMEOBJECT = 2,
        STARTER_ITEM       = 3,
        STARTER_AUTO       = 4      // no giver at all (auto-accepted / scripted)
    };

    struct NpcRef
    {
        StarterKind kind = STARTER_NONE;
        uint32 entry = 0;
        std::string name;
        bool hasLocation = false;
        Point location;
        uint32 zoneId = 0;
        uint32 areaId = 0;
    };

    struct QuestNode
    {
        uint32 id = 0;
        std::string title;
        uint32 level = 0;
        uint32 minLevel = 0;
        uint32 maxLevel = 0;
        uint32 reqRaces = 0;
        uint32 reqClasses = 0;
        uint32 reqSkill = 0;
        uint32 reqSkillValue = 0;
        int32 zoneOrSort = 0;
        bool repeatable = false;
        bool disabled = false;          // Quest::IsActive() was false at generation time
        bool seasonal = false;          // in game_event_quest or a seasonal quest sort
        std::vector<uint32> prev;       // positive prerequisites: any one rewarded unlocks this quest
        std::vector<uint32> prevActive; // negative prerequisites: one of them must be in the quest log
        std::vector<uint32> next;       // quests that list this one as a prerequisite / chain predecessor
        int32 exclusiveGroup = 0;       // >0: one of the group; <0: all of the group
        uint32 nextInChain = 0;
        bool breadcrumb = false;        // leads to nextInChain, which does not need it
        NpcRef giver;
        NpcRef ender;
        std::vector<std::string> objectives;
        std::string summary;            // first sentence of the quest objectives text
        bool optional = false;          // never counts towards completion
        std::string optionalReason;
        uint32 storyline = 0;
    };

    struct Storyline
    {
        uint32 id = 0;                  // the root quest id; objective id "storyline:<id>"
        std::string title;
        std::string summary;
        uint32 zoneId = 0;
        std::vector<uint32> quests;     // topological order (prerequisites first)
        std::vector<uint32> roots;
        std::vector<uint32> terminals;
        std::vector<uint32> branches;   // quests with more than one follow-up
        std::map<int32, std::vector<uint32>> exclusiveGroups;   // groups > 0 among this storyline's quests
        uint8 teamMask = TEAM_MASK_BOTH;
        uint32 levelMin = 0;
        uint32 levelMax = 0;
        bool bonus = false;
        std::string bonusReason;
    };

    struct CreatureObjective
    {
        uint32 entry = 0;
        std::string name;
        std::string subname;
        uint32 levelMin = 0;
        uint32 levelMax = 0;
        uint32 rank = 0;                // CreatureEliteType
        uint32 spawnCount = 0;
        std::vector<Point> spawns;      // capped sample
        std::vector<uint32> spawnAreas; // distinct area ids
        uint32 respawnMin = 0;
        uint32 respawnMax = 0;
        std::vector<uint32> relatedQuests;
        std::vector<std::pair<uint32, float>> loot;     // notable items (uncommon+), chance %
        uint8 attackableBy = TEAM_MASK_BOTH;
        uint32 importance = 0;          // elite encounter score
        std::vector<std::string> signals;
        std::string hint;
        bool bonus = false;
        std::string bonusReason;
    };

    struct TravelObjective
    {
        uint32 nodeId = 0;
        std::string name;
        uint8 teamMask = TEAM_MASK_BOTH;
        Point position;
        uint32 areaId = 0;
    };

    struct ZoneDef
    {
        uint32 zoneId = 0;
        uint32 mapId = 0;
        std::string name;
        uint32 levelMin = 0;
        uint32 levelMax = 0;
        uint32 version = 1;
        uint64 contentHash = 0;
        std::vector<ExplorationObjective> exploration;
        std::vector<uint32> storylines;
        std::vector<CreatureObjective> rares;
        std::vector<CreatureObjective> elites;
        std::vector<TravelObjective> travel;
        std::vector<Exclusion> excluded;
    };

    struct ObjectiveRef
    {
        uint32 zoneId = 0;
        uint32 index = 0;               // index into the zone's vector (storylines: unused)
    };

    struct Definitions
    {
        time_t generatedAt = 0;
        uint32 generation = 0;
        std::map<uint32, ZoneDef> zones;
        std::unordered_map<uint32, QuestNode> quests;   // only quests that belong to a storyline
        std::unordered_map<uint32, Storyline> storylines;

        // lookups
        std::unordered_map<uint32, ObjectiveRef> explorationByArea;
        std::unordered_map<uint32, ObjectiveRef> rareByEntry;
        std::unordered_map<uint32, ObjectiveRef> eliteByEntry;
        std::unordered_map<uint32, ObjectiveRef> travelByNode;
        std::unordered_map<uint32, uint32> zoneOfArea;  // every area id -> top zone id
        std::unordered_map<uint32, std::string> areaNames;

        // generator totals for ".ac status"
        std::vector<std::string> globalLog;

        ZoneDef const* FindZone(uint32 zoneId) const
        {
            auto itr = zones.find(zoneId);
            return itr == zones.end() ? nullptr : &itr->second;
        }
        Storyline const* FindStoryline(uint32 id) const
        {
            auto itr = storylines.find(id);
            return itr == storylines.end() ? nullptr : &itr->second;
        }
        QuestNode const* FindQuest(uint32 id) const
        {
            auto itr = quests.find(id);
            return itr == quests.end() ? nullptr : &itr->second;
        }
        std::string AreaName(uint32 areaId) const
        {
            auto itr = areaNames.find(areaId);
            return itr == areaNames.end() ? std::string() : itr->second;
        }
    };

    using DefinitionsPtr = std::shared_ptr<Definitions const>;

    struct Settings
    {
        bool enabled = true;
        std::set<uint32> maps = { 0, 1 };
        std::array<uint32, CAT_COUNT> weights = { { 30, 30, 15, 15, 10 } };
        uint32 syncIntervalMs = 2000;
        uint32 minExploreCells = 3;
        uint32 maxStorylineQuests = 40;
        uint32 minStorylineQuests = 2;
        uint32 eliteMaxSpawns = 2;
        uint32 eliteMinScore = 4;
        bool worldBossesMandatory = false;
        uint32 hiddenInfo = 1;          // 0 strip, 1 send with hidden flag, 2 reveal
        bool exposeRespawn = true;
        bool exposeLoot = true;
        uint32 chatNotify = 1;          // 0 off, 1 only without addon, 2 always
        bool rewardsEnabled = true;
        bool rewardsRetroactive = false;
        std::vector<uint32> milestones = { 25, 50, 75, 100 };
        uint32 chunkSize = 200;
        uint32 maxRequestsPer10s = 30;
        uint32 suggestionCount = 6;
        float suggestionRange = 800.0f;
        bool skipBots = true;
        bool logGeneratorDetail = false;
    };

    Settings const& GetConfig();
    void LoadConfig();

    DefinitionsPtr GetDefinitions();
    void SetDefinitions(DefinitionsPtr defs);

    // "rare:448" <-> (CAT_RARE, 448)
    std::string ObjectiveId(Category cat, uint32 key);
    bool ParseObjectiveId(std::string const& text, Category& cat, uint32& key);

    uint8 TeamMaskOfRaces(uint32 races);    // 0 races = both teams
    inline uint8 TeamMaskOfQuest(QuestNode const& node) { return TeamMaskOfRaces(node.reqRaces); }

    inline uint64 MakeObjectiveKey(Category cat, uint32 key) { return (uint64(cat) << 32) | key; }
}

#endif
