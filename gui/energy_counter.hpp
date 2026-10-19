#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>
namespace codec_gui::gui {
inline std::optional<uint64_t> read_energy_counter(int fd) {
	std::array<char, 64> buffer{};
	const auto n = ::pread(fd, buffer.data(), buffer.size(), 0);
	if (n <= 0) return {};
	uint64_t value = 0;
	const auto [end, error] = std::from_chars(buffer.data(), buffer.data() + n, value);
	if (error != std::errc{} || end == buffer.data()) return {};
	return value;
}
struct EnergyCounter {
	int fd = -1;
	uint64_t range = 0;
	std::string domain;
	EnergyCounter() = default;
	EnergyCounter(const EnergyCounter&) = delete;
	~EnergyCounter() { if (fd >= 0) ::close(fd); }
};
inline std::shared_ptr<EnergyCounter> open_energy_counter() {
	std::error_code error;
	std::vector<std::filesystem::path> paths;
	for (const auto &entry : std::filesystem::directory_iterator("/sys/class/powercap", error)) {
		const auto name = entry.path().filename().string();
		if (name.starts_with("intel-rapl:") && name.find(':', 11) == std::string::npos)
			paths.push_back(entry.path());
	}
	std::sort(paths.begin(), paths.end(), [](const auto &a, const auto &b) {
		std::string an, bn;
		std::ifstream(a / "name") >> an;
		std::ifstream(b / "name") >> bn;
		return (an == "psys") != (bn == "psys") ? an == "psys" : a < b;
	});
	for (const auto &path : paths) {
		auto counter = std::make_shared<EnergyCounter>();
		counter->fd = ::open((path / "energy_uj").c_str(), O_RDONLY | O_CLOEXEC);
		if (counter->fd < 0) continue;
		int range = ::open((path / "max_energy_range_uj").c_str(), O_RDONLY | O_CLOEXEC);
		auto maximum = range < 0 ? std::nullopt : read_energy_counter(range);
		if (range >= 0) ::close(range);
		auto reading = read_energy_counter(counter->fd);
		if (!maximum || !*maximum || !reading || *reading >= *maximum) continue;
		counter->range = *maximum;
		std::ifstream(path / "name") >> counter->domain;
		counter->domain += " (" + path.filename().string() + ")";
		return counter;
	}
	return {};
}
// Shared leases keep a running measurement valid if access is disabled meanwhile.
inline std::mutex sessionEnergyMutex;
inline std::shared_ptr<EnergyCounter> sessionEnergyCounter;
inline bool sessionEnergyDisabled = false;
inline void set_session_energy_counter(std::shared_ptr<EnergyCounter> counter) {
	std::lock_guard lock(sessionEnergyMutex);
	sessionEnergyDisabled = !counter;
	sessionEnergyCounter = std::move(counter);
}
inline std::shared_ptr<EnergyCounter> session_energy_counter() {
	std::lock_guard lock(sessionEnergyMutex);
	return sessionEnergyCounter;
}
inline std::shared_ptr<EnergyCounter> measurement_energy_counter() {
	std::lock_guard lock(sessionEnergyMutex);
	if (sessionEnergyDisabled) return {};
	return sessionEnergyCounter ? sessionEnergyCounter : open_energy_counter();
}
} // namespace codec_gui::gui
