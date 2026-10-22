// Versioned console-only character operations. Runs synchronously on the world thread.
#include "ScriptObjects.h"
#include "Chat.h"
#include "Config/Config.h"
#include "Database/DatabaseEnv.h"
#include "Handlers/CharacterCreation.h"
#include "PlayerLoginQueryHolder.h"
#include "PlayerDump.h"
#include "Player.h"
#include "Item.h"
#include "Creature.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "World.h"
#include "WorldSession.h"
#include "MapManager.h"
#include "GridMap.h"
#include "AccountMgr.h"
#include "Auth/Sha1.h"
#include "Util.h"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

// A save may have reached the database. The reply says so, and Forge keeps the request ID.
struct Uncertain : std::runtime_error { using std::runtime_error::runtime_error; };

uint32 Number(Json const& value, uint32 max)
{
    if (!value.is_number_integer() || value.get<int64>() < 0 || value.get<uint64>() > max)
        throw std::runtime_error("Invalid integer in character request.");
    return value.get<uint32>();
}
std::string Digest(std::string const& text)
{
    Sha1Hash hash;
    hash.UpdateData(text); hash.Finalize();
    std::ostringstream out;
    for (int i = 0; i < hash.GetLength(); ++i)
        out << std::hex << std::setw(2) << std::setfill('0') << unsigned(hash.GetDigest()[i]);
    return out.str();
}
std::string Dump(uint32 guid)
{
    std::string dump;
    PlayerDumpWriter writer;
    if (writer.ReturnDump(dump, guid) != DUMP_SUCCESS || dump.find("INSERT INTO `characters`") == std::string::npos)
        throw std::runtime_error("Cannot back up character; no changes were made.");
    return dump;
}
void Write(fs::path const& path, std::string const& value)
{
    // Never overwrite an audit or receipt. All calls are on the world thread.
    // A rename publishes the file whole, so a crash never leaves a truncated receipt.
    if (fs::exists(path)) throw std::runtime_error("Audit file already exists.");
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write character audit.");
        fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write);
        out << value; out.flush();
        if (!out) throw std::runtime_error("Cannot finish character audit.");
    }
    fs::rename(temporary, path);
}
void RequireOffline(uint32 account, uint32 guid = 0)
{
    if (!account || sWorld.HasOtherSessionForAccount(account))
        throw std::runtime_error("Close every game client on this account before editing.");
    std::unique_ptr<QueryResult> count(CharacterDatabase.PQuery(
        "SELECT COUNT(*) FROM characters WHERE account=%u AND online<>0", account));
    if (!count || count->Fetch()[0].GetUInt32())
        throw std::runtime_error("Account is online or its state could not be verified.");
    std::vector<PlayerCacheData*> members;
    sObjectMgr.GetPlayerDataForAccount(account, members);
    for (auto member : members)
    {
        ObjectGuid id(HIGHGUID_PLAYER, member->uiGuid);
        if (sObjectAccessor.FindPlayerNotInWorld(id) ||
            sWorld.GetHeadlessSessionState(id) != HeadlessSessionState::NotFound)
            throw std::runtime_error("Account has an active or loading character.");
    }
    if (guid)
    {
        auto cache = sObjectMgr.GetPlayerDataByGUID(guid);
        if (!cache || cache->uiAccount != account || sWorld.IsCharacterLocked(guid))
            throw std::runtime_error("Character is locked, missing or belongs to another account.");
    }
}
// The login path's own loader, minus the world: never added to a map, never registered.
struct OfflinePlayer
{
    WorldSession session;
    LoginQueryHolder holder;
    Player player;
    OfflinePlayer(uint32 account, uint32 guid)
        : session(account, nullptr, sAccountMgr.GetSecurity(account), 0, LOCALE_enUS,
            "127.0.0.1", 0, SessionTransport::Headless),
          holder(account, ObjectGuid(HIGHGUID_PLAYER, guid)), player(&session)
    {
        if (!holder.Initialize()) throw std::runtime_error("Cannot initialize character load.");
        holder.ExecuteSynchronous(CharacterDatabase);
        if (!player.LoadFromDB(ObjectGuid(HIGHGUID_PLAYER, guid), &holder))
            throw std::runtime_error("Cannot load character through game APIs.");
    }
};
std::array<char const*, EQUIPMENT_SLOT_END> const slotKeys = {{
    "head", "neck", "shoulders", "shirt", "chest", "belt", "legs", "boots", "wrists",
    "gloves", "finger1", "finger2", "trinket1", "trinket2", "back", "", "", "", "tabard"
}};
bool Weapon(unsigned slot) { return slot >= EQUIPMENT_SLOT_MAINHAND && slot <= EQUIPMENT_SLOT_RANGED; }
bool Accessory(unsigned slot) { return slot == EQUIPMENT_SLOT_NECK || (slot >= EQUIPMENT_SLOT_FINGER1 && slot <= EQUIPMENT_SLOT_BACK); }
Json Draft(Player& player)
{
    Json outfit = Json::object(), accessories = Json::object(), equipment = Json::array();
    for (unsigned slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        auto item = player.GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        uint32 entry = item ? item->GetEntry() : 0;
        if (Weapon(slot)) equipment.push_back(entry);
        else if (Accessory(slot)) accessories[slotKeys[slot]] = entry;
        else outfit[slotKeys[slot]] = entry;
    }
    uint32 bytes = player.GetUInt32Value(PLAYER_BYTES);
    return Json{{"guid", player.GetGUIDLow()}, {"name", player.GetName()},
        {"race", player.GetRace()}, {"classId", player.GetClass()},
        {"gender", player.GetGender()}, {"level", player.GetLevel()},
        {"money", player.GetMoney() / 10000}, {"resetTalents", false},
        {"skin", bytes & 255}, {"face", (bytes >> 8) & 255},
        {"hairStyle", (bytes >> 16) & 255}, {"hairColor", bytes >> 24},
        {"facialHair", player.GetByteValue(PLAYER_BYTES_2, 0)},
        {"outfit", outfit}, {"accessories", accessories}, {"equipment", equipment},
        {"position", {{"map", player.GetMapId()}, {"x", player.GetPositionX()},
            {"y", player.GetPositionY()}, {"z", player.GetPositionZ()},
            {"orientation", MapManager::NormalizeOrientation(player.GetOrientation())}}}};
}
void Validate(Json const& draft)
{
    if (!draft.is_object()) throw std::runtime_error("Missing character draft.");
    Number(draft.at("guid"), 0xFFFFFFFF);
    for (auto key : {"race", "classId", "skin", "face", "hairStyle", "hairColor", "facialHair"})
        Number(draft.at(key), 255);
    Number(draft.at("gender"), 1);
    if (!Number(draft.at("level"), 60)) throw std::runtime_error("Level must be 1–60.");
    Number(draft.at("money"), MAX_MONEY_AMOUNT / 10000);
    // Name rules are applied by creation and by Apply on a rename, so legacy names stay editable.
    std::string name = draft.at("name").get<std::string>();
    if (!normalizePlayerName(name)) throw std::runtime_error("Invalid character name.");
    if (!draft.at("equipment").is_array() || draft.at("equipment").size() != 3 ||
        !draft.at("outfit").is_object() || !draft.at("accessories").is_object())
        throw std::runtime_error("Invalid equipment.");
    for (auto const& entry : draft.at("equipment")) Number(entry, 0xFFFFFF);
    for (auto const& key : {"outfit", "accessories"})
        for (auto const& entry : draft.at(key)) Number(entry, 0xFFFFFF);
    if (draft.count("resetTalents") && !draft.at("resetTalents").is_boolean())
        throw std::runtime_error("Invalid talent reset flag.");
    auto const& position = draft.at("position");
    if (!position.is_null())
    {
        uint32 map = Number(position.at("map"), 65535);
        float x = position.at("x").get<float>(), y = position.at("y").get<float>(),
            z = position.at("z").get<float>(), o = position.at("orientation").get<float>();
        if (!MapManager::IsValidMapCoord(map, x, y, z, o))
            throw std::runtime_error("Invalid destination.");
    }
}
// Class trainers cover armor (Plate Mail at 40) only on a raise; weapon skills come from weapon
// masters, and an edit at the same level trains nothing. So an item's armor or weapon proficiency
// is taught where a trainer could teach it now: the trainers' own check (the race and class
// abilities, SkillRaceClassInfo's untrainable flag and minimum level).
void LearnProficiency(Player& player, uint32 entry)
{
    ItemPrototype const* prototype = sObjectMgr.GetItemPrototype(entry);
    uint32 spell = prototype ? prototype->GetProficiencySpell() : 0;
    if (spell && !player.HasSpell(spell) && player.IsSpellFitByClassAndRace(spell))
        player.LearnSpell(spell, false);
}
std::string EquipError(InventoryResult result)
{
    switch (result)
    {
        case EQUIP_ERR_CANT_EQUIP_LEVEL_I: return "the character's level is too low";
        case EQUIP_ERR_CANT_EQUIP_SKILL: return "the character's skill is too low";
        case EQUIP_ERR_ITEM_DOESNT_GO_TO_SLOT: return "it does not go in that slot";
        case EQUIP_ERR_NO_REQUIRED_PROFICIENCY: return "this class cannot learn its armor or weapon type yet";
        case EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM:
        case EQUIP_ERR_YOU_CAN_NEVER_USE_THAT_ITEM2: return "this race or class can never use it";
        case EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED: return "a two-handed weapon is in the main hand";
        case EQUIP_ERR_CANT_DUAL_WIELD: return "this character cannot dual wield";
        case EQUIP_ERR_CANT_CARRY_MORE_OF_THIS: return "it is unique and the character already has one";
        case EQUIP_ERR_CANT_EQUIP_RANK: return "it needs a higher honor rank";
        default: return "the game refused it";
    }
}
// What this class's trainers would sell at the new level, free: .learn all_trainer's own helper
// (green lessons only, ranks in order), limited to class trainers as local Forge does, so no
// professions, riding or pet training.
void LearnClassTrainerSpells(ChatHandler& handler, Player& player)
{
    std::set<uint32> templates;
    for (auto const& entry : sObjectMgr.GetCreatureInfoMap())
    {
        CreatureInfo const* info = entry.second.get();
        if (!info || !(info->npc_flags & UNIT_NPC_FLAG_TRAINER) ||
            info->trainer_type != TRAINER_TYPE_CLASS || info->trainer_class != player.GetClass())
            continue;
        if (TrainerSpellData const* spells = sObjectMgr.GetNpcTrainerSpells(entry.first))
            handler.HandleLearnTrainerHelper(&player, spells);
        if (info->trainer_id && templates.insert(info->trainer_id).second)
            if (TrainerSpellData const* spells = sObjectMgr.GetNpcTrainerTemplateSpells(info->trainer_id))
                handler.HandleLearnTrainerHelper(&player, spells);
    }
}
// Where the saved character stands; moved means the draft chose a new place.
struct Placement { WorldLocation at; bool moved = false; };
// Runs after the save and before the receipt, synchronously, so a login or a crash never sees the
// save without it. An offline player's cached zone is 0 (only UpdateZone, in the world, sets it),
// so SaveToDB wrote zone 0; this writes the real one. A move is .tele name's offline move
// (Player::SavePositionInDB, which is asynchronous): it also leaves any transport or flight path.
void Place(uint32 guid, Placement const& place)
{
    WorldLocation const& at = place.at;
    uint32 zone = sTerrainMgr.GetZoneId(at.mapId, at.x, at.y, at.z);
    bool written = place.moved
        ? CharacterDatabase.DirectPExecute("UPDATE characters SET position_x=%f,position_y=%f,position_z=%f,orientation=%f,"
            "map=%u,zone=%u,trans_x=0,trans_y=0,trans_z=0,transguid=0,taxi_path='' WHERE guid=%u",
            at.x, at.y, at.z, at.o, at.mapId, zone, guid)
        : CharacterDatabase.DirectPExecute("UPDATE characters SET zone=%u WHERE guid=%u", zone, guid);
    if (!written)
        throw Uncertain("The character was saved, but its position or zone could not be written. Inspect it before retrying.");
    if (place.moved)
        sObjectMgr.UpdatePlayerCachedPosition(guid, at.mapId, at.x, at.y, at.z, at.o, false);
    if (PlayerCacheData* cache = sObjectMgr.GetPlayerDataByGUID(guid))
        cache->uiZoneId = zone;
}
bool HasChallengeMode(Player const& player)
{
    if (player.IsHardcore()) return true;
    for (uint32 challenge = CHALLENGE_SLOW_AND_STEADY; challenge <= CHALLENGE_HEROIC; ++challenge)
        if (player.HasChallenge(Challenges(challenge))) return true;
    return false;
}
// Changes only the in-memory player; nothing reaches the database before SaveToDB.
Placement Apply(ChatHandler& handler, Player& player, Json const& draft)
{
    if (player.GetRace() != draft.at("race").get<uint32>() ||
        player.GetClass() != draft.at("classId").get<uint32>())
        throw std::runtime_error("Changing an existing character's race or class needs a migration and is not supported.");
    std::string name = draft.at("name").get<std::string>(); normalizePlayerName(name);
    if (name != player.GetName())
    {
        // The client's own creation rules, as CharacterCreation applies them.
        if (ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS ||
            (player.GetSession()->GetSecurity() == SEC_PLAYER && sObjectMgr.IsReservedName(name)))
            throw std::runtime_error("This name is not allowed.");
        auto named = sObjectMgr.GetPlayerDataByName(name);
        if (named && named->uiGuid != player.GetGUIDLow()) throw std::runtime_error("Name is already taken.");
        std::string escapedName = name;
        CharacterDatabase.escape_string(escapedName);
        std::unique_ptr<QueryResult> names(CharacterDatabase.PQuery(
            "SELECT COUNT(*) FROM characters WHERE name='%s' AND guid<>%u", escapedName.c_str(), player.GetGUIDLow()));
        if (!names || names->Fetch()[0].GetUInt32())
            throw std::runtime_error("Name is taken or availability could not be verified.");
        player.SetName(name);
        player.RemoveAtLoginFlag(AT_LOGIN_RENAME);
    }
    // .character level's own helper; a raise also trains what the class trainers teach by then.
    std::array<uint32, EQUIPMENT_SLOT_END> entries{};
    for (unsigned slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
        entries[slot] = Weapon(slot)
            ? draft.at("equipment")[slot - EQUIPMENT_SLOT_MAINHAND].get<uint32>()
            : draft.at(Accessory(slot) ? "accessories" : "outfit").value(slotKeys[slot], uint32(0));
    // CanEquipItem passes a two-hander over an off-hand that fits in the bags, because the game
    // then moves the off-hand there; nothing here would, so both would stay equipped.
    ItemPrototype const* mainHand = sObjectMgr.GetItemPrototype(entries[EQUIPMENT_SLOT_MAINHAND]);
    if (mainHand && mainHand->InventoryType == INVTYPE_2HWEAPON && entries[EQUIPMENT_SLOT_OFFHAND])
        throw std::runtime_error("A two-handed weapon leaves no room for an off-hand item.");
    // .character level's own helper; a raise also trains what the class trainers teach by then.
    uint32 level = draft.at("level").get<uint32>(), oldLevel = player.GetLevel();
    // GiveLevel mails challenge rewards, awards titles and announces hardcore levels at once,
    // even when a later step refuses the edit.
    if (level != oldLevel && HasChallengeMode(player))
        throw std::runtime_error("Level changes are not supported for hardcore or challenge-mode characters.");
    if (level != oldLevel)
        handler.HandleCharacterLevel(&player, player.GetObjectGuid(), oldLevel, level);
    if (level > oldLevel)
        LearnClassTrainerSpells(handler, player);
    // Remove changed items first, so the native checks below see the final layout:
    // a replaced two-hander, a unique item moving slot, an off-hand that is going away.
    for (unsigned slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        auto old = player.GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (old && old->GetEntry() != entries[slot]) player.DestroyItem(INVENTORY_SLOT_BAG_0, slot, false);
    }
    // Ascending order equips the main hand before the off hand.
    for (unsigned slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (!entries[slot] || player.GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) continue;
        LearnProficiency(player, entries[slot]);
        uint16 destination = 0;
        InventoryResult result = player.CanEquipNewItem(slot, destination, entries[slot], false);
        if (result != EQUIP_ERR_OK || !player.EquipNewItem(destination, entries[slot], false))
            throw std::runtime_error("Item " + std::to_string(entries[slot]) + " cannot be equipped in slot " +
                std::to_string(slot) + ": " + EquipError(result) + " (game error " + std::to_string(result) + ").");
    }
    // Gender lives in two fields; Player::ChangeRace writes both.
    uint8 gender = draft.at("gender").get<uint8>();
    player.SetByteValue(UNIT_FIELD_BYTES_0, 2, gender);
    player.SetUInt16Value(PLAYER_BYTES_3, 0, uint16(gender) | (player.GetDrunkValue() & 0xFFFE));
    player.SetUInt32Value(PLAYER_BYTES, draft.at("skin").get<uint32>() | (draft.at("face").get<uint32>() << 8) |
        (draft.at("hairStyle").get<uint32>() << 16) | (draft.at("hairColor").get<uint32>() << 24));
    player.SetByteValue(PLAYER_BYTES_2, 0, draft.at("facialHair").get<uint8>());
    player.InitPlayerDisplayIds();
    // SetMoney, as .modify money uses; the copper and silver stay, within the game's limit.
    player.SetMoney(uint32(std::min<uint64>(uint64(draft.at("money").get<uint32>()) * 10000 + player.GetMoney() % 10000,
        MAX_MONEY_AMOUNT)));
    if (draft.value("resetTalents", false)) player.ResetTalents(true);
    // Forge echoes the loaded position, so an unchanged one is not a move.
    Placement place{WorldLocation(player.GetMapId(), player.GetPositionX(), player.GetPositionY(),
        player.GetPositionZ(), player.GetOrientation())};
    auto const& position = draft.at("position");
    if (position.is_null()) return place;
    WorldLocation to(position.at("map").get<uint32>(), position.at("x").get<float>(), position.at("y").get<float>(),
        position.at("z").get<float>(), position.at("orientation").get<float>());
    if (to.mapId == player.GetMapId() && std::fabs(to.x - player.GetPositionX()) <= 0.01f &&
        std::fabs(to.y - player.GetPositionY()) <= 0.01f && std::fabs(to.z - player.GetPositionZ()) <= 0.01f)
        return place;
    return Placement{to, true};
}
std::string CreateError(uint8 result)
{
    switch (result)
    {
        case CHAR_CREATE_NAME_IN_USE: return "That name is already taken.";
        case CHAR_NAME_RESERVED: return "That name is reserved.";
        case CHAR_CREATE_SERVER_LIMIT: return "This account already has the most characters a realm allows.";
        case CHAR_CREATE_PVP_TEAMS_VIOLATION: return "This realm does not allow both factions on one account.";
        case CHAR_CREATE_DISABLED: return "Character creation is disabled for this race or faction.";
        default: return "The game rejected this character (code " + std::to_string(result) + ").";
    }
}
bool Handle(ChatHandler* handler, char* args)
{
    Json response;
    try
    {
        if (handler->GetSession()) throw std::runtime_error("Forge character operations are console-only.");
        if (!sConfig.GetBoolDefault("ForgeCharacters.Enable", false))
            throw std::runtime_error("ForgeCharacters.Enable is disabled on this server.");
        if (!args || std::strlen(args) > 32768) throw std::runtime_error("Invalid request size.");
        Json request = Json::parse(args);
        if (Number(request.at("version"), 1) != 1) throw std::runtime_error("Unsupported protocol.");
        uint32 account = Number(request.at("account"), 0xFFFFFFFF);
        std::string operation = request.at("operation").get<std::string>();
        if (operation != "inspect" && operation != "create" && operation != "update")
            throw std::runtime_error("Unknown character operation.");
        uint32 guid = operation == "create" ? 0 : Number(request.at("guid"), 0xFFFFFFFF);
        if (operation == "inspect")
        {
            RequireOffline(account, guid);
            std::string before = Dump(guid);
            OfflinePlayer loaded(account, guid);
            response = {{"ok", true}, {"guid", guid}, {"draft", Draft(loaded.player)},
                {"revision", Digest(before)}};
        }
        else
        {
            std::string token = request.at("request").get<std::string>();
            if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::runtime_error("Invalid request ID.");
            fs::path directory(sConfig.GetStringDefault("ForgeCharacters.AuditDirectory", "forge-characters-audit"));
            fs::create_directories(directory);
            fs::permissions(directory, fs::perms::owner_all);
            fs::path prepared = directory / (token + ".prepared.json");
            fs::path receipt = directory / (token + ".result.json");
            std::string fingerprint = Digest(request.dump());
            std::string uncertain = "The previous attempt of this request has an uncertain outcome. Inspect the character and audit " +
                token + " before resolving it; do not retry it as a new request.";
            // A replay needs no game state: answer it before the online checks.
            if (fs::exists(prepared))
            {
                std::ifstream in(prepared); Json audit; in >> audit;
                if (audit.at("fingerprint").get<std::string>() != fingerprint) throw std::runtime_error("Request ID reused with different content.");
                if (!fs::exists(receipt)) throw Uncertain(uncertain);
                std::ifstream result(receipt); result >> response;
            }
            else
            {
                RequireOffline(account, guid);
                Json draft = request.at("draft"); Validate(draft);
                if (draft.at("guid").get<uint32>() != guid) throw std::runtime_error("Character ID mismatch.");
                if (operation == "create")
                {
                    std::string accountName;
                    if (!sAccountMgr.GetName(account, accountName)) throw std::runtime_error("This account does not exist.");
                    CharacterCreateInfo info;
                    info.name = draft.at("name").get<std::string>();
                    info.race = draft.at("race").get<uint8>(); info.class_ = draft.at("classId").get<uint8>();
                    info.gender = draft.at("gender").get<uint8>(); info.skin = draft.at("skin").get<uint8>();
                    info.face = draft.at("face").get<uint8>(); info.hairStyle = draft.at("hairStyle").get<uint8>();
                    info.hairColor = draft.at("hairColor").get<uint8>(); info.facialHair = draft.at("facialHair").get<uint8>();
                    // Exceptions never cross the core; a rejection inside the callback is reported here.
                    std::string rejected;
                    Placement place;
                    auto created = CharacterCreation::CreateCharacter(account, info, [&](Player& player) {
                        try
                        {
                            place = Apply(*handler, player, draft);
                            // Written last: a prepared record without a receipt means a save was attempted.
                            Write(prepared, Json{{"fingerprint", fingerprint}, {"request", request},
                                {"guid", player.GetGUIDLow()}, {"before", nullptr}}.dump());
                            return true;
                        }
                        catch (std::exception const& error) { rejected = error.what(); return false; }
                    }, true);
                    if (!rejected.empty()) throw std::runtime_error(rejected);
                    if (created.result != CHAR_CREATE_SUCCESS && fs::exists(prepared)) throw Uncertain(uncertain);
                    if (created.result != CHAR_CREATE_SUCCESS) throw std::runtime_error(CreateError(created.result));
                    guid = created.guid.GetCounter();
                    Place(guid, place);
                }
                else
                {
                    std::string before = Dump(guid);
                    if (request.at("revision").get<std::string>() != Digest(before)) throw std::runtime_error("Character changed. Reload it before saving.");
                    OfflinePlayer loaded(account, guid);
                    std::string oldName = loaded.player.GetName();
                    Placement place = Apply(*handler, loaded.player, draft);
                    Write(prepared, Json{{"fingerprint", fingerprint}, {"request", request},
                        {"guid", guid}, {"before", before}}.dump());
                    if (!loaded.player.SaveToDB(false, true, true)) throw Uncertain(uncertain);
                    if (oldName != loaded.player.GetName())
                        sObjectMgr.ChangePlayerNameInCache(guid, oldName, loaded.player.GetName());
                    sObjectMgr.UpdatePlayerCache(&loaded.player);
                    sObjectMgr.UpdatePlayerCachedPosition(&loaded.player);
                    Place(guid, place);
                    sWorld.InvalidatePlayerDataToAllClients(loaded.player.GetObjectGuid());
                }
                response = {{"ok", true}, {"guid", guid}};
                try { Write(receipt, response.dump()); }
                catch (std::exception const& error)
                {
                    // The save committed; report it. A lost reply to this request will read as uncertain.
                    sLog.outError("ForgeCharacters: request %s saved character %u but its receipt failed: %s",
                        token.c_str(), guid, error.what());
                }
            }
        }
    }
    catch (Uncertain const& error) { response = {{"ok", false}, {"error", error.what()}, {"uncertain", true}}; }
    catch (std::exception const& error) { response = {{"ok", false}, {"error", error.what()}}; }
    handler->SendSysMessage(("FORGE_CHAR_V1 " + response.dump()).c_str());
    return true;
}
class ForgeCommands final : public CommandScript
{
public:
    ForgeCommands() : CommandScript("forge_character_commands") {}
    std::vector<ChatCommand> GetCommands() const override
    {
        return {{"forgecharacter", SEC_CONSOLE, true, nullptr,
            "Versioned offline character operations (JSON)", nullptr, 0, "", 0, &Handle}};
    }
};
}
void AddForgeCharacterScripts() { new ForgeCommands(); }
