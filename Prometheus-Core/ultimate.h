#pragma once

//Ultimate charge: the server raises it (damage, healing, over time), so offline it stayed at 0 and no ultimate
//could start. An ultimate's start condition (STUStatescriptStateAbility m_startCondition, e.g. Tracer's graph
//0x580000000000034 node 38) compares two of the hero's entity variables: the charge 0xD80000000000156 and the
//hero's ultimate cost 0xD80000000000152 (Tracer 900, Pharah 1500). Saved to Prometheus/ultimate_settings.json.
namespace ultimate {
	enum class Mode {
		OverTime, //Fills from 0 to the cost in CHARGE_SECS, starting again after an ultimate
		Ready,    //Always full
		Off,      //Left alone
	};

	Mode mode();
	void set_mode(Mode mode);

	//An ultimate started: the server sets the charge to 0 then, nothing does offline (it stayed full, so the ultimate
	//could start again at once, e.g. RIP-Tire every time Junkrat's graph returned to its ready state).
	void spend();

	//Called every tick from PrometheusSystem::OnTick.
	void tick();
}
