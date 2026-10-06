#!/usr/bin/env python3
"""Quick C++ behavioral checks using production code with small core API doubles.

No server build, database, or external Python dependencies required.
"""
from pathlib import Path
import subprocess
import tempfile

SRC = Path(__file__).resolve().parents[1] / 'src'


def between(file, start, end):
    source = (SRC / file).read_text()
    return source[source.index(start):source.index(end, source.index(start))]


preamble = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
using uint32 = uint32_t;
using ItemPosCountVec = std::vector<int>;
constexpr int NULL_BAG=0, NULL_SLOT=0, EQUIP_ERR_OK=0;
constexpr int MAIL_NORMAL=0, MAIL_STATIONERY_GM=0;
constexpr char ADDON_PREFIX[]="AZC";
struct Config { size_t chunkSize=64; };
Config const& GetConfig() { static Config c; return c; }
struct Proto {
    uint32 ItemId=1, stack=20;
    std::string Name1="Reward";
    uint32 GetMaxStackSize() const { return stack; }
} prototype;
using ItemPrototype = Proto;
struct Player;
struct Item {
    uint32 count;
    static Item* CreateItem(uint32, uint32 count, Player*) {
        return new Item{std::min(count, prototype.stack)};
    }
    static uint32 GenerateItemRandomPropertyId(uint32) { return 0; }
    void SaveToDB(bool) {}
};
struct Player {
    bool fits=true;
    uint32 stored=0, notified=0;
    std::vector<std::string> packets;
    uint32 GetGUIDLow() { return 1; }
    int CanStoreNewItem(int, int, ItemPosCountVec& dest, uint32, uint32 count) {
        dest.push_back(count); return fits ? EQUIP_ERR_OK : 1;
    }
    Item* StoreNewItem(ItemPosCountVec const& dest, uint32, bool, uint32) {
        stored += dest.front(); static Item result{}; return &result;
    }
    void SendNewItem(Item*, uint32 count, bool, bool) { notified += count; }
    void SendAddonMessage(char const*, std::string packet) {
        // Simulate client trimming chat packets.
        while (!packet.empty() && packet.back()==' ') packet.pop_back();
        packets.push_back(packet);
    }
    std::set<uint32> rewarded;
    bool exclusiveOK=true, nextOK=true;
    bool SatisfyQuestExclusiveGroup(void const*, bool) { return exclusiveOK; }
    bool SatisfyQuestNextChain(void const*, bool) { return nextOK; }
    bool GetQuestRewardStatus(uint32 id) { return rewarded.count(id); }
};
struct Quest {
    int GetExclusiveGroup() const { return 1; }
    uint32 GetNextQuestInChain() const { return 3; }
};
struct ObjectMgr {
    std::multimap<int,uint32> groups{{1,1},{1,2}};
    Proto const* GetItemPrototype(uint32) { return &prototype; }
    auto GetExclusiveQuestGroupsMapBounds(int group) { return groups.equal_range(group); }
} sObjectMgr;
struct Log { template<class... T> void outError(T...) {} } sLog;
struct MailReceiver { explicit MailReceiver(Player*) {} };
struct MailSender { MailSender(int,uint32,int) {} };
std::vector<uint32> mailed;
struct MailDraft {
    Item* item=nullptr;
    void SetSubjectAndBody(std::string, std::string) {}
    void AddItem(Item* i) { assert(!item); item=i; }
    void SendMailTo(MailReceiver, MailSender) {
        assert(item); mailed.push_back(item->count); delete item;
    }
};
struct RewardRow { std::string type="ITEM"; int value1=1, value2=1; };
struct RewardContext { std::string mailBody="Westfall 25% milestone reward."; };
'''

item_branch = between('AzcRewards.cpp', '            if (row.type == "ITEM")', '            if (row.type == "SPELL")')
quest_branch = between('AzcProgress.cpp', '        if (!player->SatisfyQuestExclusiveGroup', '        if (quest->GetMaxLevel()')
code = preamble
code += between('AzcProtocol.cpp', '        void SendFramed(', '        void SendResponse(')
code += between('AzcProtocol.cpp', '    std::string Escape(', '    RecordWriter& RecordWriter::Rec(')
code += 'std::string Reward(Player* player, RewardRow row, bool grant) { RewardContext ctx;\n' + item_branch + '\nreturn ""; }\n'
code += '''
bool Permanent(Player* player) {
    Quest q; Quest const* quest=&q;
    struct { uint32 id=1; } node;
    bool permanent=false;
    auto fail=[&](char const*, bool p) { permanent |= p; };
''' + quest_branch + '\nreturn permanent; }\n'
code += r'''
std::string Decode(std::string const& encoded) {
    std::string out;
    for (size_t i=0;i<encoded.size();++i) {
        if (encoded[i]=='%') {
            out += char(std::stoul(encoded.substr(i+1,2), nullptr,16)); i+=2;
        } else out+=encoded[i];
    }
    return out;
}
int main() {
    for (uint32 stack : {1u, 20u}) {
        prototype.stack=stack;
        for (int count : {1, 20, 21, 45}) {
            for (bool fits : {true,false}) {
                Player p; p.fits=fits; mailed.clear();
                RewardRow row; row.value2=count;
                Reward(&p,row,true);
                uint32 total=p.stored;
                for (auto n:mailed) { assert(n>0 && n<=stack); total+=n; }
                assert(total==uint32(count));
                assert(fits ? mailed.empty() : p.stored==0);
                assert(!fits || p.notified==uint32(count));
            }
        }
    }
    Player preview; mailed.clear(); Reward(&preview,RewardRow{},false);
    assert(preview.stored==0 && mailed.empty());
    Player p; p.exclusiveOK=false;
    assert(!Permanent(&p)); // active alternative can be abandoned
    p.rewarded.insert(2); assert(Permanent(&p));
    p.rewarded.clear(); p.exclusiveOK=true; p.nextOK=false;
    assert(!Permanent(&p)); // active follow-up can be abandoned
    p.rewarded.insert(3); assert(Permanent(&p));
    p.rewarded.clear(); p.nextOK=true; assert(!Permanent(&p));
    for (std::string raw : {std::string("Name "), std::string(300,' '),
         std::string(63,'a')+"é😃 ", std::string("% ^ ; = |\n")}) {
        Player client;
        auto encoded=Escape(raw);
        SendFramed(&client,'R',123,encoded);
        std::string assembled;
        for (auto const& packet:client.packets) {
            auto start=packet.find(' ',packet.find(' ')+1)+1;
            auto chunk=packet.substr(start);
            assert(chunk.size()<=GetConfig().chunkSize);
            assert((static_cast<unsigned char>(chunk[0])&0xc0)!=0x80);
            assembled+=chunk;
        }
        assert(Decode(assembled)==raw);
    }
    std::cout << "PASS: inventory/mail quantities, reward previews, temporary quest branches, UTF-8/whitespace framing\n";
}
'''
rows_code = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
using uint32 = uint32_t;
using int8 = int8_t;
struct RewardRow { uint32 zoneId=0, percent=0; std::string type; int value1=0, value2=0; std::string text; };
std::mutex rewardsMutex;
std::vector<RewardRow> rows;
struct Log { template<class... T> void outError(T...) {} } sLog;
struct Player { std::vector<int> titles; void AwardTitle(int8 t) { titles.push_back(t); } };
''' + between('AzcRewards.cpp', '        std::vector<RewardRow> RowsFor(', '        uint32 XpFor(') + '''
std::string Title(Player* player, RewardRow const& row, bool grant) {
''' + between('AzcRewards.cpp', '            if (row.type == "TITLE")', '            if (row.type == "HOOK")') + r'''
    return "?";
}
int main() {
    rows = {{0, 50, "XP", 0, 0, ""}, {40, 50, "ITEM", 0, 0, ""}, {12, 50, "ITEM", 0, 0, ""}, {0, 100, "XP", 0, 0, ""}, {40, 100, "TITLE", 0, 0, ""}};
    auto westfall = RowsFor(40, 50);
    assert(westfall.size() == 2 && westfall[0].type == "XP" && westfall[1].zoneId == 40);  // zone rows add to the generic ones
    assert(RowsFor(1, 50).size() == 1);                                                   // other zones: generic only
    assert(RowsFor(40, 25).empty());
    Player p;
    RewardRow title{40, 100, "TITLE", 71, 0, "Sentinel of Westfall"};
    assert(Title(&p, title, false) == "title \"Sentinel of Westfall\"" && p.titles.empty());  // preview grants nothing
    Title(&p, title, true);
    assert(p.titles.size() == 1 && p.titles[0] == 71);
    for (int bad : {0, -3, 128, 200}) {
        RewardRow r = title; r.value1 = bad;
        assert(Title(&p, r, true).empty() && p.titles.size() == 1);  // never a removal or a wrapped id
    }
    std::cout << "PASS: zone reward rows add to the generic ones, TITLE rewards\n";
}
'''

with tempfile.TemporaryDirectory(prefix='azc-regressions-') as directory:
    for name, program in (('regressions', code), ('reward_rows', rows_code)):
        source = Path(directory) / f'{name}.cpp'
        binary = Path(directory) / name
        source.write_text(program)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', str(source), '-o', str(binary)], check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=5)
