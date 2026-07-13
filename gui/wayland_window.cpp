#include "wayland_window.hpp"

#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace codec_gui::gui {
namespace {

void require(bool ok, const char* message) {
	if (!ok) {
		throw std::runtime_error(message);
	}
}

} // namespace

struct WaylandWindow::Impl {
	wl_display* display = nullptr;
	wl_registry* registry = nullptr;
	wl_compositor* compositor = nullptr;
	xdg_wm_base* wmBase = nullptr;
	wl_data_device_manager* dataDeviceManager = nullptr;
	wp_fractional_scale_manager_v1* fractionalScaleManager = nullptr;
	wp_fractional_scale_v1* fractionalScale = nullptr;
	wp_viewporter* viewporter = nullptr;
	wp_viewport* viewport = nullptr;
	wl_surface* surface = nullptr;
	xdg_surface* xdgSurface = nullptr;
	xdg_toplevel* toplevel = nullptr;
	wl_seat* seat = nullptr;
	wl_pointer* pointer = nullptr;
	wl_keyboard* keyboard = nullptr;
	wl_data_device* dataDevice = nullptr;
	wl_data_offer* currentOffer = nullptr;
	xkb_context* xkbContext = nullptr;
	xkb_keymap* xkbKeymap = nullptr;
	xkb_state* xkbState = nullptr;
	std::vector<std::string> currentOfferMimeTypes;
	int width = 0;
	int height = 0;
	float outputScale = 1.0f;
	uint32_t outputScale120 = 120;
	bool configured = false;
	bool closeRequested = false;
	Point pointerPosition;
	std::vector<Action> actions;
};

namespace {

void update_surface_viewport(WaylandWindow::Impl& impl) {
	if (impl.viewport != nullptr && impl.width > 0 && impl.height > 0) {
		wp_viewport_set_destination(impl.viewport, impl.width, impl.height);
	}
}

void preferred_fractional_scale(void* data, wp_fractional_scale_v1*, uint32_t scale120) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.outputScale120 = std::max(120u, scale120);
	impl.outputScale = std::max(1.0f, static_cast<float>(scale120) / 120.0f);
	update_surface_viewport(impl);
}

constexpr wp_fractional_scale_v1_listener FractionalScaleListener{preferred_fractional_scale};

} // namespace

std::string decode_wayland_file_uri(std::string uri) {
	const std::string prefix = "file://";
	if (uri.rfind(prefix, 0) != 0) {
		return uri;
	}
	uri.erase(0, prefix.size());
	std::string out;
	out.reserve(uri.size());
	for (std::size_t i = 0; i < uri.size(); ++i) {
		if (uri[i] == '%' && i + 2 < uri.size()) {
			auto hex_value = [](char c) -> int {
				if (c >= '0' && c <= '9') {
					return c - '0';
				}
				if (c >= 'a' && c <= 'f') {
					return 10 + c - 'a';
				}
				if (c >= 'A' && c <= 'F') {
					return 10 + c - 'A';
				}
				return -1;
			};
			const int hi = hex_value(uri[i + 1]);
			const int lo = hex_value(uri[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(uri[i]);
	}
	return out;
}

std::string first_uri_from_uri_list(const std::string& text) {
	std::size_t start = 0;
	while (start < text.size()) {
		std::size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string line = text.substr(start, end - start);
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (!line.empty() && line.front() != '#') {
			return line;
		}
		start = end + 1;
	}
	return {};
}

namespace {

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	const std::string iface = interface == nullptr ? "" : interface;
	if (iface == wl_compositor_interface.name) {
		impl.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
	} else if (iface == xdg_wm_base_interface.name) {
		impl.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
	} else if (iface == wl_seat_interface.name) {
		impl.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
	} else if (iface == wl_data_device_manager_interface.name) {
		impl.dataDeviceManager = static_cast<wl_data_device_manager*>(wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3));
	} else if (iface == wp_fractional_scale_manager_v1_interface.name) {
		impl.fractionalScaleManager = static_cast<wp_fractional_scale_manager_v1*>(wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
	} else if (iface == wp_viewporter_interface.name) {
		impl.viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
	}
}

void registry_remove(void*, wl_registry*, uint32_t) {}

constexpr wl_registry_listener RegistryListener{registry_global, registry_remove};

void wm_base_ping(void*, xdg_wm_base* wmBase, uint32_t serial) {
	xdg_wm_base_pong(wmBase, serial);
}

constexpr xdg_wm_base_listener WmBaseListener{wm_base_ping};

void xdg_surface_configure(void* data, xdg_surface* surface, uint32_t serial) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	xdg_surface_ack_configure(surface, serial);
	impl.configured = true;
}

constexpr xdg_surface_listener XdgSurfaceListener{xdg_surface_configure};

void toplevel_configure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	if (width > 0) {
		impl.width = width;
	}
	if (height > 0) {
		impl.height = height;
	}
	update_surface_viewport(impl);
}

void toplevel_close(void* data, xdg_toplevel*) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.closeRequested = true;
}

