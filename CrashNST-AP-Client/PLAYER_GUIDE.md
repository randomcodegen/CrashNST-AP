# Crash N. Sane Trilogy — Archipelago

## Play and configure

| Input | Action |
|---|---|
| F1 / Esc | Open / close the AP console |
| Tab | Complete commands, `!hint` item names and `!hint_location` location names |
| Page Up / Page Down, mouse wheel | Scroll console history |
| F2 | Open / close unlocked entrances. 1/2/3 or Tab switches hub campaign |
| F7 | Toggle AP markers, labels and HUD |
| F10 | Restore normal movement if you're stuck in Crashsanity |

Console input supports Left/Right, Home/End, Shift selection and Ctrl+A/C/V.
Automatic connection attempts stop after five failures.  
Correct the connection settings and use `/connect` to start a fresh attempt.

HUD counters use **`checked/in logic (total)`**. `C`, `P` and `R` show selected gem, power-crystal and relic checks.   
Campaign totals appear in the game choose menu and on hub maps.  
Approach an unlocked hub entrance to see its destination label and check counts.  
`/hubdistance_c1`, `/hubdistance_c2` and `/hubdistance_c3` show each hub's label
distance.   
Add a number to change it in game units (default 250, maximum 100000).  
Use 0 to hide that hub's labels.  
`/hubdistance 500` sets all three hubs to 500.  
Settings are saved locally and restored next time.

Useful local commands:

```text
/help
/status
/boxmarkers tint/bounds/off
/tint_opacity 0.5
/hubdistance 500
/hubdistance_c1 1000
/hubdistance_c2 250
/hubdistance_c3 500
/outoflogic on/off
/disconnect
/connect
```

Markers normally show unchecked selected crates that are in logic. `/outoflogic on` also shows out-of-logic crates. 
`/tint_opacity` shows the current tint strength.  
Set it from 0 (no tint) to 1 (full tint), for example `/tint_opacity 0.5` for 50%. The default is 0.75.
`box_item_spoilers` in the yaml can color markers by item category. Chat and `!` commands go to the AP server.

When item spoilers are enabled, the client scouts selected locations on connection without creating hints. Item flags are cached per seed and slot for offline play. Boxes use the normal marker color until their flags arrive.

Press **F2** for unlocked entrances, grouped by physical hub campaign.

## World options

Secret destinations include required source levels automatically.

- Choose any campaign combination, level pool and level counts.
- `starting_levels`: starting entrances per enabled campaign, from 1–31 (default 2), capped at the selected regular entrances. Fill the earliest available hub areas first. Later areas start unlocked only if the requested count needs them or the pool has no earlier entrances. The selected pool is preserved.
- `level_shuffle`: `off`, `within_campaign` or `across_campaigns`.
- `boxsanity`: 0–100% of the boxes turn into ap locations. There are 5291 boxes across 85 stages.
- `white_gem_checks`, `colored_gem_checks`, `crystal_checks`, `relic_checks`: percentages of collectible locations to select as AP checks. Unselected collectibles stay vanilla or are overwritten by collectible_rewards.
- `collectible_rewards`: per-category `randomized`, `vanilla` or `precollected`. Randomized: Default shuffle behaviour. Vanilla: selected checks keep their normal items. Precollected: randomizes selected checks and gives the unselected collectibles from the start.
- `goal`: `all_cortex`, `cortex_1`, `cortex_2`, `cortex_3`, `level_clears` or `token_hunt`. `goal_count` sets the level-clear target and is capped at the selected regular-level count.
- `require_all_bosses`: `false` (default), `true`, or `cortex_last`. For Cortex goals, `true` requires every boss in the goal campaigns in any order. `cortex_last` also locks each goal campaign's Cortex until its other bosses are defeated.
- `cortex_requirements`: optional list containing `level_clears`, `token_hunt`, or both. With a Cortex goal, every selected requirement must be met before the goal Cortex entrance(s) unlock.
- `token_percentage`: replace 1–100% of the remaining filler slots with AP Tokens (default 20%).
- `token_required_percentage`: collect 1–100% of those tokens to win (default 80%). 
- `death_link` is optional and defaults off. Deaths in bonus rounds do not send DeathLink.
- `crashsanity` is experimental and defaults off.

Secret-route logic requires **Snow Go's red gem → Air Crash**, **Air Crash's box gem → Bear Down**, and **Road to Ruin's box gem → Diggin' It**.

Totally Bear/Fly, Hot Coco and Eggipus Rex require their usual secret paths and the chosen access locks.  
The CB3 secret-room elevator opens automatically while AP is active, without relics or touching a level button first. Its level entrances still follow AP access locks.

With Hub Area Locks enabled, each **Crash 1 - Progressive Island Access** opens the next island (two copies total).  
Each **Crash 2 - Progressive Elevator Access** opens the next warp-room floor (four copies total).  
Starting entrances fill the earliest selected hub areas first. Any required later-area copies are granted from the start.
Crash 3 uses a separate **Hub Section Unlock** for each gated section.  
Boss defeats do not open hub areas.

Leaving a level returns to the physical entrance used, including shuffled levels in another campaign.
Continue remembers the last hub entrance used in each campaign for this seed and slot, including cross-campaign shuffled levels.

Filler rewards are Wumpa Fruit and 1-Ups. Rewards received on hubs wait until gameplay and apply once.

## Update, disable and troubleshoot

Replace the package's `dinput8.dll` and merge its `CrashNSTAP` folder into the game folder.

To disable AP, close NST and remove `dinput8.dll` from the game folder.

Local profiles, connection settings, display settings and runtime logs are under `%LOCALAPPDATA%/CrashNSTAP/state`
The game-folder loader log is `CrashNSTAP/loader.log`

Example: collect tokens and clear levels before fighting Cortex:

```yaml
goal: all_cortex
cortex_requirements:
  - level_clears
  - token_hunt
goal_count: 10
token_percentage: 20
token_required_percentage: 80
```
