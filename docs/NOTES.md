# Engine notes

Findings about the 0.8 beta (build 24919) engine, gathered with Prometheus' in-game tools.
Script IDs are `STUStatescriptGraph` resource IDs (`0x058…`); node numbers are as shown in
Statescript Graph Neo / Node Search (`<instance> <node index>`).

## Tooling

- **Hash names.** Node/field names show as raw CRC32 hashes unless `hashlibrary.json` knows them.
  overtools' `TankLibHelper/DataPreHashChange/KnownTypes.csv` and `KnownFields.csv` (OWLib, `develop`)
  use the same hash: `crc32(lowercase(name))`, names stored with their `STU`/`m_` prefixes.
  Merging them into the `db` object of `hashlibrary.json` (loaded at startup only) makes Node Search readable.
- **Keyboard while the overlay has focus.** `WndProc` in `dllmain.cpp` drops `WM_KEY*`/`WM_CHAR` whenever
  `ImGui::GetIO().WantCaptureKeyboard` is set, so keys never reach the game while a Prometheus window is
  focused. Mouse buttons still pass through. Click the game view before testing key bindings.
- **Node Search by type hash.** Typing a type's hash (e.g. `ff77458c`) into Statescript Node Search lists
  every node of that type, even without the hash library.
- Useful type hashes:
  - `FF77458C` `STUStatescriptStateLogicalButton` (state active while a logical button is held; its
    `m_logicalButton` (`3C9341F5`) is an `STUConfigVarLogicalButton` (`5C7964FF`), and the graph view
    shows the resolved button, e.g. `93: fire`, `134: spray`)
  - `EB923A6A` `STUStatescriptActionEffect`, `4698E4B6` `STUStatescriptActionSetVar`
  - `7AD0304A` `STUStatescriptStateUXMatchStatus`, `99C92F33` `STUStatescriptStateUXMatchPlayer`,
    `C739BAAC` `STUStatescriptStateUXScreen`

## Local hero model (Tracer, 2026-10-17 session)

- The controller entity was `800009d4`; the hero model was `800009d5` (it holds components 0x12, 0x15,
  0x23 Statescript, 0x28 health). The model usually comes right after the controller.
- Scripts on the model: top level `0x1a, 0x24, 0x34, 0x33, 0x43, 0x4b, 0x3d, 0x2cf`, plus server-created
  instances with positive IDs (1, 2). `0x4b` is the HUD parent; its children are
  `0x1c7, 0x1ca, 0x1d8, 0x1cf, 0x215, 0x246, 0x2c5, 0x2cf, 0x352, 0x5ad, 0x5ac, 0xb3b, 0xb2a`.