void toplevel_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void toplevel_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}

constexpr xdg_toplevel_listener ToplevelListener{
	toplevel_configure,
	toplevel_close,
	toplevel_configure_bounds,
	toplevel_wm_capabilities,
};

Action& append_action(WaylandWindow::Impl& impl, ActionKind kind) {
	Action& action = impl.actions.emplace_back();
	action.kind = kind;
	return action;
}

void pointer_enter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t surfaceX, wl_fixed_t surfaceY) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.pointerPosition = Point{static_cast<float>(wl_fixed_to_double(surfaceX)), static_cast<float>(wl_fixed_to_double(surfaceY))};
	Action& action = append_action(impl, ActionKind::PointerMoved);
	action.point = impl.pointerPosition;
}

void pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*) {}

void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t surfaceX, wl_fixed_t surfaceY) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.pointerPosition = Point{static_cast<float>(wl_fixed_to_double(surfaceX)), static_cast<float>(wl_fixed_to_double(surfaceY))};
	Action& action = append_action(impl, ActionKind::PointerMoved);
	action.point = impl.pointerPosition;
}

void pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t state) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	Action& action = append_action(impl, state == WL_POINTER_BUTTON_STATE_PRESSED ? ActionKind::PointerPressed : ActionKind::PointerReleased);
	action.point = impl.pointerPosition;
}

void pointer_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	Action& action = append_action(impl, ActionKind::PointerScrolled);
	action.point = impl.pointerPosition;
	action.value = wl_fixed_to_double(value);
}

void pointer_frame(void*, wl_pointer*) {}
void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
void pointer_axis_value120(void*, wl_pointer*, uint32_t, int32_t) {}
void pointer_axis_relative_direction(void*, wl_pointer*, uint32_t, uint32_t) {}

constexpr wl_pointer_listener PointerListener{
	pointer_enter,
	pointer_leave,
	pointer_motion,
	pointer_button,
	pointer_axis,
	pointer_frame,
	pointer_axis_source,
	pointer_axis_stop,
	pointer_axis_discrete,
	pointer_axis_value120,
	pointer_axis_relative_direction,
};

void push_key_action(WaylandWindow::Impl& impl, std::string text) {
	if (text.empty()) {
		return;
	}
	Action& action = append_action(impl, ActionKind::KeyPressed);
	action.text = std::move(text);
}

void keyboard_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || fd < 0 || size == 0) {
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	std::string map;
	map.resize(size);
	std::size_t offset = 0;
	while (offset < map.size()) {
		const ssize_t n = read(fd, map.data() + offset, map.size() - offset);
		if (n <= 0) {
			break;
		}
		offset += static_cast<std::size_t>(n);
	}
	close(fd);
	if (offset == 0) {
		return;
	}
	if (impl.xkbState != nullptr) {
		xkb_state_unref(impl.xkbState);
		impl.xkbState = nullptr;
	}
	if (impl.xkbKeymap != nullptr) {
		xkb_keymap_unref(impl.xkbKeymap);
		impl.xkbKeymap = nullptr;
	}
	if (impl.xkbContext == nullptr) {
		impl.xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	}
	if (impl.xkbContext == nullptr) {
		return;
	}
	impl.xkbKeymap = xkb_keymap_new_from_string(impl.xkbContext, map.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (impl.xkbKeymap != nullptr) {
		impl.xkbState = xkb_state_new(impl.xkbKeymap);
	}
}

void keyboard_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void keyboard_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}

