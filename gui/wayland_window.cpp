#include "wayland_window.hpp"

#include "app_state.hpp"
#include "xdg-shell-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "color-management-v1-client-protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <poll.h>
#include <map>
#include <thread>
#include <csignal>
#include <pthread.h>
#include <fcntl.h>
#include <sys/types.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon-keysyms.h>
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
    wp_color_manager_v1* colorManager=nullptr;
    wp_color_management_surface_v1* colorSurface=nullptr;
    wp_image_description_v1* colorDescription=nullptr;
    uint32_t colorFeatures=0;
    bool colorLinear=false,color2020=false,colorPerceptual=false,colorReady=false;
    std::string colorError;
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
	std::map<wl_data_offer*,std::vector<std::string>> offerMimeTypes;
    wl_data_offer* selectionOffer=nullptr;
    wl_data_source* clipboardSource=nullptr;
    std::string clipboardText,clipboardReadBuffer;
    int clipboardReadFd=-1;
    uint32_t inputSerial=0;
    uint32_t leftPressSerial=0;
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
	if (!uri.starts_with(prefix)) {
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

void color_intent(void* data,wp_color_manager_v1*,uint32_t value){
    if(value==WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL)static_cast<WaylandWindow::Impl*>(data)->colorPerceptual=true;
}
void color_feature(void* data,wp_color_manager_v1*,uint32_t value){
    if(value<32)static_cast<WaylandWindow::Impl*>(data)->colorFeatures|=1u<<value;
}
void color_tf(void* data,wp_color_manager_v1*,uint32_t value){
    if(value==WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR)static_cast<WaylandWindow::Impl*>(data)->colorLinear=true;
}
void color_primaries(void* data,wp_color_manager_v1*,uint32_t value){
    if(value==WP_COLOR_MANAGER_V1_PRIMARIES_BT2020)static_cast<WaylandWindow::Impl*>(data)->color2020=true;
}
void color_done(void*,wp_color_manager_v1*){}
const wp_color_manager_v1_listener ColorManagerListener{
    .supported_intent=color_intent,.supported_feature=color_feature,.supported_tf_named=color_tf,
    .supported_primaries_named=color_primaries,.done=color_done};
void color_failed(void* data,wp_image_description_v1*,uint32_t,const char* message){
    static_cast<WaylandWindow::Impl*>(data)->colorError=message?message:"image description rejected";
}
void color_ready(void* data,wp_image_description_v1*,uint32_t){static_cast<WaylandWindow::Impl*>(data)->colorReady=true;}
const wp_image_description_v1_listener ColorDescriptionListener{.failed=color_failed,.ready=color_ready};

void registry_global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	const std::string iface = interface == nullptr ? "" : interface;
	if(iface==wp_color_manager_v1_interface.name){
        impl.colorManager=static_cast<wp_color_manager_v1*>(wl_registry_bind(registry,name,&wp_color_manager_v1_interface,1));
        wp_color_manager_v1_add_listener(impl.colorManager,&ColorManagerListener,&impl);
    } else if (iface == wl_compositor_interface.name) {
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

constexpr wl_registry_listener RegistryListener{.global=registry_global, .global_remove=registry_remove};

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
	.configure=toplevel_configure,
	.close=toplevel_close,
	.configure_bounds=toplevel_configure_bounds,
	.wm_capabilities=toplevel_wm_capabilities,
};

Action& append_action(WaylandWindow::Impl& impl, ActionKind kind) {
	Action& action = impl.actions.emplace_back();
	action.kind = kind;
	return action;
}

void pointer_enter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t surfaceX, wl_fixed_t surfaceY) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.pointerPosition = Point{.x=static_cast<float>(wl_fixed_to_double(surfaceX)), .y=static_cast<float>(wl_fixed_to_double(surfaceY))};
	Action& action = append_action(impl, ActionKind::PointerMoved);
	action.point = impl.pointerPosition;
}

void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    static_cast<WaylandWindow::Impl*>(data)->leftPressSerial=0;
}

void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t surfaceX, wl_fixed_t surfaceY) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.pointerPosition = Point{.x=static_cast<float>(wl_fixed_to_double(surfaceX)), .y=static_cast<float>(wl_fixed_to_double(surfaceY))};
	Action& action = append_action(impl, ActionKind::PointerMoved);
	action.point = impl.pointerPosition;
}

