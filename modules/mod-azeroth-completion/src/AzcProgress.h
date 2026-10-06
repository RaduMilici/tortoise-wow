#ifndef AZC_PROGRESS_H
#define AZC_PROGRESS_H

#include "AzcDefs.h"
#include "AzcRewards.h"
#include <deque>
#include <functional>
#include <unordered_set>

class Player;
class Creature;

namespace Azc
{
    enum CompletionSource : uint8
    {
        SOURCE_UNKNOWN = 0,
        SOURCE_RETRO   = 1,     // derived from existing character data at login
        SOURCE_EXPLORE = 2,
        SOURCE_KILL    = 3,
        SOURCE_QUEST   = 4,
        SOURCE_TAXI    = 5,
        SOURCE_ADMIN   = 6,
        SOURCE_LORE    = 7,     // walked up to a lore object
        SOURCE_SECRET  = 8      // preserves secret credit when definitions change
    };
    char const* SourceName(uint8 source);

    struct CompletionRecord
    {
        time_t at = 0;
        uint32 version = 0;
        uint8 source = SOURCE_UNKNOWN;
        uint32 zoneId = 0;
    };

    enum QuestState : uint8
    {
        QUEST_STATE_DONE           = 0,
        QUEST_STATE_ACTIVE         = 1,
        QUEST_STATE_AVAILABLE      = 2,
        QUEST_STATE_BLOCKED        = 3,     // obtainable later
        QUEST_STATE_NOT_APPLICABLE = 4      // never for this character (class, faction, lost branch...)
    };
    char const* QuestStateName(QuestState state);

    struct QuestEval
    {
        QuestNode const* node = nullptr;
        QuestState state = QUEST_STATE_BLOCKED;
        std::string reason;                 // primary machine-readable reason (empty when done/active/available)
        std::vector<std::string> reasons;   // every failed requirement
        std::vector<uint32> missingPrereqs;
        bool counted = false;               // part of the storyline's completion for this character
    };

    struct StorylineEval
    {
        Storyline const* story = nullptr;
        std::vector<QuestEval> quests;      // in story->quests order
        uint32 unitsDone = 0;               // an exclusive (one-of) group counts as one unit
        uint32 unitsTotal = 0;
        uint32 questsDone = 0;              // counted quests done (display "4 / 7")
        uint32 questsTotal = 0;
        bool applicable = false;
        bool complete = false;
        time_t completedAt = 0;
        uint32 nextQuest = 0;
        std::vector<uint32> blocked;

        QuestEval const* Find(uint32 questId) const
        {
            for (QuestEval const& q : quests)
                if (q.node->id == questId)
                    return &q;
            return nullptr;
        }
    };

    struct ObjectiveEval
    {
        Category cat = CAT_EXPLORATION;
        uint32 key = 0;
        uint32 index = 0;                   // index in the zone's category vector
        std::string name;
        bool done = false;
        time_t at = 0;
        bool bonus = false;
        bool hidden = false;                // the addon may show "???"
        int32 storyIndex = -1;              // storylines: index into ZoneEval::stories
    };

    struct CategoryEval
    {
        uint32 done = 0;
        uint32 total = 0;
        uint32 bonusDone = 0;
        uint32 bonusTotal = 0;
        uint32 weight = 0;                  // effective weight after redistribution (0 when hidden)
        uint32 percent = 0;
        bool visible = false;
        std::vector<ObjectiveEval> objectives;  // applicable ones only (mandatory + bonus)
    };

    struct ZoneEval
    {
        ZoneDef const* zone = nullptr;
        std::array<CategoryEval, CAT_COUNT> cats;
        std::vector<StorylineEval> stories;
        uint32 percent = 0;
        bool allDone = false;
        bool earned = false;                // completion earned at some point; never revoked
        time_t earnedAt = 0;
        uint32 earnedVersion = 0;
        uint32 newSinceEarned = 0;          // objectives added by a later definition version
    };

    // One zone as part of a region.
    struct RegionZoneEval
    {
        ZoneDef const* zone = nullptr;
        bool earned = false;                // completed at some point (never revoked)
        bool applicable = false;            // has something for this character (else it does not count)
        uint32 percent = 0;                 // only filled when asked for
    };

    struct RegionEval
    {
        RegionDef const* region = nullptr;
        std::vector<RegionZoneEval> zones;
        uint32 done = 0;                    // applicable zones completed
        uint32 total = 0;                   // applicable zones
        bool complete = false;              // every applicable zone completed
        bool earned = false;                // region completion recorded (never revoked)
        time_t earnedAt = 0;
    };

