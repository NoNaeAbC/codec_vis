#include "test_check.hpp"
#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "image_list_model.hpp"
#include "layout.hpp"
#include "ui_interaction.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

std::shared_ptr<const RawImage> make_image() {
	auto image = std::make_shared<RawImage>();
	image->width = 320;
	image->height = 240;
	image->format = PixelFormat::YUV420P8;
	return image;
}

AppState make_state() {
	AppState state;
	Action loaded;
	loaded.kind = ActionKind::SourceLoaded;
	loaded.sourceLoaded.path = "source.jpg";
	loaded.sourceLoaded.image = make_image();
	state = update(std::move(state), loaded).state;

	BackendInfo backend;
	backend.id = BackendId{1};
	backend.name = "fake";
	backend.codec = "FAKE";
	backend.capabilities.implementation = "fake implementation";
	EncoderParamInfo toggle;
	toggle.name = "tool";
	toggle.label = "Tool";
	toggle.group = "Tools";
	toggle.kind = ParamKind::Bool;
	toggle.defaultValue = false;
	backend.params.push_back(toggle);
	EncoderParamInfo mode;
	mode.name = "mode";
	mode.label = "Mode";
	mode.group = "Tools";
	mode.kind = ParamKind::Enum;
	mode.defaultValue = std::string{"a"};
	mode.enumValues = {{.value="a", .label="A"}, {.value="b", .label="B"}};
	backend.params.push_back(mode);
	EncoderParamInfo qp;
	qp.name = "qp";
	qp.label = "QP";
	qp.group = "Tools";
	qp.kind = ParamKind::Int;
	qp.defaultValue = int64_t{40};
	qp.intRange = IntRange{.min=0, .max=100, .step=10};
	backend.params.push_back(qp);
	EncoderParamInfo gain;
	gain.name = "gain";
	gain.label = "Gain";
	gain.group = "Tools";
	gain.kind = ParamKind::Float;
	gain.defaultValue = 0.5;
	gain.floatRange = FloatRange{.min=0.0, .max=1.0, .step=0.1};
	backend.params.push_back(gain);
	state.backends.push_back(backend);

	BackendInfo second;
	second.id = BackendId{2};
	second.name = "other";
	second.codec = "OTHER";
	second.capabilities.implementation = "other implementation";
	state.backends.push_back(second);
	state.selection.selectedBackend = backend.id;
	return state;
}

} // namespace