- `0x352` is the map (upstream's M-key workaround toggles state 17; it is a toggle, not hold-to-show).
- `0x2cf` is the spray script: its LogicalButton state (node 5) is bound to `134: spray`.
- `0x1ca` is Tracer's weapon script. Almost every node is tagged `[SERVER]`; its LogicalButton state
  (node 56) is bound to `93: fire`. The client never runs the server half, which is likely why weapons
  don't shoot (work-queue item 4).
- Observed on the `artpop2` build (Apr 2026): Tracer's primary fire and Blink both work, despite the
  server-only fire state above. Recall is the ability that breaks things: after using it, every other
  ability and primary fire stop working for the rest of the life. Working theory: Recall enters a state that
  blocks other abilities/weapon while it runs, and the exit from that state (or the "recall finished"
  acknowledgement) is server-driven, so the client stays stuck mid-Recall (work-queue item 6).
  Recorded test (2026-10-17): during Recall the viewmodel guns disappear and the health bar changes style;
  afterwards the guns reappear but nothing fires. The model's Statescript instance list is identical
  before and after (top-level scripts plus runtime instances `(1) 0x15`, `(2) 0x16`), so the blocker is a
  state or variable inside an existing script, not a leftover/missing script instance.
- No client-side LogicalButton state on the model is bound to the scoreboard/Tab button. Holding Tab
  (with the game focused) left `latest_logbtn` = 195 (`0xC3`, invalid) and `LogicalBtns` empty in
  Statescript Info, so the tablist is driven from somewhere else.
- `UXMatchStatus` (`7AD0304A`) nodes exist in neither the hero model's scripts nor the lobby admin's
  Statescript entity. 149 game-admin entities carry a Statescript (mostly map props such as entity def
  `0x0400000000000167`). Working theory: the tablist lives in a game-mode/match script that a real server
  tells the client to load, and Prometheus never loads it.

## Full graph dump (Statescript → Graph Scanner)

The Graph Scanner walks graph IDs `0x058…000`–`0x058…BFF`, loads each through the resource manager and
writes every node (type, state/client-only/server-only flags, config var fields, constant values, links)
to `Overwatch/Prometheus/graph_dump.json`. During a Hanamura/Tracer session 299 graphs loaded
(IDs `0x00F`–`0xB6C`; 11413 nodes, 636 server-only). Constant config vars are evaluated without an
instance, so only ~2k of 22k config vars get a value (dynamic/expression ones and some types need one).

- **HUD parent `0x4B`** starts 14 sub-scripts. The only one behind a condition (node 11 → node 5) is
  **`0x1CB`**: camera look-at, PhaseOut, Invisible, movement/aim locks, respawn waits and a UX screen, i.e.
  the death / kill-cam / respawn flow. It is correctly idle while alive.
- **Tablist:** no `UXMatchStatus`/`UXMatchPlayer` node exists in any of the 299 graphs, and the 7 graphs
  where a LogicalButton state leads to a UX screen (`0xF2, 0xF4, 0x169, 0x17E, 0x47E, 0x5FE, 0x622`) are hero
  weapon/ability scripts (scopes/reticles). The scoreboard graph is not loadable during a plain match;
  it most likely ships with the game-mode assets. Treat it as part of game-mode support (work item 11).
- **Tracer:** `0x16` is the gun (fire LogicalButton + `WeaponVolley`, no server-only nodes, hence firing
  works), `0x15` is the ability-input handler (5 LogicalButton states + Stack locks), `0x24` is Blink
  (`ActionBlink`, `BlinkEffect`), **`0x1A` is Recall**.
- **Recall (`0x1A`) flow:** `<28>` `StateAbility` (start button 78) → `<31>` state with subgraphs `<4>`
  (0.1 s wind-up, its end starts `<1>`), `<9>` Recall anim and `<27>` Stack lock (=true). `<1>`
  `HistoryRewinder` (3.0 s history) runs `<3>` PhaseOut (→ `<18>` prioritised Stack lock), `<2>` Invisible,
  `<6>` MovementMod, `<10>` Stack (→ `<26>` Stack) and `<32>` Wait → `<20>` SendGameMessage
  `0x024000000000007D` to the owner. `<34>` is an entry for game message `0x0240000000000011`.
  `<5>` `HistoryBuilder` (10 s) records history under `<11>` (expression-gated).
  Stack locks only release when their owning state ends, so a Recall state that never finishes
  client-side would leave abilities/weapon locked.
- **Recall, observed live** (graph window "Running states" line, 2026-10-17):
  before: `<5> <11> <23> <28> <35> <42> <44> <46>`; on Recall `<4> <9> <12> <13> <14> <19> <27> <31>`
  start; afterwards `<9>` and `<14>` end but **`<4>` never finishes**, so `<1>` HistoryRewinder never
  starts (no actual rewind) and `<31>` with its `<27>` Stack lock (=true) stays active forever. That lock
  is what blocks weapon and abilities.
- **Node type `CD396790`** (no known name; config var `6B7C7D86` float, Recall uses 0.1; optional
  `B4FE98D8` bool) is `<4>`. It appears 28 times across hero ability graphs (`0x20, 0x48, 0x7D, 0xE2, 0xF3,
  0xFE, 0x101, 0x169, 0x17B, 0x17E, 0x1F0, 0x232, 0x26A, 0x304, 0x9BB`). Output plugs seen: `3F6FFA05`
  (subgraphs), `D2CD7C9F` (Recall uses it to start `<1>`, so probably on-end), `FE841C79`, `025C665A`.
  Blink (`0x24`) does not use it, which matches Blink working.
- **`CD396790` is a timed wait whose end is server-decided** (native analysis, 2026-10-18). Its runtime class
  (vtable RVA `0x15D4840`, constructor `0xD62890`) overrides vfunc 34 `0xD67920` (on start: evaluate
  `STU+0x100` as seconds, write start/duration/rate to the dynamic out vars at `+0x110/+0x118/+0x120`, start
  timer 1 via `0xF47A50`) and vfunc 40 `0xD67BF0` (on timer 1: end the state via `0xF4A9C0` only if the
  condition `STU+0x108` evaluates true). The condition evaluator `0xD16BF0` returns its default (false) for an
  empty config var, and Recall's `<4>` (like most uses) leaves `B4FE98D8` empty, so the client never ends the
  wait; the server did. Not the cause: the authority check inside `0xF4A9C0` (instance vfunc `+0x170`, check
  fn `0xD73E90`) — traced offline, it allowed every end request.
  A second class registered under the same hash (vtable `0x15D86C8`, factory `0x1085740`) only overrides vfunc
  40 = `0xF531E0` ("on timer 1, end"); it was not seen running. Vfunc 40 is the per-state timer callback
  (timer id as argument); vfuncs 34/35/36 are start/resume/update for the client-side wait (`0x16052B0`).
- **Fix: `timed_wait_on_timer_hook`** hooks `0xD67BF0`: after the original runs on timer 1, a wait with an empty
  condition is ended (`0xF4A9C0(state, 1, 0)`), as the server would. Covers every ability that uses the node,
  not just Recall. `finish_server_wait_states` keeps a fallback (configured duration + 0.25 s) in case the
  timer never fires. Verified in game: Recall's `<4>` ends ~0.09 s after it starts, the rewind plays, other
  abilities and fire keep working; the fallback never triggered.
- **Former workaround** (replaced by the fix above): states of type `CD396790` were finished after 0.3 s and
  `HistoryRewinder` (`7211CF70`) after 1.5 s (the latter fallback is still there). Trace of a
  Recall with it: `<4>` start → forced end at +0.31 s → `<1>` HistoryRewinder starts with `<2>` Invisible,
  `<3>` PhaseOut, `<6>` MovementMod and Stack locks `<10>`/`<18>`/`<26>`; `<5>` HistoryBuilder pauses;
  `<9>` anim ends at +0.9 s; `<20>` SendGameMessage at +1.05 s; `<1>` never ends by itself → forced end
  at +1.8 s → all locks release, `<5>` restarts. Result in game: Recall goes on cooldown and the player
  can move/shoot/Blink afterwards, but the position is **not** rewound. The rewind itself is evidently
  applied by the server (movement corrections arrive through MovementState, component 0x12), so
  Prometheus has to record the local hero's positions and move the hero back itself.
- **Rewind emulation** (`emulate_history_rewind`): the local hero's position (component 0x15 + 0xC70)
  is sampled every tick for ~4 s. While a `HistoryRewinder` state runs, the hero is moved back along that
  history (3.0 s of history over 1.25 s), then the rewinder state is ended so its locks release
  immediately. With the CD396790 wait at 0.1 s, a full Recall takes ~1.4 s (press → arrival). Verified
  in game: the hero returns to the position from 3 s earlier, all Recall visuals play (gun throw anim,
  time bubble, blue rewind screen, end burst), Recall goes on cooldown, no lockout afterwards.
  Not emulated yet: health rewind (HP is stuck anyway, item 3), and the playback speed curve (linear).
  Right after spawning the hero's position reads (0, 0, 0) for a few ticks; recording those made the first
  Recall after a spawn drop the hero at the map origin, off the map (`rewind start: ... to (0.00 0.00 0.00)`).
  Zero positions are now skipped, and a jump of more than 10 m in one tick restarts the history.
- **Trace logging** (`trace_state_changes`, `trace_logical_buttons`) writes `[TRACE]` lines to
  `Overwatch/Prometheus/Logs/Prometheus_Prod.txt`: every state start/end in the local hero's scripts and
  every logical button change with its in-game name.

## Logical buttons (from the trace log)

| ID | Name | | ID | Name |
|---|---|---|---|---|
| 61 | move_forward | | 93 | fire |
| 64 | move_left | | 119 | ability_1 (Tracer: Blink) |
| 65 | move_right | | 120 | ability_2 (Tracer: Recall; `StateAbility` start button `0x78`) |
| 66 | jump | | 134 | spray |
| 80 | reload | | 164 | escape_menu |
| | | | 168 | **scoreboard** (Tab) |

Holding Tab does register as `168 scoreboard` in the input component; no loaded script listens for it.

## Auto Test (`auto_test.cpp`)

Plays every hero without a person: start it from the top bar (Quick Play → "Auto Test all heroes") or set
`PROMETHEUS_AUTOTEST=all,exit` (or e.g. `tracer,genji`) in the game's environment. It turns on demo mode and
loads the lobby data packs (the top bar's "Demo" and "DP Load", which load the hero data), loads the Quick Play
map, then per hero: spawns it like hero select does, presses fire, then each of `secondary_fire`, `ability_1`,
`ability_2`, `ability_3` (ultimate), `quick_weapon` (melee), each followed by a fire probe. Results go to
`Overwatch/Prometheus/auto_test_report.json` and `[AUTOTEST]` log lines; a full run of 21 heroes takes ~20 min.
- **Input:** writing the logical button into Component 0x50's buffers is not enough for gameplay (the game turns
  real input into Statescript `LogicalButton` notifications earlier in the frame and overwrites the buffers);
  the test enqueues `ETYPE_LOGICALBUTTON` notifications to each of the hero's script instances (`EnqueueE`) and
  also holds the buffers for "is held" checks.
- **Hero select** opens by itself after spawning (`STUConfigVarIsHeroSelectActive`, getter `0xD8EE00`, stays
  true) and pressing `hero_select` does not close it, but it does not block the notifications above.
- **Verdicts:** `no_reaction` (no state of the hero's scripts started/ended within 1 s, ignoring states that
  changed on their own while settling), `breaks_fire` (fire reacted before, not after), `left_running` (states
  started by the press still run 8 s later), `ok`.
- **First full run (2026-10-18):** fire works on all 21 heroes; Tracer is fully `ok` (Blink also listens to
  `secondary_fire`). **`breaks_fire`: Pharah `ability_1`, D.Va `quick_weapon`.** Many `left_running` results are
  aiming/placement modes waiting for a confirm (Reaper Shadow Step, Torbjorn turret, Symmetra turret, Mei wall:
  `StateLogicalButton` `FF77458C`, `StateUXScreen` `C739BAAC`, `StateCosmeticEntity` `6E81F6D4`); the most common
  other leftovers are `StateStack` `F7D6498C` (×20), unnamed `381D6654` (×17, movement abilities), and
  `StateChaseVar` `970EC198` (×12, secondary fires). Ultimates mostly show `no_reaction` (no charge).

## D.Va's mech
- `STUHero` `0x02E000000000007A`'s `m_gameplayEntity` is **`0x40A`, the pilot** (weapon slots 1–2 empty, slot 3
  `0x2B8` = pilot's gun; graphs `0x33, 0x43, 0x4B, 0x26A, 0xB49`). Spawning that leaves D.Va as the pilot.
- **The mech is `0x407`:** model `0x11CE` (the child the lineup/lobby entities `0x73E`/`0xDD1` attach at a hard
  point), first person `0x41F`, weapon `0x50A`, graphs `0x33, 0x4B, 0x276, 0x2CC, 0x47E, 0x43, 0xB2F`, child
  `0x10B4`. Nothing on the client references it (no CreateEntity-style node in D.Va's graphs, not in `STUHero`):
  the server spawned it. Found by dumping every loaded `STUEntityDefinition` (`PROMETHEUS_DUMP_ENTITIES`, see
  Auto Test) and matching the lobby model's child.
- `hero_spawn_entity()` (player_spawner.h) maps D.Va to `0x407` for hero select, the state replicator and the
  Auto Test. Auto Test (Numbani): fire, secondary (Defense Matrix), ability_1/2, ultimate and melee all `ok`.
  Not handled yet: ejecting into the pilot when the mech dies, and the pilot sitting in the cockpit (3rd person).
- Upstream: PR saturn-xvi/prometheus#39 (per-hero health) leaves D.Va out ("meka handled separately");
  issue saturn-xvi/prometheus#35 also calls D.Va "more complex".

## One-click Play and map spawn points (2026-10-18)

- **Spawn points are map entities with component `0x2E`**, 6 per spawn room (one per player), all resource
  `0x400000000000001`. Their `Component_1_SceneRendering` rotation is the facing direction as a quaternion.
  Numbani has 30 (5 rooms), Hanamura 18 (3 rooms). Found with `PROMETHEUS_DUMP_MAP_ENTITIES` (Auto Test), which
  writes every map entity with its components, mirror data type, position and rotation to `map_entities.json`.
- The demo placed every spawned hero at a fixed Hanamura spawn spot `(108.3, 1.0, 0.14)`, so on any other map the
  hero started outside the level and fell. `find_spawn_point()` (entity_admin.cpp) now uses the map's first spawn
  point instead.
- `Component_2F_LocalPlayer::SetRotation` (`0xca6fb0`) takes a full quaternion (its code is the standard
  quaternion → rotation matrix expansion). The demo passed `(0, -0.713, 0, 0)`, which has w = 0.
- Tested with `C:\tools\re\play_maps_test.ps1` (Play at launch, hero position logged on spawn and 5 s later):
  Cairo, London, Kyoto, Gibraltar, Numbani, Russia, Hollywood and Mexico all spawn in a spawn room and stay there.
- Maps in this build (`STUMapHeader` m_mapName → m_map): Cairo `0x5B`, London `0xD4`, Kyoto (Hanamura) `0x165`,
  Gibraltar `0x184`, AutomationStressArena `0x1C2`, Numbani `0x1D4`, Russia (Volskaya) `0x1DB`, Lobby `0x223`,
  Hollywood `0x2AF`, Mexico (Dorado) `0x2C3`, Tutorial `0x661`, login scenes `0x664`–`0x67B` (all `0x08000000000xxxxx`).
- One-click Play (quick_play.cpp): the main menu's Play button does what the top bar's Demo + DP Load did, then the
  matchmaking screens, map and hero. The map, the hero (also set by picking one in hero select) and "start a game
  at launch" are saved to `Overwatch/Prometheus/play_settings.json`. The old demo flythrough moved to the top bar's
  Play menu.

## Spawn room doors, map switching, borderless window (2026-10-18)

- **Spawn room doors** are map entities running script `0x5800000000000F6` (11 nodes): on game message
  `0x240000000000071` (the match starting) a condition gates `STUStatescriptActionDestroyEntity(GetOwner)`, so the
  door deletes itself. Offline nothing sends the message, so heroes were shut in. `open_spawn_doors()`
  (entity_admin.cpp) deletes those entities when the hero spawns: 26 on Hanamura, then 0 for later spawns.
  Spawn points' own component data (`0x2E`, mirror type `2256DF7B`) has a single field that is 0 everywhere; the
  team is not in it.
- **Switching maps** works as long as the local hero is deleted before the map unloads. Auto Test now takes
  `map=numbani+kyoto+mexico` and plays every map in one session (tested, no crash). The top bar's Map menu and
  the Play menu's map list go through `quick_play::switch_map()` when a hero is in the game.
- **Borderless window**: the beta reads no settings file (every ini setting shows `parsed: <none>`) and starts in
  exclusive fullscreen (`IDXGISwapChain::GetFullscreenState`). Display settings are static bool setting objects:
  `Render.WindowedFullscreen` at RVA `0x17A4CC0`, `Render.FullscreenWindow` at `0x17A4D30` (value at +0x38,
  runtime ini copy at +0x8 → value pointer +0x90, callback at +0x40 with argument +0x48). The Video options apply
  (`0x85D270`) and the setter `0x85EC00` set them and call vfunc `0x598` on the renderer at `0x181E3E0`.
  `display.cpp` sets WindowedFullscreen = 1 the same way: exclusive fullscreen goes from yes to no and the window
  stays 2560×1440 without borders. The game state tick (`0xBFF460`) can force FullscreenWindow back to 1 under
  some flags, so display.cpp keeps checking.
- The user's `Documents\Overwatch\Settings\Settings_v0.ini` belongs to the live game (last written April 2026, before
  any beta run) and must not be edited.
- Found with Ghidra headless (`C:\tools\re\ghidra_headless.ps1`, scripts StringRefs/RefsTo on project `OW08`) and
  new Auto Test dumps: `PROMETHEUS_DUMP_INI` (ini settings) and the map dump's script ids.
- **TODO when game modes are emulated:** removing every door at spawn skips the setup phase (attackers could leave
  early) and skips the door script's own condition (`EC502A81`, not decoded yet), which decides which doors open.
  26 entities run the script on Hanamura but only 8 are near spawn rooms, so a few may be other pre-match
  barriers. Once a game mode runs, turn `open_spawn_doors()` off and instead send game message
  `0x240000000000071` when setup ends (the host in LAN), so each door decides for itself.

## Projectiles (2026-10-18)

- Weapon fire: `STUStatescriptStateWeaponVolley` (runtime vtable `0x15D2450`, factory `0xD43DB0`). Its update
  (slot 36, `0xD44D60`) calls the fire-shot method (slot 78, `0xD456C0`) per pending shot. That packs the shot
  into a job (callback `0xD455E0`), which runs `0xD45240` per projectile: reuse a projectile the server announced,
  or create one with **`0xD354B0(callbacks, shot, &result, predicted)`**.
- `0xD354B0` result codes: 0 created, 1 predicted + the projectile entity has a statescript component (`0x23`),
  2 no entity, 3 load failed, 4 no projectile component (`0x27`), 8 the spawner at entity admin `+0x210` returned
  nothing (shot command `+0x104 == 1`), 5/9/10 other checks. The job always passes predicted = 1, so scripted
  projectiles (rockets, grenades: explosion and damage are scripted) were refused and left to the server.
  For shot command type 1 the spawner at `+0x210` exists offline but returns nothing; other types load the entity
  straight into the world (`prom_load`) and, when not predicted, register it through `callbacks` vfunc 1.
- Fix (`create_projectile_hook`, entity_admin.cpp): call with predicted = 0, and for type 1 switch the command to the
  direct-load branch for that call. Auto Test on Numbani: Pharah's fire and Junkrat's fire now create projectile
  entities (resource ids, not just effects); no "not created" results, no crash.
- Auto Test now records the entities created after each press (`entities` / `fire_entities`, resource id or
  `?:<component ids>`). Effects show up as `?:1,3,D` and `?:1,3,B` for every hero; placed objects (Junkrat's
  mine and trap) as `?:1,3,4`.
- Not checked yet: Reinhardt's Fire Strike shows no projectile entity (may not be a volley), and nobody has
  looked at the projectiles in game (flight, explosion, damage).

## Ultimates (open)

- Every ultimate shows `no_reaction` except D.Va's (her mech's ultimate worked). Only one
  `STUStatescriptStateAbility` uses the ultimate button `0x79` as a constant (graph `0x580000000000034`, node 38);
  most start buttons and `m_startCondition` (`EC502A81`, 35 nodes) are not recorded by the graph dump.
