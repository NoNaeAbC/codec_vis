#pragma once

#include <cstdio>
#include <cstdlib>

namespace codec_gui::gui::test {

[[noreturn]] inline void check_failed(const char* expression, const char* file, int line) noexcept {
	std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
	std::abort();
}

} // namespace codec_gui::gui::test

#define CODEC_VIS_CHECK(expression) \
	((expression) ? static_cast<void>(0) : ::codec_gui::gui::test::check_failed(#expression, __FILE__, __LINE__))
