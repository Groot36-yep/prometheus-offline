#pragma once
#include "entity_admin.h"

//Auto Test: plays every hero by itself and reports which abilities work offline.
//For each hero it spawns the hero, then presses each ability button and checks whether the hero's scripts react,
//whether states started by the ability are still running afterwards, and whether primary fire still works (a
//stuck ability state blocks it, like Recall did). Results go to Prometheus/auto_test_report.json and [AUTOTEST]
//log lines.
//Start it from the top bar, or set the environment variable PROMETHEUS_AUTOTEST before launching the game:
//  "all" (every hero) or a comma separated list of hero names, e.g. "tracer,genji". Add ",exit" to close the game
//  once the report is written.
namespace auto_test {
	void start(const std::string& heroes = "all", bool exit_when_done = false);
	bool is_running();

	//Called every tick from PrometheusSystem::OnTick.
	void tick(Component_50_Input* input);
}
