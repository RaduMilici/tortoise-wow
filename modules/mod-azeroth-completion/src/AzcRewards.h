#ifndef AZC_REWARDS_H
#define AZC_REWARDS_H

#include "AzcDefs.h"

class Player;

namespace Azc
{
    // What a milestone grants (or granted): `text` describes everything ("250 XP, 5x Candle"),
    // `extra` only the parts that are not items, `items` the items as "id:count,id:count".
    struct RewardSummary
    {
        std::string text;
        std::string extra;
        std::string items;
    };

    // Loads azcomp_milestone_reward (world DB). Safe to call again for a reload.
    void LoadRewards();
    uint32 RewardRowCount();

    // Grants every reward configured for (zone, percent) and describes what was granted.
    // All fields are empty when nothing is configured.
    RewardSummary GrantMilestoneRewards(Player* player, ZoneDef const& zone, uint32 percent);

    // Describes what a milestone would grant, without granting it.
    RewardSummary DescribeMilestoneRewards(Player* player, ZoneDef const& zone, uint32 percent);
}

#endif
