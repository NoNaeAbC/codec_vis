#include "test_check.hpp"
#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "layout.hpp"
#include "ui_widgets.hpp"

#include <algorithm>
#include <cassert>
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
	mode.enumValues = {{.value="qp", .label="QP"}, {.value="crf", .label="CRF"}};
	backend.params.push_back(mode);
	EncoderParamInfo qp;
	qp.name = "qp";
	qp.label = "QP";
	qp.group = "Rate Control";
	qp.kind = ParamKind::Int;
	qp.defaultValue = int64_t{22};
	qp.intRange = IntRange{.min=0, .max=51, .step=1};
	qp.enabledWhen = {{.parameter="rate-control", .acceptedValues={"qp"}, .explanation="QP mode only"}};
	backend.params.push_back(qp);
	EncoderParamInfo transformSkip;
	transformSkip.name = "TS";
	transformSkip.label = "Transform skip";
	transformSkip.group = "Transform";
	transformSkip.kind = ParamKind::Bool;
	transformSkip.defaultValue = false;
	backend.params.push_back(transformSkip);
	EncoderParamInfo bdpcm;
	bdpcm.name = "BDPCM";
	bdpcm.label = "BDPCM";
	bdpcm.group = "Screen Content";
	bdpcm.kind = ParamKind::Bool;
	bdpcm.defaultValue = false;
	bdpcm.enabledWhen = {{.parameter="TS", .acceptedValues={"true"}, .explanation="BDPCM requires transform skip."}};
	backend.params.push_back(bdpcm);
	state.backends.push_back(backend);
	state.selection.selectedBackend = backend.id;
	return state;
}

const WidgetInfo* find_widget(const std::vector<WidgetInfo>& widgets, const std::string& id) {
	const auto it = std::ranges::find_if(widgets, [&](const WidgetInfo& widget) {
		return widget.id == id;
	});
	return it == widgets.end() ? nullptr : &*it;
}

} // namespace

int main() {
	AppState state = make_state();
	LayoutResult const layout = compute_layout(state.layout, 1920, 1080, 1.0f);
	state.interaction.lastPointer = Point{.x=150, .y=20};
	state.interaction.focusedWidget = "param:1:preset-name";

	const std::vector<WidgetInfo> widgets = collect_widgets(state, layout);
	const WidgetInfo* import = find_widget(widgets, "command:import");
	CODEC_VIS_CHECK(import != nullptr);
	CODEC_VIS_CHECK(import->kind == WidgetKind::Button);
	CODEC_VIS_CHECK(import->hovered);

	const WidgetInfo* text = find_widget(widgets, "param:1:preset-name");
	CODEC_VIS_CHECK(text != nullptr);
	CODEC_VIS_CHECK(text->kind == WidgetKind::TextInput);
	CODEC_VIS_CHECK(text->focused);
	CODEC_VIS_CHECK(text->enabled);

	const WidgetInfo* backend = find_widget(widgets, "backend:1");
	CODEC_VIS_CHECK(backend != nullptr);
	CODEC_VIS_CHECK(backend->kind == WidgetKind::Button);
	CODEC_VIS_CHECK(backend->enabled);
	const WidgetInfo* disabledQp = find_widget(widgets, "param:1:qp");
	CODEC_VIS_CHECK(disabledQp != nullptr);
	CODEC_VIS_CHECK(!disabledQp->enabled);
	const WidgetInfo* disabledBdpcm = find_widget(widgets, "param:1:BDPCM");
	CODEC_VIS_CHECK(disabledBdpcm != nullptr);
	CODEC_VIS_CHECK(!disabledBdpcm->enabled);
	Action selectQpMode;
	selectQpMode.kind = ActionKind::SetEncoderParam;
	selectQpMode.backend = BackendId{1};
	selectQpMode.param = {.name="rate-control", .value=std::string{"qp"}};
	state = update(std::move(state), selectQpMode).state;
	const std::vector<WidgetInfo> updatedWidgets = collect_widgets(state, layout);
	const WidgetInfo* enabledQp = find_widget(updatedWidgets, "param:1:qp");
	CODEC_VIS_CHECK(enabledQp != nullptr && enabledQp->enabled);
	Action enableTransformSkip;
	enableTransformSkip.kind = ActionKind::SetEncoderParam;
	enableTransformSkip.backend = BackendId{1};
	enableTransformSkip.param = {.name="TS", .value=true};
	state = update(std::move(state), enableTransformSkip).state;
	const std::vector<WidgetInfo> transformSkipWidgets = collect_widgets(state, layout);
	const WidgetInfo* enabledBdpcm = find_widget(transformSkipWidgets, "param:1:BDPCM");
	CODEC_VIS_CHECK(enabledBdpcm != nullptr && enabledBdpcm->enabled);

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
	CODEC_VIS_CHECK(state.encoderConfigs.size() == 1);
	auto presetValue = [&]() -> const EncoderParam& {
		const auto it = std::find_if(state.encoderConfigs.front().params.begin(), state.encoderConfigs.front().params.end(), [](const EncoderParam& param) { return param.name == "preset-name"; });
		CODEC_VIS_CHECK(it != state.encoderConfigs.front().params.end());
		return *it;
	};
	CODEC_VIS_CHECK(std::get<std::string>(presetValue().value) == "frogx");

	Action backspace;
	backspace.kind = ActionKind::KeyPressed;
	backspace.text = "backspace";
	state = update(std::move(state), backspace).state;
	CODEC_VIS_CHECK(std::get<std::string>(presetValue().value) == "frog");

	return 0;
}
