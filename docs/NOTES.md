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
