"""player options"""

from dataclasses import dataclass

from Options import (
    Choice,
    DefaultOnToggle,
    OptionDict,
    OptionSet,
    PerGameCommonOptions,
    Range,
    Toggle,
    DeathLink,
)
from .data import LEVELS


class HubAreaLocks(Toggle):
    """Require AP area-unlock items for later islands, elevator floors and gated hub sections.

    Crash 1 has Progressive Island Access items,
    Crash 2 has Progressive Elevator Access items
    Crash 3 sections has Hub Section X Unlock items.
    """

    display_name = "Hub Area Locks"


class LevelAccessLocks(DefaultOnToggle):
    """Require each selected level's Access item. Independent of Hub Area Locks.

    When disabled, selected levels only require their hub area and secret-route requirements.
    """

    display_name = "Level Access Locks"


class ActiveHubs(Choice):
    """Choose which games' hubs and bosses are used. Does not filter the level pool."""

    display_name = "Active Hubs"
    option_all = 0
    option_crash_1 = 1
    option_crash_2 = 2
    option_crash_3 = 3
    option_crash_1_2 = 4
    option_crash_1_3 = 5
    option_crash_2_3 = 6
    default = option_all


class LevelCount(Range):
    """Total regular levels to draw from Level Pool, including secret stages.

    Limited by the pool and available hub entrances. Bosses are added separately.
    """

    display_name = "Level Count"
    range_start = 1
    range_end = 85
    default = 85


class StartingLevels(Range):
    """Number of starting level entrances per active hub.

    Fills the earliest hub areas available among the selected entrances first,
    then unlocks more if needed.
    """

    display_name = "Starting Levels per Hub"
    range_start = 1
    range_end = 31
    default = 2


class Goal(Choice):
    """Choose what the player must complete to win.

    All Cortex requires defeating Cortex in every active hub game.
    Single-Cortex goals require that game's hub to be active.
    Cortex entrances follow the chosen hub area and level access locks.

    Level clears count distinct regular levels from the selected destination pool.

    Token hunt replaces a percentage of filler items with AP Tokens.
    Collecting the required percentage of those tokens wins.
    """

    display_name = "Goal"
    option_all_cortex = 0
    option_cortex_1 = 1
    option_cortex_2 = 2
    option_cortex_3 = 3
    option_level_clears = 4
    option_token_hunt = 5
    default = option_all_cortex


class RequireAllBosses(Choice):
    """Require every boss, including Cortex, in the campaigns targeted by the goal.

    Applies only to Cortex goals. Required bosses are included even if omitted
    from the level pool. Level-clear and token-hunt goals are unaffected.
    Cortex Last also locks each goal campaign's Cortex until its other bosses are defeated.
    """

    display_name = "Require All Goal Campaign Bosses"
    option_false = 0
    option_true = 1
    option_cortex_last = 2
    alias_off = 0
    alias_no = 0
    alias_on = 1
    alias_yes = 1
    default = option_false


class CortexRequirements(OptionSet):
    """Extra requirements before the goal's Cortex entrances unlock.

    Select level_clears, token_hunt, or both. Every selected requirement must
    be met, then defeat the goal bosses to win. Used only with Cortex goals.
    """

    display_name = "Cortex Unlock Requirements"
    valid_keys = {"level_clears", "token_hunt"}
    default = frozenset()


class GoalCount(Range):
    """Distinct regular levels to clear for the level-clears goal or Cortex requirement."""

    display_name = "Goal Count"
    range_start = 1
    range_end = 1000
    default = 10


class TokenPercentage(Range):
    """Percentage of filler slots replaced with AP Tokens for token hunt or a Cortex requirement."""

    display_name = "Token Hunt Filler Percentage"
    range_start = 1
    range_end = 100
    default = 20


class TokenRequiredPercentage(Range):
    """Percentage of the available AP Tokens required to win or unlock the goal Cortex bosses."""

    display_name = "Token Hunt Required Percentage"
    range_start = 1
    range_end = 100
    default = 80


class WhiteGemChecks(Range):
    """Percentage of eligible white-gem checks in selected levels."""

    display_name = "White Gem Checks Percent"
    range_start = 0
    range_end = 100
    default = 100


