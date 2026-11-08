/*
 * mod_smartmobspawning
 *
 * Shortens the respawn timer of open-world creatures while a nearby player still
 * needs a quest item from their loot. The fewer of those creatures are alive around
 * the player, the sooner they come back. The module only ever shortens a timer.
 *
 * Two entry points:
 *  - OnUnitDeath: the creature that just died gets its timer adjusted.
 *  - OnPlayerUpdate (periodic scan): creatures that died before the player needed
 *    them are found in the map's respawn queue and pulled forward.
 *
 * All work happens from hooks that run on the map's own update thread, and only
 * touches that map and the player being updated.
 */

#include "CellImpl.h"
#include "Config.h"
#include "Creature.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GroupReference.h"
#include "Log.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PoolMgr.h"
#include "ScriptMgr.h"
#include "SpawnData.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <unordered_map>
#include <vector>

namespace
{
    enum BotMode : uint32
    {
        BOTS_IGNORED          = 0,
        BOTS_WITH_HUMAN_GROUP = 1,
        BOTS_ALL              = 2
    };

    struct Settings
    {
        bool enabled = true;
        bool allowElites = false;
        bool includePools = false;
        bool debug = false;
        float radius = 100.0f;
        uint32 minDelay = 30;
        uint32 maxDelay = 120;
        uint32 aliveTargetPerPlayer = 5;
        uint32 maxSpawnTime = 1800;
        uint32 scanIntervalMs = 15000;
        uint32 botMode = BOTS_WITH_HUMAN_GROUP;
    };

    Settings settings;

    // A new timer must beat the current one by this much before the scan rewrites it,
    // so repeated scans do not produce a database write each time.
    constexpr time_t SCAN_MIN_IMPROVEMENT = 5;

    constexpr char const* SCAN_TIMER_KEY = "SmartMobSpawning.ScanTimer";

    struct ScanTimer : public DataMap::Base
    {
        uint32 remainingMs = 0;
    };

    // Sessions without a socket are given this address by WorldSession's constructor.
    // mod-playerbots creates its bots that way.
    bool IsBot(Player* player)
    {
        WorldSession* session = player->GetSession();
        return session && session->GetRemoteAddress() == "headless";
    }

    bool IsGroupedWithHuman(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* member = itr->GetSource())
                if (!IsBot(member))
                    return true;

