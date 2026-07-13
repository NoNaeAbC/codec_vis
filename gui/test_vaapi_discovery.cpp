#include "encoder_backends.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <set>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

std::size_t hardware_codec_count(std::span<const EncoderBackend> backends, std::string_view codec) {
	return static_cast<std::size_t>(std::count_if(backends.begin(), backends.end(), [&](const EncoderBackend& backend) {
		return backend.kind == BackendKind::Hardware && backend.codec == codec;
	}));
}

} // namespace

int main() {
	const std::vector<EncoderBackend> noDevices = initial_encoder_backends(std::span<const VaapiDeviceInfo>{});
	const EncoderBackend* noHevc = find_backend(noDevices, BackendId{4});
	const EncoderBackend* noAv1 = find_backend(noDevices, BackendId{5});
	assert(noHevc != nullptr && noAv1 != nullptr);
	assert(!noHevc->queryCapabilities().snapshot.available);
	assert(!noAv1->queryCapabilities().snapshot.available);
	assert(noHevc->queryCapabilities().snapshot.error.find("No DRM render nodes") != std::string::npos);

	const std::vector<VaapiDeviceInfo> syntheticDevices{
		{.path = "/dev/dri/renderD128", .vendor = "HEVC-only driver", .error = {}, .initialized = true, .supportsHevcEncode = true},
		{.path = "/dev/dri/renderD129", .vendor = "AV1-only driver", .error = {}, .initialized = true, .supportsAv1Encode = true},
		{.path = "/dev/dri/renderD130", .vendor = "multi-codec driver", .error = {}, .initialized = true, .supportsHevcEncode = true, .supportsAv1Encode = true},
		{.path = "/dev/dri/renderD131", .vendor = {}, .error = "driver initialization failed"},
	};
	const std::vector<EncoderBackend> multiple = initial_encoder_backends(syntheticDevices);
	assert(hardware_codec_count(multiple, "HEVC") == 2);
	assert(hardware_codec_count(multiple, "AV1") == 2);
	assert(find_backend(multiple, BackendId{4}) != nullptr);
	assert(find_backend(multiple, BackendId{5}) != nullptr);
	assert(std::any_of(multiple.begin(), multiple.end(), [](const EncoderBackend& backend) {
		return backend.name.find("HEVC-only driver") != std::string::npos && backend.name.find("renderD128") != std::string::npos;
	}));
	assert(std::any_of(multiple.begin(), multiple.end(), [](const EncoderBackend& backend) {
		return backend.name.find("AV1-only driver") != std::string::npos && backend.name.find("renderD129") != std::string::npos;
	}));
	std::set<uint64_t> ids;
	for (const EncoderBackend& backend : multiple) assert(ids.insert(backend.id.value).second);

	const std::vector<EncoderBackend> hevcOnly = initial_encoder_backends(std::span<const VaapiDeviceInfo>{syntheticDevices.data(), 1});
	const EncoderBackend* missingAv1 = find_backend(hevcOnly, BackendId{5});
	assert(missingAv1 != nullptr && !missingAv1->queryCapabilities().snapshot.available);
	assert(missingAv1->queryCapabilities().snapshot.error.find("No usable VA-API AV1") != std::string::npos);

	const std::vector<std::string> missingPaths{"/definitely/missing/renderD128", "/definitely/missing/renderD129"};
	const std::vector<VaapiDeviceInfo> failed = probe_vaapi_devices(missingPaths);
	assert(failed.size() == missingPaths.size());
	for (std::size_t i = 0; i < failed.size(); ++i) {
		assert(failed[i].path == missingPaths[i]);
		assert(!failed[i].initialized);
		assert(!failed[i].error.empty());
	}
	const auto descriptor_count = [] {
		return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{}));
	};
	const std::size_t descriptorsBefore = descriptor_count();
	const std::vector<std::string> nonDrmPath{"/dev/null"};
	const std::vector<VaapiDeviceInfo> noDriver = probe_vaapi_devices(nonDrmPath);
	assert(noDriver.size() == 1 && !noDriver.front().initialized && !noDriver.front().error.empty());
	assert(descriptor_count() == descriptorsBefore);

	for (const VaapiDeviceInfo& device : discover_vaapi_devices()) {
		std::cout << device.path << " | " << (device.initialized ? device.vendor : device.error)
		          << " | HEVC=" << device.supportsHevcEncode << " AV1=" << device.supportsAv1Encode << '\n';
	}
	return 0;
}