class ColoredGemChecks(Range):
    """Percentage of eligible colored-gem checks in selected levels."""

    display_name = "Colored Gem Checks Percent"
    range_start = 0
    range_end = 100
    default = 100


class CrystalChecks(Range):
    """Percentage of eligible power-crystal checks in selected levels."""

    display_name = "Power Crystal Checks Percent"
    range_start = 0
    range_end = 100
    default = 100


class RelicChecks(Range):
    """Percentage of eligible relic checks in selected levels."""

    display_name = "Relic Checks Percent"
    range_start = 0
    range_end = 100
    default = 100


class LevelShuffle(Choice):
    """Shuffle regular level destinations. Bosses and secret stages keep their entrances.

    Across Campaigns can place any pool level in any active hub.
    Off and Within Campaign use only levels belonging to the active hubs.
    Secret stages keep their vanilla paths and need their source levels in the pool.
    """

    display_name = "Level Placement Shuffle"
    option_off = 0
    option_within_campaign = 1
    option_across_campaigns = 2
    default = option_off


class Boxsanity(Range):
    """Percentage of eligible breakable boxes in selected levels that become AP checks."""

    display_name = "Boxsanity Percent"
    range_start = 0
    range_end = 100
    default = 0


class Crashsanity(Toggle):
    """Experimental: assign Crash 1/2/3 basic movement rules per regular level.

    F10 restores vanilla controls if a style prevents progress."""

    display_name = "Crashsanity"


class CollectibleRewards(OptionDict):
    """Reward modes for power_crystals, white_gems, colored_gems and relics (ankhs).

    randomized (default): shuffle selected rewards.
    vanilla: selected checks keep their original local rewards.
    precollected: shuffle selected rewards and grant unselected collectibles at startup.
    """

    display_name = "Collectible Rewards"
    valid_keys = ("power_crystals", "white_gems", "colored_gems", "relics")
    default = dict.fromkeys(valid_keys, "randomized")

    def modes(self):
        if not isinstance(self.value, dict) or any(
            key not in self.valid_keys
            or value not in ("randomized", "vanilla", "precollected")
            for key, value in self.value.items()
        ):
            raise ValueError(
                f"{self.display_name} accepts power_crystals, white_gems, colored_gems "
                "and relics. Each value must be randomized, vanilla or precollected."
            )
        return {key: self.value.get(key, "randomized") for key in self.valid_keys}

    def verify(self, world, player_name, plando_options):
        super().verify(world, player_name, plando_options)
        self.modes()


class BoxItemSpoilers(Toggle):
    """Color selected, reachable AP boxes by their item classification."""

    display_name = "Box Item Classification Spoilers"


class LevelPool(OptionDict):
    """Allowed levels, grouped under crash_1, crash_2 and crash_3.

    Remove unwanted levels. An empty list or omitted game excludes its levels.
    Use titles, full names or level keys.
    Secret routes also need their source levels in the pool.
    """

    display_name = "Available Level Pool"
    valid_keys = ("crash_1", "crash_2", "crash_3")
    default = {
        f"crash_{game}": [level.title for level in LEVELS if level.campaign == game]
        for game in range(1, 4)
    }

    def verify(self, world, player_name, plando_options):
        super().verify(world, player_name, plando_options)
        from .data import level_pools

        level_pools(self.value)


@dataclass
class CrashOptions(PerGameCommonOptions):
    active_hubs: ActiveHubs
    hub_area_locks: HubAreaLocks
    level_access_locks: LevelAccessLocks
    goal: Goal
    require_all_bosses: RequireAllBosses
    cortex_requirements: CortexRequirements
    goal_count: GoalCount
    token_percentage: TokenPercentage
    token_required_percentage: TokenRequiredPercentage
    level_pool: LevelPool
    level_count: LevelCount
    starting_levels: StartingLevels
    white_gem_checks: WhiteGemChecks
    colored_gem_checks: ColoredGemChecks
    crystal_checks: CrystalChecks
    relic_checks: RelicChecks
    level_shuffle: LevelShuffle
    boxsanity: Boxsanity
    collectible_rewards: CollectibleRewards
    box_item_spoilers: BoxItemSpoilers
    crashsanity: Crashsanity
    death_link: DeathLink