        return false;
    }

    bool IsCountedPlayer(Player* player)
    {
        if (!player->IsInWorld() || player->IsGameMaster() || player->IsSpectator())
            return false;

        if (!IsBot(player))
            return true;

        switch (settings.botMode)
        {
            case BOTS_ALL:
                return true;
            case BOTS_WITH_HUMAN_GROUP:
                return IsGroupedWithHuman(player);
            default:
                return false;
        }
    }

    bool IsEligibleTemplate(CreatureTemplate const* cinfo)
    {
        if (!cinfo || !cinfo->lootid)
            return false;

        // Read from the template: a dead creature has already had its npc flags cleared.
        if (cinfo->npcflag & UNIT_NPC_FLAG_QUESTGIVER)
            return false;

        switch (cinfo->rank)
        {
            case CREATURE_ELITE_NORMAL:
                return true;
            case CREATURE_ELITE_ELITE:
                return settings.allowElites;
            default: // rare, rare elite and world boss timers are part of their design
                return false;
        }
    }

    bool IsEligibleSpawn(ObjectGuid::LowType spawnId, uint32 spawnTimeSecs)
    {
        if (!spawnId || spawnTimeSecs <= settings.minDelay || spawnTimeSecs > settings.maxSpawnTime)
            return false;

        return settings.includePools || !sPoolMgr->IsPartOfAPool<Creature>(spawnId);
    }

    // Remembers per loot id whether any of the given players needs that loot, because
    // the answer requires walking the loot template against each player's quest log.
    class LootNeedCache
    {
    public:
        explicit LootNeedCache(std::vector<Player*> const& players) : _players(players) { }

        bool IsNeeded(uint32 lootId)
        {
            auto [itr, inserted] = _known.try_emplace(lootId, false);
            if (inserted)
                itr->second = std::any_of(_players.begin(), _players.end(), [lootId](Player const* player)
                {
                    return LootTemplates_Creature.HaveQuestLootForPlayer(lootId, player);
                });

            return itr->second;
        }

    private:
        std::vector<Player*> const& _players;
        std::unordered_map<uint32, bool> _known;
    };

    class AliveCreatureInRange
    {
    public:
        AliveCreatureInRange(WorldObject const* origin, float range) : _origin(origin), _range(range) { }

        bool operator()(Creature* creature) const
        {
            return creature->IsAlive() && _origin->IsWithinDistInMap(creature, _range);
        }

    private:
        WorldObject const* _origin;
        float _range;
    };

    // Counts every living creature nearby that drops something the players need, so
    // different creature types sharing one quest item are counted together.
    uint32 CountAliveQuestCreatures(WorldObject const* origin, LootNeedCache& needs)
    {
        std::list<Creature*> creatures;
        AliveCreatureInRange check(origin, settings.radius);
        Acore::CreatureListSearcher<AliveCreatureInRange> searcher(origin, creatures, check);
        Cell::VisitObjects(origin, searcher, settings.radius);

        return static_cast<uint32>(std::count_if(creatures.begin(), creatures.end(), [&needs](Creature* creature)
        {
            CreatureTemplate const* cinfo = creature->GetCreatureTemplate();
            return cinfo && cinfo->lootid && needs.IsNeeded(cinfo->lootid);
        }));
    }

    uint32 CalculateDelay(uint32 baseDelay, uint32 aliveCount, uint32 playerCount, ObjectGuid::LowType spawnId)
    {
        uint32 const upper = std::min(baseDelay, settings.maxDelay);
        uint32 const lower = std::min(settings.minDelay, upper);
        uint32 const wanted = settings.aliveTargetPerPlayer * playerCount;
        double const availability = wanted ? std::min(1.0, double(aliveCount) / wanted) : 1.0;

        uint32 const delay = lower + static_cast<uint32>(std::lround((upper - lower) * availability));

        // Derived from the spawn id so a pack does not reappear all at once, and so
        // the value stays the same between scans.
        return delay + spawnId % (delay / 4 + 1);
    }

    void HandleCreatureDeath(Creature* creature)
    {
        // A script's JustDied can already have despawned or resurrected it.
        if (!creature->IsInWorld() || creature->getDeathState() != DeathState::Corpse)
            return;

        Map* map = creature->GetMap();
        if (!map->IsWorldMap() || creature->IsSummon())
            return;

        if (!IsEligibleSpawn(creature->GetSpawnId(), creature->GetRespawnDelay()))
            return;

        CreatureTemplate const* cinfo = creature->GetCreatureTemplate();
        if (!IsEligibleTemplate(cinfo))
            return;

        std::list<Player*> nearby;
        Acore::AnyPlayerInObjectRangeCheck check(creature, settings.radius, false, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(creature, nearby, check);
        Cell::VisitObjects(creature, searcher, settings.radius);

        std::vector<Player*> needers;
        for (Player* player : nearby)
            if (IsCountedPlayer(player) && LootTemplates_Creature.HaveQuestLootForPlayer(cinfo->lootid, player))
                needers.push_back(player);

        if (needers.empty())
            return;

        LootNeedCache needs(needers);
        uint32 const alive = CountAliveQuestCreatures(creature, needs);
        uint32 const delay = CalculateDelay(creature->GetRespawnDelay(), alive, static_cast<uint32>(needers.size()), creature->GetSpawnId());

        time_t const current = creature->GetRespawnTime();
        if (GameTime::GetGameTime().count() + time_t(delay) >= current)
            return;

        creature->SetRespawnTime(delay);

        // Elites have a saved row from the moment they died; keep it in step.
        if (map->GetCreatureRespawnTime(creature->GetSpawnId()))
            creature->SaveRespawnTime();

        if (settings.debug)
            LOG_INFO("module", "SmartMobSpawning: {} (spawn {}) respawns in {}s, {} alive, {} player(s) need it",
                creature->GetName(), creature->GetSpawnId(), delay, alive, needers.size());
    }

    // A creature can have up to three possible entries; all of them must be allowed
    // to change, and at least one must drop what the player needs.
    bool SpawnHasNeededLoot(CreatureData const* data, LootNeedCache& needs)
    {
        bool needed = false;
        for (uint32 entry : { data->id, data->id2, data->id3 })
        {
            if (!entry)
                continue;

            CreatureTemplate const* cinfo = sObjectMgr->GetCreatureTemplate(entry);
            if (!IsEligibleTemplate(cinfo))
                return false;

            needed = needs.IsNeeded(cinfo->lootid) || needed;
        }

        return needed;
    }

    bool UsesRespawnQueue(CreatureData const* data)
    {
        SpawnGroupTemplateData const* group = sObjectMgr->GetSpawnGroupData(data->spawnGroupId);
        return group && !(group->flags & SPAWNGROUP_FLAG_COMPATIBILITY_MODE);
    }

    void ScanPendingRespawns(Player* player)
    {
        Map* map = player->GetMap();
        if (!map->IsWorldMap() || sWorld->getBoolConfig(CONFIG_RESPAWN_FORCE_COMPATIBILITY_MODE))
            return;

        struct Candidate
        {
            ObjectGuid::LowType spawnId;
            time_t respawnTime;
            uint32 spawnTimeSecs;
        };

        std::vector<Player*> players{ player };
        LootNeedCache needs(players);
        std::vector<Candidate> candidates;

        time_t const now = GameTime::GetGameTime().count();
        float const radiusSq = settings.radius * settings.radius;

        // Collected first: saving a timer modifies the container being walked here.
        for (auto const& [spawnId, respawnTime] : map->GetCreatureRespawnTimes())
        {
            if (respawnTime <= now + time_t(settings.minDelay))
                continue;

            CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
            if (!data || data->mapid != map->GetId() || !player->InSamePhase(data->phaseMask))
                continue;

            float const dx = data->posX - player->GetPositionX();
            float const dy = data->posY - player->GetPositionY();
            if (dx * dx + dy * dy > radiusSq)
                continue;

            if (!IsEligibleSpawn(spawnId, data->spawntimesecs) || !UsesRespawnQueue(data))
                continue;

            // Still present as a corpse: its own timer overwrites the queue when it
            // decays, so it is picked up by the first scan after that.
            auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
            if (bounds.first != bounds.second)
                continue;

            if (SpawnHasNeededLoot(data, needs))
                candidates.push_back({ spawnId, respawnTime, data->spawntimesecs });
        }

        if (candidates.empty())
            return;

        uint32 const alive = CountAliveQuestCreatures(player, needs);
        for (Candidate const& candidate : candidates)
        {
            time_t newTime = now + time_t(CalculateDelay(candidate.spawnTimeSecs, alive, 1, candidate.spawnId));
            if (newTime + SCAN_MIN_IMPROVEMENT >= candidate.respawnTime)
                continue;

            map->SaveCreatureRespawnTime(candidate.spawnId, newTime);

            if (settings.debug)
                LOG_INFO("module", "SmartMobSpawning: spawn {} pulled forward by {}s for {}, {} alive",
                    candidate.spawnId, candidate.respawnTime - newTime, player->GetName(), alive);
        }
    }
}

