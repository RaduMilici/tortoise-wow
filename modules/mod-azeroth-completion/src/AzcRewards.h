#ifndef AZC_REWARDS_H
#define AZC_REWARDS_H

#include "AzcDefs.h"

class Player;

namespace Azc
{
    // Loads azcomp_milestone_reward (world DB). Safe to call again for a reload.
    void LoadRewards();
    uint32 RewardRowCount();

    // Grants every reward configured for (zone, percent) and returns a short description
    // ("250 XP, 1g 20s") for the event and chat line. Empty when nothing is configured.
    std::string GrantMilestoneRewards(Player* player, ZoneDef const& zone, uint32 percent);

    // Describes what a milestone would grant, without granting it.
    std::string DescribeMilestoneRewards(Player* player, ZoneDef const& zone, uint32 percent);
}

#endif
