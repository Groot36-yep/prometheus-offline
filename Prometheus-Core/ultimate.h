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

	//Called every tick from PrometheusSystem::OnTick.
	void tick();
}
