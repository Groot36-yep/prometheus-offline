# Offline play progress (Overwatch 0.8 beta, build 24919)

This branch makes the archived 0.8 beta playable offline through Prometheus. The beta was built to be a client of a
server that no longer exists, so much of the game (abilities, deployables, ultimates) waited for answers that never
came. The work here plays the server's part for the local player. Engine details, addresses and how each finding was
made are in [NOTES.md](NOTES.md).

Scope: only the archived beta. Nothing here touches the live game, Battle.net or Blizzard servers, the patcher, or
anything to do with anti-cheat. No game files are in this repository.

## Works

**Starting a game**
- The main menu's Play button starts a game in one press, on any of the 8 maps (map and hero saved in
  `Overwatch/Prometheus/play_settings.json`; optional start at launch).
- Heroes spawn at the map's spawn points, facing the right way; spawn room doors open.
- Switching maps in game (Play menu), without the crash that a switch caused before.
- Borderless window instead of exclusive fullscreen.
- Changing hero anywhere (Play menu > "Change hero anywhere", on by default); the new hero takes the old one's place.
  *New, not tested in game yet.*

**Weapons and abilities**
- Projectiles (rockets, grenades, Fire Strike, Concussive Blast, Symmetra's orb) are created and explode.
- Abilities that waited on a server-decided timer finish (they used to get stuck).
- Tracer's Recall rewinds correctly; D.Va spawns in her mech.
- Ultimates charge (over 30 s, always ready, or off: Play menu > Ultimate charge).

**Deployables and summons** (server-only script nodes emulated)
- Torbjorn's turret: placed, friendly outline, removed when you change hero.
- Hanzo's Dragonstrike: the dragons fly.
- Junkrat's RIP-Tire: held during the wind-up, then driven along the floor with the chase camera; stops at walls,
  detonates.
- Mei's Blizzard: the drone lands and the storm deploys, in the friendly color.
- Mei's Ice Wall: all 5 pillars are created (only 1 was before), but they are not visible yet.

**Teams**
- The player and what it creates are on a team, so they count as friendly (no enemy outlines or colors).

**Testing tools**
- Auto Test plays every hero by itself, presses each ability and reports what reacts, with optional screenshots,
  aiming (`pitch=`, `yaw=`, `sweep=`), button sequences and projectile tracking.

## Not working yet

- Ice Wall pillars and the Blizzard drone are invisible although they exist.
- Thrown items vanish when they land instead of sticking: Junkrat's Concussion Mine and Steel Trap, Torbjorn's armor
  pack, Hanzo's Sonic Arrow.
- Some projectiles reportedly pass through walls (Pharah's rockets, Zarya's grenade and Graviton Surge); in tests in
  spawn rooms they hit the walls, so where this happens is not known yet.
- Zarya's Graviton Surge sticks but shows no vortex.
- Torbjorn's turret cannot be upgraded with the hammer.
- No game modes, no other players or bots; LAN play is a later goal.
