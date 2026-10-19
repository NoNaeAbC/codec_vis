#include "image_size_target.hpp"
#include "test_check.hpp"
#include <iostream>
#include <limits>
using namespace codec_gui;
int main() {
	std::vector<EncoderParam> old{{"bitrate-kbps", int64_t(2000)}};
	CODEC_VIS_CHECK(requested_image_size_bytes(old) == 250000u);
	CODEC_VIS_CHECK(vaapi_still_target_bps(old) == 2000000u);
	for (double size: {.125, 18.7, 250., 250.001, 125000.}) {
		std::vector<EncoderParam> params{{"target-size-kb", size}};
		const auto                bytes = uint32_t(std::llround(size * 1000.));
		CODEC_VIS_CHECK(requested_image_size_bytes(params) == bytes);
		CODEC_VIS_CHECK(vaapi_still_target_bps(params) == bytes * 8u);
	}
	CODEC_VIS_CHECK(vaapi_still_target_bps({}) == 2000000u);
	old.push_back({"target-size-kb", 100.});
	CODEC_VIS_CHECK(requested_image_size_bytes(old) == 100000u);
	for (double size:
		 {-1., 0., 125001., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
		bool rejected = false;
		try {
			std::vector<EncoderParam> params{{"target-size-kb", size}};
			(void) vaapi_still_target_bps(params);
		} catch (const std::invalid_argument &) { rejected = true; }
		CODEC_VIS_CHECK(rejected);
	}
	std::cout << "Image budget: decimal kB -> bytes -> bits at 1 fps, exact 2000 kbit = 250000 B, legacy settings, "
				 "bounds and overflow checks passed\n";
}