void pointer_button(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.inputSerial=serial;
	Action& action = append_action(impl, state == WL_POINTER_BUTTON_STATE_PRESSED ? ActionKind::PointerPressed : ActionKind::PointerReleased);
    action.button=button;
    action.pointerSerial=serial;
    if(button==272)impl.leftPressSerial=state==WL_POINTER_BUTTON_STATE_PRESSED?serial:0;
    if(impl.xkbState){
        action.shift=xkb_state_mod_name_is_active(impl.xkbState,XKB_MOD_NAME_SHIFT,XKB_STATE_MODS_EFFECTIVE)>0;
        action.control=xkb_state_mod_name_is_active(impl.xkbState,XKB_MOD_NAME_CTRL,XKB_STATE_MODS_EFFECTIVE)>0;
        action.alt=xkb_state_mod_name_is_active(impl.xkbState,XKB_MOD_NAME_ALT,XKB_STATE_MODS_EFFECTIVE)>0;
    }
	action.point = impl.pointerPosition;
}

void pointer_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	Action& action = append_action(impl, ActionKind::PointerScrolled);
	action.point = impl.pointerPosition;
	if(axis==WL_POINTER_AXIS_VERTICAL_SCROLL)action.value=wl_fixed_to_double(value);
    else action.scrollX=wl_fixed_to_double(value);
}

void pointer_frame(void*, wl_pointer*) {}
void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
void pointer_axis_value120(void*, wl_pointer*, uint32_t, int32_t) {}
void pointer_axis_relative_direction(void*, wl_pointer*, uint32_t, uint32_t) {}

constexpr wl_pointer_listener PointerListener{
	.enter=pointer_enter,
	.leave=pointer_leave,
	.motion=pointer_motion,
	.button=pointer_button,
	.axis=pointer_axis,
	.frame=pointer_frame,
	.axis_source=pointer_axis_source,
	.axis_stop=pointer_axis_stop,
	.axis_discrete=pointer_axis_discrete,
	.axis_value120=pointer_axis_value120,
	.axis_relative_direction=pointer_axis_relative_direction,
};

void push_key_action(WaylandWindow::Impl& impl, std::string text, bool pressed=true) {
	if (text.empty()) {
		return;
	}
	Action& action = append_action(impl, pressed?ActionKind::KeyPressed:ActionKind::KeyReleased);
	action.text = std::move(text);
    if(impl.xkbState){
        action.shift=xkb_state_mod_name_is_active(impl.xkbState,XKB_MOD_NAME_SHIFT,XKB_STATE_MODS_EFFECTIVE)>0;
        action.control=xkb_state_mod_name_is_active(impl.xkbState,XKB_MOD_NAME_CTRL,XKB_STATE_MODS_EFFECTIVE)>0;
        action.alt=xkb_state_mod_name_is_active(impl.xkbState,XKB_MOD_NAME_ALT,XKB_STATE_MODS_EFFECTIVE)>0;
    }
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
void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    append_action(*static_cast<WaylandWindow::Impl*>(data),ActionKind::WindowFocusLost);
}

