#pragma once
#include "../codec_gui_x265.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
namespace codec_gui {
	// Decimal kB, bytes and bits. Still-image VA-API uses one frame per second.
	inline std::optional<uint32_t> requested_image_size_bytes(std::span<const EncoderParam> params) {
		for (const auto &p: params)
			if (p.name == "target-size-kb") {
				double kb;
				if (const auto *v = std::get_if<double>(&p.value)) kb = *v;
				else if (const auto *v = std::get_if<int64_t>(&p.value))
					kb = double(*v);
				else
					throw std::invalid_argument("Target image size must be numeric");
				if (!std::isfinite(kb) || kb < .125 || kb > 125000.)
					throw std::invalid_argument("Target image size must be 0.125..125000 kB (1 kB = 1000 bytes)");
				return uint32_t(std::llround(kb * 1000.));
			}
		// Retained configurations and linked clients using the previous units.
		for (const auto &p: params)
			if (p.name == "bitrate-kbps")
				return uint32_t(std::clamp<int64_t>(std::get<int64_t>(p.value), 1, 1000000) * 125);
		return {};
	}
	inline uint32_t vaapi_still_target_bps(std::span<const EncoderParam> params) {
		return requested_image_size_bytes(params).value_or(250000u) * 8u;
	}
} // namespace codec_gui
