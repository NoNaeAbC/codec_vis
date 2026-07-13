#include "app_state.hpp"
#include "layout.hpp"
#include "ui_interaction.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
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
	mode.enumValues = {{"a", "A"}, {"b", "B"}};
	backend.params.push_back(mode);
	EncoderParamInfo qp;
	qp.name = "qp";
	qp.label = "QP";
	qp.group = "Tools";
	qp.kind = ParamKind::Int;
	qp.defaultValue = int64_t{40};
	qp.intRange = IntRange{0, 100, 10};
	backend.params.push_back(qp);
	EncoderParamInfo gain;
	gain.name = "gain";
	gain.label = "Gain";
	gain.group = "Tools";
	gain.kind = ParamKind::Float;
	gain.defaultValue = 0.5;
	gain.floatRange = FloatRange{0.0, 1.0, 0.1};
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
	LayoutResult layout = compute_layout(state.layout, 1280, 720, 1.0f);

	{
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{150, 20});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::RequestOpenSource);
	}

	{
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{240, 20});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::StartEncodeRun);
		assert(actions.front().image == state.images.front().id);
		assert(actions.front().backend == state.backends.front().id);
		assert(!actions.front().params.empty());
	}

	{
		EncoderParamInfo emptyEnum;
		emptyEnum.name = "empty-enum";
		emptyEnum.label = "Empty enum";
		emptyEnum.group = "Tools";
		emptyEnum.kind = ParamKind::Enum;
		emptyEnum.defaultValue = std::string{""};
		emptyEnum.enumValues = {{"", "None"}, {"value", "Value"}};
		state.backends.front().params.push_back(emptyEnum);
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{240, 20});
		assert(actions.size() == 1);
		const auto empty = std::find_if(actions.front().params.begin(), actions.front().params.end(), [](const EncoderParam& param) {
			return param.name == "empty-enum";
		});
		assert(empty == actions.front().params.end());
	}

	{
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{layout.imageList.x + 20, layout.imageList.y + 12});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::SetImageListSort);
		assert(actions.front().sortKey == ImageSortKey::EncodedSize);
	}

	{
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{layout.imageList.x + 20, layout.imageList.y + 50});
		assert(actions.size() == 2);
		assert(actions[0].kind == ActionKind::SelectImage);
		assert(actions[1].kind == ActionKind::AssignImageToPane);
		assert(actions[1].pane == state.selection.activePane);
	}

	{
		Action duplicate;
		duplicate.kind = ActionKind::DuplicatePane;
		duplicate.pane = state.panes.front().id;
		state = update(std::move(state), duplicate).state;
			const Rect row{layout.imageList.x + 6, layout.imageList.y + 42 - 2, layout.imageList.w - 12, 50};
			const Point secondPaneButton{row.x + row.w - 76.0f + 6.0f, row.y + 26.0f + 6.0f};
		std::vector<Action> actions = actions_for_pointer_press(state, layout, secondPaneButton);
		assert(actions.size() == 2);
		assert(actions[0].kind == ActionKind::SelectImage);
		assert(actions[1].kind == ActionKind::AssignImageToPane);
		assert(actions[1].pane == state.panes[1].id);
		assert(actions[1].image == state.images.front().id);
	}

	{
		EncodeRun run;
		run.id = next_run_id(state);
		run.source = state.images.front().id;
		run.backend = state.backends.front().id;
		state.encodeRuns.push_back(run);
		const float queueY = layout.inspector.y + 42.0f + 22.0f + static_cast<float>(state.backends.size()) * 24.0f + 8.0f + 22.0f + 24.0f + static_cast<float>(state.backends.front().params.size()) * 24.0f + 26.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{layout.inspector.x + 20, queueY});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::SelectEncodeRun);
		assert(actions.front().run == run.id);
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
			const float rowY = layout.imageList.y + 42 + 56;
			const Rect row{layout.imageList.x + 6, rowY - 2, layout.imageList.w - 12, 50};
			const std::size_t paneButtons = std::min<std::size_t>(state.panes.size(), 4);
			const Point diffButton{row.x + row.w - 46.0f * static_cast<float>(paneButtons + 1u) + 6.0f, row.y + 32.0f};
		std::vector<Action> actions = actions_for_pointer_press(state, layout, diffButton);
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::CreateDifferenceImage);
		assert(actions.front().image == state.images.front().id);
		assert(actions.front().otherImage == encoded.id);
	}

	{
		const float backendY = layout.inspector.y + 42.0f + 22.0f + 24.0f + 6.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{layout.inspector.x + 20.0f, backendY});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::SelectBackend);
		assert(actions.front().backend == BackendId{2});
	}

	const float firstParamY = layout.inspector.y + 42.0f + 22.0f + static_cast<float>(state.backends.size()) * 24.0f + 8.0f + 22.0f + 24.0f + 4.0f;

	{
		const float x = layout.inspector.x + layout.inspector.w * 0.55f;
		const float y = firstParamY;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{x, y});
		assert(actions.size() == 1);
		assert(actions.front().kind == ActionKind::SetEncoderParam);
		assert(actions.front().param.name == "tool");
		assert(std::get<bool>(actions.front().param.value));
	}

	{
		const float controlX = layout.inspector.x + layout.inspector.w * 0.50f;
		const float controlW = layout.inspector.w * 0.44f;
		const float enumY = firstParamY + 24.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{controlX + controlW * 0.75f, enumY});
		assert(actions.size() == 1);
		assert(actions.front().param.name == "mode");
		assert(std::get<std::string>(actions.front().param.value) == "b");
	}

	{
		const float controlX = layout.inspector.x + layout.inspector.w * 0.50f;
		const float controlW = layout.inspector.w * 0.44f;
		const float intY = firstParamY + 48.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{controlX + controlW * 0.75f, intY});
		assert(actions.size() == 1);
		assert(actions.front().param.name == "qp");
		assert(std::get<int64_t>(actions.front().param.value) == 80);
		state = update(std::move(state), actions.front()).state;
		assert(state.interaction.activePointerCapture == "param:1:qp");
		std::vector<Action> dragActions = actions_for_pointer_move(state, layout, Point{controlX + controlW * 0.10f, intY});
		assert(dragActions.size() == 1);
		assert(dragActions.front().param.name == "qp");
		assert(std::get<int64_t>(dragActions.front().param.value) == 10);
		Action release;
		release.kind = ActionKind::PointerReleased;
		release.point = Point{controlX + controlW * 0.10f, intY};
		state = update(std::move(state), release).state;
		assert(state.interaction.activePointerCapture.empty());
	}

	{
		const float controlX = layout.inspector.x + layout.inspector.w * 0.50f;
		const float controlW = layout.inspector.w * 0.44f;
		const float floatY = firstParamY + 72.0f;
		std::vector<Action> actions = actions_for_pointer_press(state, layout, Point{controlX + controlW * 0.24f, floatY});
		assert(actions.size() == 1);
		assert(actions.front().param.name == "gain");
		assert(std::get<double>(actions.front().param.value) > 0.19);
		assert(std::get<double>(actions.front().param.value) < 0.21);
	}

	return 0;
}
