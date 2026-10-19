#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace codec_gui {
struct RawImage;
struct EncodedImage;
} // namespace codec_gui
namespace codec_gui::gui {
struct DecoderInfo {
	std::string id, name;
	bool hardware = false;
};
struct DecodeSample {
	double seconds = 0;
	std::optional<double> codecSeconds;
	std::optional<double> joules;
	double energySeconds = 0;
	std::string error;
};
struct SampleStatistics {
	std::size_t count = 0;
	double mean = 0;
	std::optional<double> standardDeviation, standardError;
};
inline SampleStatistics sample_statistics(const std::vector<double> &values) {
	SampleStatistics s;
	double m2 = 0;
	for (double v : values) {
		++s.count;
		const double delta = v - s.mean;
		s.mean += delta / double(s.count);
		m2 += delta * (v - s.mean);
	}
	if (s.count > 1) {
		s.standardDeviation = std::sqrt(m2 / double(s.count - 1));
		s.standardError = *s.standardDeviation / std::sqrt(double(s.count));
	}
	return s;
}
struct DecodeMeasurement {
	std::string decoderId, decoderName, energyDomain, energyNote;
	int requested = 1;
	bool finished = false, canceled = false;
	std::vector<DecodeSample> samples;
};
struct ImageDecoderState {
	bool querying = false, queried = false, running = false;
	int repetitions = 10;
	std::string selected, error;
	std::vector<DecoderInfo> available;
	std::vector<DecodeMeasurement> history;
	uint64_t generation = 0;
};
struct DecoderNotification {
	std::vector<DecoderInfo> available;
	DecodeMeasurement measurement;
	std::string error;
	uint64_t generation = 0;
};
} // namespace codec_gui::gui
