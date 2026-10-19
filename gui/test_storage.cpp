#include "test_check.hpp"
#include "app_state.hpp"
#include "storage.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

using namespace codec_gui;
using namespace codec_gui::gui;

int main() {
	const std::filesystem::path root = std::filesystem::temp_directory_path();
	AppState state;
	state.storage.lastExportDirectory = root / "export";
	ImageObject source;
	source.id = ImageId{7};
	source.type = ImageObjectType::Source;
	source.displayName = "frog source.png";
	state.images.push_back(source);

	ImageObject image;
	image.id = ImageId{42};
	image.displayName = "frog result";
	image.parents.push_back(source.id);
	EncodedMetadata metadata;
	metadata.codecName = "AV1";
	metadata.backendName = "VAAPI Intel";
	metadata.keyParams = {{.name="qp", .value="35"}};
	image.encoded = metadata;

	const std::filesystem::path path = default_export_path(state, image);
	CODEC_VIS_CHECK(path.parent_path() == root / "export");
	CODEC_VIS_CHECK(path.filename().string() == "frog_source_VAAPI_Intel_qp-35_42.ivf");

	image.encoded->codecName = "HEVC";
	CODEC_VIS_CHECK(default_export_path(state, image).extension() == ".h265");

	image.encoded->codecName = "VVC";
	CODEC_VIS_CHECK(default_export_path(state, image).extension() == ".h266");

	image.encoded->codecName = "JPEG XS";
	CODEC_VIS_CHECK(default_export_path(state, image).extension() == ".jxs");
	image.encoded->codecName = "HEVC";
	image.encoded->backendName = "VA-API HEVC - Intel iHD driver for Intel(R) Gen Graphics [renderD128]";
	for (int i = 0; i < 40; ++i) image.encoded->keyParams.push_back({.name="parameter-" + std::to_string(i), .value="long-value"});
	state.storage.lastExportDirectory = root;
	const auto longPath = default_export_path(state, image);
	CODEC_VIS_CHECK(longPath.filename().string().size() <= 240);
	CODEC_VIS_CHECK(longPath.filename().string().ends_with("_42.h265"));
	std::ofstream exported(longPath, std::ios::binary);
	exported << "export regression";
	exported.close();
	CODEC_VIS_CHECK(exported.good());
	CODEC_VIS_CHECK(std::filesystem::file_size(longPath) == 17);
	std::filesystem::remove(longPath);
	image.id = ImageId{43};
	CODEC_VIS_CHECK(default_export_path(state, image) != longPath);

	return 0;
}
