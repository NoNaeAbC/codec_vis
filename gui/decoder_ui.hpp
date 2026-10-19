#pragma once
#include "app_state.hpp"
#include <algorithm>
namespace codec_gui::gui {
struct DecoderControl {
	std::string id, label;
	Rect rect;
	bool enabled = true;
	Action action;
};
inline bool decode_metrics_busy(const AppState &state, const ImageObject &image) {
	return std::any_of(state.images.begin(), state.images.end(), [&](const auto &item) {
		bool depends =
			item.id == image.id || std::find(item.parents.begin(), item.parents.end(), image.id) != item.parents.end();
		return depends && item.encoded &&
			   (item.encoded->metricState == MetricState::Running || item.encoded->metricState == MetricState::Queued);
	});
}
inline std::vector<DecoderControl> decoder_controls(const AppState &state, Rect rect) {
	std::vector<DecoderControl> out;
	if (rect.w < 160 || rect.h < 40)
		return out;
	auto add = [&](std::string id, std::string label, Rect area, bool enabled, ActionKind kind, double value = 0,
				   std::string text = {}) {
		Action a;
		a.kind = kind;
		a.image = state.selection.selectedImage;
		a.value = value;
		a.text = std::move(text);
		out.push_back({std::move(id), std::move(label), area, enabled, std::move(a)});
	};
	add("inspector:encoder", "Encoder", {rect.x + 8, rect.y + 6, rect.w / 2 - 12, 28}, true,
		ActionKind::ShowEncoderInspector);
	add("inspector:decoder", "Decoder", {rect.x + rect.w / 2 + 4, rect.y + 6, rect.w / 2 - 12, 28}, true,
		ActionKind::ShowDecoderInspector);
	if (!state.decoderInspector)
		return out;
	const auto *image = find_image(state, state.selection.selectedImage);
	if (!image)
		return out;
	const auto &d = image->decoder;
	float y = rect.y + 112 - state.layout.inspectorScrollOffset;
	for (const auto &info : d.available) {
		add("decoder:" + info.id, info.name, {rect.x + 12, y, rect.w - 24, 26}, !d.running, ActionKind::SelectDecoder,
			0, info.id);
		y += 30;
	}
	y += 12;
	add("decode:minus", "-", {rect.x + 12, y, 30, 26}, !d.running && d.repetitions > 1,
		ActionKind::SetDecodeRepetitions, d.repetitions - 1);
	add("decode:plus", "+", {rect.x + rect.w - 88, y, 30, 26}, !d.running && d.repetitions < 1000,
		ActionKind::SetDecodeRepetitions, d.repetitions + 1);
	add("decode:plus10", "+10", {rect.x + rect.w - 52, y, 40, 26}, !d.running && d.repetitions < 1000,
		ActionKind::SetDecodeRepetitions, d.repetitions + 10);
	y += 34;
	bool metricsBusy = decode_metrics_busy(state, *image);
	bool ready = !d.running && !d.querying && !d.selected.empty() && !metricsBusy;
	add("decode:once", "Decode once", {rect.x + 12, y, rect.w / 2 - 18, 28}, ready, ActionKind::StartDecode, 1);
	add("decode:measure", "Measure", {rect.x + rect.w / 2 + 6, y, rect.w / 2 - 18, 28}, ready, ActionKind::StartDecode);
	y += 36;
	add("decode:cancel", "Cancel", {rect.x + 12, y, rect.w / 2 - 18, 28}, d.running, ActionKind::CancelDecode);
	add("decode:refresh", "Refresh", {rect.x + rect.w / 2 + 6, y, rect.w / 2 - 18, 28}, !d.running && !d.querying,
		ActionKind::QueryDecoders);
	y += 36;
	add("energy:access", state.energyAccessPending ? "Cancel energy request" : state.energyAccessEnabled ? "Disable energy" : "Enable energy...",
        {rect.x + 12, y, rect.w - 24, 28}, true,
        state.energyAccessPending || state.energyAccessEnabled ? ActionKind::ReleaseEnergyAccess : ActionKind::RequestEnergyAccess);
	return out;
}
inline float decoder_content_height(const ImageObject *image) {
	if (!image)
		return 120;
	float height = 720 + float(image->decoder.available.size()) * 30;
	for (const auto &history : image->decoder.history)
		height += 430 + float(history.samples.size()) * 88;
	return height;
}
} // namespace codec_gui::gui
