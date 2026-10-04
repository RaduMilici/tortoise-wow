#ifndef AZC_GENERATOR_H
#define AZC_GENERATOR_H

#include "AzcDefs.h"

namespace Azc
{
    // Builds the whole checklist from world data (area_template, .map terrain, spawns, quest
    // templates, taxi nodes) plus the azcomp_override table, assigns definition versions
    // (persisted in azcomp_zone_definition) and logs what was included and excluded.
    // Runs synchronously; call from the world thread.
    std::shared_ptr<Definitions> GenerateDefinitions(Definitions const* previous);
}

#endif
