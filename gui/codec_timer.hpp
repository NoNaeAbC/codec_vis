#pragma once
#include <chrono>
#include <functional>
#include <type_traits>
#include <utility>

namespace codec_gui::gui {
// Accumulate only completed codec calls. Setup, output allocation, pixel copies
// and application color conversion stay outside this interval.
class CodecTimer {
	double seconds_ = 0;
public:
	template<class F> auto measure(F&& call) {
		const auto start = std::chrono::steady_clock::now();
		auto record = [&] {
			seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		};
		if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
			std::invoke(std::forward<F>(call));
			record();
		} else {
			auto result = std::invoke(std::forward<F>(call));
			record();
			return result;
		}
	}
	double seconds() const { return seconds_; }
};
}
