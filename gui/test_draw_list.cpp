#include "app_state.hpp"
#include "draw_list.hpp"
#include "layout.hpp"
#include "text_atlas.hpp"

#include <cassert>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

std::shared_ptr<const RawImage> make_image() {
	auto image = std::make_shared<RawImage>();
	image->width = 320;
	image->height = 240;
	image->format = PixelFormat::YUV420P8;
	image->planes[0].strideBytes = image->width;
	image->planes[1].strideBytes = image->width / 2;
	image->planes[2].strideBytes = image->width / 2;
	image->planes[0].bytes.resize(static_cast<std::size_t>(image->width * image->height));
	image->planes[1].bytes.resize(static_cast<std::size_t>((image->width / 2) * (image->height / 2)));
	image->planes[2].bytes.resize(static_cast<std::size_t>((image->width / 2) * (image->height / 2)));
	return image;
}

std::shared_ptr<const RawImage> make_422_image() {
	auto image = std::make_shared<RawImage>();
	image->width = 8;
	image->height = 8;
	image->format = PixelFormat::YUV422P8;
	image->planes[0].strideBytes = image->width;
	image->planes[1].strideBytes = image->width / 2;
	image->planes[2].strideBytes = image->width / 2;
	image->planes[0].bytes.resize(static_cast<std::size_t>(image->width * image->height), 16);
	image->planes[1].bytes.resize(static_cast<std::size_t>((image->width / 2) * image->height), 128);
	image->planes[2].bytes.resize(static_cast<std::size_t>((image->width / 2) * image->height), 128);
	image->planes[0].bytes[1 * image->planes[0].strideBytes + 1] = 42;
	image->planes[1].bytes[1 * image->planes[1].strideBytes + 0] = 111;
	image->planes[2].bytes[1 * image->planes[2].strideBytes + 0] = 222;
	return image;
}

void assert_images_are_scissored(const std::vector<DrawCommand>& commands) {
	int scissorDepth = 0;
	for (const DrawCommand& command : commands) {
		if (command.kind == DrawCommandKind::ScissorBegin) {
			++scissorDepth;
		} else if (command.kind == DrawCommandKind::ScissorEnd) {
			assert(scissorDepth > 0);
			--scissorDepth;
		} else if (command.kind == DrawCommandKind::Image) {
			assert(scissorDepth > 0);
		}
	}
	assert(scissorDepth == 0);
}

void assert_text_clips_to_rect(const DrawCommand& command, TextShaper& shaper) {
	assert(command.kind == DrawCommandKind::Text);
	TextAtlas atlas = build_text_atlas({command}, shaper, 128);
	for (const GlyphQuad& quad : atlas.quads) {
		assert(quad.rect.x >= command.rect.x - 0.01f);
		assert(quad.rect.y >= command.rect.y - 0.01f);
		assert(quad.rect.x + quad.rect.w <= command.rect.x + command.rect.w + 0.01f);
		assert(quad.rect.y + quad.rect.h <= command.rect.y + command.rect.h + 0.01f);
	}
}

} // namespace