- No component stores ultimate charge; it is most likely a statescript variable on the hero that the server
  raises. Next: enumerate the hero's varbag (the Varbag Window is an empty stub) and find the variable the
  ultimate's start condition reads.

## Script-started volleys (ability projectiles, 2026-10-18)

- Concussive Blast (`0x58000000000007B`), Fire Strike (`0x5800000000000E2`) and Symmetra's orb (`0x580000000000139`)
  are volleys an ability's script starts (Ability -> State -> Wait -> Volley), not held fire. Two server-side
  inputs were missing:
  1. The weapon state start `0x1077B30` (volley on-start, via thunk `0xD44B50`) ends the state at once when the
     shooter lacks component `0x26` or **bit 3 of the script instance flags (`+0x258`)** is clear. Weapon scripts
     have flags `0x600089`; ability scripts `0x400081`, so their volleys are left to the server.
  2. The volley update `0xD44D60` fires only when its command (`+0xd0`, looked up from the weapon's commands by
     `0x10798F0`) has type 3 (server) or its **fire flag `+0x108`** is set. Held fire has it (input); the command
     a volley creates for itself on start (`0x1078A10` -> `0x10451C0`) does not.
- Fix (entity_admin.cpp): `weapon_state_start_hook` sets bit 3 for the start call of volleys (vtable `0x15D2450`);
  `volley_update_hook` marks a volley whose first update finds its command without the fire flag as script-started
  and sets the flag for that activation. Auto Test: Fire Strike, Concussive Blast and Symmetra's orb now create
  projectile entities; Hanzo's and Junkrat's abilities still do.
