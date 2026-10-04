#ifndef AZEROTH_COMPLETION_PUBLIC_H
#define AZEROTH_COMPLETION_PUBLIC_H

// Public API for other modules.
//
// A milestone reward row with reward_type = 'HOOK' and text = '<name>' calls the hook
// registered under that name when a character reaches the milestone:
//
//   AzerothCompletion::RegisterRewardHook("my_title", [](Player* player, uint32 zoneId, uint32 percent)
//   {
//       ...
//   });

#include "Common.h"
#include <functional>
#include <string>

class Player;

namespace AzerothCompletion
{
    using RewardHook = std::function<void(Player* player, uint32 zoneId, uint32 percent)>;

    void RegisterRewardHook(std::string const& name, RewardHook hook);

    // Current completion of a zone for an online character (0 when unknown).
    uint32 GetZonePercent(Player* player, uint32 zoneId);
}

#endif
