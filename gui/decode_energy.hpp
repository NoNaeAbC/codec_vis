#pragma once
// Counter timing and wrap arithmetic for in-process decode measurements.
#include "energy_counter.hpp"
#include <chrono>
namespace codec_gui::gui {
class DecodeEnergy {
	std::shared_ptr<EnergyCounter> counter_;
  public:
	std::string domain;
	std::string note = "No energy was recorded for this run. Enable energy before starting a new measurement. Timing remains available.";
	using Clock = std::chrono::steady_clock;
	struct Reading { uint64_t uj; Clock::time_point time; };
	DecodeEnergy() : counter_(measurement_energy_counter()) {
		if (counter_) {
			domain = counter_->domain;
			note = "Raw RAPL domain energy; no idle subtraction. Includes other activity. Firmware-dependent coverage; not wall power or decoder-only energy.";
		}
	}
	DecodeEnergy(const DecodeEnergy &) = delete;
	std::optional<Reading> read() const {
		if (!counter_) return {};
		auto a = Clock::now();
		auto energy = read_energy_counter(counter_->fd);
		auto b = Clock::now();
		if (!energy || *energy >= counter_->range) return {};
		return Reading{*energy, a + (b - a) / 2};
	}
	static std::optional<double> delta(uint64_t before, uint64_t after, uint64_t range) {
		if (!range || before >= range || after >= range) return {};
		return double(after >= before ? after - before : range - before + after) * 1e-6;
	}
	std::optional<double> joules(Reading before, Reading after) const {
		return counter_ ? delta(before.uj, after.uj, counter_->range) : std::nullopt;
	}
};
} // namespace codec_gui::gui
