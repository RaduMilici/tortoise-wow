#include "AzcDefs.h"
#include "Config/Config.h"
#include "SharedDefines.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <sstream>

namespace Azc
{
    namespace
    {
        // Map threads read the settings while ".reload config" replaces them, so a reload adds a
        // new snapshot and never touches one a reader may hold. Reloads are rare; the old
        // snapshots are kept for the life of the process.
        Settings const defaultSettings;
        std::mutex settingsMutex;
        std::deque<Settings> settingsHistory;
        std::atomic<Settings const*> currentSettings{ &defaultSettings };
        std::mutex definitionsMutex;
        DefinitionsPtr definitions;

        std::vector<uint32> ParseUIntList(std::string const& text)
        {
            std::vector<uint32> values;
            std::string token;
            std::istringstream in(text);
            while (std::getline(in, token, ','))
            {
                token.erase(0, token.find_first_not_of(" \t"));
                if (!token.empty())
                    values.push_back(uint32(std::strtoul(token.c_str(), nullptr, 10)));
            }
            return values;
        }

        uint32 UIntDefault(char const* name, uint32 def)
        {
            int32 value = sConfig.GetIntDefault(name, int32(def));
            return value < 0 ? def : uint32(value);
        }
    }

    char const* CategoryKey(Category cat)
    {
        switch (cat)
        {
            case CAT_EXPLORATION: return "exploration";
            case CAT_STORYLINE:   return "storylines";
            case CAT_RARE:        return "rares";
            case CAT_ELITE:       return "elites";
            case CAT_TRAVEL:      return "travel";
            default:              return "unknown";
        }
    }

    char const* CategoryEventName(Category cat)
    {
        switch (cat)
        {
            case CAT_EXPLORATION: return "EXPLORATION";
            case CAT_STORYLINE:   return "STORYLINE";
            case CAT_RARE:        return "RARE";
            case CAT_ELITE:       return "ELITE";
            case CAT_TRAVEL:      return "TRAVEL";
            default:              return "UNKNOWN";
        }
    }

    char const* ObjectivePrefix(Category cat)
    {
        switch (cat)
        {
            case CAT_EXPLORATION: return "exploration";
            case CAT_STORYLINE:   return "storyline";
            case CAT_RARE:        return "rare";
            case CAT_ELITE:       return "elite";
            case CAT_TRAVEL:      return "travel";
            default:              return "unknown";
        }
    }

    char const* CategoryTitle(Category cat)
    {
        switch (cat)
        {
            case CAT_EXPLORATION: return "Exploration";
            case CAT_STORYLINE:   return "Storylines";
            case CAT_RARE:        return "Rare Hunts";
            case CAT_ELITE:       return "Elite Encounters";
            case CAT_TRAVEL:      return "Travel";
            default:              return "Unknown";
        }
    }

    bool ParseCategory(std::string const& text, Category& out)
    {
        for (uint8 i = 0; i < CAT_COUNT; ++i)
        {
            Category cat = Category(i);
            if (text == CategoryKey(cat) || text == ObjectivePrefix(cat) || text == CategoryEventName(cat))
            {
                out = cat;
                return true;
            }
        }
        return false;
    }

    std::string ObjectiveId(Category cat, uint32 key)
    {
        // Exploration ids carry the "area_" infix so they read like the area they name.
        if (cat == CAT_EXPLORATION)
            return std::string(ObjectivePrefix(cat)) + ":area_" + std::to_string(key);
        return std::string(ObjectivePrefix(cat)) + ":" + std::to_string(key);
    }

    bool ParseObjectiveId(std::string const& text, Category& cat, uint32& key)
    {
        size_t colon = text.find(':');
        if (colon == std::string::npos)
            return false;
        if (!ParseCategory(text.substr(0, colon), cat))
            return false;
        std::string rest = text.substr(colon + 1);
        if (rest.compare(0, 5, "area_") == 0)
            rest = rest.substr(5);
        if (rest.empty() || rest.find_first_not_of("0123456789") != std::string::npos)
            return false;
        key = uint32(std::strtoul(rest.c_str(), nullptr, 10));
        return key != 0;
    }

    uint8 TeamMaskOfRaces(uint32 races)
    {
        if (!races)
            return TEAM_MASK_BOTH;
        uint8 mask = 0;
        if (races & RACEMASK_ALLIANCE)
            mask |= TEAM_MASK_ALLIANCE;
        if (races & RACEMASK_HORDE)
            mask |= TEAM_MASK_HORDE;
        return mask;
    }

