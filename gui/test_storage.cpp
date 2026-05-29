#include "storage.hpp"

#include <cassert>
#include <filesystem>

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
	metadata.keyParams = {{"qp", "35"}};
	image.encoded = metadata;

	const std::filesystem::path path = default_export_path(state, image);
	assert(path.parent_path() == root / "export");
	assert(path.filename().string() == "frog_source_VAAPI_Intel_qp-35_42.ivf");

	image.encoded->codecName = "HEVC";
	assert(default_export_path(state, image).extension() == ".h265");

	image.encoded->codecName = "VVC";
	assert(default_export_path(state, image).extension() == ".h266");

	return 0;
}