    // A one-time claim: a completed region or a lore count reached.
    struct RegionClaim
    {
        time_t at = 0;
        RewardSummary reward;               // what it granted (empty for old or unrewarded claims)
    };
    using Claim = RegionClaim;

    struct Event
    {
        std::string type;
        std::vector<std::pair<std::string, std::string>> fields;
        std::string chat;                   // one-line text for players without the addon
    };

    struct PlayerState
    {
        uint32 guid = 0;
        bool addonActive = false;
        uint32 addonProtocol = 0;
        std::unordered_map<uint64, CompletionRecord> records;
        std::map<uint32, std::pair<time_t, uint32>> zonesEarned;    // zone -> (at, version)
        std::map<std::pair<uint32, uint32>, RewardSummary> milestones;  // (zone, percent) -> what the claim granted
        std::map<uint32, RegionClaim> regionsEarned;
        std::map<std::pair<uint8, uint32>, Claim> loreClaims;  // (LoreKind, count) -> claim; kept across resets

        // runtime only
        uint32 tickTimer = 0;
        uint32 forceTimer = 0;
        uint32 lastZone = 0;
        uint32 lastLevel = 0;
        uint64 exploredSig = 0;
        uint64 taxiSig = 0;
        uint64 questSig = 0;
        uint32 generation = 0;
        bool resync = false;                // rebuild without rewards after reset; persisted as milestone (0, 0)
        bool regionCheck = false;           // a zone was completed: look at the regions
        std::map<uint32, uint32> storyUnits;
        std::set<std::pair<uint32, uint8>> catComplete;
        std::map<uint32, std::set<uint32>> availableQuests;
        std::set<uint32> baselined;
        std::deque<uint32> requestTimes;
        uint32 eventSeq = 0;

        CompletionRecord const* Find(Category cat, uint32 key) const
        {
            auto itr = records.find(MakeObjectiveKey(cat, key));
            return itr == records.end() ? nullptr : &itr->second;
        }
    };

    uint8 PlayerTeamMask(Player const* player);

    // Closest copy within the horizontal suggestion range on the same map.
    Point const* NearestLoreSpawn(LoreObjective const& lore, uint32 map, float x, float y, float range);

    // Whether a character at (map, x, y, z) is close enough to any copy of a lore object to find
    // it: within `range` yards of its centre, scaled up by the object's size.
    bool IsNearLore(LoreObjective const& lore, uint32 map, float x, float y, float z, float range);

    // Lore objectives found by the character, by kind (every lore object, secrets).
    std::array<uint32, LORE_KIND_COUNT> LoreFound(PlayerState const& state, Definitions const& defs);

    // All of these lock the progress store internally; callers pass the live Player.
    void ProgressOnLogin(Player* player);
    void ProgressOnLogout(Player* player);
    void ProgressOnDelete(uint32 guidLow);
    void ProgressOnUpdate(Player* player, uint32 diff);
    void ProgressOnKill(Player* player, Creature* creature);
    void ProgressOnZoneChange(Player* player);
    void ProgressOnDefinitionsChanged(DefinitionsPtr const& oldDefs, DefinitionsPtr const& newDefs);
    uint32 ProgressTrackedPlayers();

    // Runs fn with the player's state under the store lock (nullptr state if not tracked).
    void WithState(Player* player, std::function<void(PlayerState*)> const& fn);

    // Pure evaluation; caller must hold the store (use inside WithState).
    ZoneEval EvaluateZone(Player* player, PlayerState const& state, Definitions const& defs, ZoneDef const& zone, bool withQuestDetail = true);
    StorylineEval EvaluateStoryline(Player* player, PlayerState const& state, Definitions const& defs, Storyline const& story);
    QuestEval EvaluateQuest(Player* player, QuestNode const& node);
    // withPercent also evaluates completed zones, for display. `cache` (zone -> percent, applicable)
    // lets several regions share the zone evaluations.
    using ZoneBriefCache = std::unordered_map<uint32, std::pair<uint32, bool>>;
    RegionEval EvaluateRegion(Player* player, PlayerState const& state, Definitions const& defs, RegionDef const& region, bool withPercent,
        ZoneBriefCache* cache = nullptr);

    // Admin: forget one character's progress in one zone (0 = every zone). Works offline.
    void ResetProgress(uint32 guidLow, uint32 zoneId);
}

#endif