int main() {
	TextShaper shaper(default_font_path(), 16.0f);

	AppState state;
	Action loaded;
	loaded.kind = ActionKind::SourceLoaded;
	loaded.sourceLoaded.path = "frog.jpg";
	loaded.sourceLoaded.image = make_image();
	state = update(std::move(state), loaded).state;

	ImageObject encoded;
	encoded.id = next_image_id(state);
	encoded.type = ImageObjectType::EncodedResult;
	encoded.displayName = "q35";
	encoded.width = 320;
	encoded.height = 240;
	encoded.decoded = make_image();
	EncodedMetadata metadata;
	metadata.backendName = "x265";
	metadata.bytes.resize(1536);
	metadata.byteSize = metadata.bytes.size();
	metadata.previewError = "decoder unavailable";
		metadata.metricError = "metrics unavailable because decoded preview is unavailable";
	encoded.encoded = metadata;
	state.images.push_back(encoded);
	state.selection.selectedImage = encoded.id;

	LayoutResult layout = compute_layout(state.layout, 1280, 720, 1.0f);
	ResourceSnapshot resources;
	resources.textures.push_back({state.images.front().id, TextureId{12}});
	std::vector<DrawCommand> commands = build_draw_list(state, layout, resources, 0.0);
	assert_images_are_scissored(commands);

	bool sawImageCommand = false;
	bool sawByteSize = false;
	bool sawExactByteSize = false;
	bool sawPaneAssignButton = false;
	bool sawDifferenceButton = false;
	bool sawMetricUnavailable = false;
	bool sawPreviewUnavailableReason = false;
	bool sawImageRatio = false;
	for (const DrawCommand& command : commands) {
		if (command.kind == DrawCommandKind::Image) {
			sawImageCommand = true;
		}
		if (command.kind == DrawCommandKind::Text && command.text.find("1.5 KiB") != std::string::npos) {
			sawByteSize = true;
		}
		if (command.kind == DrawCommandKind::Text && command.text.find("1536 B / 1.5 KiB") != std::string::npos) {
			sawExactByteSize = true;
		}
		if (command.kind == DrawCommandKind::Text && command.text == "1") {
			sawPaneAssignButton = true;
		}
		if (command.kind == DrawCommandKind::Text && command.text == "Diff") {
			sawDifferenceButton = true;
		}
		if (command.kind == DrawCommandKind::Text && command.text.find("Metrics unavailable") != std::string::npos) {
			sawMetricUnavailable = true;
		}
		if (command.kind == DrawCommandKind::Text && command.text.find("decoder unavailable") != std::string::npos) {
			sawPreviewUnavailableReason = true;
		}
		if (command.kind == DrawCommandKind::Text && command.text.find("Encoded/source ratio: 0.01333x") != std::string::npos) {
			sawImageRatio = true;
		}
	}
	assert(sawImageCommand);
	assert(sawByteSize);
	assert(sawExactByteSize);
	assert(sawPaneAssignButton);
	assert(sawDifferenceButton);
	assert(sawMetricUnavailable);
	assert(sawPreviewUnavailableReason);
	assert(sawImageRatio);
	assert(resolve_texture(resources, state.images.front().id).value == 12);
	assert(!valid(resolve_texture(resources, encoded.id)));

	{
		AppState byteOnly = state;
		byteOnly.images.back().decoded.reset();
		byteOnly.images.back().encoded->previewError = "preview decoder is not available";
		byteOnly.panes.front().image = byteOnly.images.back().id;
		std::vector<DrawCommand> byteOnlyCommands = build_draw_list(byteOnly, layout, resources, 0.0);
		const bool sawUnavailablePane = std::any_of(byteOnlyCommands.begin(), byteOnlyCommands.end(), [](const DrawCommand& command) {
			return command.kind == DrawCommandKind::Text && command.text.find("Preview unavailable") != std::string::npos;
		});
		assert(sawUnavailablePane);
	}

	{
		AppState yuv422;
		Action loaded422;
		loaded422.kind = ActionKind::SourceLoaded;
		loaded422.sourceLoaded.path = "422.yuv";
		loaded422.sourceLoaded.image = make_422_image();
		yuv422 = update(std::move(yuv422), loaded422).state;
		LayoutResult yuv422Layout = compute_layout(yuv422.layout, 640, 480, 1.0f);
		yuv422.panes.front().transform.scale = 1.0;
		yuv422.panes.front().transform.centerX = 0.0;
		yuv422.panes.front().transform.centerY = 0.0;
		yuv422.interaction.hoveredPane = yuv422.panes.front().id;
		yuv422.interaction.lastPointer = Point{
			yuv422Layout.viewer.x + yuv422Layout.viewer.w * 0.5f + 1.0f,
			yuv422Layout.viewer.y + yuv422Layout.viewer.h * 0.5f + 1.0f
		};
		std::vector<DrawCommand> yuv422Commands = build_draw_list(yuv422, yuv422Layout, resources, 0.0);
		const bool sawYuv422Sample = std::any_of(yuv422Commands.begin(), yuv422Commands.end(), [](const DrawCommand& command) {
			return command.kind == DrawCommandKind::Text && command.text.find("YUV422 42,111,222") != std::string::npos;
		});
		assert(sawYuv422Sample);

		ImageObject yuv422Result;
		yuv422Result.id = next_image_id(yuv422);
		yuv422Result.type = ImageObjectType::EncodedResult;
		yuv422Result.displayName = "422-result";
		yuv422Result.width = 8;
		yuv422Result.height = 8;
		yuv422Result.decoded = make_422_image();
		yuv422.images.push_back(yuv422Result);
		Action reload;
		reload.kind = ActionKind::SourceLoaded;
		reload.sourceLoaded.path = "replacement.yuv";
		reload.sourceLoaded.image = make_image();
		yuv422 = update(std::move(yuv422), reload).state;
		assert(yuv422.images.size() == 1);
		assert(yuv422.images.front().type == ImageObjectType::Source);
	}

	{
		BackendInfo backend;
		backend.id = BackendId{1};
		backend.name = "fake";
		backend.codec = "FAKE";
		backend.capabilities.implementation = "fake implementation";
		backend.capabilities.details = {"Rate Control: rate-control = cqp icq"};
		EncoderParamInfo implementation;
		implementation.name = "implementation";
		implementation.label = "Implementation";
		implementation.group = "Backend";
		implementation.kind = ParamKind::Enum;
		implementation.defaultValue = std::string{"fake"};
		implementation.enumValues = {{"fake", "Fake driver"}};
		backend.params.push_back(implementation);
		EncoderParamInfo unavailable;
		unavailable.name = "rate-control";
		unavailable.label = "Rate control";
		unavailable.group = "Backend";
		unavailable.kind = ParamKind::Enum;
		unavailable.defaultValue = std::string{"cqp"};
		backend.params.push_back(unavailable);
		state.backends.push_back(backend);
		state.selection.selectedBackend = backend.id;
		state.debug.enabled = true;
		state.debug.recent.push_back({1, "action SourceLoaded -> UploadImageTexture"});
		std::vector<DrawCommand> debugCommands = build_draw_list(state, layout, resources, 0.0);
		const bool sawDebug = std::any_of(debugCommands.begin(), debugCommands.end(), [](const DrawCommand& command) {
			return command.kind == DrawCommandKind::Text && command.text.find("Debug log") != std::string::npos;
		});
		const bool sawCapabilityDetail = std::any_of(debugCommands.begin(), debugCommands.end(), [](const DrawCommand& command) {
			return command.kind == DrawCommandKind::Text && command.text.find("rate-control = cqp icq") != std::string::npos;
		});
		const bool sawIdentityLimitation = std::any_of(debugCommands.begin(), debugCommands.end(), [](const DrawCommand& command) {
			return command.kind == DrawCommandKind::Text && command.text.find("identity only") != std::string::npos;
		});
		const bool sawUnavailableControl = std::any_of(debugCommands.begin(), debugCommands.end(), [](const DrawCommand& command) {
			return command.kind == DrawCommandKind::Text && command.text == "unavailable";
		});
		assert(sawDebug);
		assert(sawCapabilityDetail);
		assert(sawIdentityLimitation);
		assert(sawUnavailableControl);
		state.debug.enabled = false;
	}

	{
		EncodeRun run;
		run.id = next_run_id(state);
		run.source = state.images.front().id;
		run.backend = BackendId{1};
		run.state = EncodeRunState::Failed;
		run.error = "backend failed";
		state.encodeRuns.push_back(run);
		state.selection.selectedRun = run.id;
		std::vector<DrawCommand> runCommands = build_draw_list(state, layout, resources, 0.0);
		const bool sawRunDetails = std::any_of(runCommands.begin(), runCommands.end(), [](const DrawCommand& command) {
			return command.kind == DrawCommandKind::Text && command.text.find("Selected run") != std::string::npos;
		});
		const bool sawRunError = std::any_of(runCommands.begin(), runCommands.end(), [](const DrawCommand& command) {
			return command.kind == DrawCommandKind::Text && command.text.find("backend failed") != std::string::npos;
		});
		assert(sawRunDetails);
		assert(sawRunError);
	}

	{
		AppState narrow = state;
		const std::string longBackend = "vaapi Intel iHD driver for Intel(R) Gen Graphics - 26.1.5 with a deliberately long implementation label";
		const std::string longError = "driver rejected VAConfigAttribRateControl=QVBR after queried capability remap with a deliberately long technical diagnostic";
		narrow.backends.clear();
		BackendInfo backend;
		backend.id = BackendId{88};
		backend.name = longBackend;
		backend.codec = "HEVC";
		backend.capabilities.available = false;
		backend.capabilities.implementation = longBackend;
		backend.capabilities.error = longError;
		narrow.backends.push_back(backend);
		narrow.selection.selectedBackend = backend.id;
		narrow.images.clear();
		narrow.panes.clear();

		ImageObject source;
		source.id = next_image_id(narrow);
		source.type = ImageObjectType::Source;
		source.displayName = "source-frog-reference-with-long-file-name.jpg";
		source.width = 320;
		source.height = 240;
		source.decoded = make_image();
		narrow.images.push_back(source);
		Pane sourcePane;
		sourcePane.id = next_pane_id(narrow);
		sourcePane.image = source.id;
		narrow.panes.push_back(sourcePane);
		narrow.selection.activePane = sourcePane.id;

		ImageObject result;
		result.id = next_image_id(narrow);
		result.type = ImageObjectType::EncodedResult;
		result.displayName = "encoded-vaapi-hevc-q35-result-with-long-name";
		result.width = 320;
		result.height = 240;
		result.decoded = make_image();
		EncodedMetadata resultMetadata;
		resultMetadata.backendName = longBackend;
		resultMetadata.bytes.resize(1536);
		resultMetadata.byteSize = resultMetadata.bytes.size();
		resultMetadata.psnrY = 42.13;
		resultMetadata.metrics.push_back(QualityMetricRecord{"ms-ssim", "MS-SSIM", 0.98765, "", true, {}});
		result.encoded = resultMetadata;
		narrow.images.push_back(result);
		narrow.selection.selectedImage = result.id;

		EncodeRun failed;
		failed.id = next_run_id(narrow);
		failed.source = source.id;
		failed.backend = backend.id;
		failed.state = EncodeRunState::Failed;
		failed.error = longError;
		failed.producedImage = result.id;
		narrow.encodeRuns.push_back(failed);
		narrow.selection.selectedRun = failed.id;

		LayoutResult narrowLayout = compute_layout(narrow.layout, 760, 540, 1.0f);
		ResourceSnapshot narrowResources;
		std::vector<DrawCommand> narrowCommands = build_draw_list(narrow, narrowLayout, narrowResources, 0.0);
		bool sawLongBackend = false;
		bool sawLongError = false;
		bool sawCriticalBytes = false;
		bool sawCriticalPsnr = false;
		for (const DrawCommand& command : narrowCommands) {
			if (command.kind != DrawCommandKind::Text) {
				continue;
			}
			assert(command.rect.w > 0.0f);
			assert(command.rect.h > 0.0f);
			if (command.text.find(longBackend) != std::string::npos) {
				sawLongBackend = true;
				assert_text_clips_to_rect(command, shaper);
			}
			if (command.text.find(longError) != std::string::npos) {
				sawLongError = true;
				assert_text_clips_to_rect(command, shaper);
			}
			if (command.text.find("1536 B / 1.5 KiB") != std::string::npos) {
				sawCriticalBytes = true;
				assert_text_clips_to_rect(command, shaper);
			}
			if (command.text.find("MS-SSIM 0.98765") != std::string::npos || command.text.find("MS-SSIM: 0.98765") != std::string::npos) {
				sawCriticalPsnr = true;
				assert_text_clips_to_rect(command, shaper);
			}
		}
		assert(sawLongBackend);
		assert(sawLongError);
		assert(sawCriticalBytes);
		assert(sawCriticalPsnr);
	}

	{
		AppState collapsed = state;
		collapsed.layout.imageListCollapsed = true;
		collapsed.layout.inspectorCollapsed = true;
		EncodeRun active;
		active.id = next_run_id(collapsed);
		active.source = collapsed.images.front().id;
		active.backend = BackendId{1};
		active.state = EncodeRunState::Running;
		collapsed.encodeRuns.push_back(active);
		collapsed.selection.selectedRun = active.id;
		LayoutResult collapsedLayout = compute_layout(collapsed.layout, 760, 540, 1.0f);
		std::vector<DrawCommand> collapsedCommands = build_draw_list(collapsed, collapsedLayout, resources, 0.0);
		const bool sawStatusBarRun = std::any_of(collapsedCommands.begin(), collapsedCommands.end(), [](const DrawCommand& command) {
			return command.kind == DrawCommandKind::Text &&
			       command.text.find("Run ") != std::string::npos &&
			       command.text.find("running") != std::string::npos;
		});
		assert(sawStatusBarRun);
		assert(collapsedLayout.statusBar.h > 0.0f);
	}

	{
		Action duplicate;
		duplicate.kind = ActionKind::DuplicatePane;
		duplicate.pane = state.panes.front().id;
		state = update(std::move(state), duplicate).state;
		Action assign;
		assign.kind = ActionKind::AssignImageToPane;
		assign.pane = state.panes.back().id;
		assign.image = encoded.id;
		state = update(std::move(state), assign).state;
		state.viewMode.kind = ViewModeKind::Blink;
		state.viewMode.paneOrder = {state.panes.front().id, state.panes.back().id};
		resources.textures.push_back({encoded.id, TextureId{13}});
		std::vector<DrawCommand> firstBlink = build_draw_list(state, layout, resources, 0.0);
		std::vector<DrawCommand> secondBlink = build_draw_list(state, layout, resources, 0.6);
		auto blinkImage = [](const std::vector<DrawCommand>& list) {
			const auto it = std::find_if(list.begin(), list.end(), [](const DrawCommand& command) {
				return command.kind == DrawCommandKind::Image;
			});
			return it == list.end() ? ImageId{} : it->image;
		};
		assert(state.panes.front().image);
		assert(blinkImage(firstBlink) == *state.panes.front().image);
		assert(blinkImage(secondBlink) == encoded.id);
		auto blinkRect = [](const std::vector<DrawCommand>& list) {
			const auto it = std::find_if(list.begin(), list.end(), [](const DrawCommand& command) {
				return command.kind == DrawCommandKind::Image;
			});
			return it == list.end() ? Rect{} : it->rect;
		};
		const Rect firstRect = blinkRect(firstBlink);
		const Rect secondRect = blinkRect(secondBlink);
		assert(std::fabs(firstRect.x - secondRect.x) < 0.01f);
		assert(std::fabs(firstRect.y - secondRect.y) < 0.01f);
		assert(std::fabs(firstRect.w - secondRect.w) < 0.01f);
		assert(std::fabs(firstRect.h - secondRect.h) < 0.01f);
	}

	{
		AppState splitState;
		Action splitLoaded;
		splitLoaded.kind = ActionKind::SourceLoaded;
		splitLoaded.sourceLoaded.path = "source.jpg";
		splitLoaded.sourceLoaded.image = make_image();
		splitState = update(std::move(splitState), splitLoaded).state;
		ImageObject splitEncoded = encoded;
		splitEncoded.id = next_image_id(splitState);
		splitEncoded.displayName = "encoded";
		splitState.images.push_back(splitEncoded);
		Action splitMode;
		splitMode.kind = ActionKind::SetViewMode;
		splitMode.viewMode = ViewModeKind::Split;
		splitState = update(std::move(splitState), splitMode).state;
		Action splitAssign;
		splitAssign.kind = ActionKind::AssignImageToPane;
		splitAssign.pane = splitState.panes.back().id;
		splitAssign.image = splitEncoded.id;
		splitState = update(std::move(splitState), splitAssign).state;
		LayoutResult splitLayout = compute_layout(splitState.layout, 1280, 720, 1.0f);
		ResourceSnapshot splitResources;
		splitResources.textures.push_back({splitState.images.front().id, TextureId{21}});
		splitResources.textures.push_back({splitEncoded.id, TextureId{22}});
		std::vector<DrawCommand> splitCommands = build_draw_list(splitState, splitLayout, splitResources, 0.0);
		std::vector<Rect> imageRects;
		std::vector<Rect> scissors;
		for (const DrawCommand& command : splitCommands) {
			if (command.kind == DrawCommandKind::ScissorBegin) {
				scissors.push_back(command.rect);
			}
			if (command.kind == DrawCommandKind::Image) {
				imageRects.push_back(command.rect);
			}
		}
		assert(imageRects.size() >= 2);
		assert(scissors.size() >= 2);
		assert(std::fabs(imageRects[0].x - imageRects[1].x) < 0.01f);
		assert(std::fabs(imageRects[0].y - imageRects[1].y) < 0.01f);
		assert(std::fabs(imageRects[0].w - imageRects[1].w) < 0.01f);
		assert(std::fabs(imageRects[0].h - imageRects[1].h) < 0.01f);
		assert(scissors[0].w < splitLayout.viewer.w);
		assert(scissors[1].x > splitLayout.viewer.x);
	}

	return 0;
}
