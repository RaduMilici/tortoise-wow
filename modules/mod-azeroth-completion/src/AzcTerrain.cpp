#include "AzcTerrain.h"
#include "Maps/GridMapDefines.h"
#include <cstdio>
#include <cstring>

namespace Azc
{
    namespace
    {
        constexpr float TILE_SIZE = 533.33333f;
        constexpr uint32 TILES = 64;
        constexpr uint16 NO_AREA = 0xFFFF;
    }

    TerrainAreas::Tile const& TerrainAreas::GetTile(uint32 mapId, uint32 a, uint32 b)
    {
        uint64 key = (uint64(mapId) << 16) | (a << 8) | b;
        auto itr = m_tiles.find(key);
        if (itr != m_tiles.end())
            return itr->second;

        Tile& tile = m_tiles[key];

        // Same naming as TerrainInfo::LoadMapAndVMap: map, then the index derived from world x,
        // then the one derived from world y.
        char name[32];
        snprintf(name, sizeof(name), "maps/%03u%02u%02u.map", mapId, a, b);
        std::string path = m_dataPath + name;

        FILE* in = fopen(path.c_str(), "rb");
        if (!in)
            return tile;

        GridMapFileHeader header;
        if (fread(&header, sizeof(header), 1, in) == 1 && memcmp(&header.mapMagic, "MAPS", 4) == 0 && header.areaMapOffset)
        {
            GridMapAreaHeader areaHeader;
            if (fseek(in, header.areaMapOffset, SEEK_SET) == 0 &&
                fread(&areaHeader, sizeof(areaHeader), 1, in) == 1 &&
                memcmp(&areaHeader.fourcc, "AREA", 4) == 0)
            {
                tile.present = true;
                tile.gridArea = areaHeader.gridArea;
                tile.flat = (areaHeader.flags & MAP_AREA_NO_AREA) != 0;
                if (!tile.flat && fread(tile.cells.data(), sizeof(uint16), 256, in) != 256)
                    tile.present = false;
            }
        }
        fclose(in);
        return tile;
    }

    uint16 TerrainAreas::GetAreaFlag(uint32 mapId, float x, float y)
    {
        float fa = 32.0f - x / TILE_SIZE;
        float fb = 32.0f - y / TILE_SIZE;
        if (fa < 0.0f || fb < 0.0f || fa >= float(TILES) || fb >= float(TILES))
            return NO_AREA;

        Tile const& tile = GetTile(mapId, uint32(fa), uint32(fb));
        if (!tile.present)
            return NO_AREA;
        if (tile.flat)
            return tile.gridArea;

        // GridMap::getArea
        int lx = int(16.0f * fa) & 15;
        int ly = int(16.0f * fb) & 15;
        return tile.cells[lx * 16 + ly];
    }

    void TerrainAreas::ForEachCell(uint32 mapId, std::function<void(uint16, float, float)> const& fn)
    {
        for (uint32 a = 0; a < TILES; ++a)
        {
            for (uint32 b = 0; b < TILES; ++b)
            {
                Tile const& tile = GetTile(mapId, a, b);
                if (!tile.present)
                    continue;

                for (uint32 lx = 0; lx < 16; ++lx)
                {
                    for (uint32 ly = 0; ly < 16; ++ly)
                    {
                        uint16 flag = tile.flat ? tile.gridArea : tile.cells[lx * 16 + ly];
                        float x = (32.0f - (float(a) + (float(lx) + 0.5f) / 16.0f)) * TILE_SIZE;
                        float y = (32.0f - (float(b) + (float(ly) + 0.5f) / 16.0f)) * TILE_SIZE;
                        fn(flag, x, y);
                    }
                }
            }
        }
    }
}
