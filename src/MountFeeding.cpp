#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"

#include <unordered_map>

// Satisfaction thresholds (same as pet happiness: HAPPINESS_LEVEL_SIZE = 333000)
static constexpr int32 HAPPINESS_LEVEL_SIZE  = 333000;
static constexpr int32 SATISFACTION_MAX      = 999000;
static constexpr int32 THRESHOLD_HAPPY       = 2 * HAPPINESS_LEVEL_SIZE; // 666000
static constexpr int32 THRESHOLD_CONTENT     = HAPPINESS_LEVEL_SIZE;     // 333000

// Grace period for detecting food use right after client-side dismount (ms)
static constexpr uint32 DISMOUNT_GRACE_MS    = 1000;

enum SatisfactionState : uint8
{
    STATE_UNHAPPY = 0,
    STATE_CONTENT = 1,
    STATE_HAPPY   = 2
};

// Config values
static bool   sEnabled                  = true;
static float  sContentSpeedMultiplier   = 0.75f;
static float  sUnhappySpeedMultiplier   = 0.50f;
static int32  sDecayAmount              = 670;
static int32  sDecayInterval            = 7500;
static bool   sDecayOnlyWhileMounted    = true;
static float  sDecayMultStationary      = 0.5f;
static float  sDecayMultMoving          = 1.0f;
static float  sDecayMultFlying          = 1.5f;
static int32  sDefaultSatisfaction      = SATISFACTION_MAX;
static bool   sUnhappyNoFly            = true;
static int32  sSaveInterval             = 300000;

// Per-player data
struct MountFeedingData
{
    int32    satisfaction;
    bool     pendingSpeedUpdate;
    int32    decayTimer;
    int32    saveTimer;
    uint8    lastState;
    int32    baseGroundSpeed;
    int32    baseFlyingSpeed;
    uint32   lastMountSpellId;   // spell ID of the last mount used
    uint32   dismountTimeMs;     // GameTimeMS when dismounted (for grace period)
    bool     flyingDisabled;     // true if we've called SetCanFly(false) for unhappy state
};

static std::unordered_map<ObjectGuid, MountFeedingData> sMountFeedingStore;

static uint8 GetSatisfactionState(int32 satisfaction)
{
    if (satisfaction >= THRESHOLD_HAPPY)
        return STATE_HAPPY;
    if (satisfaction >= THRESHOLD_CONTENT)
        return STATE_CONTENT;
    return STATE_UNHAPPY;
}

static float GetSpeedMultiplier(uint8 state)
{
    switch (state)
    {
        case STATE_HAPPY:   return 1.0f;
        case STATE_CONTENT: return sContentSpeedMultiplier;
        case STATE_UNHAPPY: return sUnhappySpeedMultiplier;
        default:            return 1.0f;
    }
}

// Food benefit based on food level vs player level
// One good feed should move up roughly one satisfaction tier (~333000)
static int32 GetFoodBenefit(uint8 playerLevel, uint32 itemLevel)
{
    if (playerLevel <= itemLevel + 5)
        return 350000;
    else if (playerLevel <= itemLevel + 10)
        return 175000;
    else if (playerLevel <= itemLevel + 14)
        return 80000;
    else
        return 0;
}

static void ApplySpeedPenalty(Player* player, MountFeedingData& data)
{
    uint8 state = GetSatisfactionState(data.satisfaction);
    float multiplier = GetSpeedMultiplier(state);

    for (AuraEffect* effect : player->GetAuraEffectsByType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED))
    {
        int32 newAmount = static_cast<int32>(data.baseGroundSpeed * multiplier);
        if (newAmount > 0)
            effect->ChangeAmount(newAmount);
    }

    for (AuraEffect* effect : player->GetAuraEffectsByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED))
    {
        int32 newAmount = static_cast<int32>(data.baseFlyingSpeed * multiplier);
        if (newAmount > 0)
            effect->ChangeAmount(newAmount);
    }
}

static void UpdateFlyingState(Player* player, MountFeedingData& data)
{
    if (!sUnhappyNoFly || !player->IsMounted())
        return;

    uint8 state = GetSatisfactionState(data.satisfaction);
    bool hasFlightAura = !player->GetAuraEffectsByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED).empty();

    if (!hasFlightAura)
        return; // ground mount, nothing to do

    if (state == STATE_UNHAPPY && !data.flyingDisabled)
    {
        // Disable flying — player stays mounted but descends and can't take off
        player->SetCanFly(false);
        data.flyingDisabled = true;
    }
    else if (state != STATE_UNHAPPY && data.flyingDisabled)
    {
        // Re-enable flying
        player->SetCanFly(true);
        data.flyingDisabled = false;
    }
}