void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	if (impl.xkbState == nullptr) {
		return;
	}
	const xkb_keycode_t keycode = key + 8;
	const xkb_keysym_t sym = xkb_state_key_get_one_sym(impl.xkbState, keycode);
	switch (sym) {
		case XKB_KEY_BackSpace:
			push_key_action(impl, "backspace");
			return;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			push_key_action(impl, "enter");
			return;
		case XKB_KEY_Escape:
			push_key_action(impl, "escape");
			return;
		default:
			break;
	}
	char utf8[64]{};
	const int bytes = xkb_state_key_get_utf8(impl.xkbState, keycode, utf8, sizeof(utf8));
	if (bytes > 0) {
		push_key_action(impl, std::string(utf8, static_cast<std::size_t>(bytes)));
	}
}

void keyboard_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t modsDepressed, uint32_t modsLatched, uint32_t modsLocked, uint32_t group) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	if (impl.xkbState != nullptr) {
		xkb_state_update_mask(impl.xkbState, modsDepressed, modsLatched, modsLocked, 0, 0, group);
	}
}

void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}

constexpr wl_keyboard_listener KeyboardListener{
	keyboard_keymap,
	keyboard_enter,
	keyboard_leave,
	keyboard_key,
	keyboard_modifiers,
	keyboard_repeat_info,
};

void data_offer_offer(void* data, wl_data_offer*, const char* mimeType) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	if (mimeType != nullptr) {
		impl.currentOfferMimeTypes.emplace_back(mimeType);
	}
}

void data_offer_source_actions(void*, wl_data_offer*, uint32_t) {}
void data_offer_action(void*, wl_data_offer*, uint32_t) {}

constexpr wl_data_offer_listener DataOfferListener{
	data_offer_offer,
	data_offer_source_actions,
	data_offer_action,
};

bool current_offer_has_uri_list(const WaylandWindow::Impl& impl) {
	for (const std::string& mime : impl.currentOfferMimeTypes) {
		if (mime == "text/uri-list") {
			return true;
		}
	}
	return false;
}

void destroy_current_offer(WaylandWindow::Impl& impl) {
	if (impl.currentOffer != nullptr) {
		wl_data_offer_destroy(impl.currentOffer);
		impl.currentOffer = nullptr;
	}
	impl.currentOfferMimeTypes.clear();
}

void data_device_data_offer(void* data, wl_data_device*, wl_data_offer* offer) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	destroy_current_offer(impl);
	impl.currentOffer = offer;
	if (offer != nullptr) {
		wl_data_offer_add_listener(offer, &DataOfferListener, &impl);
	}
}

void data_device_enter(void* data, wl_data_device*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y, wl_data_offer* offer) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.pointerPosition = Point{static_cast<float>(wl_fixed_to_double(x)), static_cast<float>(wl_fixed_to_double(y))};
	if (offer != nullptr && current_offer_has_uri_list(impl)) {
		wl_data_offer_accept(offer, serial, "text/uri-list");
	}
}

void data_device_leave(void* data, wl_data_device*) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	destroy_current_offer(impl);
}

void data_device_motion(void* data, wl_data_device*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.pointerPosition = Point{static_cast<float>(wl_fixed_to_double(x)), static_cast<float>(wl_fixed_to_double(y))};
}

void data_device_drop(void* data, wl_data_device*) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	if (impl.currentOffer == nullptr || !current_offer_has_uri_list(impl)) {
		destroy_current_offer(impl);
		return;
	}
	int fds[2]{-1, -1};
	if (pipe(fds) != 0) {
		destroy_current_offer(impl);
		return;
	}
	wl_data_offer_receive(impl.currentOffer, "text/uri-list", fds[1]);
	close(fds[1]);
	wl_display_flush(impl.display);

	std::string text;
	char buffer[4096];
	for (;;) {
		const ssize_t n = read(fds[0], buffer, sizeof(buffer));
		if (n <= 0) {
			break;
		}
		text.append(buffer, static_cast<std::size_t>(n));
	}
	close(fds[0]);

	const std::string uri = first_uri_from_uri_list(text);
	if (!uri.empty()) {
		Action& action = append_action(impl, ActionKind::SourcePathChosen);
		action.path = decode_wayland_file_uri(uri);
	}
	destroy_current_offer(impl);
}

