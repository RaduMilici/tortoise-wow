#include "AzcRewards.h"
#include "AzerothCompletion.h"
#include "Database/DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ReputationMgr.h"
#include "World.h"
#include <algorithm>
#include <mutex>

namespace Azc
{
    namespace
    {
        struct RewardRow
        {
            uint32 zoneId = 0;      // 0 = every zone
            uint32 percent = 0;
            std::string type;
            int32 value1 = 0;
            int32 value2 = 0;
            std::string text;
        };

        // Who is being rewarded for what: a zone milestone or a completed region.
        struct RewardContext
        {
            uint32 zoneId = 0;      // 0 for a region
            uint32 regionId = 0;
            uint32 percent = 0;
            uint32 levelMax = 0;
            std::string mailBody;   // for reward items sent by mail
        };

        std::mutex rewardsMutex;
        std::vector<RewardRow> rows;
        std::vector<RewardRow> regionRows;     // zoneId holds the region id
        std::vector<RewardRow> loreRows;       // zoneId holds the lore kind, percent the count
        std::map<std::string, AzerothCompletion::RewardHook> hooks;

        std::string Money(uint32 copper)
        {
            std::string out;
            if (copper >= 10000) out += std::to_string(copper / 10000) + "g ";
            if (copper % 10000 >= 100) out += std::to_string(copper % 10000 / 100) + "s ";
            if (copper % 100 || out.empty()) out += std::to_string(copper % 100) + "c ";
            out.pop_back();
            return out;
        }

        std::vector<RewardRow> RowsFor(uint32 zoneId, uint32 percent)
        {
            std::lock_guard<std::mutex> lock(rewardsMutex);
            // The generic (zone 0) rows first, then the zone's own rows on top of them.
            std::vector<RewardRow> out;
            for (RewardRow const& row : rows)
                if (row.percent == percent && row.zoneId == 0)
                    out.push_back(row);
            for (RewardRow const& row : rows)
                if (row.percent == percent && row.zoneId != 0 && row.zoneId == zoneId)
                    out.push_back(row);
            return out;
        }

        uint32 XpFor(Player* player, RewardRow const& row)
        {
            if (row.type == "XP")
                return uint32(std::max(0, row.value1));
            // XP_PCT: percent of the character's current level bar
            return player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) * uint32(std::max(0, row.value1)) / 100;
        }

        std::vector<RewardRow> RowsForRegion(uint32 regionId)
        {
            std::lock_guard<std::mutex> lock(rewardsMutex);
            std::vector<RewardRow> out;
            for (RewardRow const& row : regionRows)
                if (row.zoneId == regionId)
                    out.push_back(row);
            return out;
        }

        std::vector<RewardRow> RowsForLore(LoreKind kind, uint32 count)
        {
            std::lock_guard<std::mutex> lock(rewardsMutex);
            std::vector<RewardRow> out;
            for (RewardRow const& row : loreRows)
                if (row.zoneId == kind && row.percent == count)
                    out.push_back(row);
            return out;
        }

        uint32 MoneyFor(RewardContext const& ctx, RewardRow const& row)
        {
            if (row.type == "MONEY")
                return uint32(std::max(0, row.value1));
            // MONEY_PER_LEVEL: scales with the zone, so high zones pay more
            return uint32(std::max(0, row.value1)) * std::max<uint32>(1, ctx.levelMax);
        }

