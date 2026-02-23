# mod-mount-feeding

Mount satisfaction system for AzerothCore 3.3.5a. Mounts must be fed to maintain full speed — similar to the hunter pet happiness mechanic. Right-clicking food while mounted feeds the mount instead of the player. Hungry mounts move slower, and unhappy mounts cannot fly.

Designed for the [Emerald Dream](https://github.com/claudevandort/azerothcore-wotlk) private server. Compatible with [mod-mount-scaling](https://github.com/claudevandort/mod-mount-scaling).

## How It Works

Each player has a **satisfaction** value (0–999,000) that decays while mounted and increases when the mount is fed. Satisfaction determines the mount's state:

| State | Satisfaction Range | Speed | Flying |
|---|---|---|---|
| Happy (green) | 666,000 – 999,000 | 100% of mount speed | Allowed |
| Content (yellow) | 333,000 – 665,999 | 75% of mount speed | Allowed |
| Unhappy (red) | 0 – 332,999 | 50% of mount speed | Disabled |

Speed penalties are applied as multipliers on top of the base mount speed (including mod-mount-scaling values if present). For example, a mount with 150% speed bonus at Content state: 150% × 0.75 = 112% bonus → 212% total movement speed.

New characters start at maximum satisfaction (999,000 — Happy).

## Feeding

Right-click any food item while mounted to feed the mount. The food is consumed, satisfaction increases, and the normal eat/sit behavior is blocked.

Any food with a `FoodType` (meat, fish, bread, cheese, fruit, fungus, etc.) works — mounts are not picky.

### Food Benefit

The benefit depends on the level gap between the player and the food's item level:

| Player Level − Food Item Level | Benefit |
|---|---|
| 0 to 5 (level-appropriate) | +350,000 |
| 6 to 10 | +175,000 |
| 11 to 14 | +80,000 |
| 15+ (too low) | 0 (rejected) |

One level-appropriate food moves the mount roughly one full tier (e.g., Unhappy → Content or Content → Happy). Food that is too low-level for the player is rejected with a message.

### Feeding While Mounted — Client Behavior

The 3.3.5a WoW client automatically dismounts the player when food is right-clicked (it sends a cancel-mount packet before the item-use packet). The module handles this with a 1-second grace period: if the player was mounted within the last second and uses food, it intercepts the food for mount feeding instead of normal consumption. The player is dismounted briefly by the client but does not sit down or eat — they simply remount manually.

## Decay

Satisfaction decays over time while mounted. The base rate is 670 points every 7.5 seconds, scaled by a multiplier based on movement state:

| Movement State | Default Multiplier | Time from Happy → Content |
|---|---|---|
| Stationary (mounted, not moving) | 0.5× | ~124 minutes |
| Moving on ground | 1.0× | ~62 minutes |
| Flying | 1.5× | ~41 minutes |

Decay only occurs while mounted (configurable). Dismounting preserves the current satisfaction value.

## Flying Prevention

When a mount reaches Unhappy state on a flying mount:

- **In the air**: `SetCanFly(false)` is sent to the client. The player stays mounted but begins a natural descent. Slow Fall is automatically applied while falling and removed on landing, preventing fall damage.
- **On the ground**: The player cannot take off. Spacebar has no effect. Ground movement continues at the Unhappy speed penalty.
- **Recovery**: Feeding the mount back to Content or Happy re-enables flight via `SetCanFly(true)`.

## mod-mount-scaling Integration

Both modules hook `OnAuraApply` and call `ChangeAmount()` on mount speed auras. Since hook execution order between modules is not guaranteed, mod-mount-feeding uses a **deferred speed capture**:

1. `OnAuraApply` detects a mount speed aura → sets a `pendingSpeedUpdate` flag
2. `OnPlayerUpdate` (next server tick) reads the current aura amounts — by this point mod-mount-scaling has already set its values
3. These values are stored as `baseGroundSpeed` / `baseFlyingSpeed`
4. The satisfaction multiplier is applied: `base × multiplier` via `ChangeAmount()`
5. On level-up while mounted, the flag is set again to re-capture post-scaling values

## Installation

### Local (Docker Compose)

Clone into the `modules/` directory — AzerothCore auto-discovers it via cmake:

```bash
git clone https://github.com/claudevandort/mod-mount-feeding.git modules/mod-mount-feeding
docker compose up -d --build
```

The `docker-compose.override.yml` should mount `./modules` into the build container:

```yaml
services:
  ac-worldserver:
    volumes:
      - ./modules:/azerothcore/modules:ro
```

### SQL

Apply the character database table after the first build:

```bash
docker compose exec ac-database mysql -uroot -p<password> acore_characters < \
  modules/mod-mount-feeding/data/sql/db_characters/mount_feeding.sql
```

This creates the `mount_feeding` table in `acore_characters`:

```sql
CREATE TABLE IF NOT EXISTS `mount_feeding` (
    `guid` INT UNSIGNED NOT NULL,
    `satisfaction` INT UNSIGNED NOT NULL DEFAULT 999000,
    PRIMARY KEY (`guid`)
);
```

## Configuration

All settings have sensible defaults and work without a config file. To override, create `mount_feeding.conf` in the modules config directory, or use environment variables with the `AC_` prefix (e.g., `AC_MOUNT_FEEDING_ENABLE=0`).

| Config | Default | Description |
|---|---|---|
| `MountFeeding.Enable` | `1` | Enable/disable the module |
| `MountFeeding.ContentSpeedMultiplier` | `0.75` | Speed multiplier at Content state |
| `MountFeeding.UnhappySpeedMultiplier` | `0.50` | Speed multiplier at Unhappy state |
| `MountFeeding.DecayAmount` | `670` | Base satisfaction loss per decay tick |
| `MountFeeding.DecayInterval` | `7500` | Decay tick interval in milliseconds |
| `MountFeeding.DecayOnlyWhileMounted` | `1` | Only decay while mounted |
| `MountFeeding.DecayMultiplier.Stationary` | `0.50` | Decay multiplier when mounted but idle |
| `MountFeeding.DecayMultiplier.Moving` | `1.0` | Decay multiplier when moving on ground |
| `MountFeeding.DecayMultiplier.Flying` | `1.5` | Decay multiplier when flying |
| `MountFeeding.DefaultSatisfaction` | `999000` | Starting satisfaction for new characters |
| `MountFeeding.UnhappyNoFly` | `1` | Prevent flying at Unhappy state |
| `MountFeeding.SaveInterval` | `300000` | Periodic DB save interval (ms, default 5 min) |

## Player Feedback

State change messages appear as colored system chat:

- Feeding: `"Your mount happily eats the [item name]."`
- Already full: `"Your mount is already fully satisfied."`
- Food too low: `"That food is too low level for your mount."`
- → Happy: `|cff00ff00Your mount is happy and moving at full speed.|r`
- → Content: `|cffffff00Your mount is getting hungry. Speed reduced to 75%.|r`
- → Unhappy: `|cffff0000Your mount is unhappy! Speed reduced to 50% and cannot fly.|r`

## File Structure

```
modules/mod-mount-feeding/
├── src/
│   ├── MountFeeding_loader.cpp      # Entry point: Addmod_mount_feedingScripts()
│   └── MountFeeding.cpp             # All script classes and logic
├── conf/
│   └── mount_feeding.conf.dist      # Default configuration
├── data/
│   └── sql/
│       └── db_characters/
│           └── mount_feeding.sql    # Character DB table
└── README.md
```

### Script Classes

| Class | Base | Hooks | Purpose |
|---|---|---|---|
| `MountFeedingWorldScript` | `WorldScript` | `OnAfterConfigLoad` | Loads config values |
| `MountFeedingItemScript` | `AllItemScript` | `CanItemUse` | Intercepts food use while mounted |
| `MountFeedingUnitScript` | `UnitScript` | `OnAuraApply`, `OnAuraRemove` | Tracks mount auras, defers speed update |
| `MountFeedingPlayerScript` | `PlayerScript` | `OnLogin`, `OnLogout`, `OnUpdate`, `OnLevelChanged` | DB load/save, decay, speed application |

## Edge Cases

- **Dismount/remount**: Satisfaction is preserved. Base speeds are recaptured on remount via `pendingSpeedUpdate`.
- **Death**: Dismounts the player. Satisfaction is unchanged.
- **Level up while mounted**: `pendingSpeedUpdate` flag re-captures post-mod-mount-scaling values.
- **Feeding when not mounted**: Falls through to normal food consumption (player eats normally).
- **Already at max satisfaction**: Message shown, food is NOT consumed.
- **Server crash**: Periodic save (every 5 min) limits data loss. Satisfaction is also saved on logout.
- **Ground mounts**: Flying prevention logic is skipped (no flight speed aura to manage).
- **Falling while unhappy on flying mount**: Slow Fall is applied on each tick while falling and removed immediately on landing.

## Testing

For faster testing, override decay via environment variables in `docker-compose.override.yml`:

```yaml
services:
  ac-worldserver:
    environment:
      - AC_MOUNT_FEEDING_DECAY_AMOUNT=3000
      - AC_MOUNT_FEEDING_DECAY_INTERVAL=5000
```

This makes satisfaction drop ~6× faster (~90 seconds from Happy to Content instead of ~62 minutes).

### GM Commands for Testing

```
.additem 35953 20     -- Mead Basted Caribou (level 85 food)
.additem 117 5        -- Tough Jerky (level 5 food — too low at high levels)
.learn 33388          -- Apprentice Riding
.learn 34090          -- Expert Riding (flying)
```

### Speed Check Macro

```
/script DEFAULT_CHAT_FRAME:AddMessage("Speed: "..string.format("%.1f%%", GetUnitSpeed("player")/7*100))
```