static void SendStateMessage(Player* player, uint8 state)
{
    ChatHandler chat(player->GetSession());
    switch (state)
    {
        case STATE_HAPPY:
            chat.PSendSysMessage("|cff00ff00Your mount is happy and moving at full speed.|r");
            break;
        case STATE_CONTENT:
            chat.PSendSysMessage("|cffffff00Your mount is getting hungry. Speed reduced to {:.0f}%.|r",
                sContentSpeedMultiplier * 100.0f);
            break;
        case STATE_UNHAPPY:
            if (sUnhappyNoFly)
                chat.PSendSysMessage("|cffff0000Your mount is unhappy! Speed reduced to {:.0f}% and cannot fly.|r",
                    sUnhappySpeedMultiplier * 100.0f);
            else
                chat.PSendSysMessage("|cffff0000Your mount is unhappy! Speed reduced to {:.0f}%.|r",
                    sUnhappySpeedMultiplier * 100.0f);
            break;
    }
}

static void SaveSatisfaction(ObjectGuid guid, int32 satisfaction)
{
    CharacterDatabase.Execute("REPLACE INTO `mount_feeding` (`guid`, `satisfaction`) VALUES ({}, {})",
        guid.GetCounter(), satisfaction);
}

// ------- World Script: config loading -------

class MountFeedingWorldScript : public WorldScript
{
public:
    MountFeedingWorldScript() : WorldScript("MountFeedingWorldScript", {WORLDHOOK_ON_AFTER_CONFIG_LOAD}) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sEnabled                = sConfigMgr->GetOption<bool>("MountFeeding.Enable", true);
        sContentSpeedMultiplier = sConfigMgr->GetOption<float>("MountFeeding.ContentSpeedMultiplier", 0.75f);
        sUnhappySpeedMultiplier = sConfigMgr->GetOption<float>("MountFeeding.UnhappySpeedMultiplier", 0.50f);
        sDecayAmount            = sConfigMgr->GetOption<int32>("MountFeeding.DecayAmount", 670);
        sDecayInterval          = sConfigMgr->GetOption<int32>("MountFeeding.DecayInterval", 7500);
        sDecayOnlyWhileMounted  = sConfigMgr->GetOption<bool>("MountFeeding.DecayOnlyWhileMounted", true);
        sDecayMultStationary    = sConfigMgr->GetOption<float>("MountFeeding.DecayMultiplier.Stationary", 0.5f);
        sDecayMultMoving        = sConfigMgr->GetOption<float>("MountFeeding.DecayMultiplier.Moving", 1.0f);
        sDecayMultFlying        = sConfigMgr->GetOption<float>("MountFeeding.DecayMultiplier.Flying", 1.5f);
        sDefaultSatisfaction    = sConfigMgr->GetOption<int32>("MountFeeding.DefaultSatisfaction", SATISFACTION_MAX);
        sUnhappyNoFly           = sConfigMgr->GetOption<bool>("MountFeeding.UnhappyNoFly", true);
        sSaveInterval           = sConfigMgr->GetOption<int32>("MountFeeding.SaveInterval", 300000);
    }
};

// ------- Item Script: food interception -------

class MountFeedingItemScript : public AllItemScript
{
public:
    MountFeedingItemScript() : AllItemScript("MountFeedingItemScript") { }

    bool CanItemUse(Player* player, Item* item, SpellCastTargets const& /*targets*/) override
    {
        if (!sEnabled || !player || !item)
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->FoodType == 0)
            return false;

        auto it = sMountFeedingStore.find(player->GetGUID());
        if (it == sMountFeedingStore.end())
            return false;

        MountFeedingData& data = it->second;

        // Check if player is mounted OR was just dismounted by the client to eat food
        bool isMounted = player->IsMounted();
        bool wasMountedRecently = false;

        if (!isMounted && data.lastMountSpellId != 0)
        {
            uint32 now = GameTime::GetGameTimeMS().count();
            uint32 elapsed = now - data.dismountTimeMs;
            wasMountedRecently = elapsed < DISMOUNT_GRACE_MS;
        }

