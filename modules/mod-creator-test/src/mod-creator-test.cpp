#include "CreatorTestProtocol.hpp"
#include "ScriptObjects.h"
#include "Config/Config.h"
#include "Log.h"
#include "Player.h"
#include "WorldSession.h"
#include "MapManager.h"
#include <cstdio>
#include <fstream>
#include <mutex>
#include <atomic>

namespace {
constexpr char requestPath[]="creator-test.request";
constexpr char claimedPath[]="creator-test.consuming";
std::mutex requestMutex;
std::atomic_bool enabled{false};

bool localOnly() {
    return CreatorTest::localOnly(sConfig.GetBoolDefault("CreatorTest.Enable",false),
        sConfig.GetStringDefault("BindIP",""),sConfig.GetStringDefault("WorldDatabase.Info",""),
        sConfig.GetStringDefault("CharacterDatabase.Info",""),sConfig.GetStringDefault("LoginDatabase.Info",""));
}
void result(std::string const& token, char const* message) {
    std::ofstream out("creator-test.result.tmp",std::ios::trunc);
    out<<token<<'\n'<<message<<'\n';out.close();
    std::remove("creator-test.result");std::rename("creator-test.result.tmp","creator-test.result");
}
class CreatorWorld final : public WorldScript {
public:
    CreatorWorld():WorldScript("creator_test_world",{WORLDHOOK_ON_AFTER_CONFIG_LOAD,WORLDHOOK_ON_STARTUP}){}
    // Only reached on ".reload config": Tortoise registers module scripts after the first config load.
    void OnAfterConfigLoad(bool) override { enabled=localOnly(); }
    void OnStartup() override {
        enabled=localOnly();
        sLog.outString(enabled ? "[creator-test] Enabled for the local Creator runtime."
                               : "[creator-test] Disabled (needs CreatorTest.Enable = 1 and loopback-only addresses).");
        if(!enabled) return;
        std::ifstream in(requestPath);CreatorTest::Request request;
        if(!CreatorTest::read(in,request,std::time(nullptr))) return;
        std::ofstream ready("creator-test.ready",std::ios::trunc);
        ready<<request.token<<'\n';
        sLog.outString("[creator-test] Local test request armed for next character login.");
    }
};
class CreatorPlayer final : public PlayerScript {
public:
    CreatorPlayer():PlayerScript("creator_test_player",{PLAYERHOOK_ON_LOGIN}){}
    void OnLogin(Player* player) override {
        if(!enabled || !player || !player->IsInWorld()) return;
        std::lock_guard<std::mutex> lock(requestMutex);
        std::ifstream in(requestPath);CreatorTest::Request request;
        if(!CreatorTest::read(in,request,std::time(nullptr))) return;
        if(!CreatorTest::matches(request,player->GetGUIDLow(),player->GetSession()->GetAccountId(),std::time(nullptr))) return;
        in.close();
        if(!MapManager::IsValidMapCoord(request.map,request.x,request.y,request.z,request.orientation)) {
            result(request.token,"Invalid destination");std::remove(requestPath);return;
        }
        // Claim once before teleporting. A crash cannot replay the request on every login.
        std::remove(claimedPath);
        if(std::rename(requestPath,claimedPath)!=0) return;
        bool ok=player->TeleportTo(request.map,request.x,request.y,request.z,request.orientation);
        result(request.token,ok?"OK":"The server rejected the destination. Check map access and character state.");
        std::remove(claimedPath);
    }
};
}
void Addmod_creator_testScripts() { new CreatorWorld();new CreatorPlayer(); }