void data_device_selection(void*, wl_data_device*, wl_data_offer*) {}

constexpr wl_data_device_listener DataDeviceListener{
	data_device_data_offer,
	data_device_enter,
	data_device_leave,
	data_device_motion,
	data_device_drop,
	data_device_selection,
};

void seat_capabilities(void* data, wl_seat* seat, uint32_t capabilities) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && impl.pointer == nullptr) {
		impl.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(impl.pointer, &PointerListener, &impl);
	} else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 && impl.pointer != nullptr) {
		wl_pointer_destroy(impl.pointer);
		impl.pointer = nullptr;
	}
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && impl.keyboard == nullptr) {
		impl.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(impl.keyboard, &KeyboardListener, &impl);
	} else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0 && impl.keyboard != nullptr) {
		wl_keyboard_destroy(impl.keyboard);
		impl.keyboard = nullptr;
	}
	if (impl.dataDeviceManager != nullptr && impl.dataDevice == nullptr) {
		impl.dataDevice = wl_data_device_manager_get_data_device(impl.dataDeviceManager, seat);
		wl_data_device_add_listener(impl.dataDevice, &DataDeviceListener, &impl);
	}
}

void seat_name(void*, wl_seat*, const char*) {}

constexpr wl_seat_listener SeatListener{seat_capabilities, seat_name};

} // namespace

WaylandWindow::WaylandWindow(Impl* impl) : impl_(impl) {}

WaylandWindow::WaylandWindow(WaylandWindow&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}

WaylandWindow& WaylandWindow::operator=(WaylandWindow&& other) noexcept {
	if (this != &other) {
		reset();
		impl_ = std::exchange(other.impl_, nullptr);
	}
	return *this;
}

WaylandWindow::~WaylandWindow() {
	reset();
}

WaylandWindow WaylandWindow::create(int width, int height, const char* title) {
	Impl* impl = new Impl;
	impl->width = width;
	impl->height = height;
	impl->actions.reserve(64);

	try {
		impl->display = wl_display_connect(nullptr);
		require(impl->display != nullptr, "Wayland display is unavailable");

		impl->registry = wl_display_get_registry(impl->display);
		require(impl->registry != nullptr, "Wayland registry unavailable");
		wl_registry_add_listener(impl->registry, &RegistryListener, impl);
		wl_display_roundtrip(impl->display);
		require(impl->compositor != nullptr, "Wayland compositor interface unavailable");
		require(impl->wmBase != nullptr, "xdg_wm_base interface unavailable");
		xdg_wm_base_add_listener(impl->wmBase, &WmBaseListener, impl);
		if (impl->seat != nullptr) {
			wl_seat_add_listener(impl->seat, &SeatListener, impl);
		}

		impl->surface = wl_compositor_create_surface(impl->compositor);
		require(impl->surface != nullptr, "Wayland surface creation failed");
		if (impl->viewporter != nullptr) {
			impl->viewport = wp_viewporter_get_viewport(impl->viewporter, impl->surface);
			require(impl->viewport != nullptr, "Wayland viewport creation failed");
		}
		if (impl->fractionalScaleManager != nullptr) {
			impl->fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(impl->fractionalScaleManager, impl->surface);
			require(impl->fractionalScale != nullptr, "Wayland fractional-scale object creation failed");
			wp_fractional_scale_v1_add_listener(impl->fractionalScale, &FractionalScaleListener, impl);
		}
		wl_surface_set_buffer_scale(impl->surface, 1);
		impl->xdgSurface = xdg_wm_base_get_xdg_surface(impl->wmBase, impl->surface);
		require(impl->xdgSurface != nullptr, "xdg_surface creation failed");
		xdg_surface_add_listener(impl->xdgSurface, &XdgSurfaceListener, impl);
		impl->toplevel = xdg_surface_get_toplevel(impl->xdgSurface);
		require(impl->toplevel != nullptr, "xdg_toplevel creation failed");
		xdg_toplevel_add_listener(impl->toplevel, &ToplevelListener, impl);
		xdg_toplevel_set_title(impl->toplevel, title == nullptr ? "codec_vis" : title);
		wl_surface_commit(impl->surface);
		wl_display_roundtrip(impl->display);
	} catch (...) {
		WaylandWindow cleanup(impl);
		throw;
	}

	return WaylandWindow(impl);
}

