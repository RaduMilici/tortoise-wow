#include "ScriptObjects.h"
#include "Config/Config.h"
#include "Log.h"
#include "Player.h"
#include "Creature.h"
#include "SpellMgr.h"

// Thrill of the Kill: every kill grants a stack of +1% damage done for 60 seconds.
// Stacks up to 10, and each new kill refreshes the duration. The stacking, cap and
// refresh are all handled by the aura itself (spell_template stackAmount = 10,
// durationIndex = 3), so the script only has to recast it on each kill.

namespace
{
    constexpr uint32 SPELL_THRILL_OF_THE_KILL = 61500;

    struct ThrillOfTheKillConfig
    {
        bool enabled = true;
        bool countPvPKills = true;
        bool requireXPOrHonor = true;
    };

    ThrillOfTheKillConfig config;

    void GrantThrill(Player* killer, Unit* victim)
    {
        if (!config.enabled || !killer || !victim || !killer->IsAlive())
            return;

        // Grey mobs, critters, totems and pets don't count, so the buff can't be farmed for free.
        if (config.requireXPOrHonor && !killer->IsHonorOrXPTarget(victim))
            return;

        killer->CastSpell(killer, SPELL_THRILL_OF_THE_KILL, true);
    }

    class ThrillOfTheKillWorldScript : public WorldScript
    {
    public:
        ThrillOfTheKillWorldScript()
            : WorldScript("mod-thrill-of-the-kill_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP })
        {
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            config.enabled = sConfig.GetBoolDefault("ThrillOfTheKill.Enable", true);
            config.countPvPKills = sConfig.GetBoolDefault("ThrillOfTheKill.CountPvPKills", true);
            config.requireXPOrHonor = sConfig.GetBoolDefault("ThrillOfTheKill.RequireXPOrHonor", true);
        }

        void OnStartup() override
        {
            if (!sSpellMgr.GetSpellEntry(SPELL_THRILL_OF_THE_KILL))
            {
                sLog.outError("[mod-thrill-of-the-kill] Spell %u is missing from spell_template; the buff is disabled. "
                    "Make sure the module's world SQL was applied.", SPELL_THRILL_OF_THE_KILL);
                config.enabled = false;
                return;
            }

            sLog.outString("[mod-thrill-of-the-kill] Loaded (%s).", config.enabled ? "enabled" : "disabled");
        }
    };

    class ThrillOfTheKillPlayerScript : public PlayerScript
    {
    public:
        ThrillOfTheKillPlayerScript()
            : PlayerScript("mod-thrill-of-the-kill_player", { PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_PVP_KILL })
        {
        }

        void OnCreatureKill(Player* killer, Creature* killed) override
        {
            GrantThrill(killer, killed);
        }

        void OnPVPKill(Player* killer, Player* killed) override
        {
            if (config.countPvPKills)
                GrantThrill(killer, killed);
        }
    };
}

void Addmod_thrill_of_the_killScripts()
{
    new ThrillOfTheKillWorldScript();
    new ThrillOfTheKillPlayerScript();
}
