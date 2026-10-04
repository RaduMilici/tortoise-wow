#ifndef AZC_TERRAIN_H
#define AZC_TERRAIN_H

#include "Common.h"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace Azc
{
    // Reads only the area section of the server's .map files: 16x16 area flags per 533-yard
    // tile, 512 bytes each. That answers "which area is this point in" for every spawn in the
    // world without loading heights, vmaps or mmaps (the core's TerrainInfo would load all of
    // them, which a small server cannot afford for a whole-world scan). Areas defined only by
    // WMO groups (some caves and buildings) are not visible here; callers treat this as an
    // outdoor approximation.
    class TerrainAreas
    {
    public:
        explicit TerrainAreas(std::string dataPath) : m_dataPath(std::move(dataPath)) {}

        // Area flag of the terrain cell at (x, y), or 0xFFFF when the map has no data there.
        uint16 GetAreaFlag(uint32 mapId, float x, float y);

        // Calls fn(areaFlag, x, y) for the centre of every terrain cell of the map.
        void ForEachCell(uint32 mapId, std::function<void(uint16, float, float)> const& fn);

        uint32 LoadedTiles() const { return uint32(m_tiles.size()); }

    private:
        struct Tile
        {
            bool present = false;
            bool flat = true;
            uint16 gridArea = 0;
            std::array<uint16, 256> cells{};
        };

        Tile const& GetTile(uint32 mapId, uint32 a, uint32 b);

        std::string m_dataPath;
        std::unordered_map<uint64, Tile> m_tiles;
    };
}

#endif