bool WaylandWindow::valid() const {
	return impl_ != nullptr && impl_->display != nullptr && impl_->surface != nullptr;
}

bool WaylandWindow::configured() const {
	return impl_ != nullptr && impl_->configured;
}

bool WaylandWindow::close_requested() const {
	return impl_ != nullptr && impl_->closeRequested;
}

wl_display* WaylandWindow::display() const {
	return impl_ == nullptr ? nullptr : impl_->display;
}

wl_surface* WaylandWindow::surface() const {
	return impl_ == nullptr ? nullptr : impl_->surface;
}

int WaylandWindow::width() const {
	return impl_ == nullptr ? 0 : impl_->width;
}

int WaylandWindow::height() const {
	return impl_ == nullptr ? 0 : impl_->height;
}

int WaylandWindow::framebuffer_width() const {
	return impl_ == nullptr ? 0 : std::max(1, static_cast<int>((static_cast<int64_t>(impl_->width) * impl_->outputScale120 + 119) / 120));
}

int WaylandWindow::framebuffer_height() const {
	return impl_ == nullptr ? 0 : std::max(1, static_cast<int>((static_cast<int64_t>(impl_->height) * impl_->outputScale120 + 119) / 120));
}

float WaylandWindow::output_scale() const {
	return impl_ == nullptr ? 1.0f : impl_->outputScale;
}

void WaylandWindow::dispatch_pending() {
	if (impl_ == nullptr || impl_->display == nullptr) {
		return;
	}
	wl_display_dispatch_pending(impl_->display);
	wl_display_flush(impl_->display);
}

std::vector<Action> WaylandWindow::take_actions() {
	if (impl_ == nullptr) {
		return {};
	}
	std::vector<Action> out;
	out.swap(impl_->actions);
	return out;
}

void WaylandWindow::reset() {
	if (impl_ == nullptr) {
		return;
	}
	if (impl_->toplevel != nullptr) {
		xdg_toplevel_destroy(impl_->toplevel);
	}
	if (impl_->fractionalScale != nullptr) {
		wp_fractional_scale_v1_destroy(impl_->fractionalScale);
	}
	if (impl_->viewport != nullptr) {
		wp_viewport_destroy(impl_->viewport);
	}
	destroy_current_offer(*impl_);
	if (impl_->dataDevice != nullptr) {
		wl_data_device_destroy(impl_->dataDevice);
	}
	if (impl_->pointer != nullptr) {
		wl_pointer_destroy(impl_->pointer);
	}
	if (impl_->keyboard != nullptr) {
		wl_keyboard_destroy(impl_->keyboard);
	}
	if (impl_->xkbState != nullptr) {
		xkb_state_unref(impl_->xkbState);
	}
	if (impl_->xkbKeymap != nullptr) {
		xkb_keymap_unref(impl_->xkbKeymap);
	}
	if (impl_->xkbContext != nullptr) {
		xkb_context_unref(impl_->xkbContext);
	}
	if (impl_->seat != nullptr) {
		wl_seat_destroy(impl_->seat);
	}
	if (impl_->dataDeviceManager != nullptr) {
		wl_data_device_manager_destroy(impl_->dataDeviceManager);
	}
	if (impl_->xdgSurface != nullptr) {
		xdg_surface_destroy(impl_->xdgSurface);
	}
	if (impl_->surface != nullptr) {
		wl_surface_destroy(impl_->surface);
	}
	if (impl_->wmBase != nullptr) {
		xdg_wm_base_destroy(impl_->wmBase);
	}
	if (impl_->compositor != nullptr) {
		wl_compositor_destroy(impl_->compositor);
	}
	if (impl_->fractionalScaleManager != nullptr) {
		wp_fractional_scale_manager_v1_destroy(impl_->fractionalScaleManager);
	}
	if (impl_->viewporter != nullptr) {
		wp_viewporter_destroy(impl_->viewporter);
	}
	if (impl_->registry != nullptr) {
		wl_registry_destroy(impl_->registry);
	}
	if (impl_->display != nullptr) {
		wl_display_disconnect(impl_->display);
	}
	delete impl_;
	impl_ = nullptr;
}

} // namespace codec_gui::gui