        std::string Apply(Player* player, RewardContext const& ctx, RewardRow const& row, bool grant)
        {
            if (row.type == "XP" || row.type == "XP_PCT")
            {
                uint32 xp = XpFor(player, row);
                if (!xp || player->GetLevel() >= sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
                    return "";
                if (grant)
                    player->GiveXP(xp, nullptr);
                return std::to_string(xp) + " XP";
            }
            if (row.type == "MONEY" || row.type == "MONEY_PER_LEVEL")
            {
                uint32 copper = MoneyFor(ctx, row);
                if (!copper)
                    return "";
                if (grant)
                    player->ModifyMoney(int32(copper));
                return Money(copper);
            }
            if (row.type == "REPUTATION")
            {
                FactionEntry const* faction = sObjectMgr.GetFactionEntry(uint32(row.value1));
                if (!faction || !row.value2)
                    return "";
                if (grant)
                    player->GetReputationMgr().ModifyReputation(faction, row.value2);
                return std::to_string(row.value2) + " reputation with " + faction->name[0];
            }
            if (row.type == "ITEM")
            {
                ItemPrototype const* proto = sObjectMgr.GetItemPrototype(uint32(row.value1));
                uint32 count = uint32(std::max(1, row.value2));
                if (!proto)
                    return "";
                if (grant)
                {
                    // Preflight the whole reward; the starting-equipment helper can equip
                    // part of it before failing, which would duplicate those items by mail.
                    ItemPosCountVec dest;
                    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, proto->ItemId, count) == EQUIP_ERR_OK)
                    {
                        if (Item* item = player->StoreNewItem(dest, proto->ItemId, true, Item::GenerateItemRandomPropertyId(proto->ItemId)))
                            player->SendNewItem(item, count, true, false);
                    }
                    else
                    {
                        // CreateItem clamps to one stack; vanilla mail permits one
                        // attachment, so send a separate mail for each remaining stack.
                        uint32 remaining = count;
                        while (remaining)
                        {
                            uint32 stack = std::min(remaining, proto->GetMaxStackSize());
                            Item* item = Item::CreateItem(proto->ItemId, stack, player);
                            if (!item)
                            {
                                sLog.outError("[mod-azeroth-completion] Could not create reward item %u for player %u.", proto->ItemId, player->GetGUIDLow());
                                break;
                            }
                            item->SaveToDB(true);
                            MailDraft draft;
                            draft.SetSubjectAndBody("Azeroth Completion", ctx.mailBody);
                            draft.AddItem(item);
                            draft.SendMailTo(MailReceiver(player), MailSender(MAIL_NORMAL, uint32(0), MAIL_STATIONERY_GM));
                            remaining -= stack;
                        }
                    }
                }
                return (count > 1 ? std::to_string(count) + "x " : std::string()) + proto->Name1;
            }
            if (row.type == "SPELL")
            {
                if (!row.value1)
                    return "";
                if (grant)
                    player->CastSpell(player, uint32(row.value1), true);
                return row.text;
            }
            if (row.type == "TITLE")
            {
                // Player::AwardTitle takes an int8 and treats negative ids as removals.
                if (row.value1 <= 0 || row.value1 > 127)
                {
                    if (grant)
                        sLog.outError("[mod-azeroth-completion] TITLE reward %i is out of range (1-127).", row.value1);
                    return "";
                }
                if (grant)
                    player->AwardTitle(int8(row.value1));
                return "title \"" + row.text + "\"";
            }
            if (row.type == "HOOK")
            {
                AzerothCompletion::RewardHook hook;
                {
                    std::lock_guard<std::mutex> lock(rewardsMutex);
                    auto itr = hooks.find(row.text);
                    if (itr != hooks.end())
                        hook = itr->second;
                }
                if (!hook)
                {
                    if (grant)
                        sLog.outError("[mod-azeroth-completion] Reward hook '%s' is not registered.", row.text.c_str());
                    return "";
                }
                // A region reward calls the hook with zone 0 and the region id as "percent",
                // a lore reward with zone 0 and the count.
                if (grant)
                    hook(player, ctx.zoneId, ctx.regionId ? ctx.regionId : ctx.percent);
                return "";
            }
            if (grant)
                sLog.outError("[mod-azeroth-completion] Unknown reward_type '%s' in azcomp_milestone_reward.", row.type.c_str());
            return "";
        }

        RewardSummary Run(Player* player, RewardContext const& ctx, std::vector<RewardRow> const& list, bool grant)
        {
            RewardSummary out;
            for (RewardRow const& row : list)
            {
                std::string part = Apply(player, ctx, row, grant);
                if (part.empty())
                    continue;
                out.text += (out.text.empty() ? "" : ", ") + part;
                if (row.type == "ITEM")
                    out.items += (out.items.empty() ? "" : ",") + std::to_string(row.value1) + ":" + std::to_string(std::max(1, row.value2));
                else
                    out.extra += (out.extra.empty() ? "" : ", ") + part;
            }
            return out;
        }

        RewardSummary RunZone(Player* player, ZoneDef const& zone, uint32 percent, bool grant)
        {
            RewardContext ctx;
            ctx.zoneId = zone.zoneId;
            ctx.percent = percent;
            ctx.levelMax = zone.levelMax;
            ctx.mailBody = zone.name + " " + std::to_string(percent) + "% milestone reward.";
            return Run(player, ctx, RowsFor(zone.zoneId, percent), grant);
        }

        RewardSummary RunRegion(Player* player, RegionDef const& region, bool grant)
        {
            RewardContext ctx;
            ctx.regionId = region.id;
            ctx.percent = 100;
            ctx.levelMax = region.levelMax;
            ctx.mailBody = region.name + " completion reward.";
            return Run(player, ctx, RowsForRegion(region.id), grant);
        }

        RewardSummary RunLore(Player* player, LoreKind kind, uint32 count, bool grant)
        {
            RewardContext ctx;
            ctx.percent = count;
            ctx.levelMax = player->GetLevel();
            ctx.mailBody = std::string(kind == LORE_KIND_SECRET ? "Secrets" : "Lore") + " reward: " + std::to_string(count) + " found.";
            return Run(player, ctx, RowsForLore(kind, count), grant);
        }
    }

    void LoadRewards()
    {
        std::vector<RewardRow> loaded;
        if (std::unique_ptr<QueryResult> result{ WorldDatabase.Query(
            "SELECT `zone_id`, `percent`, `reward_type`, `value1`, `value2`, `text` FROM `azcomp_milestone_reward`") })
        {
            do
            {
                Field* f = result->Fetch();
                RewardRow row;
                row.zoneId = f[0].GetUInt32();
                row.percent = f[1].GetUInt32();
                row.type = f[2].GetCppString();
                std::transform(row.type.begin(), row.type.end(), row.type.begin(), ::toupper);
                row.value1 = f[3].GetInt32();
                row.value2 = f[4].GetInt32();
                row.text = f[5].GetCppString();
                loaded.push_back(row);
            } while (result->NextRow());
        }
        std::vector<RewardRow> loadedRegion;
        if (std::unique_ptr<QueryResult> result{ WorldDatabase.Query(
            "SELECT `region_id`, `reward_type`, `value1`, `value2`, `text` FROM `azcomp_region_reward` ORDER BY `id`") })
        {
            do
            {
                Field* f = result->Fetch();
                RewardRow row;
                row.zoneId = f[0].GetUInt32();
                row.percent = 100;
                row.type = f[1].GetCppString();
                std::transform(row.type.begin(), row.type.end(), row.type.begin(), ::toupper);
                row.value1 = f[2].GetInt32();
                row.value2 = f[3].GetInt32();
                row.text = f[4].GetCppString();
                loadedRegion.push_back(row);
            } while (result->NextRow());
        }
        std::vector<RewardRow> loadedLore;
        if (std::unique_ptr<QueryResult> result{ WorldDatabase.Query(
            "SELECT `kind`, `count`, `reward_type`, `value1`, `value2`, `text` FROM `azcomp_lore_reward` ORDER BY `id`") })
        {
            do
            {
                Field* f = result->Fetch();
                RewardRow row;
                row.zoneId = f[0].GetUInt32();
                row.percent = f[1].GetUInt32();
                row.type = f[2].GetCppString();
                std::transform(row.type.begin(), row.type.end(), row.type.begin(), ::toupper);
                row.value1 = f[3].GetInt32();
                row.value2 = f[4].GetInt32();
                row.text = f[5].GetCppString();
                if (row.zoneId < LORE_KIND_COUNT && row.percent > 0)
                    loadedLore.push_back(row);
            } while (result->NextRow());
        }
        std::lock_guard<std::mutex> lock(rewardsMutex);
        rows = std::move(loaded);
        regionRows = std::move(loadedRegion);
        loreRows = std::move(loadedLore);
    }

    uint32 RewardRowCount()
    {
        std::lock_guard<std::mutex> lock(rewardsMutex);
        return uint32(rows.size() + regionRows.size() + loreRows.size());
    }

    RewardSummary GrantMilestoneRewards(Player* player, ZoneDef const& zone, uint32 percent)
    {
        return RunZone(player, zone, percent, true);
    }

    RewardSummary DescribeMilestoneRewards(Player* player, ZoneDef const& zone, uint32 percent)
    {
        return RunZone(player, zone, percent, false);
    }

    RewardSummary GrantRegionRewards(Player* player, RegionDef const& region)
    {
        return RunRegion(player, region, true);
    }

    RewardSummary DescribeRegionRewards(Player* player, RegionDef const& region)
    {
        return RunRegion(player, region, false);
    }

    std::vector<uint32> LoreRewardCounts(LoreKind kind)
    {
        std::lock_guard<std::mutex> lock(rewardsMutex);
        std::vector<uint32> counts;
        for (RewardRow const& row : loreRows)
            if (row.zoneId == kind)
                counts.push_back(row.percent);
        std::sort(counts.begin(), counts.end());
        counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
        return counts;
    }

    RewardSummary GrantLoreRewards(Player* player, LoreKind kind, uint32 count)
    {
        return RunLore(player, kind, count, true);
    }

    RewardSummary DescribeLoreRewards(Player* player, LoreKind kind, uint32 count)
    {
        return RunLore(player, kind, count, false);
    }
}

namespace AzerothCompletion
{
    void RegisterRewardHook(std::string const& name, RewardHook hook)
    {
        std::lock_guard<std::mutex> lock(Azc::rewardsMutex);
        Azc::hooks[name] = std::move(hook);
    }
}
