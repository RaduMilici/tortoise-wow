#!/usr/bin/env python3
"""Exercise generator/evaluator override handling without a server or database."""
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def between(file, start, end):
    source = (SRC / file).read_text()
    return source[source.index(start):source.index(end, source.index(start))]


code = r'''
#include "AzcProgress.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
struct Player {
    uint32 GetMapId() const { return 0; }
    bool IsAlive() const { return true; }
    struct Taxi { bool IsTaximaskNodeKnown(uint32) const { return false; } } taxi;
    Taxi const& GetTaxi() const { return taxi; }
};
struct TaxiNodesEntry {
    uint32 map_id=0;
    std::string name[1]={"Test flight path"};
    uint32 MountCreatureID[2]={1,1};
    float x=0,y=0,z=0;
};
struct ObjectMgr {
    TaxiNodesEntry node;
    uint32 GetMaxTaxiNodeId() const { return 2; }
    TaxiNodesEntry const* GetTaxiNodeEntry(uint32) const { return &node; }
} sObjectMgr;
std::vector<uint32> sTaxiNodesMask{1};
namespace Azc {
Settings const& GetConfig() { static Settings s; return s; }
uint8 PlayerTeamMask(Player const*) { return TEAM_MASK_ALLIANCE; }
uint8 TeamMaskOfRaces(uint32) { return TEAM_MASK_BOTH; }
bool IsExplored(Player*, uint32) { return true; }
StorylineEval EvaluateStoryline(Player*, PlayerState const&, Definitions const&, Storyline const&) { return {}; }
char const* CategoryKey(Category) { return "test"; }
enum { OVERRIDE_NONE, OVERRIDE_EXCLUDE, OVERRIDE_MANDATORY, OVERRIDE_BONUS };
struct Override { uint8 mode=0; std::string hint; };
bool IsTechnicalName(std::string const&) { return false; }
class Generator {
public:
    Settings m_cfg;
    std::shared_ptr<Definitions> m_defs=std::make_shared<Definitions>();
    Override override;
    Override const* FindOverride(Category, uint32) { return &override; }
    uint32 ZoneOfPoint(uint32, float, float, uint32* area) { *area=2; return 1; }
    bool IsEligibleZone(uint32) { return true; }
    ZoneDef& Zone(uint32 id) { auto& z=m_defs->zones[id]; z.zoneId=id; return z; }
    void Exclude(uint32, Category, uint32, std::string const&, std::string const&) {}
    void Log(std::string const&) {}
    void BuildTravel();
    void Finish();
    Storyline Story(std::vector<QuestNode>& nodes) {
        Storyline story; story.id=1;
        uint32 zoneId=1, bonusStorylines=0;
        std::map<std::string,uint32> excludedCounts;
        do {
'''
code += between('AzcGenerator.cpp', '                // Apply forced requirements', '                for (QuestNode& n : nodes)\n                {\n                    if (n.optional)\n                        ++optionalQuests;')
code += '\n} while (false); return story; } };\n'
code += between('AzcGenerator.cpp', '        void Generator::BuildTravel()', '        void Generator::BuildCreatures()')
code += between('AzcGenerator.cpp', '        void Generator::Finish()', '        void Generator::BuildRegions()')
code += between('AzcProgress.cpp', '    ZoneEval EvaluateZone(', '    RegionEval EvaluateRegion(')
code += between('AzcProgress.cpp', '    RegionEval EvaluateRegion(', '    void WithState(')
code += between('AzcProgress.cpp', '    bool IsNearLore(', '    QuestEval EvaluateQuest(')
code += between('AzcGenerator.cpp', '        uint64 Fnv(', '        std::string Lower(')
code += 'void Hash(std::shared_ptr<Definitions> m_defs) {\n'
code += between('AzcGenerator.cpp', '            // Content hash of everything', '            std::unordered_map<uint32, std::pair<uint32, uint64>> stored;')
code += '}\n'
code += r'''
}
int main() {
    using namespace Azc;
    int failures=0;
    auto check=[&](bool ok, char const* label) {
        std::cout << (ok ? "PASS: " : "FAIL: ") << label << '\n'; failures+=!ok;
    };
    Player player; PlayerState state;
    for (uint8 mode : {OVERRIDE_NONE, OVERRIDE_MANDATORY, OVERRIDE_BONUS, OVERRIDE_EXCLUDE}) {
        Generator g; g.override.mode=mode; g.BuildTravel();
        auto& zone=g.Zone(1);
        ExplorationObjective exploration; exploration.areaId=2;
        zone.exploration.push_back(exploration); // already explored
        auto ev=EvaluateZone(&player,state,*g.m_defs,zone,true);
        bool required=mode==OVERRIDE_NONE || mode==OVERRIDE_MANDATORY;
        check(ev.cats[CAT_TRAVEL].total==(required ? 1u:0u), "travel requirement follows override");
        check(ev.cats[CAT_TRAVEL].bonusTotal==(mode==OVERRIDE_BONUS ? 1u:0u), "travel bonus count follows override");
        check(ev.allDone==!required, "unknown bonus flight path does not block completion");
        zone.exploration.clear();
        g.Finish();
        check(g.m_defs->zones.empty()==!required, "bonus-only zone is discarded");
    }
    for (uint8 mode : {OVERRIDE_NONE, OVERRIDE_MANDATORY, OVERRIDE_BONUS}) {
        Generator g; g.override.mode=mode;
        std::vector<QuestNode> nodes(2);
        for (auto& q:nodes) { q.optional=true; q.minLevel=10; q.level=12; }
        nodes[1].disabled=true;
        auto story=g.Story(nodes);
        check(story.bonus==(mode!=OVERRIDE_MANDATORY), "force-mandatory overrides all-optional classification");
        check(nodes[0].optional==(mode!=OVERRIDE_MANDATORY), "force-mandatory counts enabled optional quests");
        check(nodes[1].optional, "force-mandatory keeps disabled quests optional");
        if (mode==OVERRIDE_MANDATORY)
            check(story.levelMin==10 && story.levelMax==12, "forced story level range includes counted quests");
    }
    Generator disabled; disabled.override.mode=OVERRIDE_MANDATORY;
    std::vector<QuestNode> nodes(2);
    for (auto& q:nodes) { q.optional=true; q.disabled=true; }
    check(disabled.Story(nodes).bonus, "all-disabled storyline stays bonus even when forced");
    Generator mandatory, bonus;
    mandatory.override.mode=OVERRIDE_MANDATORY;
    bonus.override.mode=OVERRIDE_BONUS;
    mandatory.BuildTravel(); bonus.BuildTravel();
    Hash(mandatory.m_defs); Hash(bonus.m_defs);
    check(mandatory.Zone(1).contentHash!=bonus.Zone(1).contentHash, "changing travel bonus status changes definition hash");

    // Regions: a zone with nothing for this character does not count; completed zones do.
    {
        Definitions defs;
        ZoneDef& done = defs.zones[1]; done.zoneId=1;
        ExplorationObjective explored; explored.areaId=10; done.exploration.push_back(explored);
        ZoneDef& horde = defs.zones[2]; horde.zoneId=2;
        TravelObjective hordeNode; hordeNode.nodeId=5; hordeNode.teamMask=TEAM_MASK_HORDE; horde.travel.push_back(hordeNode);
        ZoneDef& open = defs.zones[3]; open.zoneId=3;
        TravelObjective node; node.nodeId=6; node.teamMask=TEAM_MASK_BOTH; open.travel.push_back(node);
        RegionDef region; region.id=1; region.zones={1, 2};
        PlayerState st; st.zonesEarned[1]={1, 1};
        ZoneBriefCache cache;
        RegionEval re=EvaluateRegion(&player, st, defs, region, false, &cache);
        check(re.total==1 && re.done==1 && re.complete, "zone with nothing for the character does not hold a region back");
        region.zones={1, 2, 3};
        re=EvaluateRegion(&player, st, defs, region, true, &cache);
        check(re.total==2 && re.done==1 && !re.complete, "unfinished applicable zone keeps a region open");
        check(re.zones.size()==3 && !re.zones[1].applicable && re.zones[2].applicable, "region lists every zone with its applicability");
        st.regionsEarned[1]=RegionClaim{ 5, {} };
        re=EvaluateRegion(&player, st, defs, region, false, &cache);
        check(re.earned && !re.complete, "earned region stays earned while new zones are open");
    }

    // Lore: extra by default, secrets hidden until found, counts survive regeneration.
    {
        Definitions defs;
        ZoneDef& zone = defs.zones[1]; zone.zoneId=1;
        ExplorationObjective explored; explored.areaId=10; zone.exploration.push_back(explored);
        LoreObjective book; book.key=100; book.name="Book";
        LoreObjective secret; secret.key=101; secret.name="Grave"; secret.secret=true; secret.bonusReason="secret";
        zone.lore={book, secret};
        defs.loreByKey[100]={1, 0}; defs.loreByKey[101]={1, 1};
        PlayerState st;
        ZoneEval ev=EvaluateZone(&player, st, defs, zone, false);
        check(ev.allDone && ev.percent==100, "unfound bonus lore does not hold a zone back");
        check(!ev.cats[CAT_LORE].visible && ev.cats[CAT_LORE].bonusTotal==2, "bonus-only lore is listed as bonus");
        check(!ev.cats[CAT_LORE].objectives[0].hidden && ev.cats[CAT_LORE].objectives[1].hidden, "only unfound secrets are hidden");
        st.records[MakeObjectiveKey(CAT_LORE, 101)].zoneId=1;
        ev=EvaluateZone(&player, st, defs, zone, false);
        check(ev.cats[CAT_LORE].bonusDone==1 && !ev.cats[CAT_LORE].objectives[1].hidden, "a found secret is revealed");
        auto found=LoreFound(st, defs);
        check(found[LORE_KIND_ANY]==1 && found[LORE_KIND_SECRET]==1, "found lore is counted by kind");
        st.records[MakeObjectiveKey(CAT_LORE, 999)].zoneId=1;       // dropped by a later generation
        st.records[MakeObjectiveKey(CAT_RARE, 100)].zoneId=1;       // same key, other category
        found=LoreFound(st, defs);
        check(found[LORE_KIND_ANY]==2 && found[LORE_KIND_SECRET]==1, "lore found earlier still counts after it is dropped");
        zone.lore[0].bonus=false;
        ev=EvaluateZone(&player, st, defs, zone, false);
        check(ev.cats[CAT_LORE].visible && !ev.allDone && ev.percent<100, "mandatory lore counts towards the zone");

        Generator plain, required;
        plain.Zone(1).lore={book};
        required.Zone(1).lore={book}; required.Zone(1).lore[0].bonus=false;
        Generator none; none.Zone(1);
        Hash(plain.m_defs); Hash(required.m_defs); Hash(none.m_defs);
        check(plain.Zone(1).contentHash==none.Zone(1).contentHash, "bonus lore leaves the definition hash alone");
        check(required.Zone(1).contentHash!=none.Zone(1).contentHash, "mandatory lore changes the definition hash");
        Generator onlyLore; onlyLore.Zone(1).lore={book};
        onlyLore.Finish();
        check(onlyLore.m_defs->zones.empty(), "a zone with only bonus lore gets no checklist");
        Generator indexed; auto& iz=indexed.Zone(1);
        ExplorationObjective e2; e2.areaId=11; iz.exploration.push_back(e2);
        iz.lore={book, secret};
        indexed.Finish();
        check(indexed.m_defs->loreByKey.size()==2 && indexed.m_defs->loreTotals[LORE_KIND_ANY]==2 &&
              indexed.m_defs->loreTotals[LORE_KIND_SECRET]==1, "lore is indexed and totalled");
    }

    // Finding lore: within range of any copy, on the same map, further for big objects.
    {
        LoreObjective l;
        l.spawns = { Point{ 0, 100.0f, 100.0f, 10.0f }, Point{ 0, 500.0f, 500.0f, 0.0f } };
        check(IsNearLore(l, 0, 104.0f, 103.0f, 10.0f, 6.0f), "within range of a lore object finds it");
        check(!IsNearLore(l, 0, 105.0f, 104.0f, 10.0f, 6.0f), "just out of range does not");
        check(!IsNearLore(l, 0, 100.0f, 100.0f, 20.0f, 6.0f), "a floor above or below does not");
        check(IsNearLore(l, 0, 500.0f, 505.0f, 0.0f, 6.0f), "any copy in the zone will do");
        check(!IsNearLore(l, 1, 100.0f, 100.0f, 10.0f, 6.0f), "the same spot on another map does not");
        l.scale = 2.0f;
        check(IsNearLore(l, 0, 110.0f, 100.0f, 10.0f, 6.0f), "a big monument is found from further away");
        check(!IsNearLore(l, 0, 113.0f, 100.0f, 10.0f, 6.0f), "but not from anywhere");
    }
    return failures ? 1:0;
}
'''
with tempfile.TemporaryDirectory(prefix='azc-overrides-') as directory:
    directory = Path(directory)
    (directory / 'Common.h').write_text('#include <cstdint>\nusing uint8=uint8_t; using uint32=uint32_t; using uint64=uint64_t; using int32=int32_t;\n')
    source = directory / 'overrides.cpp'
    source.write_text(code)
    binary = directory / 'overrides'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-I'+str(directory), '-I'+str(SRC), str(source), '-o', str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=5)
