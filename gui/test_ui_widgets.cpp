#include "app_state.hpp"
#include "layout.hpp"
#include "ui_widgets.hpp"

#include <algorithm>
#include <cassert>
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
	EncoderParamInfo name;
	name.name = "preset-name";
	name.label = "Preset";
	name.group = "General";
	name.kind = ParamKind::String;
	name.defaultValue = std::string{"frog"};
	backend.params.push_back(name);
	EncoderParamInfo mode;
	mode.name = "rate-control";
	mode.label = "Rate control";
	mode.group = "Rate Control";
	mode.kind = ParamKind::Enum;
	mode.defaultValue = std::string{"crf"};
	mode.enumValues = {{"qp", "QP"}, {"crf", "CRF"}};
	backend.params.push_back(mode);
	EncoderParamInfo qp;
	qp.name = "qp";
	qp.label = "QP";
	qp.group = "Rate Control";
	qp.kind = ParamKind::Int;
	qp.defaultValue = int64_t{22};
	qp.intRange = IntRange{0, 51, 1};
	qp.enabledWhen = {{"rate-control", {"qp"}, "QP mode only"}};
	backend.params.push_back(qp);
	state.backends.push_back(backend);
	state.selection.selectedBackend = backend.id;
	return state;
}

const WidgetInfo* find_widget(const std::vector<WidgetInfo>& widgets, const std::string& id) {
	const auto it = std::find_if(widgets.begin(), widgets.end(), [&](const WidgetInfo& widget) {
		return widget.id == id;
	});
	return it == widgets.end() ? nullptr : &*it;
}

} // namespace

int main() {
	AppState state = make_state();
	LayoutResult layout = compute_layout(state.layout, 1920, 1080, 1.0f);
	state.interaction.lastPointer = Point{150, 20};
	state.interaction.focusedWidget = "param:1:preset-name";

	const std::vector<WidgetInfo> widgets = collect_widgets(state, layout);
	const WidgetInfo* import = find_widget(widgets, "command:import");
	assert(import != nullptr);
	assert(import->kind == WidgetKind::Button);
	assert(import->hovered);

	const WidgetInfo* text = find_widget(widgets, "param:1:preset-name");
	assert(text != nullptr);
	assert(text->kind == WidgetKind::TextInput);
	assert(text->focused);
	assert(text->enabled);

	const WidgetInfo* backend = find_widget(widgets, "backend:1");
	assert(backend != nullptr);
	assert(backend->kind == WidgetKind::Button);
	assert(backend->enabled);
	const WidgetInfo* disabledQp = find_widget(widgets, "param:1:qp");
	assert(disabledQp != nullptr);
	assert(!disabledQp->enabled);
	Action selectQpMode;
	selectQpMode.kind = ActionKind::SetEncoderParam;
	selectQpMode.backend = BackendId{1};
	selectQpMode.param = {"rate-control", std::string{"qp"}};
	state = update(std::move(state), selectQpMode).state;
	const std::vector<WidgetInfo> updatedWidgets = collect_widgets(state, layout);
	const WidgetInfo* enabledQp = find_widget(updatedWidgets, "param:1:qp");
	assert(enabledQp != nullptr && enabledQp->enabled);

	Action focus;
	focus.kind = ActionKind::SetEncoderParam;
	focus.backend = BackendId{1};
	focus.param.name = "preset-name";
	focus.param.value = std::string{"frog"};
	state = update(std::move(state), focus).state;

	Action typed;
	typed.kind = ActionKind::KeyPressed;
	typed.text = "x";
	state = update(std::move(state), typed).state;
	assert(state.encoderConfigs.size() == 1);
	auto presetValue = [&]() -> const EncoderParam& {
		const auto it = std::find_if(state.encoderConfigs.front().params.begin(), state.encoderConfigs.front().params.end(), [](const EncoderParam& param) { return param.name == "preset-name"; });
		assert(it != state.encoderConfigs.front().params.end());
		return *it;
	};
	assert(std::get<std::string>(presetValue().value) == "frogx");

	Action backspace;
	backspace.kind = ActionKind::KeyPressed;
	backspace.text = "backspace";
	state = update(std::move(state), backspace).state;
	assert(std::get<std::string>(presetValue().value) == "frog");

	return 0;
}
