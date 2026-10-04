#include "AzcCommands.h"
#include "AzcGenerator.h"
#include "AzcProgress.h"
#include "AzcProtocol.h"
#include "AzcRewards.h"
#include "Creature.h"
#include "Log.h"
#include "Player.h"
#include "ScriptObjects.h"

// Azeroth Completion: every outdoor zone becomes a checklist generated from world data
// (exploration, storylines, rare hunts, elite encounters, flight paths). The server owns the
// truth and explains it to an addon over a versioned protocol; see README.md and PROTOCOL.md.

namespace
{
    class AzerothCompletionWorldScript : public WorldScript
    {
    public:
        AzerothCompletionWorldScript()
            : WorldScript("mod-azeroth-completion_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP })
        {
        }

        // Only reached on ".reload config": module scripts register after the first config load.
        void OnAfterConfigLoad(bool /*reload*/) override
        {
            Azc::LoadConfig();
        }

        void OnStartup() override
        {
            Azc::LoadConfig();
            if (!Azc::GetConfig().enabled)
            {
                sLog.outString("[mod-azeroth-completion] Disabled.");
                return;
            }
            Azc::LoadRewards();
            Azc::SetDefinitions(Azc::GenerateDefinitions(nullptr));
            sLog.outString("[mod-azeroth-completion] Loaded (protocol %u).", Azc::AZCOMP_PROTOCOL);
        }
    };

    class AzerothCompletionPlayerScript : public PlayerScript
    {
    public:
        AzerothCompletionPlayerScript()
            : PlayerScript("mod-azeroth-completion_player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_DELETE,
                PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_UPDATE_ZONE, PLAYERHOOK_ON_ADDON_MESSAGE })
        {
        }

        void OnLogin(Player* player) override { Azc::ProgressOnLogin(player); }
        void OnLogout(Player* player) override { Azc::ProgressOnLogout(player); }
        void OnDelete(ObjectGuid guid, uint32 /*accountId*/) override { Azc::ProgressOnDelete(guid.GetCounter()); }
        void OnUpdate(Player* player, uint32 diff) override { Azc::ProgressOnUpdate(player, diff); }

        // Called for every player that gets kill credit (group members in range included),
        // so the killing blow does not matter.
        void OnCreatureKill(Player* killer, Creature* killed) override { Azc::ProgressOnKill(killer, killed); }

        void OnUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override { Azc::ProgressOnZoneChange(player); }

        bool OnAddonMessage(Player* from, std::string const& msg) override { return Azc::HandleAddonMessage(from, msg); }
    };

    class AzerothCompletionCommandScript : public CommandScript
    {
    public:
        AzerothCompletionCommandScript() : CommandScript("mod-azeroth-completion_commands") {}

        std::vector<ChatCommand> GetCommands() const override { return Azc::GetAcCommands(); }
    };
}

void Addmod_azeroth_completionScripts()
{
    new AzerothCompletionWorldScript();
    new AzerothCompletionPlayerScript();
    new AzerothCompletionCommandScript();
}