int main() {
	AppState state = make_state();
	LayoutResult const layout = compute_layout(state.layout, 1280, 720, 1.0f);

	{
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=150, .y=20});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::RequestOpenSource);
	}

	{
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=240, .y=20});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::StartEncodeRun);
		CODEC_VIS_CHECK(actions.front().image == state.images.front().id);
		CODEC_VIS_CHECK(actions.front().backend == state.backends.front().id);
		CODEC_VIS_CHECK(!actions.front().params.empty());
	}

	{
		EncoderParamInfo emptyEnum;
		emptyEnum.name = "empty-enum";
		emptyEnum.label = "Empty enum";
		emptyEnum.group = "Tools";
		emptyEnum.kind = ParamKind::Enum;
		emptyEnum.defaultValue = std::string{""};
		emptyEnum.enumValues = {{.value="", .label="None"}, {.value="value", .label="Value"}};
		state.backends.front().params.push_back(emptyEnum);
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=240, .y=20});
		CODEC_VIS_CHECK(actions.size() == 1);
		const auto empty = std::find_if(actions.front().params.begin(), actions.front().params.end(), [](const EncoderParam& param) {
			return param.name == "empty-enum";
		});
		CODEC_VIS_CHECK(empty == actions.front().params.end());
	}

	{
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=layout.imageList.x + 20, .y=layout.imageList.y + 12});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::SetImageListSort);
		CODEC_VIS_CHECK(actions.front().sortKey == ImageSortKey::EncodedSize);
	}

	{
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=layout.imageList.x + 20, .y=layout.imageList.y + ImageListHeaderHeight + 10.0f});
		CODEC_VIS_CHECK(actions.size() == 2);
		CODEC_VIS_CHECK(actions[0].kind == ActionKind::SelectImage);
		CODEC_VIS_CHECK(actions[1].kind == ActionKind::AssignImageToPane);
		CODEC_VIS_CHECK(actions[1].pane == state.selection.activePane);
	}

	{
		Action duplicate;
		duplicate.kind = ActionKind::DuplicatePane;
		duplicate.pane = state.panes.front().id;
		state = update(std::move(state), duplicate).state;
			const float rowY = layout.imageList.y + ImageListHeaderHeight + 4.0f;
			const Rect row = image_list_row_rect(layout.imageList, rowY);
			const Rect button = image_list_pane_button_rect(row, 1);
			const Point secondPaneButton{.x=button.x + 6.0f, .y=button.y + 6.0f};
		std::vector<Action> actions = actions_for_pointer_press(state, layout, secondPaneButton);
		CODEC_VIS_CHECK(actions.size() == 2);
		CODEC_VIS_CHECK(actions[0].kind == ActionKind::SelectImage);
		CODEC_VIS_CHECK(actions[1].kind == ActionKind::AssignImageToPane);
		CODEC_VIS_CHECK(actions[1].pane == state.panes[1].id);
		CODEC_VIS_CHECK(actions[1].image == state.images.front().id);
	}

	{
		EncodeRun run;
		run.id = next_run_id(state);
		run.source = state.images.front().id;
		run.backend = state.backends.front().id;
		state.encodeRuns.push_back(run);
		const float queueY = layout.inspector.y + 42.0f + 22.0f + static_cast<float>(state.backends.size()) * 24.0f + 8.0f + 22.0f + 24.0f + static_cast<float>(state.backends.front().params.size()) * 24.0f + 26.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=layout.inspector.x + 20, .y=queueY});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::SelectEncodeRun);
		CODEC_VIS_CHECK(actions.front().run == run.id);
	}

	{
		ImageObject encoded;
		encoded.id = next_image_id(state);
		encoded.type = ImageObjectType::EncodedResult;
		encoded.displayName = "encoded";
		encoded.width = 320;
		encoded.height = 240;
		encoded.decoded = make_image();
		state.images.push_back(encoded);
		state.selection.selectedImage = state.images.front().id;
			const float rowY = layout.imageList.y + ImageListHeaderHeight + 4.0f + ImageListRowStep;
			const Rect row = image_list_row_rect(layout.imageList, rowY);
			const std::size_t paneButtons = std::min<std::size_t>(state.panes.size(), 4);
			const Rect button = image_list_difference_button_rect(row, paneButtons);
			const Point diffButton{.x=button.x + 6.0f, .y=button.y + 6.0f};
		std::vector<Action> actions = actions_for_pointer_press(state, layout, diffButton);
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::CreateDifferenceImage);
		CODEC_VIS_CHECK(actions.front().image == state.images.front().id);
		CODEC_VIS_CHECK(actions.front().otherImage == encoded.id);
	}

	{
		const float backendY = layout.inspector.y + 42.0f + 22.0f + 24.0f + 6.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=layout.inspector.x + 20.0f, .y=backendY});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::SelectBackend);
		CODEC_VIS_CHECK(actions.front().backend == BackendId{2});
	}

	const float firstParamY = layout.inspector.y + 42.0f + 22.0f + static_cast<float>(state.backends.size()) * 24.0f + 8.0f + 22.0f + 24.0f + 4.0f;

	{
		const float x = layout.inspector.x + layout.inspector.w * 0.55f;
		const float y = firstParamY;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=x, .y=y});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().kind == ActionKind::SetEncoderParam);
		CODEC_VIS_CHECK(actions.front().param.name == "tool");
		CODEC_VIS_CHECK(std::get<bool>(actions.front().param.value));
	}

	{
		const float controlX = layout.inspector.x + layout.inspector.w * 0.50f;
		const float controlW = layout.inspector.w * 0.44f;
		const float enumY = firstParamY + 24.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=controlX + controlW * 0.75f, .y=enumY});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().param.name == "mode");
		CODEC_VIS_CHECK(std::get<std::string>(actions.front().param.value) == "b");
	}

	{
		const float controlX = layout.inspector.x + layout.inspector.w * 0.50f;
		const float controlW = layout.inspector.w * 0.44f;
		const float intY = firstParamY + 48.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=controlX + controlW * 0.75f, .y=intY});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().param.name == "qp");
		CODEC_VIS_CHECK(std::get<int64_t>(actions.front().param.value) == 80);
		state = update(std::move(state), actions.front()).state;
		CODEC_VIS_CHECK(state.interaction.activePointerCapture == "param:1:qp");
		std::vector<Action> dragActions = actions_for_pointer_move(state, layout, Point{.x=controlX + controlW * 0.10f, .y=intY});
		CODEC_VIS_CHECK(dragActions.size() == 1);
		CODEC_VIS_CHECK(dragActions.front().param.name == "qp");
		CODEC_VIS_CHECK(std::get<int64_t>(dragActions.front().param.value) == 10);
		Action release;
		release.kind = ActionKind::PointerReleased;
		release.point = Point{.x=controlX + controlW * 0.10f, .y=intY};
		state = update(std::move(state), release).state;
		CODEC_VIS_CHECK(state.interaction.activePointerCapture.empty());
	}

	{
		const float controlX = layout.inspector.x + layout.inspector.w * 0.50f;
		const float controlW = layout.inspector.w * 0.44f;
		const float floatY = firstParamY + 72.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{.x=controlX + controlW * 0.24f, .y=floatY});
		CODEC_VIS_CHECK(actions.size() == 1);
		CODEC_VIS_CHECK(actions.front().param.name == "gain");
		CODEC_VIS_CHECK(std::get<double>(actions.front().param.value) > 0.19);
		CODEC_VIS_CHECK(std::get<double>(actions.front().param.value) < 0.21);
	}

	{
		AppState selected = make_state();
		ImageObject source = selected.images.front();
		source.id = next_image_id(selected);
		selected.images.push_back(source);
		selected.selection.selectedImage = source.id;
		auto actions = actions_for_pointer_press(selected, layout, {.x=240, .y=20});
		CODEC_VIS_CHECK(actions.size() == 1 && actions[0].image == source.id);
		ImageObject result;
		result.id = next_image_id(selected);
		result.type = ImageObjectType::EncodedResult;
		result.parents = {source.id};
		result.encoded.emplace();
		selected.images.push_back(result);
		selected.selection.selectedImage = result.id;
		actions = actions_for_pointer_press(selected, layout, {.x=240, .y=20});
		CODEC_VIS_CHECK(actions.size() == 1 && actions[0].image == source.id);
		actions = actions_for_pointer_press(selected, layout, {.x=410, .y=20});
		CODEC_VIS_CHECK(actions.size() == 1 && actions[0].image == result.id);
		selected.selection.selectedImage = source.id;
		CODEC_VIS_CHECK(actions_for_pointer_press(selected, layout, {.x=410, .y=20}).empty());
		for (int i = 0; i < 2; ++i) {
			EncodeRun run;
			run.id = next_run_id(selected);
			selected.encodeRuns.push_back(run);
		}
		selected.selection.selectedRun = selected.encodeRuns.back().id;
		actions = actions_for_pointer_press(selected, layout, {.x=330, .y=20});
		CODEC_VIS_CHECK(actions.size() == 1 && actions[0].run == selected.encodeRuns.back().id);
		Action cancel = actions[0];
		auto canceled = update(std::move(selected), cancel);
		CODEC_VIS_CHECK(canceled.state.encodeRuns.back().state == EncodeRunState::CancelRequested);
		CODEC_VIS_CHECK(canceled.commands.size() == 1 && canceled.commands[0].kind == CommandKind::RequestEncodeCancel);
		Action lateStart;
		lateStart.kind = ActionKind::EncodeRunStarted;
		lateStart.run = cancel.run;
		auto late = update(std::move(canceled.state), lateStart);
		CODEC_VIS_CHECK(late.state.encodeRuns.back().state == EncodeRunState::CancelRequested);
		const auto imageCount = late.state.images.size();
		Action lateCompletion;
		lateCompletion.kind = ActionKind::EncodeRunCompleted;
		lateCompletion.encodeCompleted.run = cancel.run;
		auto finished = update(std::move(late.state), lateCompletion);
		CODEC_VIS_CHECK(finished.state.encodeRuns.back().state == EncodeRunState::Canceled);
		CODEC_VIS_CHECK(finished.state.images.size() == imageCount);
	}
	return 0;
}
