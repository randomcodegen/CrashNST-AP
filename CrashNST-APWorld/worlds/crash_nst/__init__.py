"""NST Trilogy world"""

import hashlib
from collections import Counter

from BaseClasses import Item, ItemClassification, Location, Region
from worlds.AutoWorld import World
from .Options import CrashOptions
from .data import (
    GAME,
    PROTOCOL,
    LEVELS,
    ITEMS,
    LOCATIONS,
    FINALS,
    GEMS,
    ABILITIES,
    FILLER,
    BY_KEY,
    check_category,
    checks,
    requirements,
    CRATES,
    select_crates,
    level_pools,
    VANILLA_REWARDS,
    VANILLA_ITEMS,
    breakable_crate,
    crate_event,
    crate_location,
    TOKEN,
    validate_victory_goal,
    SECRET_SOURCES,
    ALTERNATE_SOURCES,
    EVENT_IDS,
    check_routes,
    lock_requirements,
    hub_area,
    valid_requirement_counts,
    validate_level_placements,
)


class CrashItem(Item):
    game = GAME


class CrashLocation(Location):
    game = GAME


CAMPAIGN_SETS = {
    0: (1, 2, 3),
    1: (1,),
    2: (2,),
    3: (3,),
    4: (1, 2),
    5: (1, 3),
    6: (2, 3),
}