- Shot commands otherwise arrive as network messages: `0xD44EE0` dequeues them per tick and `0x1079BC0` reads
  them from a bitstream.
- Tracing with `auto_test` `started` (every state an ability press starts, with node types) showed the volleys
  starting and finishing within a tick; graph dump node indexes are not runtime state indexes (`idx_in_m_states`).

## Ultimates (2026-10-18)

- An ultimate's `STUStatescriptStateAbility` (Tracer: graph `0x580000000000034`, node 38, start button `0x79`) has an
  `m_startCondition` expression over two of the hero's **entity variables**: the charge **`0xD80000000000156`** and
  the hero's cost **`0xD80000000000152`** (Tracer 900, Pharah 1500). Read with `PROMETHEUS_DUMP_STU=580000000000034`
  (the graph dump does not record expressions). The node sits under a boolean switch on entity variable `0x1880`.
- The server raises the charge; offline it stayed 0. `ultimate.cpp` fills it: over 30 s (default), always ready, or
  off (Play menu > Ultimate charge, `Prometheus/ultimate_settings.json`), following the value when the ultimate
  spends it. Auto Test with "always ready": Pulse Bomb, Earthshatter and Barrage (~90 rockets) start.
- Auto Test `ultprobe` (set each integer entity variable to 100, wait, press) found nothing because 100 is below
  every cost; `0xD800000000001DE` reacts to being set (likely "ultimate ready" feedback), not the ultimate itself.
