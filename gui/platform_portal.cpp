#include "platform_portal.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace codec_gui::gui {
namespace {

std::string percent_decode(std::string_view text) {
	std::string out;
	out.reserve(text.size());
	for (std::size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '%' && i + 2 < text.size()) {
			const std::string hex{text.substr(i + 1, 2)};
			char* end = nullptr;
			const long value = std::strtol(hex.c_str(), &end, 16);
			if (end != nullptr && *end == '\0') {
				out.push_back(static_cast<char>(value));
				i += 2;
				continue;
			}
		}
		out.push_back(text[i]);
	}
	return out;
}

} // namespace

std::filesystem::path decode_portal_file_uri(std::string_view uri) {
	const std::string prefix = "file://";
	if (uri.rfind(prefix, 0) != 0) {
		return std::filesystem::path{std::string{uri}};
	}
	std::string_view path = uri.substr(prefix.size());
	const std::string localhost = "localhost/";
	if (path.rfind(localhost, 0) == 0) {
		path.remove_prefix(localhost.size() - 1);
	} else if (!path.empty() && path.front() != '/') {
		const std::size_t slash = path.find('/');
		if (slash != std::string_view::npos) {
			path.remove_prefix(slash);
		}
	}
	return std::filesystem::path{percent_decode(path)};
}

Action action_from_open_portal_response(uint32_t response, const std::vector<std::string>& uris) {
	Action action;
	action.kind = ActionKind::OpenSourceCanceled;
	if (response == 0 && !uris.empty()) {
		action.kind = ActionKind::SourcePathChosen;
		action.path = decode_portal_file_uri(uris.front());
	}
	return action;
}

Action action_from_save_portal_response(ImageId image, uint32_t response, const std::vector<std::string>& uris) {
	Action action;
	action.kind = ActionKind::SaveCanceled;
	action.image = image;
	if (response == 0 && !uris.empty()) {
		action.kind = ActionKind::SaveEncodedResult;
		action.path = decode_portal_file_uri(uris.front());
		action.text.clear();
		action.value = 1.0;
	}
	return action;
}

Action show_open_file_portal() {
	auto connection = sdbus::createSessionBusConnection();
	auto portal = sdbus::createProxy(
		*connection,
		sdbus::ServiceName{"org.freedesktop.portal.Desktop"},
		sdbus::ObjectPath{"/org/freedesktop/portal/desktop"}
	);

	std::map<std::string, sdbus::Variant> options;
	options.emplace("modal", sdbus::Variant{true});
	options.emplace("multiple", sdbus::Variant{false});
	sdbus::ObjectPath requestPath;
	portal->callMethod("OpenFile")
		.onInterface("org.freedesktop.portal.FileChooser")
		.withArguments(std::string{}, std::string{"Open image"}, options)
		.storeResultsTo(requestPath);

	std::atomic<bool> done = false;
	Action action;
	action.kind = ActionKind::OpenSourceCanceled;
	auto request = sdbus::createProxy(
		*connection,
		sdbus::ServiceName{"org.freedesktop.portal.Desktop"},
		requestPath
	);
	request->uponSignal("Response")
		.onInterface("org.freedesktop.portal.Request")
		.call([&](uint32_t response, const std::map<std::string, sdbus::Variant>& results) {
			std::vector<std::string> uris;
			if (response == 0) {
				const auto it = results.find("uris");
				if (it != results.end()) {
					uris = it->second.get<std::vector<std::string>>();
				}
			}
			action = action_from_open_portal_response(response, uris);
			done = true;
			connection->leaveEventLoop();
		});

	connection->enterEventLoopAsync();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
	while (!done && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	connection->leaveEventLoop();
	if (!done) {
		throw std::runtime_error("file chooser did not return a response");
	}
	return action;
}

Action show_save_file_portal(ImageId image, const std::filesystem::path& suggestedPath) {
	auto connection = sdbus::createSessionBusConnection();
	auto portal = sdbus::createProxy(
		*connection,
		sdbus::ServiceName{"org.freedesktop.portal.Desktop"},
		sdbus::ObjectPath{"/org/freedesktop/portal/desktop"}
	);

	std::map<std::string, sdbus::Variant> options;
	options.emplace("modal", sdbus::Variant{true});
	if (!suggestedPath.filename().empty()) {
		options.emplace("current_name", sdbus::Variant{suggestedPath.filename().string()});
	}
	sdbus::ObjectPath requestPath;
	portal->callMethod("SaveFile")
		.onInterface("org.freedesktop.portal.FileChooser")
		.withArguments(std::string{}, std::string{"Save encoded image"}, options)
		.storeResultsTo(requestPath);

	std::atomic<bool> done = false;
	Action action;
	action.kind = ActionKind::SaveCanceled;
	action.image = image;
	auto request = sdbus::createProxy(
		*connection,
		sdbus::ServiceName{"org.freedesktop.portal.Desktop"},
		requestPath
	);
	request->uponSignal("Response")
		.onInterface("org.freedesktop.portal.Request")
		.call([&](uint32_t response, const std::map<std::string, sdbus::Variant>& results) {
			std::vector<std::string> uris;
			if (response == 0) {
				const auto it = results.find("uris");
				if (it != results.end()) {
					uris = it->second.get<std::vector<std::string>>();
				}
			}
			action = action_from_save_portal_response(image, response, uris);
			done = true;
			connection->leaveEventLoop();
		});

	connection->enterEventLoopAsync();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
	while (!done && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	connection->leaveEventLoop();
	if (!done) {
		throw std::runtime_error("save file chooser did not return a response");
	}
	return action;
}

} // namespace codec_gui::gui
