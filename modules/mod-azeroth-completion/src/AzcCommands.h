#ifndef AZC_COMMANDS_H
#define AZC_COMMANDS_H

#include "Chat.h"
#include <vector>

namespace Azc
{
    // The ".ac" command tree.
    std::vector<ChatCommand> GetAcCommands();
}

#endif