class CrashWorld(World):
    """NST integration with selectable campaigns and checks"""

    game = GAME
    options_dataclass = CrashOptions
    item_name_to_id = ITEMS
    location_name_to_id = LOCATIONS
    ut_can_gen_without_yaml = True
    required_client_version = (1, 0, 6)

    def create_item(self, name):
        classification = (
            ItemClassification.filler
            if name in (*FILLER, "Nothing")
            else ItemClassification.progression
        )
        if name in VANILLA_ITEMS:
            classification = ItemClassification.useful
        return CrashItem(name, classification, ITEMS[name], self.player)

    def get_filler_item_name(self):
        return self.random.choice(FILLER)

    @staticmethod
    def interpret_slot_data(slot_data):
        # forward slot_data to Universal Tracker
        return slot_data

    def lock_requirements(self, names):
        return lock_requirements(
            names, self.level_placements, self.hub_area_locks, self.level_access_locks
        )

    def requirements(self, level, event):
        return self.lock_requirements(requirements(level, event))

    def box_requirements(self, level, event="box:"):
        return self.requirements(level, event)

    def check_requirements(self, level, event):
        return check_routes(
            level,
            event,
            self.level_placements,
            self.hub_area_locks,
            self.level_access_locks,
        )[0]

    def location_rule(self, level, event, address):
        if self.tracker_slot is None:
            routes = check_routes(
                level,
                event,
                self.level_placements,
                self.hub_area_locks,
                self.level_access_locks,
            )
        elif address in self.tracker_rules:
            routes = [
                self.tracker_rules[address],
                *(
                    tuple(self.item_id_to_name[item] for item in needed)
                    for needed in self.tracker_slot.get(
                        "alternate_check_requirements", {}
                    ).get(str(address), [])
                ),
            ]
        else:
            source = self.tracker_slot["local_reward_sources"][str(address)]
            routes = [
                tuple(self.item_id_to_name[item] for item in needed)
                for needed in [
                    source["requirements"],
                    *source.get("alternate_requirements", []),
                ]
            ]
        routes = tuple(Counter(needed) for needed in routes)
        return lambda state: any(
            state.has_all_counts(needed, self.player) for needed in routes
        )

    def _load_tracker_slot(self):
        """Rebuild tracker state from the server slot without rerolling the seed."""
        slot = self.tracker_slot
        if slot.get("protocol") != PROTOCOL:
            raise ValueError("Unsupported NST tracker slot protocol")
        for name in ("hub_area_locks", "level_access_locks"):
            if type(slot.get(name)) is not bool:
                raise ValueError("Invalid NST tracker lock settings")
            setattr(self, name, slot[name])
        selected = slot["selected_levels"]
        if (
            not isinstance(selected, list)
            or not selected
            or len(set(selected)) != len(selected)
            or any(key not in BY_KEY for key in selected)
        ):
            raise ValueError("Invalid NST tracker level selection")
        self.active_levels = [BY_KEY[key] for key in selected]
        self.goal_levels = slot.get("goal_levels")
        self.victory_goal = validate_victory_goal(
            selected, self.goal_levels, slot.get("victory_goal")
        )
        self.enabled_campaigns = tuple(slot["selected_campaigns"])
        if self.enabled_campaigns not in CAMPAIGN_SETS.values():
            raise ValueError("Invalid NST tracker campaigns")
        self.level_placements = slot["level_placements"]
        validate_level_placements(
            selected,
            self.level_placements,
            self.enabled_campaigns,
            slot["level_shuffle"],
        )
        catalogue = {
            str(address): {"level": level.key, "event": event}
            for level in self.active_levels
            for event, _, address in checks(level)
        }
        crates = {
            str(crate["id"]): crate for crate in CRATES if crate["level"] in selected
        }
        catalogue.update(
            {
                address: {"level": crate["level"], "event": crate_event(crate)}
                for address, crate in crates.items()
            }
        )
        server_checks = slot["checks"]
        if not isinstance(server_checks, dict) or any(
            catalogue.get(address) != check for address, check in server_checks.items()
        ):
            raise ValueError("Invalid NST tracker locations")
        rules = slot["check_requirements"]
        if (
            not isinstance(rules, dict)
            or set(rules) != set(server_checks)
            or any(
                not isinstance(needed, list)
                or not 0 <= len(needed) <= 40
                or any(
                    type(item) is not int or item not in self.item_id_to_name
                    for item in needed
                )
                or not valid_requirement_counts(
                    self.item_id_to_name[item] for item in needed
                )
                for needed in rules.values()
            )
        ):
            raise ValueError("Invalid NST tracker requirements")
        self.tracker_rules = {
            int(address): tuple(self.item_id_to_name[item] for item in needed)
            for address, needed in rules.items()
        }
        alternatives = slot.get("alternate_check_requirements", {})
        if type(alternatives) is not dict or set(alternatives) - set(server_checks):
            raise ValueError("Invalid NST alternate check routes")

        def valid_alternatives(routes):
            return (
                type(routes) is list
                and len(routes) <= 2
                and all(
                    type(needed) is list
                    and len(needed) <= 40
                    and all(
                        type(item) is int and item in self.item_id_to_name
                        for item in needed
                    )
                    and valid_requirement_counts(
                        self.item_id_to_name[item] for item in needed
                    )
                    for needed in routes
                )
            )

        if any(not valid_alternatives(routes) for routes in alternatives.values()):
            raise ValueError("Invalid NST alternate check requirements")
        self.active_checks = {int(address) for address in server_checks}
        if self.victory_goal.get("requirements", {}).get("token_hunt", 0) > len(
            self.active_checks
        ) or (
            self.victory_goal["type"] == "token_hunt"
            and self.victory_goal["required"] > len(self.active_checks)
        ):
            raise ValueError("Token target exceeds selected checks")
        self.active_crates = [
            crate for address, crate in crates.items() if address in server_checks
        ]
        self.locked_collectibles = {}
        self.local_reward_sources = {}
        if not isinstance(slot.get("collectible_locations"), dict) or not isinstance(
            slot.get("local_reward_sources"), dict
        ):
            raise ValueError(
                "Unsupported NST collectible data. Generate a seed with the current APWorld"
            )
        for address, source in slot["local_reward_sources"].items():
            if (
                address not in catalogue
                or address in server_checks
                or int(address) not in VANILLA_REWARDS
                or not isinstance(source, dict)
                or {key: source.get(key) for key in ("level", "event")}
                != catalogue[address]
                or source.get("item") != ITEMS.get(VANILLA_REWARDS.get(int(address)))
                or not isinstance(source.get("requirements"), list)
                or len(source["requirements"]) > 40
                or any(
                    type(item) is not int or item not in self.item_id_to_name
                    for item in source["requirements"]
                )
                or not valid_requirement_counts(
                    self.item_id_to_name[item] for item in source["requirements"]
                )
                or not valid_alternatives(source.get("alternate_requirements", []))
            ):
                raise ValueError("Invalid NST tracker local collectible")
            self.local_reward_sources[int(address)] = VANILLA_REWARDS[int(address)]

    def generate_early(self):
        self.tracker_slot = getattr(self.multiworld, "re_gen_passthrough", {}).get(GAME)
        if self.tracker_slot is not None:
            self._load_tracker_slot()
            return
        self.hub_area_locks = bool(self.options.hub_area_locks.value)
        self.level_access_locks = bool(self.options.level_access_locks.value)
        rewards = self.options.collectible_rewards.modes()
        self.vanilla_collectibles = {
            key: mode == "vanilla" for key, mode in rewards.items()
        }
        self.selected_collectibles = {}
        self.local_reward_sources = {}
        self.shuffled_collectibles = {}
        self.precollected_collectibles = {}
        self.locked_collectibles = {}
        self.enabled_campaigns = CAMPAIGN_SETS[self.options.active_hubs.value]
        pools = level_pools(self.options.level_pool.value)
        self.available_level_pool = {
            f"crash_{game}": [level.key for level in pools.get(game, ())]
            for game in (1, 2, 3)
        }
        self.level_placements = self.select_pool_placements(pools)
        chosen = set(self.level_placements.values())
        chosen.update(key for key in FINALS if int(key[1]) in self.enabled_campaigns)
        chosen.update(
            level.key
            for game in self.enabled_campaigns
            for level in pools.get(game, ())
            if level.key.startswith("B")
        )
        goal = self.options.goal.value
        requirements = self.options.cortex_requirements.value
        if requirements and goal > 3:
            raise ValueError("Cortex Unlock Requirements need a Cortex goal")
        all_bosses = goal <= 3 and bool(self.options.require_all_bosses.value)
        if goal == 0:
            self.goal_levels = [key for key in FINALS if key in chosen]
        elif goal <= 3:
            self.goal_levels = [FINALS[goal - 1]]
        else:
            self.goal_levels = []
        if any(
            BY_KEY[key].campaign not in self.enabled_campaigns
            for key in self.goal_levels
        ):
            raise ValueError("The goal hub must be active")
        if all_bosses:
            goal_campaigns = {BY_KEY[key].campaign for key in self.goal_levels}
            self.goal_levels = [
                level.key
                for level in LEVELS
                if level.key.startswith("B") and level.campaign in goal_campaigns
            ]
            chosen.update(self.goal_levels)
        self.active_levels = [level for level in LEVELS if level.key in chosen]
        if goal == 4 or "level_clears" in requirements:
            self.options.goal_count.value = min(
                self.options.goal_count.value,
                sum(level.key.startswith("L") for level in self.active_levels),
            )
        if goal <= 3:
            victory = {
                "type": (
                    "cortex_last"
                    if self.options.require_all_bosses.value == 2
                    else "all_bosses" if all_bosses else "cortex"
                ),
                "required": 0,
            }
        elif goal == 4:
            victory = {
                "type": "level_clears",
                "required": self.options.goal_count.value,
            }
        else:
            # create_items sets the final token target once filler capacity is known.
            victory = {"type": "token_hunt", "required": 1}
        if requirements:
            victory = {
                "type": "cortex_requirements",
                "required": 0,
                "boss_mode": victory["type"],
                "requirements": {
                    name: self.options.goal_count.value if name == "level_clears" else 1
                    for name in sorted(requirements)
                },
            }
        self.victory_goal = validate_victory_goal(chosen, self.goal_levels, victory)
        self.level_placements.update(
            (key, key) for key in chosen if key.startswith("B")
        )
        self.movement_styles = (
            {
                level.key: 1
                + int.from_bytes(
                    hashlib.sha256(
                        f"{self.multiworld.seed}/{self.player}/crashsanity/{level.key}".encode()
                    ).digest()[:4],
                    "big",
                )
                % 3
                for level in self.active_levels
                if level.key.startswith("L")
            }
            if self.options.crashsanity.value
            else {}
        )
        self.starting_levels = []
        for game in self.enabled_campaigns:
            entrances = [
                key
                for key in self.level_placements
                if BY_KEY[key].campaign == game
                and key.startswith("L")
                and key not in SECRET_SOURCES
            ]
            entrances.sort(key=hub_area)
            # fill the earliest hub areas before granting access to later areas
            self.starting_levels.extend(
                self.level_placements[key]
                for key in entrances[: self.options.starting_levels.value]
            )
        choices = {
            "white_gem": self.options.white_gem_checks.value,
            "colored_gem": self.options.colored_gem_checks.value,
            "crystal": self.options.crystal_checks.value,
            "relic": self.options.relic_checks.value,
        }
        groups = {category: [] for category in choices}
        self.active_checks = set()
        for level in self.active_levels:
            for event, _, address in checks(level):
                category = check_category(level, event)
                if category == "clear":
                    self.active_checks.add(address)
                else:
                    groups[category].append(address)
        for category, addresses in groups.items():
            ranked = sorted(
                addresses,
                key=lambda address: hashlib.sha256(
                    f"{self.multiworld.seed}/{self.player}/{address}".encode()
                ).digest(),
            )
            percentage = choices[category]
            quota = (len(ranked) * percentage + 99) // 100
            selected = ranked[:quota]
            option = {
                "white_gem": "white_gems",
                "colored_gem": "colored_gems",
                "crystal": "power_crystals",
                "relic": "relics",
            }[category]
            self.active_checks.update(selected)
            self.selected_collectibles.update(
                (address, VANILLA_REWARDS[address]) for address in selected
            )
            self.local_reward_sources.update(
                (address, VANILLA_REWARDS[address]) for address in ranked[quota:]
            )
            if rewards[option] == "precollected":
                self.precollected_collectibles.update(
                    (address, VANILLA_REWARDS[address]) for address in ranked[quota:]
                )
            if not self.vanilla_collectibles[option]:
                self.shuffled_collectibles.update(
                    (address, VANILLA_REWARDS[address]) for address in selected
                )
        pool = {
            str(crate["id"]): crate
            for crate in CRATES
            if crate["level"] in chosen and breakable_crate(crate)
        }
        percentage = self.options.boxsanity.value
        sample = select_crates(
            pool, percentage, f"{self.multiworld.seed}/{self.player}/boxes"
        )
        self.active_crates = [pool[key] for key in sample]
        self.active_checks.update(crate["id"] for crate in self.active_crates)

    def select_pool_placements(self, pools):
        """draw one total level selection and fit it into the active hubs"""
        shuffle = self.options.level_shuffle.value
        entrances = {}
        for game in self.enabled_campaigns:
            keys = [
                level.key
                for level in LEVELS
                if level.campaign == game
                and level.key.startswith("L")
                and level.key not in SECRET_SOURCES
            ]
            entrances[game] = self.random.sample(keys, len(keys))
        entrances = [
            keys[index]
            for index in range(max(map(len, entrances.values())))
            for keys in entrances.values()
            if index < len(keys)
        ]
        candidates = [
            level.key
            for game in (1, 2, 3)
            for level in pools.get(game, ())
            if level.key.startswith("L")
            and (shuffle == 2 or level.campaign in self.enabled_campaigns)
        ]
        allowed = set(candidates)
        ranked = self.random.sample(candidates, len(candidates))
        # when hubs are the limit, reserve space for secret paths before ordinary stages
        if self.options.level_count.value > len(entrances):
            ranked.sort(key=lambda key: key not in SECRET_SOURCES)
        sources = {**SECRET_SOURCES, **ALTERNATE_SOURCES}
        chosen = set()
        for key in ranked:
            family = {key}
            while any(
                member in sources and sources[member] not in family for member in family
            ):
                family.update(
                    sources[member] for member in tuple(family) if member in sources
                )
            combined = chosen | family
            if (
                family <= allowed
                and len(combined) <= self.options.level_count.value
                and len(combined - SECRET_SOURCES.keys()) <= len(entrances)
            ):
                chosen = combined
        if not chosen:
            raise ValueError(
                "Level Pool has no playable selection. Keep secret-route source levels "
                "in the pool, increase Level Count, or use Across Campaigns shuffle."
            )
        placements = {key: key for key in sorted(chosen)}
        if shuffle:
            groups = (
                [self.enabled_campaigns]
                if shuffle == 2
                else [(game,) for game in self.enabled_campaigns]
            )
            for games in groups:
                destinations = [
                    key
                    for key in chosen
                    if key not in SECRET_SOURCES
                    and (shuffle == 2 or int(key[1]) in games)
                ]
                destinations.sort(
                    key=lambda key: hashlib.sha256(
                        f"{self.multiworld.seed}/{self.player}/level_shuffle/{shuffle}/{key}".encode()
                    ).digest()
                )
                physical = entrances if shuffle == 2 else sorted(destinations)
                for destination in destinations:
                    placements.pop(destination)
                placements.update(zip(physical, destinations))
        return placements

    def create_regions(self):
        menu = Region("Menu", self.player, self.multiworld)
        self.multiworld.regions.append(menu)
        entrances = {
            destination: source for source, destination in self.level_placements.items()
        }
        for level in self.active_levels:
            region = Region(level.name, self.player, self.multiworld)
            self.multiworld.regions.append(region)
            entry_rule = self.location_rule(
                level, "clear", EVENT_IDS[level.key, "clear"]
            )
            bosses = tuple(
                f"{BY_KEY[key].name} Defeated"
                for key in self.goal_levels
                if self.victory_goal.get("boss_mode", self.victory_goal["type"])
                == "cortex_last"
                and level.key in FINALS
                and level.key in self.goal_levels
                and key not in FINALS
                and BY_KEY[key].campaign == level.campaign
            )
            menu.connect(
                region,
                name=f"{BY_KEY[entrances[level.key]].name} Entrance",
                rule=lambda state, entry_rule=entry_rule, bosses=bosses, final=(
                    level.key in FINALS and level.key in self.goal_levels
                ): (
                    entry_rule(state)
                    and state.has_all(bosses, self.player)
                    and (not final or self.cortex_requirements_met(state))
                ),
            )
            for event, name, address in checks(level):
                if (
                    address in self.local_reward_sources
                    and self.local_reward_sources[address] in GEMS
                ):
                    location = CrashLocation(
                        self.player, f"{name} - Local Pickup Event", None, region
                    )
                    location.access_rule = self.location_rule(level, event, address)
                    location.place_locked_item(
                        CrashItem(
                            self.local_reward_sources[address],
                            ItemClassification.progression,
                            None,
                            self.player,
                        )
                    )
                    region.locations.append(location)
                if address not in self.active_checks:
                    continue
                location = CrashLocation(self.player, name, address, region)
                location.access_rule = self.location_rule(level, event, address)
                region.locations.append(location)
                category = {
                    "crystal": "power_crystals",
                    "white_gem": "white_gems",
                    "colored_gem": "colored_gems",
                    "relic": "relics",
                }.get(check_category(level, event))
                if (
                    self.tracker_slot is None
                    and category
                    and self.vanilla_collectibles[category]
                ):
                    reward = VANILLA_REWARDS[address]
                    location.place_locked_item(self.create_item(reward))
                    self.locked_collectibles[address] = reward
            for crate in self.active_crates:
                if crate["level"] != level.key:
                    continue
                location = CrashLocation(
                    self.player, crate_location(crate), crate["id"], region
                )
                location.access_rule = self.location_rule(
                    level, crate_event(crate), crate["id"]
                )
                region.locations.append(location)
            if level.key in self.goal_levels:
                victory = CrashLocation(
                    self.player, f"{level.name} - Victory Event", None, region
                )
                needed = self.requirements(level, "clear")
                if self.tracker_slot is not None:
                    needed = self.tracker_rules[
                        next(
                            address
                            for event, _, address in checks(level)
                            if event == "clear"
                        )
                    ]
                victory.access_rule = lambda state, needed=Counter(
                    needed
                ): state.has_all_counts(needed, self.player)
                victory.place_locked_item(
                    CrashItem(
                        f"{level.name} Defeated",
                        ItemClassification.progression,
                        None,
                        self.player,
                    )
                )
                region.locations.append(victory)
            if (
                self.victory_goal["type"] == "level_clears"
                or "level_clears" in self.victory_goal.get("requirements", {})
            ) and level.key.startswith("L"):
                clear = CrashLocation(
                    self.player, f"{level.name} - Clear Event", None, region
                )
                clear.access_rule = self.location_rule(
                    level, "clear", EVENT_IDS[level.key, "clear"]
                )
                clear.place_locked_item(
                    CrashItem(
                        "Level Cleared",
                        ItemClassification.progression,
                        None,
                        self.player,
                    )
                )
                region.locations.append(clear)

    def create_items(self):
        if self.tracker_slot is not None:
            return
        starts = Counter(
            self.lock_requirements(
                f"{BY_KEY[key].name} - Access" for key in self.starting_levels
            )
        )
        starts.update(self.precollected_collectibles.values())
        for name in sorted(starts.elements()):
            self.multiworld.push_precollected(self.create_item(name))
        required = Counter(
            self.lock_requirements(
                f"{level.name} - Access" for level in self.active_levels
            )
        )
        for level in self.active_levels:
            for event, _, address in checks(level):
                if (
                    address in self.active_checks
                    or self.local_reward_sources.get(address) in GEMS
                ):
                    required |= Counter(self.requirements(level, event))
        for crate in self.active_crates:
            required |= Counter(
                self.box_requirements(BY_KEY[crate["level"]], crate_event(crate))
            )
        required -= starts
        locked = set(self.locked_collectibles.values())
        local_reward_items = set(self.local_reward_sources.values())
        shuffled = set(self.shuffled_collectibles.values()) - set(starts)
        for name in locked | local_reward_items | shuffled:
            required.pop(name, None)
        optional = [
            name
            for name in (*GEMS, *ABILITIES)
            if name not in required
            and name not in starts
            and name not in locked
            and name not in shuffled
            and name not in local_reward_items
            and int(name[6]) in {level.campaign for level in self.active_levels}
        ]
        capacity = len(self.active_checks) - len(self.locked_collectibles)
        progression_count = sum(required.values()) + len(shuffled)
        if progression_count > capacity:
            raise ValueError(
                "Selected randomized checks cannot hold required progression items. "
                "Enable more box checks or select fewer AP locations"
            )
        names = (
            sorted([*required.elements(), *shuffled])
            + optional[: capacity - progression_count]
        )
        pool = [self.create_item(name) for name in names]
        for item in pool:
            if item.name not in required and not (
                item.name in shuffled and item.name in GEMS
            ):
                item.classification = ItemClassification.useful
        tokens = 0
        if self.victory_goal[
            "type"
        ] == "token_hunt" or "token_hunt" in self.victory_goal.get("requirements", {}):
            filler = capacity - len(pool)
            if not filler:
                raise ValueError(
                    "Token Hunt requires at least one filler slot after progression, "
                    "collectible rewards and useful items. Enable more box checks "
                    "or select more randomized checks."
                )
            tokens = (filler * self.options.token_percentage.value + 99) // 100
            target = (tokens * self.options.token_required_percentage.value + 99) // 100
            if self.victory_goal["type"] == "token_hunt":
                self.victory_goal["required"] = target
            else:
                self.victory_goal["requirements"]["token_hunt"] = target
        pool.extend(self.create_item(TOKEN) for _ in range(tokens))
        pool.extend(
            self.create_item(self.get_filler_item_name())
            for _ in range(capacity - len(pool))
        )
        self.multiworld.itempool.extend(pool)

    def cortex_requirements_met(self, state):
        return all(
            state.has(
                TOKEN if kind == "token_hunt" else "Level Cleared", self.player, count
            )
            for kind, count in self.victory_goal.get("requirements", {}).items()
        )

    def set_rules(self):
        kind, count = self.victory_goal["type"], self.victory_goal["required"]
        if kind in ("cortex", "all_bosses", "cortex_last", "cortex_requirements"):
            self.multiworld.completion_condition[
                self.player
            ] = lambda state: self.cortex_requirements_met(state) and state.has_all(
                tuple(f"{BY_KEY[key].name} Defeated" for key in self.goal_levels),
                self.player,
            )
        else:
            self.multiworld.completion_condition[self.player] = lambda state: state.has(
                TOKEN if kind == "token_hunt" else "Level Cleared", self.player, count
            )

    def fill_slot_data(self):
        if self.tracker_slot is not None:
            return self.tracker_slot
        result = {
            "protocol": PROTOCOL,
            "hub_area_locks": self.hub_area_locks,
            "level_access_locks": self.level_access_locks,
            "prototype": True,
            "crate_checks": bool(self.active_crates),
            "crates": self.active_crates,
            "starting_levels": self.starting_levels,
            "level_shuffle": self.options.level_shuffle.value,
            "crashsanity": bool(self.options.crashsanity.value),
            "movement_styles": self.movement_styles,
            "death_link": bool(self.options.death_link.value),
            "level_placements": self.level_placements,
            "selected_campaigns": list(self.enabled_campaigns),
            "level_pool": self.available_level_pool,
            "goal_levels": self.goal_levels,
            "victory_goal": self.victory_goal,
            "selected_levels": [level.key for level in self.active_levels],
            "checks": {
                **{
                    str(address): {"level": level.key, "event": event}
                    for level in self.active_levels
                    for event, _, address in checks(level)
                    if address in self.active_checks
                },
                **{
                    str(crate["id"]): {
                        "level": crate["level"],
                        "event": crate_event(crate),
                    }
                    for crate in self.active_crates
                },
            },
        }
        result["check_requirements"] = {
            address: [
                ITEMS[name]
                for name in self.check_requirements(
                    BY_KEY[check["level"]], check["event"]
                )
            ]
            for address, check in result["checks"].items()
        }
        result["alternate_check_requirements"] = {
            address: [[ITEMS[name] for name in needed] for needed in routes[1:]]
            for address, check in result["checks"].items()
            if len(
                routes := check_routes(
                    BY_KEY[check["level"]],
                    check["event"],
                    self.level_placements,
                    self.hub_area_locks,
                    self.level_access_locks,
                )
            )
            > 1
        }
        result["vanilla_collectibles"] = self.vanilla_collectibles
        result["collectible_locations"] = {
            str(address): {
                "item": ITEMS[name],
                "mode": (
                    "randomized" if address in self.shuffled_collectibles else "vanilla"
                ),
            }
            for address, name in self.selected_collectibles.items()
        }
        result["local_reward_sources"] = {
            str(address): {
                "level": level.key,
                "event": event,
                "item": ITEMS[self.local_reward_sources[address]],
                "requirements": [
                    ITEMS[name] for name in self.check_requirements(level, event)
                ],
                "alternate_requirements": [
                    [ITEMS[name] for name in needed]
                    for needed in check_routes(
                        level,
                        event,
                        self.level_placements,
                        self.hub_area_locks,
                        self.level_access_locks,
                    )[1:]
                ],
            }
            for level in self.active_levels
            for event, _, address in checks(level)
            if address in self.local_reward_sources
        }
        result["collectible_item_rewards"] = {
            str(ITEMS[name]): str(address)
            for address, name in self.shuffled_collectibles.items()
        }
        result["box_item_spoilers"] = bool(self.options.box_item_spoilers.value)
        return result