void keyboard_key(void* data, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key, uint32_t state) {
	const bool pressed=state==WL_KEYBOARD_KEY_STATE_PRESSED;
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
    impl.inputSerial=serial;
	if (impl.xkbState == nullptr) {
		return;
	}
	const xkb_keycode_t keycode = key + 8;
	const xkb_keysym_t sym = xkb_state_key_get_one_sym(impl.xkbState, keycode);
	switch (sym) {
		case XKB_KEY_BackSpace:
			push_key_action(impl, "backspace",pressed);
			return;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			push_key_action(impl, "enter",pressed);
			return;
		case XKB_KEY_Escape:
			push_key_action(impl, "escape",pressed);
			return;
        case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: push_key_action(impl,"tab",pressed);return;
        case XKB_KEY_Left: push_key_action(impl,"left",pressed);return;
        case XKB_KEY_Right: push_key_action(impl,"right",pressed);return;
        case XKB_KEY_Up: push_key_action(impl,"up",pressed);return;
        case XKB_KEY_Down: push_key_action(impl,"down",pressed);return;
        case XKB_KEY_Home: push_key_action(impl,"home",pressed);return;
        case XKB_KEY_End: push_key_action(impl,"end",pressed);return;
        case XKB_KEY_Delete: push_key_action(impl,"delete",pressed);return;
        case XKB_KEY_Page_Up: push_key_action(impl,"pageup",pressed);return;
        case XKB_KEY_Page_Down: push_key_action(impl,"pagedown",pressed);return;
        default: break;
    }
    // Control combinations need their printable symbol rather than an ASCII
    // control character (e.g. Ctrl+A is U+0001 from get_utf8).
    if(xkb_state_mod_name_is_active(impl.xkbState,XKB_MOD_NAME_CTRL,XKB_STATE_MODS_EFFECTIVE)>0 && sym<128){
        push_key_action(impl,std::string(1,char(sym)),pressed);return;
    }
	char utf8[64]{};
	const int bytes = xkb_state_key_get_utf8(impl.xkbState, keycode, utf8, sizeof(utf8));
	if (bytes > 0) {
		push_key_action(impl, std::string(utf8, static_cast<std::size_t>(bytes)),pressed);
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
	.keymap=keyboard_keymap,
	.enter=keyboard_enter,
	.leave=keyboard_leave,
	.key=keyboard_key,
	.modifiers=keyboard_modifiers,
	.repeat_info=keyboard_repeat_info,
};

void data_offer_offer(void* data, wl_data_offer* offer, const char* mimeType) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	if (mimeType != nullptr) {
		impl.offerMimeTypes[offer].emplace_back(mimeType);
	}
}

void data_offer_source_actions(void*, wl_data_offer*, uint32_t) {}
void data_offer_action(void*, wl_data_offer*, uint32_t) {}

constexpr wl_data_offer_listener DataOfferListener{
	.offer=data_offer_offer,
	.source_actions=data_offer_source_actions,
	.action=data_offer_action,
};

void clipboard_target(void*,wl_data_source*,const char*){}
void clipboard_send(void* data,wl_data_source*,const char* mime,int fd){
    const auto& impl=*static_cast<WaylandWindow::Impl*>(data);
    if(!mime || (std::string_view(mime)!="text/plain;charset=utf-8" && std::string_view(mime)!="text/plain")){::close(fd);return;}
    // The compositor's pipe can block: write on a worker, never the Wayland thread.
    std::thread([text=impl.clipboardText,fd]{
        sigset_t signals;sigemptyset(&signals);sigaddset(&signals,SIGPIPE);pthread_sigmask(SIG_BLOCK,&signals,nullptr);
        std::size_t offset=0;while(offset<text.size()){auto n=::write(fd,text.data()+offset,text.size()-offset);if(n>0)offset+=std::size_t(n);else if(n<0 && errno==EINTR)continue;else break;}::close(fd);
    }).detach();
}
void clipboard_canceled(void* data,wl_data_source* source){auto& impl=*static_cast<WaylandWindow::Impl*>(data);if(impl.clipboardSource==source)impl.clipboardSource=nullptr;wl_data_source_destroy(source);}
void clipboard_drop(void*,wl_data_source*){}
void clipboard_finished(void*,wl_data_source*){}
void clipboard_action(void*,wl_data_source*,uint32_t){}
const wl_data_source_listener ClipboardListener{clipboard_target,clipboard_send,clipboard_canceled,clipboard_drop,clipboard_finished,clipboard_action};

bool current_offer_has_uri_list(const WaylandWindow::Impl& impl) {
	auto found=impl.offerMimeTypes.find(impl.currentOffer);
    if(found==impl.offerMimeTypes.end())return false;
	for (const std::string& mime : found->second) {
		if (mime == "text/uri-list") {
			return true;
		}
	}
	return false;
}

void destroy_current_offer(WaylandWindow::Impl& impl) {
	if (impl.currentOffer != nullptr) {
		if(impl.currentOffer!=impl.selectionOffer){impl.offerMimeTypes.erase(impl.currentOffer);wl_data_offer_destroy(impl.currentOffer);}
		impl.currentOffer = nullptr;
	}

}

