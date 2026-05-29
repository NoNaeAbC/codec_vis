#include "app_state.hpp"
#include "layout.hpp"
#include "viewer_model.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

std::shared_ptr<const RawImage> make_image(int width = 640, int height = 480) {
	auto image = std::make_shared<RawImage>();
	image->width = width;
	image->height = height;
	image->format = PixelFormat::YUV420P8;
	image->planes[0].strideBytes = width;
	image->planes[1].strideBytes = width / 2;
	image->planes[2].strideBytes = width / 2;
	image->planes[0].bytes.resize(static_cast<std::size_t>(width * height));
	image->planes[1].bytes.resize(static_cast<std::size_t>((width / 2) * (height / 2)));
	image->planes[2].bytes.resize(static_cast<std::size_t>((width / 2) * (height / 2)));
	return image;
}

AppState load_source(AppState state) {
	Action action;
	action.kind = ActionKind::SourceLoaded;
	action.sourceLoaded.path = "source.jpg";
	action.sourceLoaded.image = make_image();
	return update(std::move(state), action).state;
}

} // namespace

int main() {
	{
		AppState state = load_source({});
		assert(state.images.size() == 1);
		assert(state.images.front().type == ImageObjectType::Source);
		assert(state.images.front().displayName == "source.jpg");
		assert(state.panes.size() == 1);
		assert(state.panes.front().image == state.images.front().id);
		assert(state.selection.selectedImage == state.images.front().id);
		assert(state.viewMode.kind == ViewModeKind::Single);
		assert(state.viewMode.paneOrder.size() == 1);
		assert(state.panes.front().transform.centerX == 320.0);
		assert(state.panes.front().transform.centerY == 240.0);
		assert(state.panes.front().transform.scale > 0.0);
	}

	{
		AppState stale;
		stale.nextId = 3;
		Pane staleA;
		staleA.id = next_pane_id(stale);
		Pane staleB;
		staleB.id = next_pane_id(stale);
		stale.panes = {staleA, staleB};
		stale.viewMode.kind = ViewModeKind::Split;
		stale.viewMode.paneOrder = {staleA.id, staleB.id};
		stale.selection.activePane = staleB.id;

		stale = load_source(std::move(stale));
		assert(stale.panes.size() == 1);
		assert(stale.viewMode.kind == ViewModeKind::Single);
		assert(stale.viewMode.paneOrder.size() == 1);
		assert(stale.viewMode.paneOrder.front() == stale.panes.front().id);
		assert(stale.selection.activePane == stale.panes.front().id);
		assert(stale.panes.front().image == stale.images.front().id);
	}

	{
		AppState state = load_source({});
		const ImageId sourceId = state.images.front().id;
		ImageObject encoded;
		encoded.id = next_image_id(state);
		encoded.type = ImageObjectType::EncodedResult;
		encoded.displayName = "encoded";
		encoded.parents.push_back(sourceId);
		EncodedMetadata metadata;
		metadata.bytes = {std::byte{0x01}, std::byte{0x02}};
		metadata.byteSize = metadata.bytes.size();
		encoded.encoded = metadata;
		state.images.push_back(encoded);
		EncodeRun run;
		run.id = next_run_id(state);
		run.source = sourceId;
		run.producedImage = encoded.id;
		run.state = EncodeRunState::Completed;
		state.encodeRuns.push_back(run);
		state.interaction.importPending = true;

		Action failed;
		failed.kind = ActionKind::SourceLoadFailed;
		failed.text = "decode failed";
		state = update(std::move(state), failed).state;
		assert(!state.interaction.importPending);
		assert(state.images.size() == 2);
		assert(state.images.front().id == sourceId);
		assert(state.images.back().id == encoded.id);
		assert(state.encodeRuns.size() == 1);
		assert(state.encodeRuns.front().id == run.id);
		assert(!state.errors.empty());
		assert(state.errors.back().message == "decode failed");
	}

	{
		AppState state = load_source({});
		const ImageId sourceId = state.images.front().id;
		ImageObject encoded;
		encoded.id = next_image_id(state);
		encoded.type = ImageObjectType::EncodedResult;
		encoded.displayName = "encoded";
		encoded.width = 640;
		encoded.height = 480;
		encoded.decoded = make_image();
		state.images.push_back(encoded);

		Action duplicate;
		duplicate.kind = ActionKind::DuplicatePane;
		duplicate.pane = state.panes.front().id;
		state = update(std::move(state), duplicate).state;
		assert(state.panes.size() == 2);

		Action assign;
		assign.kind = ActionKind::AssignImageToPane;
		assign.pane = state.panes.back().id;
		assign.image = encoded.id;
		UpdateResult result = update(std::move(state), assign);
		assert(result.state.panes.back().image == encoded.id);
		assert(result.state.selection.selectedImage == encoded.id);
		assert(result.state.panes.front().image == sourceId);
		assert(!result.commands.empty());
	}

	{
		AppState state = load_source({});
		std::vector<ImageId> imageIds;
		imageIds.push_back(state.images.front().id);
		for (int i = 0; i < 2; ++i) {
			ImageObject extra;
			extra.id = next_image_id(state);
			extra.type = ImageObjectType::EncodedResult;
			extra.displayName = "encoded " + std::to_string(i);
			extra.width = 640;
			extra.height = 480;
			extra.decoded = make_image();
			state.images.push_back(extra);
			imageIds.push_back(extra.id);
		}
		for (int i = 0; i < 2; ++i) {
			Action duplicate;
			duplicate.kind = ActionKind::DuplicatePane;
			duplicate.pane = state.panes.front().id;
			state = update(std::move(state), duplicate).state;
			Action assign;
			assign.kind = ActionKind::AssignImageToPane;
			assign.pane = state.panes.back().id;
			assign.image = imageIds[static_cast<std::size_t>(i + 1)];
			state = update(std::move(state), assign).state;
		}
		assert(state.panes.size() == 3);
		const PaneId thirdPane = state.panes[2].id;
		const ImageId thirdImage = *state.panes[2].image;

		Action single;
		single.kind = ActionKind::SetViewMode;
		single.viewMode = ViewModeKind::Single;
		state = update(std::move(state), single).state;
		assert(state.panes.size() == 3);
		assert(state.panes[2].id == thirdPane);
		assert(state.panes[2].image == thirdImage);
		assert(state.viewMode.paneOrder.size() == 3);
		std::vector<PaneRect> singleRects = compute_pane_rects(state.viewMode, state.panes, Rect{0, 0, 600, 400});
		assert(singleRects.size() == 1);

		Action grid;
		grid.kind = ActionKind::SetViewMode;
		grid.viewMode = ViewModeKind::Grid;
		state = update(std::move(state), grid).state;
		std::vector<PaneRect> gridRects = compute_pane_rects(state.viewMode, state.panes, Rect{0, 0, 600, 400});
		assert(gridRects.size() == 3);
		assert(state.panes[2].image == thirdImage);
	}

	{
		AppState state = load_source({});
		ImageObject encoded;
		encoded.id = next_image_id(state);
		encoded.type = ImageObjectType::EncodedResult;
		encoded.displayName = "encoded";
		encoded.width = 640;
		encoded.height = 480;
		encoded.decoded = make_image();
		state.images.push_back(encoded);

		Action create;
		create.kind = ActionKind::CreateDifferenceImage;
		create.image = state.images.front().id;
		create.otherImage = encoded.id;
		UpdateResult result = update(std::move(state), create);
		assert(result.commands.size() == 1);
		assert(result.commands.front().kind == CommandKind::ComputeDerivedImage);
		assert(result.commands.front().image == create.image);
		assert(result.commands.front().otherImage == encoded.id);
	}

	{
		AppState state = load_source({});
		state.panes.front().transform = one_to_one_transform(640, 480);
		Action split;
		split.kind = ActionKind::SetViewMode;
		split.viewMode = ViewModeKind::Split;
		state = update(std::move(state), split).state;
		assert(state.viewMode.kind == ViewModeKind::Split);
		assert(state.panes.size() == 2);
		assert(state.viewMode.paneOrder.size() == 2);
		assert(std::fabs(state.panes.front().transform.scale - 1.0) < 0.001);

		const std::vector<PaneRect> rects = compute_pane_rects(state.viewMode, state.panes, Rect{10, 20, 1000, 500});
		assert(rects.size() == 2);
		assert(std::fabs(rects[0].rect.w - 500.0f) < 0.01f);
		assert(std::fabs(rects[1].rect.x - 510.0f) < 0.01f);
	}

	{
		LayoutState layout;
		LayoutResult wide = compute_layout(layout, 1600, 900, 1.0f);
		assert(!wide.imageListCollapsed);
		assert(!wide.inspectorCollapsed);
		assert(wide.viewer.w > 900.0f);

		LayoutResult narrow = compute_layout(layout, 760, 540, 1.0f);
		assert(narrow.inspectorCollapsed);
		assert(narrow.viewer.w >= 480.0f);
	}

	{
		ImageObject image;
		image.width = 400;
		image.height = 200;
		Pane pane;
		pane.transform = fit_transform(image.width, image.height, Rect{0, 0, 800, 600}, 1.0f);
		assert(std::fabs(pane.transform.scale - 2.0) < 0.01);
		std::optional<ImagePixelCoord> coord = pane_to_image_coord(pane, image, Rect{0, 0, 800, 600}, Point{400, 300});
		assert(coord);
		assert(coord->x == 200);
		assert(coord->y == 100);
	}

	{
		AppState state = load_source({});
		state.interaction.framebufferWidth = 1280;
		state.interaction.framebufferHeight = 720;
		Action fit;
		fit.kind = ActionKind::FitPaneToImage;
		fit.pane = state.panes.front().id;
		state = update(std::move(state), fit).state;
		assert(state.panes.front().transform.scale > 0.0);

		const double before = state.panes.front().transform.scale;
		Action scroll;
		scroll.kind = ActionKind::PointerScrolled;
		scroll.point = Point{640, 360};
		scroll.value = -120.0;
		state = update(std::move(state), scroll).state;
		assert(state.panes.front().transform.scale > before);

		Action key;
		key.kind = ActionKind::KeyPressed;
		key.text = "100%";
		state = update(std::move(state), key).state;
		assert(std::fabs(state.panes.front().transform.scale - 1.0) < 0.001);
	}

	{
		AppState state = load_source({});
		Action split;
		split.kind = ActionKind::SetViewMode;
		split.viewMode = ViewModeKind::Split;
		state = update(std::move(state), split).state;
		state.interaction.framebufferWidth = 1280;
		state.interaction.framebufferHeight = 720;
		const LayoutResult layout = compute_layout(state.layout, state.interaction.framebufferWidth, state.interaction.framebufferHeight, 1.0f);
		const float splitX = layout.viewer.x + layout.viewer.w * 0.5f;

		Action press;
		press.kind = ActionKind::PointerPressed;
		press.point = Point{splitX, 360};
		state = update(std::move(state), press).state;
		assert(state.interaction.activePointerCapture == "split-divider");

		Action move;
		move.kind = ActionKind::PointerMoved;
		move.point = Point{splitX + 120, 360};
		state = update(std::move(state), move).state;
		assert(state.viewMode.splitPosition > 0.5);
	}

	{
		AppState state = load_source({});
		Action duplicate;
		duplicate.kind = ActionKind::DuplicatePane;
		duplicate.pane = state.panes.front().id;
		state = update(std::move(state), duplicate).state;
		Action linkA;
		linkA.kind = ActionKind::SetPaneLinkGroup;
		linkA.pane = state.panes[0].id;
		linkA.value = 1.0;
		state = update(std::move(state), linkA).state;
		Action linkB = linkA;
		linkB.pane = state.panes[1].id;
		state = update(std::move(state), linkB).state;
		Action transform;
		transform.kind = ActionKind::SetPaneTransform;
		transform.pane = state.panes[0].id;
		transform.transform.scale = 3.0;
		transform.transform.centerX = 20.0;
		transform.transform.centerY = 30.0;
		state = update(std::move(state), transform).state;
		assert(std::fabs(state.panes[1].transform.scale - 3.0) < 0.001);
		assert(std::fabs(state.panes[1].transform.centerX - 20.0) < 0.001);
		assert(std::fabs(state.panes[1].transform.centerY - 30.0) < 0.001);
	}

	{
		AppState state = load_source({});
		Action debug;
		debug.kind = ActionKind::KeyPressed;
		debug.text = "debug";
		UpdateResult debugResult = update(std::move(state), debug);
		assert(debugResult.state.debug.enabled);
		assert(!debugResult.state.debug.recent.empty());
		assert(debugResult.state.debug.recent.back().message.find("KeyPressed") != std::string::npos);
	}

	{
		AppState state = load_source({});
		Action blink;
		blink.kind = ActionKind::SetViewMode;
		blink.viewMode = ViewModeKind::Blink;
		state = update(std::move(state), blink).state;
		assert(state.panes.size() == 2);
		assert(compute_pane_rects(state.viewMode, state.panes, Rect{0, 0, 800, 600}).size() == 1);
	}

	{
		AppState state = load_source({});
		ImageObject encoded;
		encoded.id = next_image_id(state);
		encoded.type = ImageObjectType::EncodedResult;
		encoded.displayName = "encoded";
		encoded.width = 640;
		encoded.height = 480;
		encoded.decoded = make_image();
		state.images.push_back(encoded);
		Action blink;
		blink.kind = ActionKind::SetViewMode;
		blink.viewMode = ViewModeKind::Blink;
		state = update(std::move(state), blink).state;
		Action assign;
		assign.kind = ActionKind::AssignImageToPane;
		assign.pane = state.panes[1].id;
		assign.image = encoded.id;
		state = update(std::move(state), assign).state;
		assert(std::fabs(state.panes[0].transform.scale - state.panes[1].transform.scale) < 0.001);
		assert(std::fabs(state.panes[0].transform.centerX - state.panes[1].transform.centerX) < 0.001);
		assert(std::fabs(state.panes[0].transform.centerY - state.panes[1].transform.centerY) < 0.001);
		Action transform;
		transform.kind = ActionKind::SetPaneTransform;
		transform.pane = state.panes[0].id;
		transform.transform.scale = 3.0;
		transform.transform.centerX = 123.0;
		transform.transform.centerY = 234.0;
		state = update(std::move(state), transform).state;
		assert(std::fabs(state.panes[1].transform.scale - 3.0) < 0.001);
		assert(std::fabs(state.panes[1].transform.centerX - 123.0) < 0.001);
		assert(std::fabs(state.panes[1].transform.centerY - 234.0) < 0.001);
	}

	{
		AppState state = load_source({});
		for (int i = 0; i < 4; ++i) {
			Action duplicate;
			duplicate.kind = ActionKind::DuplicatePane;
			duplicate.pane = state.panes.front().id;
			state = update(std::move(state), duplicate).state;
		}
		Action side;
		side.kind = ActionKind::SetViewMode;
		side.viewMode = ViewModeKind::SideBySide;
		state = update(std::move(state), side).state;
		const std::vector<PaneRect> rects = compute_pane_rects(state.viewMode, state.panes, Rect{0, 0, 800, 600});
		assert(rects.size() == 5);
		assert(std::fabs(rects[0].rect.w - 200.0f) < 0.01f);
		assert(std::fabs(rects[0].rect.h - 300.0f) < 0.01f);
		assert(std::fabs(rects[4].rect.y - 300.0f) < 0.01f);
	}

	{
		AppState state = load_source({});
		ImageObject encoded;
		encoded.id = next_image_id(state);
		encoded.type = ImageObjectType::EncodedResult;
		encoded.displayName = "encoded";
		encoded.width = 640;
		encoded.height = 480;
		encoded.decoded = make_image();
		state.images.push_back(encoded);
		Action duplicate;
		duplicate.kind = ActionKind::DuplicatePane;
		duplicate.pane = state.panes.front().id;
		state = update(std::move(state), duplicate).state;
		Action side;
		side.kind = ActionKind::SetViewMode;
		side.viewMode = ViewModeKind::SideBySide;
		state = update(std::move(state), side).state;
		Action assign;
		assign.kind = ActionKind::AssignImageToPane;
		assign.pane = state.panes[1].id;
		assign.image = encoded.id;
		state = update(std::move(state), assign).state;
		assert(std::fabs(state.panes[0].transform.scale - state.panes[1].transform.scale) < 0.001);
		assert(std::fabs(state.panes[0].transform.centerX - state.panes[1].transform.centerX) < 0.001);
		assert(std::fabs(state.panes[0].transform.centerY - state.panes[1].transform.centerY) < 0.001);
		Action transform;
		transform.kind = ActionKind::SetPaneTransform;
		transform.pane = state.panes[0].id;
		transform.transform.scale = 2.5;
		transform.transform.centerX = 111.0;
		transform.transform.centerY = 222.0;
		state = update(std::move(state), transform).state;
		assert(std::fabs(state.panes[1].transform.scale - 2.5) < 0.001);
		assert(std::fabs(state.panes[1].transform.centerX - 111.0) < 0.001);
		assert(std::fabs(state.panes[1].transform.centerY - 222.0) < 0.001);
	}

	{
		AppState state = load_source({});
		const ImageId source = state.images.front().id;
		Action start;
		start.kind = ActionKind::StartEncodeRun;
		start.image = source;
		start.backend = BackendId{1};
		UpdateResult started = update(std::move(state), start);
		assert(started.state.encodeRuns.size() == 1);
		assert(started.state.selection.selectedRun == started.state.encodeRuns.front().id);
		Action running;
		running.kind = ActionKind::EncodeRunStarted;
		running.run = started.state.encodeRuns.front().id;
		running.value = 10.0;
		started = update(std::move(started.state), running);
		assert(started.state.encodeRuns.front().startedSeconds == 10.0);
		Action completed;
		completed.kind = ActionKind::EncodeRunCompleted;
		completed.encodeCompleted.run = started.state.encodeRuns.front().id;
		completed.encodeCompleted.metadata.backend = BackendId{1};
		completed.encodeCompleted.metadata.backendName = "x265 HEVC";
		completed.encodeCompleted.metadata.codecName = "HEVC";
		completed.encodeCompleted.metadata.bytes = {std::byte{0x00}, std::byte{0x01}};
		completed.value = 12.5;
		UpdateResult result = update(std::move(started.state), completed);
		assert(result.state.encodeRuns.front().state == EncodeRunState::Completed);
		assert(result.state.encodeRuns.front().finishedSeconds == 12.5);
		assert(result.state.images.size() == 2);
		assert(result.state.images.back().type == ImageObjectType::EncodedResult);
		assert(!result.state.images.back().decoded);
		assert(result.state.images.back().encoded->byteSize == 2);
	}

	{
		AppState state = load_source({});
		EncodeRun run;
		run.id = next_run_id(state);
		run.source = state.images.front().id;
		run.backend = BackendId{1};
		state.encodeRuns.push_back(run);
		Action select;
		select.kind = ActionKind::SelectEncodeRun;
		select.run = run.id;
		state = update(std::move(state), select).state;
		assert(state.selection.selectedRun == run.id);
	}

	{
		AppState state = load_source({});
		ImageObject encoded;
		encoded.id = next_image_id(state);
		encoded.type = ImageObjectType::EncodedResult;
		encoded.displayName = "encoded";
		encoded.width = 640;
		encoded.height = 480;
		encoded.decoded = make_image();
		state.images.push_back(encoded);

		Action mode;
		mode.kind = ActionKind::SetViewMode;
		mode.viewMode = ViewModeKind::Difference;
		UpdateResult modeResult = update(std::move(state), mode);
		assert(modeResult.state.panes.size() == 2);

		Action assign;
		assign.kind = ActionKind::AssignImageToPane;
		assign.pane = modeResult.state.panes.back().id;
		assign.image = encoded.id;
		UpdateResult assignResult = update(std::move(modeResult.state), assign);
		assert(std::any_of(assignResult.commands.begin(), assignResult.commands.end(), [](const Command& command) {
			return command.kind == CommandKind::ComputeDerivedImage;
		}));

		Action derived;
		derived.kind = ActionKind::DerivedImageComputed;
		derived.derivedImage.first = assignResult.state.images.front().id;
		derived.derivedImage.second = encoded.id;
		derived.derivedImage.gain = 3.5;
		derived.derivedImage.displayName = "Difference";
		derived.derivedImage.image = make_image();
		UpdateResult derivedResult = update(std::move(assignResult.state), derived);
		assert(derivedResult.state.images.back().type == ImageObjectType::Derived);
		assert(derivedResult.state.viewMode.generatedImage == derivedResult.state.images.back().id);
		assert(derivedResult.state.images.back().parents.size() == 2);
		assert(derivedResult.state.images.back().derived);
		assert(std::fabs(derivedResult.state.images.back().derived->gain - 3.5) < 0.001);
		assert(derivedResult.state.selection.selectedImage == encoded.id);

		const std::size_t imageCount = derivedResult.state.images.size();
		const ImageId generated = *derivedResult.state.viewMode.generatedImage;
		Action replacement = derived;
		replacement.derivedImage.gain = 7.0;
		replacement.derivedImage.image = make_image();
		UpdateResult replacementResult = update(std::move(derivedResult.state), replacement);
		assert(replacementResult.state.images.size() == imageCount);
		assert(replacementResult.state.viewMode.generatedImage == generated);
		const ImageObject* replaced = nullptr;
		for (const ImageObject& image : replacementResult.state.images) {
			if (image.id == generated) {
				replaced = &image;
				break;
			}
		}
		assert(replaced != nullptr);
		assert(replaced->derived);
		assert(std::fabs(replaced->derived->gain - 7.0) < 0.001);
	}

	return 0;
}
