#pragma once

//Display: keeps the game in borderless window mode (the beta has no settings file of its own and starts in
//exclusive fullscreen). Uses the game's own setting objects and display apply function, the same way the Video
//options do. Saved to Prometheus/display_settings.json.
namespace display {
	bool borderless();
	void set_borderless(bool enabled);

	//Called every tick from PrometheusSystem::OnTick.
	void tick();
}