class SmartMobSpawningWorldScript : public WorldScript
{
public:
    SmartMobSpawningWorldScript() : WorldScript("SmartMobSpawningWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        settings.enabled = sConfigMgr->GetOption<bool>("SmartMobSpawning.Enable", true);
        settings.radius = std::max(1.0f, sConfigMgr->GetOption<float>("SmartMobSpawning.Radius", 100.0f));
        settings.minDelay = sConfigMgr->GetOption<uint32>("SmartMobSpawning.MinDelay", 30);
        settings.maxDelay = std::max(settings.minDelay, sConfigMgr->GetOption<uint32>("SmartMobSpawning.MaxDelay", 120));
        settings.aliveTargetPerPlayer = sConfigMgr->GetOption<uint32>("SmartMobSpawning.AliveTargetPerPlayer", 5);
        settings.maxSpawnTime = sConfigMgr->GetOption<uint32>("SmartMobSpawning.MaxSpawnTime", 1800);
        settings.scanIntervalMs = sConfigMgr->GetOption<uint32>("SmartMobSpawning.ScanInterval", 15) * IN_MILLISECONDS;
        settings.allowElites = sConfigMgr->GetOption<bool>("SmartMobSpawning.AllowElites", false);
        settings.includePools = sConfigMgr->GetOption<bool>("SmartMobSpawning.IncludePools", false);
        settings.botMode = sConfigMgr->GetOption<uint32>("SmartMobSpawning.Bots", BOTS_WITH_HUMAN_GROUP);
        settings.debug = sConfigMgr->GetOption<bool>("SmartMobSpawning.Debug", false);
    }
};

class SmartMobSpawningUnitScript : public UnitScript
{
public:
    SmartMobSpawningUnitScript() : UnitScript("SmartMobSpawningUnitScript", true, { UNITHOOK_ON_UNIT_DEATH }) { }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (!settings.enabled)
            return;

        if (Creature* creature = unit->ToCreature())
            HandleCreatureDeath(creature);
    }
};

class SmartMobSpawningPlayerScript : public PlayerScript
{
public:
    SmartMobSpawningPlayerScript() : PlayerScript("SmartMobSpawningPlayerScript", { PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!settings.enabled || !settings.scanIntervalMs)
            return;

        ScanTimer* timer = player->CustomData.GetDefault<ScanTimer>(SCAN_TIMER_KEY);
        if (timer->remainingMs > diff)
        {
            timer->remainingMs -= diff;
            return;
        }

        timer->remainingMs = settings.scanIntervalMs;

        if (IsCountedPlayer(player))
            ScanPendingRespawns(player);
    }
};

void AddSC_SmartMobSpawning()
{
    new SmartMobSpawningWorldScript();
    new SmartMobSpawningUnitScript();
    new SmartMobSpawningPlayerScript();
}