        if (!isMounted && !wasMountedRecently)
            return false;

        // Player is mounted (or was just auto-dismounted by client for food) — intercept
        if (data.satisfaction >= SATISFACTION_MAX)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Your mount is already fully satisfied.");
            return true;
        }

        int32 benefit = GetFoodBenefit(player->GetLevel(), proto->ItemLevel);
        if (benefit == 0)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("That food is too low level for your mount.");
            return true;
        }

        // Consume one food item
        uint32 count = 1;
        player->DestroyItemCount(item, count, true);

        uint8 oldState = GetSatisfactionState(data.satisfaction);
        data.satisfaction = std::min(data.satisfaction + benefit, SATISFACTION_MAX);
        uint8 newState = GetSatisfactionState(data.satisfaction);
        data.lastState = newState;

        ChatHandler(player->GetSession()).PSendSysMessage("Your mount happily eats the {}.",
            proto->Name1);

        if (newState != oldState)
        {
            SendStateMessage(player, newState);
            if (player->IsMounted())
            {
                if (data.baseGroundSpeed > 0)
                    ApplySpeedPenalty(player, data);
                UpdateFlyingState(player, data);
            }
        }

        return true; // block normal food cast
    }
};

// ------- Unit Script: aura apply/remove -------

class MountFeedingUnitScript : public UnitScript
{
public:
    MountFeedingUnitScript() : UnitScript("MountFeedingUnitScript", true, {UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE}) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!sEnabled)
            return;

        Player* player = unit->ToPlayer();
        if (!player)
            return;

        bool hasMountSpeed = false;
        bool isMountAura = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            AuraEffect* effect = aura->GetEffect(i);
            if (!effect)
                continue;

            AuraType auraType = effect->GetAuraType();
            if (auraType == SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED ||
                auraType == SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
                hasMountSpeed = true;
            if (auraType == SPELL_AURA_MOUNTED)
                isMountAura = true;
        }

        if (!hasMountSpeed && !isMountAura)
            return;

        auto it = sMountFeedingStore.find(player->GetGUID());
        if (it == sMountFeedingStore.end())
            return;

        if (hasMountSpeed)
            it->second.pendingSpeedUpdate = true;

        // Track mount spell ID for food use grace period
        if (isMountAura)
        {
            it->second.lastMountSpellId = aura->GetId();
            it->second.flyingDisabled = false;
        }
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        if (!sEnabled)
            return;

        Player* player = unit->ToPlayer();
        if (!player)
            return;

        Aura* aura = aurApp->GetBase();
        if (!aura)
            return;

        bool isMountAura = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            AuraEffect* effect = aura->GetEffect(i);
            if (!effect)
                continue;

            if (effect->GetAuraType() == SPELL_AURA_MOUNTED)
            {
                isMountAura = true;
                break;
            }
        }

        if (!isMountAura)
            return;

        auto it = sMountFeedingStore.find(player->GetGUID());
        if (it == sMountFeedingStore.end())
            return;

        // Record dismount time for grace period (client dismounts before food use)
        it->second.dismountTimeMs = GameTime::GetGameTimeMS().count();

        // Clear base speeds and flying state on dismount
        it->second.baseGroundSpeed = 0;
        it->second.baseFlyingSpeed = 0;
        it->second.pendingSpeedUpdate = false;
        it->second.flyingDisabled = false;
    }
};

// ------- Player Script: login/logout/update/level -------

