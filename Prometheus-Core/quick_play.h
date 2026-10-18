#pragma once
#include "entity_admin.h"

//Quick Play: makes the main menu's Play button work offline, in one press. It loads the hero data if needed (what
//the top bar's "Demo" and "DP Load" do), runs the game's own matchmaking screens (searching -> match found ->
//joining), loads the chosen map and spawns the chosen hero. With no hero chosen it spawns a starter hero (the game
//needs a local player before hero select works) and opens hero select.
//The map, the hero and "start a game at launch" are saved to Prometheus/play_settings.json.
namespace quick_play {
	//Map loaded by Play. Defaults to Hanamura.
	__int64 selected_map();
	void select_map(__int64 map_id);

	//Hero spawned by Play; 0 = pick in hero select. Picking a hero in hero select also sets it.
	__int64 selected_hero();
	void select_hero(__int64 hero_id);

	//Press Play by itself once the main menu has loaded.
	bool autostart();
	void set_autostart(bool enabled);

	//Main menu Play button: matchmaking screens first, then the map.
	void start_from_menu();
	//Same without the matchmaking screens (top bar).
	void start_now();
	//Selects the map and loads it right away, also from inside a game: removes the hero first (unloading a map
	//with our hero in it crashes), then spawns the hero on the new map.
	void switch_map(__int64 map_id);
	bool is_running();

	//True while the matchmaking screens are shown (drives the game's "is matchmaking" config var).
	bool is_matchmaking();

	//Demo mode (hero select spawns the picked hero) and the lobby data packs (the hero data).
	void load_hero_data();
	bool hero_data_loaded();

	//Called every tick from PrometheusSystem::OnTick.
	void tick(Component_50_Input* input);
}