void data_device_data_offer(void* data, wl_data_device*, wl_data_offer* offer) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	if (offer != nullptr) {
        impl.offerMimeTypes.try_emplace(offer);
		wl_data_offer_add_listener(offer, &DataOfferListener, &impl);
	}
}

void data_device_enter(void* data, wl_data_device*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y, wl_data_offer* offer) {
	auto& impl = *static_cast<WaylandWindow::Impl*>(data);
	impl.pointerPosition = Point{.x=static_cast<float>(wl_fixed_to_double(x)), .y=static_cast<float>(wl_fixed_to_double(y))};
    impl.currentOffer=offer;
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
	impl.pointerPosition = Point{.x=static_cast<float>(wl_fixed_to_double(x)), .y=static_cast<float>(wl_fixed_to_double(y))};
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

void data_device_selection(void* data,wl_data_device*,wl_data_offer* offer){
    auto& impl=*static_cast<WaylandWindow::Impl*>(data);
    if(impl.selectionOffer && impl.selectionOffer!=offer && impl.selectionOffer!=impl.currentOffer){impl.offerMimeTypes.erase(impl.selectionOffer);wl_data_offer_destroy(impl.selectionOffer);}
    impl.selectionOffer=offer;
}
constexpr wl_data_device_listener DataDeviceListener{
	.data_offer=data_device_data_offer,
	.enter=data_device_enter,
	.leave=data_device_leave,
	.motion=data_device_motion,
	.drop=data_device_drop,
	.selection=data_device_selection,
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

constexpr wl_seat_listener SeatListener{.capabilities=seat_capabilities, .name=seat_name};

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
        require(impl->colorManager!=nullptr,"Wayland compositor lacks color-management-v1; cannot declare linear BT.2020");
        require(wl_display_roundtrip(impl->display)>=0,"Wayland color capability discovery failed");
        require((impl->colorFeatures&(1u<<WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)) &&
            impl->colorLinear && impl->color2020 && impl->colorPerceptual,
            "Wayland compositor does not support linear BT.2020");
        impl->colorSurface=wp_color_manager_v1_get_surface(impl->colorManager,impl->surface);
        auto* creator=wp_color_manager_v1_create_parametric_creator(impl->colorManager);
        wp_image_description_creator_params_v1_set_primaries_named(creator,WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
        wp_image_description_creator_params_v1_set_tf_named(creator,WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
        // Relative linear signal. Do not prescribe display luminance, reference
        // white brightness, mastering luminance, or content light levels.
        impl->colorDescription=wp_image_description_creator_params_v1_create(creator);
        wp_image_description_v1_add_listener(impl->colorDescription,&ColorDescriptionListener,impl);
        while(!impl->colorReady && impl->colorError.empty())
            require(wl_display_roundtrip(impl->display)>=0,"Wayland color description negotiation failed");
        if(!impl->colorReady)throw std::runtime_error("Linear BT.2020 image description rejected: "+impl->colorError);
        wp_color_management_surface_v1_set_image_description(impl->colorSurface,impl->colorDescription,
            WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
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
		WaylandWindow const cleanup(impl);
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

bool WaylandWindow::begin_move(uint32_t pointerSerial) {
    if(!impl_ || !impl_->toplevel || !impl_->seat || !pointerSerial ||
        pointerSerial!=impl_->leftPressSerial)return false;
    xdg_toplevel_move(impl_->toplevel,impl_->seat,pointerSerial);
    impl_->leftPressSerial=0;
    // EAGAIN leaves the request queued for dispatch_pending's next flush.
    require(wl_display_flush(impl_->display)>=0 || errno==EAGAIN,"Wayland move request flush failed");
    return true;
}

bool WaylandWindow::set_clipboard_text(std::string text){
    if(!impl_ || !impl_->dataDeviceManager || !impl_->dataDevice || !impl_->inputSerial)return false;
    if(impl_->clipboardSource)wl_data_source_destroy(impl_->clipboardSource);
    impl_->clipboardText=std::move(text);impl_->clipboardSource=wl_data_device_manager_create_data_source(impl_->dataDeviceManager);
    wl_data_source_add_listener(impl_->clipboardSource,&ClipboardListener,impl_);
    wl_data_source_offer(impl_->clipboardSource,"text/plain;charset=utf-8");wl_data_source_offer(impl_->clipboardSource,"text/plain");
    wl_data_device_set_selection(impl_->dataDevice,impl_->clipboardSource,impl_->inputSerial);wl_display_flush(impl_->display);return true;
}
bool WaylandWindow::request_clipboard_text(){
    if(!impl_)return false;
    if(impl_->clipboardSource){auto& action=append_action(*impl_,ActionKind::ClipboardText);action.text=impl_->clipboardText;return true;}
    if(!impl_->selectionOffer)return false;
    const auto& mimes=impl_->offerMimeTypes[impl_->selectionOffer];std::string mime;
    for(const char* candidate:{"text/plain;charset=utf-8","text/plain"})if(std::find(mimes.begin(),mimes.end(),candidate)!=mimes.end()){mime=candidate;break;}
    if(mime.empty())return false;
    if(impl_->clipboardReadFd>=0)::close(impl_->clipboardReadFd);
    impl_->clipboardReadFd=-1;impl_->clipboardReadBuffer.clear();
    int pipes[2];if(::pipe2(pipes,O_CLOEXEC)!=0)return false;
    ::fcntl(pipes[0],F_SETFL,O_NONBLOCK);impl_->clipboardReadFd=pipes[0];
    wl_data_offer_receive(impl_->selectionOffer,mime.c_str(),pipes[1]);::close(pipes[1]);wl_display_flush(impl_->display);return true;
}

void WaylandWindow::dispatch_pending() {
	if (impl_ == nullptr || impl_->display == nullptr) {
		return;
	}
	// Dispatching pending events alone never reads the socket. Coordinate with
	// other Wayland readers (including the Vulkan driver) before polling it.
	while (wl_display_prepare_read(impl_->display) != 0) {
		require(wl_display_dispatch_pending(impl_->display) >= 0, "Wayland event dispatch failed");
	}
	if (wl_display_flush(impl_->display) < 0 && errno != EAGAIN) {
		wl_display_cancel_read(impl_->display);
		throw std::runtime_error("Wayland display flush failed");
	}
	pollfd fd{.fd=wl_display_get_fd(impl_->display), .events=POLLIN, .revents=0};
	int ready;
	do {
		ready = poll(&fd, 1, 0);
	} while (ready < 0 && errno == EINTR);
	if (ready > 0 && (fd.revents & POLLIN) != 0) {
		require(wl_display_read_events(impl_->display) >= 0, "Wayland event read failed");
	} else {
		wl_display_cancel_read(impl_->display);
		require(ready >= 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0,
		        "Wayland display disconnected");
	}
	require(wl_display_dispatch_pending(impl_->display) >= 0, "Wayland event dispatch failed");
    if(impl_->clipboardReadFd>=0){
        char bytes[4096];
        for(;;){
            const auto n=::read(impl_->clipboardReadFd,bytes,sizeof(bytes));
            if(n>0 && impl_->clipboardReadBuffer.size()+std::size_t(n)<=1024*1024){impl_->clipboardReadBuffer.append(bytes,std::size_t(n));continue;}
            if(n<0 && errno==EINTR)continue;
            if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))break;
            ::close(impl_->clipboardReadFd);impl_->clipboardReadFd=-1;
            if(n==0){auto& action=append_action(*impl_,ActionKind::ClipboardText);action.text=std::move(impl_->clipboardReadBuffer);}
            impl_->clipboardReadBuffer.clear();break;
        }
    }

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
    for(auto& [offer,_]:impl_->offerMimeTypes)wl_data_offer_destroy(offer);
    impl_->offerMimeTypes.clear();impl_->selectionOffer=nullptr;
    if(impl_->clipboardSource)wl_data_source_destroy(impl_->clipboardSource);
    if(impl_->clipboardReadFd>=0)::close(impl_->clipboardReadFd);
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
    if(impl_->colorSurface)wp_color_management_surface_v1_destroy(impl_->colorSurface);
    if(impl_->colorDescription)wp_image_description_v1_destroy(impl_->colorDescription);
    if(impl_->colorManager)wp_color_manager_v1_destroy(impl_->colorManager);
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