class MountFeedingPlayerScript : public PlayerScript
{
public:
    MountFeedingPlayerScript() : PlayerScript("MountFeedingPlayerScript",
        {PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LEVEL_CHANGED}) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!sEnabled || !player)
            return;

        ObjectGuid guid = player->GetGUID();
        int32 satisfaction = sDefaultSatisfaction;

        QueryResult result = CharacterDatabase.Query("SELECT `satisfaction` FROM `mount_feeding` WHERE `guid` = {}",
            guid.GetCounter());

        if (result)
        {
            Field* fields = result->Fetch();
            satisfaction = fields[0].Get<int32>();
        }

        MountFeedingData data{};
        data.satisfaction       = std::clamp(satisfaction, 0, SATISFACTION_MAX);
        data.pendingSpeedUpdate = false;
        data.decayTimer         = sDecayInterval;
        data.saveTimer          = sSaveInterval;
        data.lastState          = GetSatisfactionState(data.satisfaction);
        data.baseGroundSpeed    = 0;
        data.baseFlyingSpeed    = 0;
        data.lastMountSpellId   = 0;
        data.dismountTimeMs     = 0;
        data.flyingDisabled     = false;

        sMountFeedingStore[guid] = data;
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        ObjectGuid guid = player->GetGUID();
        auto it = sMountFeedingStore.find(guid);
        if (it != sMountFeedingStore.end())
        {
            SaveSatisfaction(guid, it->second.satisfaction);
            sMountFeedingStore.erase(it);
        }
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!sEnabled || !player)
            return;

        auto it = sMountFeedingStore.find(player->GetGUID());
        if (it == sMountFeedingStore.end())
            return;

        MountFeedingData& data = it->second;

        // Handle pending speed update (deferred from OnAuraApply)
        if (data.pendingSpeedUpdate && player->IsMounted())
        {
            data.pendingSpeedUpdate = false;

            // Capture current aura amounts (mod-mount-scaling has already set them)
            data.baseGroundSpeed = 0;
            data.baseFlyingSpeed = 0;

            auto groundEffects = player->GetAuraEffectsByType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
            if (!groundEffects.empty())
                data.baseGroundSpeed = groundEffects.front()->GetAmount();

            auto flyingEffects = player->GetAuraEffectsByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
            if (!flyingEffects.empty())
                data.baseFlyingSpeed = flyingEffects.front()->GetAmount();

            // Apply satisfaction penalty and flying state
            uint8 state = GetSatisfactionState(data.satisfaction);
            if (state != STATE_HAPPY)
                ApplySpeedPenalty(player, data);
            UpdateFlyingState(player, data);

            // Show current satisfaction state on mount
            SendStateMessage(player, state);
        }

        // Slow Fall management for unhappy grounded mounts
        if (data.flyingDisabled && player->IsMounted())
        {
            if (player->IsFalling())
            {
                // Falling: apply or refresh Slow Fall so it never expires mid-air
                if (Aura* aura = player->GetAura(130))
                    aura->SetDuration(aura->GetMaxDuration());
                else
                    player->CastSpell(player, 130, true);
            }
            else if (player->HasAura(130))
            {
                // Landed: remove Slow Fall immediately
                player->RemoveAura(130);
            }
        }

        // Decay timer
        bool shouldDecay = sDecayOnlyWhileMounted ? player->IsMounted() : true;
        if (shouldDecay)
        {
            data.decayTimer -= static_cast<int32>(diff);
            if (data.decayTimer <= 0)
            {
                data.decayTimer = sDecayInterval;

                // Determine decay multiplier based on movement state
                float decayMult = sDecayMultStationary;
                if (player->IsMounted())
                {
                    if (player->IsFlying())
                        decayMult = sDecayMultFlying;
                    else if (player->isMoving())
                        decayMult = sDecayMultMoving;
                }

                int32 decayAmount = static_cast<int32>(sDecayAmount * decayMult);

                uint8 oldState = GetSatisfactionState(data.satisfaction);
                data.satisfaction = std::max(0, data.satisfaction - decayAmount);
                uint8 newState = GetSatisfactionState(data.satisfaction);
                data.lastState = newState;

                if (newState != oldState)
                {
                    SendStateMessage(player, newState);
                    if (player->IsMounted())
                    {
                        if (data.baseGroundSpeed > 0)
                            ApplySpeedPenalty(player, data);
                        UpdateFlyingState(player, data);
                    }
                }
            }
        }

        // Periodic save
        data.saveTimer -= static_cast<int32>(diff);
        if (data.saveTimer <= 0)
        {
            data.saveTimer = sSaveInterval;
            SaveSatisfaction(player->GetGUID(), data.satisfaction);
        }
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!sEnabled || !player || !player->IsMounted())
            return;

        auto it = sMountFeedingStore.find(player->GetGUID());
        if (it == sMountFeedingStore.end())
            return;

        // mod-mount-scaling will recalculate speeds, we need to re-capture
        it->second.pendingSpeedUpdate = true;
    }
};

void AddMountFeedingScripts()
{
    new MountFeedingWorldScript();
    new MountFeedingItemScript();
    new MountFeedingUnitScript();
    new MountFeedingPlayerScript();
}
