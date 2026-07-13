#pragma once

#include "app_state.hpp"

#include <string>
#include <vector>

struct wl_display;
struct wl_surface;

namespace codec_gui::gui {

	[[nodiscard]] std::string decode_wayland_file_uri(std::string uri);
	[[nodiscard]] std::string first_uri_from_uri_list(const std::string& text);

	class WaylandWindow {
	public:
		struct Impl;

		WaylandWindow() = default;
		WaylandWindow(const WaylandWindow&) = delete;
		WaylandWindow& operator=(const WaylandWindow&) = delete;
		WaylandWindow(WaylandWindow&& other) noexcept;
		WaylandWindow& operator=(WaylandWindow&& other) noexcept;
		~WaylandWindow();

		static WaylandWindow create(int width, int height, const char* title);

		[[nodiscard]] bool valid() const;
		[[nodiscard]] bool configured() const;
		[[nodiscard]] bool close_requested() const;
		[[nodiscard]] wl_display* display() const;
		[[nodiscard]] wl_surface* surface() const;
		[[nodiscard]] int width() const;
		[[nodiscard]] int height() const;
		[[nodiscard]] int framebuffer_width() const;
		[[nodiscard]] int framebuffer_height() const;
		[[nodiscard]] float output_scale() const;

		void dispatch_pending();
		[[nodiscard]] std::vector<Action> take_actions();

	private:
		Impl* impl_ = nullptr;

		explicit WaylandWindow(Impl* impl);
		void reset();
	};

} // namespace codec_gui::gui
