#pragma once
#include "entity_admin.h"

//Quick Play: makes the main menu's Play button work offline. It runs the game's own matchmaking screens
//(searching -> match found -> joining), loads the chosen map, spawns a starter hero (the game needs a
//local player before hero select works) and then opens the hero select screen.
namespace quick_play {
	//Map loaded by the Play button / "Start now". Defaults to Hanamura.
	__int64 selected_map();
	void select_map(__int64 map_id);

	//Main menu Play button: matchmaking screens first, then the map.
	void start_from_menu();
	//Load the selected map right away (top bar), without the matchmaking screens.
	void start_now();

	//True while the matchmaking screens are shown (drives the game's "is matchmaking" config var).
	bool is_matchmaking();

	//Called every tick from PrometheusSystem::OnTick.
	void tick(Component_50_Input* input);
}