- Auto Test `PROMETHEUS_DUMP_VARS` adds the hero's entity variables (id, type, value) to the report.

## Projectiles through walls (open)

- The user sees many projectiles pass through floors and walls. Measured with Auto Test `pitch=80` (aim at the
  floor) and entity lifetimes: Pharah's rocket, Concussive Blast and Junkrat's mine die within ~0.05 s (they hit
  the floor), with or without the authoritative registration (`0xD35C20`, callbacks vfunc 1). Which projectiles go
  through is not known yet.
- Projectile entities (e.g. Pharah's rocket: components `1 3 4 27 40 7A`) have no statescript component, so the
  impact is the projectile component's job, not a script. The spawner path (type 1) also sets filter bit
  `0x800000` on component `0x10` (`0xCD0B00`), but these projectiles have no component `0x10`.
- `0xD45240` clears projectile component `+0xE2A` after every creation (it is not the "predicted" difference).

## Server-only statescript nodes: deployables (2026-10-18)

- Node implementations come from registration records `{.., factory, type descriptor, group, enable flag, 3}` in
  `.data` (`0x1B2xxxx`-`0x1B4xxxx`); the lookup table is `DAT 0x7ff7b6574d18` (actions: `0xF433A0`, states: factory
  lookup `0xF43450`). Group 2 is the shared implementation (what the server runs); group 1 registers client
  versions over 21 of them (`C:\tools\re\registries2.py`). Some group-1 versions are stubs with every slot a
  no-op, so offline the state did nothing: the placement state `160C4F1D` (stub vtable `0x15D6F40`, real class
  factory `0xD64380` / vtable `0x15D5E68`), StateFindGround (stub `0x15D6598`, real `0xD64110`) and Mei's wall
  states `12AA4F0F` (stub `0x15D6CC8`) and `614C3A0A` (stub `0x15D6A70`). Exact vtables from the constructors:
  `C:\tools\re\dual_compare.py`. TrackRay, Shockwave and TrackTargets have real client versions (their group-2
  extras are a few slots; TrackTargets' and Shockwave's slot 75 is only debug drawing).
- Some node types have no implementation at all in the client: `1BB99C36` (the server's entity creation; Torbjorn's
  turret, Symmetra's turrets and teleporter, Hanzo's Dragonstrike, Junkrat's RIP-Tire, D.Va calling her mech),
  StatePet `4CCED352`, StatePossessControl `74CB0002`, StatePossessView `3A846155`, STUStatescriptActionCriteria
  `4E83BC01`, StateModifyHealth `619D9E95`, StateApplyAura `D78490D1` and more (`out/unimplemented_nodes.txt`). An
  action with none gets the base action (`0xF47740`: just follows its out plug).
- Traversing a link into a node is `0xF471F0(from, instance, from node, ?, input plug, ?)`; the target node is
  `plug + 0x18`. It skips the node when instance flags `+0x258` bit 2 is set or instance vfunc `0x88` refuses it.
  `0xF46F80(from, instance, node, output plug, 0x80)` follows every link of an output plug.
- `server_nodes.cpp`:
  - Traces nodes the graph reaches that have no implementation or that the instance refuses (`[NODES]` in the
    log, once per script and node).
  - Emulates `1BB99C36`: fields `m_entityDefinition` (`m_entityDef`), `679AF128` owner (usually
    GetEntityController(GetOwner)), `m_position` (vec3 or entity), `87018F61` forward, `99EEFF9F` up (surface
    normal), `m_basedEntity` (not used), `D7C59487` dynamic var receiving the new entity. Spawned with
    `EntityLoader` (component 1 init data: position, rotation from forward/up with Z forward and Y up, which matches
    the hero's rotation), backref `0x1F` to the owner, filter bits from the creating entity, then the output var
    set through the instance varbag (vfunc `0xE0`, as ActionSetVar does). Hanzo's dragon is created at the world
    origin on purpose: its script (`0x580000000000132`, started by PlayScript with the start position as a parameter)
    places its cosmetic dragons at absolute offsets.
  - Swaps the stubs above for the shared implementation (hook on `0xF43450`).
  - Placement (`160C4F1D`): the real class's tick (`0x1096500`) traces for a spot and exposes state values
    (position `+0xc0`, valid `+0xb0`, hit `+0xb1`, normal `+0xd0`, forward `+0xe0`) through its output vars
    (fields `+0x198`..`+0x1e8`, bound on begin by `0xD6DC20`). Input plug `C63FD388` (type `8DCB62EA`) means "place
    now" (Torbjorn's fire button, RIP-Tire on begin); the server answers with output `02701BF5` (placed) or
    `D3894419` (no valid spot) and writes the placed position/normal/forward to fields `C500D8CF`, `57216848`,
    `2650F0BE` (vars `0x65A`, `0x9B1`, `0x9B2` for Torbjorn). Offline the input only restarted the state. Emulated:
    run the update, write those three vars, fire the matching output. Input plug type `E51F3E8D` is abort.
- Possession (StatePossessControl `74CB0002` field `743B15A6`, StatePossessView `3A846155` field `D7148C41`, both
  without client implementation; Junkrat's RIP-Tire `0x232`, D.Va's Call Mech `0x26A`): the controller's
  component `0x20` holds the entities it drives (`movement_attach`, `aim`) and the camera follows (`cam_attach`);
  `server_nodes::tick` points them at the possessed entity while such a state runs on the hero, and back after.
- StateFindGround's real class (`0xD64110`, vtable `0x15D60D8`) writes its output vars itself on begin (`0xD6D270`)
  and exposes state values 0 position `+0xb0`, 1 found `+0xc8`, 2 `+0xc0`; its output `02701BF5` is the server's.
- Auto Test: Torbjorn's turret (`0x400000000000143`, runs scripts `0x33` and `0xC9`), Symmetra's turret (`0x251`,
  scripts `0x18E`) and teleporter (`0x233`), Hanzo's dragon (`0x2BA`) and RIP-Tire (`0x39C`, script `0x234`) are
  created at the placed spot, facing where the hero faces.
- Pets (STUStatescriptStatePet `4CCED352`, no client implementation; Zarya `0x244`/`0x25C`, Mei's Cryo-Freeze `0x5FE`):
  same fields as `1BB99C36`; spawned when a link enters the state's `m_beginPlug`, removed once the state finished.
- Ultimate charge: Junkrat's graph sets the charge (`0xD80000000000156`) to 0 only on game message `0x2400000000005BA`,
  which the server sends. Offline the charge stayed full and RIP-Tire restarted every time the graph came back to
  its ready state (1.5 s). `server_nodes` now calls `ultimate::spend()` when a StateAbility whose `m_startButton`
  is `0x79` follows a link (it does when it activates).
- Game messages: the client's SendGameMessage (`0xD794F0`) delivers only to the local player's own scripts; other
  targets and every server message are lost offline. A message can be delivered by running the graph's
  EntryGameMessage nodes (`BE49A4C2`) for it (`StatescriptInstance::ExecuteNode`).
- Node type `138CCAB0` (core factory `0xF469E0`, execute `0xF47430`) is a join: it records arrivals keyed by
  (source node, context) and follows its out plug once all of them came (`0xF47660`). RIP-Tire's join (node 19)
  waits for Junkrat's 1.5 s wind-up (node 22, transition plug) and server message `0x24000000000013E` (entry 18);
  it then sets var `0x26` (the possessed tire) from var `0x28` and switches the graph to driving (switch var
  `0x6C7` = 2). Emulated as a "server reply" a tick after node 32 created the tire: var `0x28` = tire, deliver
  `0x13E`. Without it the graph returned to ready after 1.5 s, which clears var `0x26` and ends possession.
- RIP-Tire ends when the server explodes it; offline it never would. Removing the tire on primary fire or after
  10 s ends the possession and the graph returns to normal (no explosion). The tire has no camera of its own (the
  server's view is third person), so the camera stays with Junkrat.
- Debug: `PROMETHEUS_TRACE_LINKS=<script id>` logs every link that script follows (`[LINK]` lines).
- Mei's Ice Wall: "place now" inputs (type `8DCB62EA`) chain: StateFindGround (input `C63FD388`, answer `02701BF5`,
  valid byte `+0xc8`) -> wall start `12AA4F0F` (input `5556C513`, answer `8CF01B33`) -> five segments `614C3A0A`
  (same plugs) -> the nodes creating the pillars. `PLACE_RULES` in `server_nodes.cpp` answers them all. Mei's
  Blizzard was blocked only because the unfinished wall left its states (and an ability lock) running.
- STUStatescriptActionCreateEntity (`0E3689F3`): the client's implementation (`0xD6EDA0`) only binds an entity the
  server announced (map at component `+0x500`). Emulated for Mei's wall (fields `m_entityDef`, `m_position`,
  `m_out_Entity`, `2108984C` random yaw range `m_min`/`m_max` in degrees). Also used by `0xC9`, `0x1C7`, `0x5AD`
  (not emulated yet; logged).
- STUStatescriptActionDestroyEntity (`5C46D801`, `m_entity`): emulated for entities created here only.
- STUStatescriptActionPlayScript (`F9EF59A6`, `0xD79C90`) and SendGameMessage play/deliver only on the local player;
  other targets go to the server. Emulated for entities created here: load the script with authority
  (Component_23 vfunc `0x60`), wait for its graph (built lazily, a few ticks), write the caller's `m_params`
  (param id `C76B6366`, `m_value`) into the variables of the script's STUStatescriptEntryPlayScript (`46C48944`,
  `m_params`: param id, `m_out_Var`; `m_out_Sender`), then run that entry. Hanzo's dragons (`0x132`), the tire
  (`0x233`), Mei's pillars (`0x636`) and Cryo-Freeze block (`0x623`) get their scripts this way.
- Instance vfunc `0x88` for entity scripts (`0xD739A0`): runs a node if it is not server-only (`+0x76`) and either
  instance flags bit 12 is clear and bit 22 (`0x400000`) set, or the node is client-only (`+0x75`). Hero scripts
  have bit 22 (`0x400081`); scripts on created entities did not (turret `0`, dragon `0x208009`), so they refused
  nearly every node. Set on every script of created entities.
- Safety: `get_argument_object`/`get_argument_primitive` crash on a field the type lacks; `field_object` looks
  fields up by hash first. The game's GetEntityPos crashed for an entity value; positions are read from
  component 1 instead.
- Open (2026-10-19): not yet confirmed in game that turrets shoot, the tire drives, dragons and pillars appear; the
  RIP-Tire explosion; Zarya's Graviton vortex.
