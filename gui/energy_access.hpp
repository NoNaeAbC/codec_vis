#pragma once
#include "energy_counter.hpp"
#include <mutex>
#include <optional>
#include <thread>
namespace codec_gui::gui {
struct EnergyAccessResult {
	std::shared_ptr<EnergyCounter> counter;
	std::string error;
};
class EnergyAccess {
	std::mutex mutex_;
	std::optional<EnergyAccessResult> result_;
	std::jthread worker_;
  public:
	~EnergyAccess() { cancel(); }
	void request(const std::filesystem::path& helper = {});
	void cancel();
	std::optional<EnergyAccessResult> take_result();
};
} // namespace codec_gui::gui
