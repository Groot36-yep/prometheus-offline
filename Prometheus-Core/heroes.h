#pragma once

struct HeroInfo {
	__int64 id;
	const char* name;
};

//The heroes the build loads (see the data pack list in dllmain.cpp).
inline constexpr HeroInfo HEROES[] = {
	{ 0x02e0000000000003, "Tracer" }, { 0x02e0000000000002, "Reaper" }, { 0x02e0000000000004, "Mercy" },
	{ 0x02e0000000000005, "Hanzo" }, { 0x02e0000000000006, "Torbjorn" }, { 0x02e0000000000007, "Reinhardt" },
	{ 0x02e0000000000008, "Pharah" }, { 0x02e0000000000009, "Winston" }, { 0x02e000000000000a, "Widowmaker" },
	{ 0x02e0000000000015, "Bastion" }, { 0x02e0000000000016, "Symmetra" }, { 0x02e0000000000020, "Zenyatta" },
	{ 0x02e0000000000029, "Genji" }, { 0x02e0000000000040, "Roadhog" }, { 0x02e0000000000042, "McCree" },
	{ 0x02e0000000000065, "Junkrat" }, { 0x02e0000000000068, "Zarya" }, { 0x02e000000000006e, "Soldier76" },
	{ 0x02e0000000000079, "Lucio" }, { 0x02e000000000007a, "DVa" }, { 0x02e00000000000dd, "Mei" },
};

inline const HeroInfo* find_hero(__int64 id) {
	for (auto& hero : HEROES) {
		if (hero.id == id)
			return &hero;
	}
	return nullptr;
}
