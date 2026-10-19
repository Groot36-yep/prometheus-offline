#pragma once

struct Entity;

//Statescript nodes only the server implements. The client registers no implementation for some node types (the
//server's entity creation, pets, possession), so a graph that reaches one does nothing offline: Torbjorn's turret,
//Symmetra's turrets and the like never appear. This module traces those nodes and emulates the ones we know.
namespace server_nodes {
	//Installs the hooks. Called once the game's statescript is running.
	void install();

	//Called every tick from PrometheusSystem::OnTick.
	void tick();

	//Called for every projectile a weapon volley creates (Mei's Blizzard drone lands with the server's help).
	void projectile_created(Entity* projectile);
}