    Settings const& GetConfig() { return *currentSettings.load(); }

    void LoadConfig()
    {
        Settings c;
        c.enabled = sConfig.GetBoolDefault("AzerothCompletion.Enable", true);

        std::vector<uint32> maps = ParseUIntList(sConfig.GetStringDefault("AzerothCompletion.Maps", "0,1"));
        c.maps = std::set<uint32>(maps.begin(), maps.end());

        c.weights[CAT_EXPLORATION] = UIntDefault("AzerothCompletion.Weight.Exploration", 30);
        c.weights[CAT_STORYLINE]   = UIntDefault("AzerothCompletion.Weight.Storylines", 30);
        c.weights[CAT_RARE]        = UIntDefault("AzerothCompletion.Weight.Rares", 15);
        c.weights[CAT_ELITE]       = UIntDefault("AzerothCompletion.Weight.Elites", 15);
        c.weights[CAT_TRAVEL]      = UIntDefault("AzerothCompletion.Weight.Travel", 10);

        c.syncIntervalMs        = std::max<uint32>(500, UIntDefault("AzerothCompletion.SyncIntervalMs", 2000));
        c.minExploreCells       = UIntDefault("AzerothCompletion.Exploration.MinTerrainCells", 3);
        c.maxStorylineQuests    = UIntDefault("AzerothCompletion.Storylines.MaxQuests", 40);
        c.minStorylineQuests    = std::max<uint32>(1, UIntDefault("AzerothCompletion.Storylines.MinQuests", 2));
        c.eliteMaxSpawns        = UIntDefault("AzerothCompletion.Elites.MaxSpawns", 2);
        c.eliteMinScore         = UIntDefault("AzerothCompletion.Elites.MinScore", 4);
        c.worldBossesMandatory  = sConfig.GetBoolDefault("AzerothCompletion.Elites.WorldBossesMandatory", false);
        c.hiddenInfo            = std::min<uint32>(2, UIntDefault("AzerothCompletion.HiddenInfo", 1));
        c.exposeRespawn         = sConfig.GetBoolDefault("AzerothCompletion.Rares.ExposeRespawn", true);
        c.exposeLoot            = sConfig.GetBoolDefault("AzerothCompletion.Rares.ExposeLoot", true);
        c.chatNotify            = std::min<uint32>(2, UIntDefault("AzerothCompletion.ChatNotify", 1));
        c.rewardsEnabled        = sConfig.GetBoolDefault("AzerothCompletion.Rewards.Enable", true);
        c.rewardsRetroactive    = sConfig.GetBoolDefault("AzerothCompletion.Rewards.Retroactive", false);
        c.milestones            = ParseUIntList(sConfig.GetStringDefault("AzerothCompletion.Rewards.Milestones", "25,50,75,100"));
        c.chunkSize             = std::min<uint32>(230, std::max<uint32>(64, UIntDefault("AzerothCompletion.Protocol.ChunkSize", 200)));
        c.maxRequestsPer10s     = std::max<uint32>(1, UIntDefault("AzerothCompletion.Protocol.MaxRequestsPer10s", 30));
        c.suggestionCount       = UIntDefault("AzerothCompletion.Suggestions.Count", 6);
        c.suggestionRange       = sConfig.GetFloatDefault("AzerothCompletion.Suggestions.Range", 800.0f);
        c.skipBots              = sConfig.GetBoolDefault("AzerothCompletion.SkipBots", true);
        c.logGeneratorDetail    = sConfig.GetBoolDefault("AzerothCompletion.Generator.LogDetail", false);

        std::vector<uint32> milestones;
        for (uint32 m : c.milestones)
            if (m > 0 && m <= 100)
                milestones.push_back(m);
        std::sort(milestones.begin(), milestones.end());
        milestones.erase(std::unique(milestones.begin(), milestones.end()), milestones.end());
        c.milestones = milestones;

        std::lock_guard<std::mutex> lock(settingsMutex);
        settingsHistory.push_back(std::move(c));
        currentSettings.store(&settingsHistory.back());
    }

    DefinitionsPtr GetDefinitions()
    {
        std::lock_guard<std::mutex> lock(definitionsMutex);
        return definitions;
    }

    void SetDefinitions(DefinitionsPtr defs)
    {
        std::lock_guard<std::mutex> lock(definitionsMutex);
        definitions = std::move(defs);
    }
}
