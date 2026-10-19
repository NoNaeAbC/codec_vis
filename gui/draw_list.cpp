#include "decoder_ui.hpp"
#include "command_controls.hpp"
#include "draw_list.hpp"

#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "image_list_model.hpp"
#include "encoder_param_state.hpp"
#include "layout.hpp"
#include "metrics.hpp"
#include "raw_image_utils.hpp"
#include "ui_widgets.hpp"
#include "viewer_model.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace codec_gui::gui {
namespace {

constexpr Color PanelBg{.r=0.08f, .g=0.085f, .b=0.09f, .a=1.0f};
constexpr Color ViewerBg{.r=0.02f, .g=0.022f, .b=0.025f, .a=1.0f};
constexpr Color BorderColor{.r=0.22f, .g=0.23f, .b=0.25f, .a=1.0f};
constexpr Color TextColor{.r=0.88f, .g=0.9f, .b=0.92f, .a=1.0f};
constexpr Color MutedTextColor{.r=0.58f, .g=0.61f, .b=0.64f, .a=1.0f};
constexpr Color AccentColor{.r=0.25f, .g=0.48f, .b=0.85f, .a=1.0f};
constexpr Color FocusColor{.r=0.95f, .g=0.72f, .b=0.24f, .a=1.0f};
constexpr Color HoverColor{.r=0.16f, .g=0.17f, .b=0.18f, .a=0.55f};
constexpr Color ActiveColor{.r=0.20f, .g=0.25f, .b=0.34f, .a=0.75f};
constexpr Color DisabledColor{.r=0.10f, .g=0.10f, .b=0.105f, .a=0.55f};


void push_rect(std::vector<DrawCommand>& out, Rect rect, Color color) {
	DrawCommand command;
	command.kind = DrawCommandKind::Rect;
	command.rect = rect;
	command.color = color;
	out.push_back(std::move(command));
}

void push_border(std::vector<DrawCommand>& out, Rect rect, Color color) {
	DrawCommand command;
	command.kind = DrawCommandKind::Border;
	command.rect = rect;
	command.color = color;
	out.push_back(std::move(command));
}

void push_text(std::vector<DrawCommand>& out, Rect rect, std::string text, Color color = TextColor) {
	DrawCommand command;
	command.kind = DrawCommandKind::Text;
	command.rect = rect;
	command.color = color;
	command.text = std::move(text);
	out.push_back(std::move(command));
}

void push_scissor(std::vector<DrawCommand>& out, DrawCommandKind kind, Rect rect) {
	DrawCommand command;
	command.kind = kind;
	command.rect = rect;
	out.push_back(std::move(command));
}

bool contains(Rect rect, Point point) {
	return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.w && point.y < rect.y + rect.h;
}

std::vector<std::string> wrap_tooltip(std::string_view text, std::size_t columns = 68) {
	std::vector<std::string> lines;
	std::string line;
	std::istringstream words{std::string{text}};
	std::string word;
	while (words >> word) {
		if (!line.empty() && line.size() + 1 + word.size() > columns) {
			lines.push_back(std::move(line));
			line.clear();
		}
		if (!line.empty()) line += ' ';
		line += word;
	}
	if (!line.empty()) lines.push_back(std::move(line));
	return lines;
}

std::string format_bytes(uint64_t bytes) {
	constexpr double KiB = 1024.0;
	constexpr double MiB = 1024.0 * 1024.0;
	std::ostringstream oss;
	oss.setf(std::ios::fixed);
	if (bytes >= static_cast<uint64_t>(MiB)) {
		oss.precision(2);
		oss << static_cast<double>(bytes) / MiB << " MiB";
	} else if (bytes >= static_cast<uint64_t>(KiB)) {
		oss.precision(1);
		oss << static_cast<double>(bytes) / KiB << " KiB";
	} else {
		oss << bytes << " B";
	}
	return oss.str();
}

std::string format_bytes_both(uint64_t bytes) {
	return std::to_string(bytes) + " B / " + format_bytes(bytes);
}

uint64_t raw_image_bytes(const RawImage& image) {
	uint64_t bytes = 0;
	for (const ImagePlane& plane : image.planes) {
		bytes += static_cast<uint64_t>(plane.bytes.size());
	}
	return bytes;
}

std::string format_file_raw_sizes(uint64_t fileBytes, uint64_t rawBytes) {
	constexpr double KiB = 1024.0;
	constexpr double MiB = 1024.0 * 1024.0;
	std::ostringstream oss;
	oss.setf(std::ios::fixed);
	if (fileBytes >= static_cast<uint64_t>(MiB) && rawBytes >= static_cast<uint64_t>(MiB)) {
		oss.precision(2);
		oss << "F/U " << static_cast<double>(fileBytes) / MiB << "/" << static_cast<double>(rawBytes) / MiB << " MiB";
	} else if (fileBytes >= static_cast<uint64_t>(KiB) && rawBytes >= static_cast<uint64_t>(KiB)) {
		oss.precision(1);
		oss << "F/U " << static_cast<double>(fileBytes) / KiB << "/" << static_cast<double>(rawBytes) / KiB << " KiB";
	} else {
		oss << "F/U " << fileBytes << "/" << rawBytes << " B";
	}
	return oss.str();
}

std::string format_encoded_source_ratio(uint64_t encodedBytes, uint64_t sourceBytes) {
	if (sourceBytes == 0) {
		return "";
	}
	std::ostringstream oss;
	oss << std::fixed << std::setprecision(5) << (static_cast<double>(encodedBytes) / static_cast<double>(sourceBytes));
	return oss.str();
}

std::string format_bpp(uint64_t encodedBytes, int width, int height) {
	if (width <= 0 || height <= 0) {
		return "";
	}
	std::ostringstream oss;
	oss << std::fixed << std::setprecision(3)
	    << (8.0 * static_cast<double>(encodedBytes) / static_cast<double>(width * height))
	    << " bpp";
	return oss.str();
}

std::string format_metric_value(const QualityMetricRecord& metric) {
	if (!metric.value) {
		return metric.label + " unavailable";
	}
	std::ostringstream oss;
	oss << metric.label << ' ';
	if (std::isinf(*metric.value)) {
		oss << "inf";
	} else {
		oss << std::fixed << std::setprecision(metric.id == "ssim" || metric.id == "ms-ssim" ? 5 : 2) << *metric.value;
	}
	if (!metric.unit.empty()) {
		oss << ' ' << metric.unit;
	}
	return oss.str();
}

std::string primary_metric_text(const EncodedMetadata& metadata) {
	if (const QualityMetricRecord* metric = primary_metric(metadata)) {
		return format_metric_value(*metric);
	}
	if (!metadata.metricError.empty()) {
		return "metric unavailable";
	}
	return "";
}

std::string metric_state_text(MetricState state) {
	switch(state) {
		case MetricState::NotRequested:return "not requested (click Metrics)";
		case MetricState::Queued:return "queued";
		case MetricState::Running:return "computing";
		case MetricState::Ready:return "ready";
		case MetricState::Failed:return "failed (click Metrics to retry)";
	}
	return {};
}

Rect backend_row_rect(Rect inspector, float y) {
	return Rect{.x=inspector.x + 12.0f, .y=y, .w=inspector.w - 24.0f, .h=22.0f};
}

uint64_t decoded_buffer_size(const ImageObject& image) {
	if (!image.decoded) {
		return 0;
	}
	uint64_t bytes = 0;
	for (const ImagePlane& plane : image.decoded->planes) {
		bytes += plane.bytes.size();
	}
	return bytes;
}

uint64_t source_decoded_size(const AppState& state) {
	for (const ImageObject& image : state.images) {
		if (image.type == ImageObjectType::Source) {
			return decoded_buffer_size(image);
		}
	}
	return 0;
}

const ImageObject* image_by_id(const AppState& state, ImageId id) {
	return find_image(state, id);
}

const BackendInfo* selected_backend(const AppState& state) {
	if (valid(state.selection.selectedBackend)) {
		const auto selected = std::ranges::find_if(state.backends, [&](const BackendInfo& backend) {
			return backend.id == state.selection.selectedBackend;
		});
		if (selected != state.backends.end()) {
			return &*selected;
		}
	}
	return state.backends.empty() ? nullptr : &state.backends.front();
}

const BackendInfo* backend_by_id(const AppState& state, BackendId id) {
	const auto it = std::ranges::find_if(state.backends, [id](const BackendInfo& backend) {
		return backend.id == id;
	});
	return it == state.backends.end() ? nullptr : &*it;
}

std::string backend_display_name(const BackendInfo& backend) {
	if (backend.codec.empty() || backend.name.find(backend.codec) != std::string::npos) {
		return backend.name;
	}
	return backend.name + " / " + backend.codec;
}

void draw_scrollbar(std::vector<DrawCommand>& out, Rect viewport, float contentHeight, float offset) {
	if (viewport.h <= 0.0f || contentHeight <= viewport.h + 1.0f) {
		return;
	}
	const float trackX = viewport.x + viewport.w - 5.0f;
	const float thumbH = std::max(28.0f, viewport.h * viewport.h / contentHeight);
	const float maximum = std::max(1.0f, contentHeight - viewport.h);
	const float travel = std::max(0.0f, viewport.h - thumbH);
	const float thumbY = viewport.y + travel * std::clamp(offset / maximum, 0.0f, 1.0f);
	push_rect(out, Rect{.x=trackX, .y=viewport.y + 2.0f, .w=2.0f, .h=viewport.h - 4.0f}, BorderColor);
	push_rect(out, Rect{.x=trackX - 1.0f, .y=thumbY, .w=4.0f, .h=thumbH}, MutedTextColor);
}

const EncodeRun* run_by_id(const AppState& state, EncodeRunId id) {
	const auto it = std::ranges::find_if(state.encodeRuns, [id](const EncodeRun& run) {
		return run.id == id;
	});
	return it == state.encodeRuns.end() ? nullptr : &*it;
}

ParamValue current_param_value(const AppState& state, BackendId backend, const EncoderParamInfo& param) {
	const auto config = std::ranges::find_if(state.encoderConfigs, [backend](const EncoderConfig& cfg) {
		return cfg.backend == backend;
	});
	if (config != state.encoderConfigs.end()) {
		const auto existing = std::find_if(config->params.begin(), config->params.end(), [&](const EncoderParam& value) {
			return value.name == param.name;
		});
		if (existing != config->params.end()) {
			return existing->value;
		}
	}
	return std::visit([](const auto& value) -> ParamValue {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, std::monostate>) {
			return std::string{};
		} else {
			return value;
		}
	}, param.defaultValue);
}

std::string encode_run_state_text(EncodeRunState state) {
	switch (state) {
		case EncodeRunState::Queued: return "queued";
		case EncodeRunState::Running: return "running";
		case EncodeRunState::Completed: return "completed";
		case EncodeRunState::Failed: return "failed";
		case EncodeRunState::Canceled: return "canceled";
		case EncodeRunState::CancelRequested: return "cancel requested";
	}
	return "unknown";
}

const EncodeRun* active_encode_run(const AppState& state) {
	const auto selected = std::ranges::find_if(state.encodeRuns, [&](const EncodeRun& run) {
		return run.id == state.selection.selectedRun &&
		       (run.state == EncodeRunState::Queued || run.state == EncodeRunState::Running || run.state == EncodeRunState::CancelRequested);
	});
	if (selected != state.encodeRuns.end()) {
		return &*selected;
	}
	const auto active = std::ranges::find_if(state.encodeRuns, [](const EncodeRun& run) {
		return run.state == EncodeRunState::Running || run.state == EncodeRunState::CancelRequested || run.state == EncodeRunState::Queued;
	});
	return active == state.encodeRuns.end() ? nullptr : &*active;
}

std::string pane_label(const AppState& state, PaneId id) {
	const auto it = std::ranges::find_if(state.panes, [id](const Pane& pane) {
		return pane.id == id;
	});
	if (it == state.panes.end()) {
		return "Pane";
	}
	return "Pane " + std::to_string(static_cast<int>(std::distance(state.panes.begin(), it)) + 1);
}

bool command_active(const AppState& state, std::string_view name) {
	if (name == "scratch") return state.scratchResults;
	if (name == "single") return state.viewMode.kind == ViewModeKind::Single;
	if (name == "side") return state.viewMode.kind == ViewModeKind::SideBySide;
	if (name == "split") return state.viewMode.kind == ViewModeKind::Split;
	if (name == "blink") return state.viewMode.kind == ViewModeKind::Blink;
	if (name == "diff") return state.viewMode.kind == ViewModeKind::Difference;
	if (name == "grid") return state.viewMode.kind == ViewModeKind::Grid;
	return false;
}

std::string param_value_to_string(const ParamValue& value) {
	return std::visit(
		[](const auto& v) -> std::string {
			using T = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<T, bool>) {
				return v ? "on" : "off";
			} else if constexpr (std::is_same_v<T, int64_t>) {
				return std::to_string(v);
			} else if constexpr (std::is_same_v<T, double>) {
				std::ostringstream oss;
				oss.precision(6);
				oss << v;
				return oss.str();
			} else {
				return v;
			}
		},
		value
	);
}

std::optional<uint32_t> pixel_sample_at(const ImagePlane& plane, int x, int y, int sampleBytes) {
	if (x < 0 || y < 0 || sampleBytes <= 0 || plane.strideBytes <= 0) {
		return std::nullopt;
	}
	const std::size_t byte =
		static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.strideBytes) +
		static_cast<std::size_t>(x) * static_cast<std::size_t>(sampleBytes);
	if (byte + static_cast<std::size_t>(sampleBytes) > plane.bytes.size()) {
		return std::nullopt;
	}
	if (sampleBytes == 1) {
		return plane.bytes[byte];
	}
	return static_cast<uint32_t>(plane.bytes[byte]) |
	       (static_cast<uint32_t>(plane.bytes[byte + 1]) << 8u);
}

const char* pixel_format_label(PixelFormat format) {
	switch (format) {
		case PixelFormat::RGBP8: return "RGB-8";
		case PixelFormat::RGBP14LE: return "RGB-14";
		case PixelFormat::RGBP16LE: return "RGB-16";
		case PixelFormat::YUV420P8: return "YUV420";
		case PixelFormat::YUV420P10LE: return "YUV420-10";
		case PixelFormat::YUV420P12LE: return "YUV420-12";
		case PixelFormat::YUV420P14LE: return "YUV420-14";
		case PixelFormat::YUV422P8: return "YUV422";
		case PixelFormat::YUV422P10LE: return "YUV422-10";
		case PixelFormat::YUV422P12LE: return "YUV422-12";
		case PixelFormat::YUV422P14LE: return "YUV422-14";
		case PixelFormat::YUV444P8: return "YUV444";
		case PixelFormat::YUV444P10LE: return "YUV444-10";
		case PixelFormat::YUV444P12LE: return "YUV444-12";
		case PixelFormat::YUV444P14LE: return "YUV444-14";
		case PixelFormat::YUV444P16LE: return "YUV444-16";
		case PixelFormat::Gray8: return "Gray";
		case PixelFormat::Gray10LE: return "Gray10";
		case PixelFormat::Gray12LE: return "Gray12";
		case PixelFormat::Gray14LE: return "Gray14";
	}
	return "";
}

std::string format_pixel_sample(const ImageObject& image, ImagePixelCoord coord) {
	if (!image.decoded) {
		return "";
	}
	const RawImage& raw = *image.decoded;
	std::ostringstream oss;
	const int sampleBytes = bytes_per_sample(raw.format);
	const std::optional<uint32_t> y = pixel_sample_at(raw.planes[0], coord.x, coord.y, sampleBytes);
	if (!y) {
		return "";
	}
	if (is_gray(raw.format)) {
		oss << pixel_format_label(raw.format) << ' ' << *y;
		return oss.str();
	}
	const int chromaX = (is_420(raw.format) || is_422(raw.format)) ? coord.x / 2 : coord.x;
	const int chromaY = is_420(raw.format) ? coord.y / 2 : coord.y;
	const uint32_t neutral = 1u << (bit_depth(raw.format) - 1);
	const uint32_t u = pixel_sample_at(raw.planes[1], chromaX, chromaY, sampleBytes).value_or(neutral);
	const uint32_t v = pixel_sample_at(raw.planes[2], chromaX, chromaY, sampleBytes).value_or(neutral);
	oss << pixel_format_label(raw.format) << ' ' << *y << ',' << u << ',' << v;
	return oss.str();
}

std::optional<int> luma_sample_value(const ImageObject& image, ImagePixelCoord coord) {
	if (!image.decoded) {
		return std::nullopt;
	}
	const RawImage& raw = *image.decoded;
	if (std::optional<uint32_t> sample = pixel_sample_at(raw.planes[0], coord.x, coord.y, bytes_per_sample(raw.format))) {
		return static_cast<int>(*sample);
	}
	return std::nullopt;
}

void draw_pane_image(std::vector<DrawCommand>& out, const ResourceSnapshot& resources, const Pane& pane, const ImageObject& image, Rect clipRect, Rect transformRect) {
	push_scissor(out, DrawCommandKind::ScissorBegin, clipRect);
	DrawCommand command;
	command.kind = DrawCommandKind::Image;
	command.rect = image_rect_in_pane(pane, image, transformRect);
	command.image = image.id;
	command.texture = resolve_texture(resources, image.id);
	command.color = Color{.r=1, .g=1, .b=1, .a=1};
	out.push_back(std::move(command));
	if (!resources.textures.empty() && !valid(out.back().texture)) {
		push_text(out, Rect{.x=clipRect.x + 12, .y=clipRect.y + 12, .w=clipRect.w - 24, .h=20}, "Texture pending", MutedTextColor);
	}
	push_scissor(out, DrawCommandKind::ScissorEnd, clipRect);
}

void draw_pane_unavailable(std::vector<DrawCommand>& out, const ImageObject& image, Rect rect) {
	push_scissor(out, DrawCommandKind::ScissorBegin, rect);
	push_rect(out, rect, Color{.r=0.04f, .g=0.045f, .b=0.05f, .a=1.0f});
	if (image.encoded && !image.encoded->previewError.empty()) {
		push_text(out, Rect{.x=rect.x + 12, .y=rect.y + 36, .w=rect.w - 24, .h=20}, "Preview unavailable: " + image.encoded->previewError, Color{.r=0.95f, .g=0.72f, .b=0.35f, .a=1.0f});
	} else {
		push_text(out, Rect{.x=rect.x + 12, .y=rect.y + 36, .w=rect.w - 24, .h=20}, "Preview unavailable", MutedTextColor);
	}
	if (image.encoded) {
		push_text(out, Rect{.x=rect.x + 12, .y=rect.y + 60, .w=rect.w - 24, .h=20}, "Encoded size: " + format_bytes_both(image.encoded->byteSize), MutedTextColor);
	}
	push_scissor(out, DrawCommandKind::ScissorEnd, rect);
}

void draw_image_list(std::vector<DrawCommand>& out, const AppState& state, Rect rect) {
	push_rect(out, rect, PanelBg);
	push_border(out, rect, BorderColor);
	push_text(out, Rect{.x=rect.x + 12, .y=rect.y + 7, .w=rect.w - 24, .h=20}, "Results", TextColor);
	std::string sort = "Sort: ";
	sort += state.imageList.sortKey == ImageSortKey::Metric ? std::string(comparison_metric_id(state)) : sort_key_label(state.imageList.sortKey);
	sort += state.imageList.ascending ? " up" : " down";
	sort += "  / click";
	push_text(out, Rect{.x=rect.x + 12, .y=rect.y + 27, .w=rect.w - 24, .h=18}, std::move(sort), MutedTextColor);

	const Rect content{.x=rect.x, .y=rect.y + ImageListHeaderHeight, .w=rect.w, .h=std::max(0.0f, rect.h - ImageListHeaderHeight)};
	push_scissor(out, DrawCommandKind::ScissorBegin, content);
	float y = rect.y + ImageListHeaderHeight + 4.0f - state.imageList.scrollOffset;
	if (state.images.empty()) {
		push_text(out, Rect{.x=rect.x + 12, .y=y + 4.0f, .w=rect.w - 24, .h=18}, "No images yet", MutedTextColor);
	}
	for (const ImageObject* imagePtr : ordered_images(state)) {
		const ImageObject& image = *imagePtr;
		const Rect row = image_list_row_rect(rect, y);
		const bool selected = image.id == state.selection.selectedImage;
		if (selected) {
			push_rect(out, row, AccentColor);
		} else {
			push_rect(out, row, Color{.r=0.10f, .g=0.105f, .b=0.115f, .a=0.72f});
		}
		push_border(out, row, selected ? AccentColor : BorderColor);
		push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, image.displayName, TextColor);
		std::string detail;
		if (image.encoded) {
			detail = format_bytes(image.encoded->byteSize);
			if(image.encoded->metricState!=MetricState::Ready && image.encoded->metrics.empty())
				detail += image.encoded->metricState==MetricState::Running?"   metrics computing":
				          image.encoded->metricState==MetricState::Queued?"   metrics queued":
				          image.encoded->metricState==MetricState::Failed?"   metrics failed":"   metrics on demand";
			const std::string metric = primary_metric_text(*image.encoded);
			if (!metric.empty()) {
				detail += "   " + metric;
			}
		} else {
			detail = std::to_string(image.width) + "x" + std::to_string(image.height);
		}
		if (!image.encoded && image.type == ImageObjectType::Source && image.sourceByteSize) {
			if (image.decoded) {
				detail += "  ";
				detail += format_file_raw_sizes(*image.sourceByteSize, raw_image_bytes(*image.decoded));
			} else {
				detail += "  file ";
				detail += format_bytes(*image.sourceByteSize);
			}
		}
		const std::size_t paneButtons = std::min<std::size_t>(state.panes.size(), 4);
		push_text(out, Rect{.x=rect.x + 12, .y=y + 20, .w=rect.w - 24.0f, .h=16}, std::move(detail), selected ? TextColor : MutedTextColor);
		push_text(out, Rect{.x=rect.x + 12, .y=y + 46, .w=64, .h=16}, "Assign", selected ? TextColor : MutedTextColor);
		if (valid(state.selection.selectedImage) && state.selection.selectedImage != image.id) {
			const Rect diffButton = image_list_difference_button_rect(row, paneButtons);
			push_rect(out, diffButton, Color{.r=0.13f, .g=0.12f, .b=0.18f, .a=1.0f});
			push_border(out, diffButton, BorderColor);
			push_text(out, Rect{.x=diffButton.x + 6.0f, .y=diffButton.y + 2.0f, .w=diffButton.w - 12.0f, .h=diffButton.h - 4.0f}, "Diff", MutedTextColor);
		}
		for (std::size_t i = 0; i < paneButtons; ++i) {
			const Pane& pane = state.panes[i];
			const Rect button = image_list_pane_button_rect(row, i);
			const bool assigned = pane.image && *pane.image == image.id;
			push_rect(out, button, assigned ? Color{.r=0.18f, .g=0.34f, .b=0.25f, .a=1.0f} : Color{.r=0.11f, .g=0.12f, .b=0.13f, .a=1.0f});
			push_border(out, button, BorderColor);
			push_text(out, Rect{.x=button.x + 5.0f, .y=button.y + 2.0f, .w=button.w - 10.0f, .h=button.h - 4.0f}, "P" + std::to_string(i + 1), assigned ? TextColor : MutedTextColor);
		}
		y += ImageListRowStep;
	}
	push_scissor(out, DrawCommandKind::ScissorEnd, content);
	const float contentHeight = 4.0f + ImageListRowStep * static_cast<float>(state.images.size());
	draw_scrollbar(out, content, contentHeight, state.imageList.scrollOffset);
}

void draw_run_details(std::vector<DrawCommand>& out, const AppState& state, const EncodeRun& run, Rect rect, float& y) {
	if (y + 120 >= rect.y + rect.h) {
		return;
	}
	push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=20}, "Selected run", TextColor);
	y += 22;
	std::string backendName = "backend " + std::to_string(run.backend.value);
	if (const BackendInfo* backend = backend_by_id(state, run.backend)) {
		backendName = backend_display_name(*backend);
	}
	push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, backendName, MutedTextColor);
	y += 20;
	push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "State: " + encode_run_state_text(run.state), run.state == EncodeRunState::Failed ? Color{.r=0.95f, .g=0.45f, .b=0.35f, .a=1.0f} : MutedTextColor);
	y += 20;
	if (!run.params.empty() && y + 18 < rect.y + rect.h) {
		std::string params = "Params:";
		for (const EncoderParam& param : run.params) {
			if (params.size() > 96) {
				params += " ...";
				break;
			}
			params += " " + param.name + "=" + param_value_to_string(param.value);
		}
		push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, std::move(params), MutedTextColor);
		y += 20;
	}
	if (!run.error.empty() && y + 18 < rect.y + rect.h) {
		for (const std::string& line : wrap_tooltip("Error: " + run.error, std::max<std::size_t>(24, static_cast<std::size_t>((rect.w - 24) / 9)))) {
			if (y + 18 >= rect.y + rect.h) break;
			push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, line, Color{.r=0.95f, .g=0.45f, .b=0.35f, .a=1.0f});
			y += 20;
		}
	}
	if (run.startedSeconds > 0 && run.finishedSeconds > run.startedSeconds && y + 18 < rect.y + rect.h) {
		std::ostringstream duration;
		duration << std::fixed << std::setprecision(3) << (run.finishedSeconds - run.startedSeconds) << " s";
		push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Total run time: " + duration.str(), MutedTextColor);
		y += 20;
	}
	if (run.producedImage) {
		const ImageObject* image = image_by_id(state, *run.producedImage);
		if (image != nullptr && image->encoded && y + 18 < rect.y + rect.h) {
			std::string output = "Result: " + format_bytes_both(image->encoded->byteSize);
			const std::string metric = primary_metric_text(*image->encoded);
			if (!metric.empty()) {
				output += "  " + metric;
			}
			push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, std::move(output), TextColor);
			y += 20;
			if (image->encoded->encodeSeconds > 0.0 && y + 18 < rect.y + rect.h) {
				std::ostringstream encodedDuration;
				encodedDuration << std::fixed << std::setprecision(3) << image->encoded->encodeSeconds << " s";
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Encode duration: " + encodedDuration.str(), MutedTextColor);
				y += 20;
			}
			if (image->encoded->decodeSeconds > 0.0 && y + 18 < rect.y + rect.h) {
				std::ostringstream duration;
				duration << std::fixed << std::setprecision(3) << image->encoded->decodeSeconds << " s";
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Decode duration: " + duration.str(), MutedTextColor);
				y += 20;
			}
			if (image->encoded->metricSeconds > 0.0 && y + 18 < rect.y + rect.h) {
				std::ostringstream duration;
				duration << std::fixed << std::setprecision(3) << image->encoded->metricSeconds << " s";
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Metric duration: " + duration.str(), MutedTextColor);
				y += 20;
			}
			if (!image->encoded->outputPath.empty() && y + 18 < rect.y + rect.h) {
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Output: " + image->encoded->outputPath.string(), MutedTextColor);
				y += 20;
			}
			if (!image->encoded->metricError.empty() && y + 18 < rect.y + rect.h) {
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Metrics unavailable: " + image->encoded->metricError, Color{.r=0.95f, .g=0.72f, .b=0.35f, .a=1.0f});
				y += 20;
			}
			if (!image->encoded->previewError.empty() && y + 18 < rect.y + rect.h) {
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Preview unavailable: " + image->encoded->previewError, Color{.r=0.95f, .g=0.72f, .b=0.35f, .a=1.0f});
				y += 20;
			}
		}
	}
}

void draw_queue_panel(std::vector<DrawCommand>& out, const AppState& state, Rect rect, float& y) {
	// Header plus an actual two-line row must fit in full.
	if (state.encodeRuns.empty() || y + 62 > rect.y + rect.h) {
		return;
	}
	push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=20}, "Encode queue", TextColor);
	y += 22;
	std::size_t shown = 0;
	for (auto it = state.encodeRuns.rbegin(); it != state.encodeRuns.rend() && shown < 4; ++it) {
		const EncodeRun& run = *it;
		if (y + 36 > rect.y + rect.h) {
			break;
		}
		const Rect row{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=36};
		if (run.id == state.selection.selectedRun) {
			push_rect(out, row, ActiveColor);
			push_border(out, row, AccentColor);
		}
		std::string title = "Run " + std::to_string(run.id.value);
		if (const BackendInfo* backend = backend_by_id(state, run.backend)) {
			title += "  " + backend->name;
		}
		push_text(out, Rect{.x=row.x + 6, .y=row.y + 2, .w=row.w - 12, .h=16}, std::move(title), TextColor);
		std::string detail = encode_run_state_text(run.state);
		const ImageObject* result = run.producedImage ? image_by_id(state, *run.producedImage) : nullptr;
		if (result && result->encoded && result->encoded->encodeSeconds > 0.0) {
			std::ostringstream duration;
			duration << std::fixed << std::setprecision(3) << result->encoded->encodeSeconds << " s";
			detail += "  encode " + duration.str();
		} else if (run.startedSeconds > 0 && run.finishedSeconds > run.startedSeconds) {
			std::ostringstream duration;
			duration << std::fixed << std::setprecision(3) << (run.finishedSeconds - run.startedSeconds) << " s";
			detail += "  total " + duration.str();
		}
		if (run.producedImage) {
			detail += "  result #" + std::to_string(run.producedImage->value);
		}
		push_text(out, Rect{.x=row.x + 6, .y=row.y + 18, .w=row.w - 12, .h=16}, std::move(detail), MutedTextColor);
		y += 40;
		++shown;
	}
	y += 6;
}

void draw_widget_state(std::vector<DrawCommand>& out, const AppState& state, const LayoutResult& layout) {
	const std::vector<WidgetInfo> widgets = collect_widgets(state, layout);
	for (const WidgetInfo& widget : widgets) {
		if (widget.rect.w <= 0.0f || widget.rect.h <= 0.0f) {
			continue;
		}
		if (widget.kind == WidgetKind::PaneView) {
			continue;
		}
		if (!widget.enabled) {
			push_rect(out, widget.rect, DisabledColor);
			continue;
		}
		if (widget.active) {
			push_rect(out, widget.rect, ActiveColor);
		} else if (widget.hovered) {
			push_rect(out, widget.rect, HoverColor);
		}
		if (widget.focused) {
			push_border(out, widget.rect, FocusColor);
		}
	}
}

void draw_decoder_inspector(std::vector<DrawCommand>& out,const AppState& state,Rect rect) {
    const Rect content{rect.x,rect.y+40,rect.w,std::max(0.0f,rect.h-40)};
    push_scissor(out,DrawCommandKind::ScissorBegin,content);
    const auto* image=find_image(state,state.selection.selectedImage);
    float y=rect.y+44-state.layout.inspectorScrollOffset;
    auto text=[&](const std::string& label,float height=22,Color color=TextColor){
        const auto lines=wrap_tooltip(label,std::max<std::size_t>(20,std::size_t((rect.w-24)/9)));
        const float start=y;
        for(const auto& line:lines){if(y+22>=content.y && y<content.y+content.h)push_text(out,{rect.x+12,y,rect.w-24,22},line,color);y+=22;}
        y=std::max(y,start+height);
    };
    if(!image)text("Select an image to decode.");
    else {
        const auto& d=image->decoder;
        text(image->displayName);
        text(d.querying?"Checking decoders against this bitstream...":d.error.empty()?"Compatible decoders for this image":d.error,44,MutedTextColor);
        for(const auto& control:decoder_controls(state,rect)) {
            if(control.id.starts_with("inspector:"))continue;
            if(control.rect.y<content.y)continue;
            const bool selected=control.action.kind==ActionKind::SelectDecoder && control.action.text==d.selected;
            push_rect(out,control.rect,selected?ActiveColor:PanelBg);push_border(out,control.rect,selected?AccentColor:BorderColor);
            push_text(out,{control.rect.x+5,control.rect.y+4,control.rect.w-10,20},control.label,control.enabled?TextColor:MutedTextColor);
        }
        y=rect.y+124+float(d.available.size())*30-state.layout.inspectorScrollOffset;
        push_text(out,{rect.x+48,y+4,rect.w-145,20},std::to_string(d.repetitions)+" repetitions",TextColor);
        y+=146;
        if(state.energyAccessPending)text("Authorize counter access in the system authentication dialog. The GUI stays unprivileged.",88,MutedTextColor);
        else if(state.energyAccessEnabled)text("Energy enabled for this session: "+state.energyAccessDomain+". Disabling affects subsequent measurements.",88,MutedTextColor);
        else text(state.energyAccessError.empty()?"Energy is disabled. Enable energy to authorize read-only RAPL access for this application session. Timing works without it.":state.energyAccessError,110,MutedTextColor);
        text("No idle subtraction. Domain energy includes other system activity; coverage depends on firmware. It is not wall power or decoder-only energy.",110,MutedTextColor);
        text("Codec time excludes setup and readback (libavcodec). Total includes fresh initialization, GPU wait, CPU readback and packing. Metrics and display excluded.",88,MutedTextColor);
        if(d.running)text("Running; cancellation takes effect after this run.",44);
        text("Every attempt is retained, including the first. Mean, sample SD and standard error (SE); no outlier removal.",66,MutedTextColor);
        for(auto it=d.history.rbegin();it!=d.history.rend();++it) {
            const auto& h=*it;std::vector<double> times,codecTimes,energies;double energyTime=0,totalEnergy=0;
            for(const auto& sample:h.samples) {if(sample.error.empty()){times.push_back(sample.seconds);if(sample.codecSeconds)codecTimes.push_back(*sample.codecSeconds);}if(sample.joules){energies.push_back(*sample.joules);totalEnergy+=*sample.joules;energyTime+=sample.energySeconds;}}
            text(h.decoderName.empty()?h.decoderId:h.decoderName);
            text(std::to_string(h.samples.size())+" / "+std::to_string(h.requested)+" attempts"+(h.canceled?" (canceled)":h.finished?" (finished)":" (running)"));
            auto statText=[&](const std::vector<double>& values,double scale,const std::string& unit) {
                const auto stats=sample_statistics(values);std::ostringstream line;line<<std::fixed<<std::setprecision(3)<<"Mean "<<stats.mean*scale<<unit;
                if(stats.standardDeviation)line<<"; SD "<<*stats.standardDeviation*scale<<"; SE "<<*stats.standardError*scale;else line<<"; uncertainty needs 2 runs";
                text(line.str(),44);
            };
            if(!codecTimes.empty()){text("Codec work (GPU completion included)");statText(codecTimes,1000," ms");}
            if(!times.empty()){text("Completed CPU image (total)");statText(times,1000," ms");}
            if(times.size()!=h.samples.size())text("Successful-decode mean only; failed attempts shown below.",44,MutedTextColor);
            if(!energies.empty()) {
                text("Energy: "+h.energyDomain,44);statText(energies,1," J");
                if(energyTime>0){std::ostringstream line;line<<"Domain average power: "<<totalEnergy/energyTime<<" W";text(line.str());}
            }
            text(h.energyNote.empty()?"Energy counter not read yet.":h.energyNote,88,MutedTextColor);
            for(std::size_t i=0;i<h.samples.size();++i) {
                const auto& sample=h.samples[i];std::ostringstream line;line<<i+1<<": "<<std::fixed<<std::setprecision(3)<<sample.seconds*1000<<" ms";
                if(sample.codecSeconds)line<<" total; codec "<<*sample.codecSeconds*1000<<" ms";
                if(sample.joules)line<<", "<<*sample.joules<<" J over "<<sample.energySeconds<<" s";
                if(!sample.error.empty())line<<" FAILED: "<<sample.error;
                text(line.str(),sample.error.empty()?44:88,sample.error.empty()?TextColor:MutedTextColor);
            }
            y+=16;
        }
    }
    push_scissor(out,DrawCommandKind::ScissorEnd,content);
    draw_scrollbar(out,content,std::max(content.h,y+state.layout.inspectorScrollOffset-content.y+12),state.layout.inspectorScrollOffset);
}

void draw_inspector(std::vector<DrawCommand>& out, const AppState& state, Rect rect) {
    if(rect.w<=0 || rect.h<=0)return;
    if(rect.w<160 && state.decoderInspector) {
        push_rect(out,rect,PanelBg);push_border(out,rect,BorderColor);
        if(rect.w>16)push_text(out,{rect.x+8,rect.y+10,rect.w-16,22},"Decoder",TextColor);
        return;
    }
	push_rect(out, rect, PanelBg);
	push_border(out, rect, BorderColor);
    for(const auto& control:decoder_controls(state,rect)) if(control.id.starts_with("inspector:")) {
        const bool selected=(control.action.kind==ActionKind::ShowDecoderInspector)==state.decoderInspector;
        push_rect(out,control.rect,selected?ActiveColor:PanelBg);push_border(out,control.rect,selected?AccentColor:BorderColor);
        push_text(out,{control.rect.x+8,control.rect.y+4,control.rect.w-16,20},control.label,TextColor);
    }
    if(state.decoderInspector){draw_decoder_inspector(out,state,rect);return;}

	const Rect content{.x=rect.x, .y=rect.y + 40.0f, .w=rect.w, .h=std::max(0.0f, rect.h - 40.0f)};
	push_scissor(out, DrawCommandKind::ScissorBegin, content);
	float y = rect.y + 42.0f - state.layout.inspectorScrollOffset;
	const BackendInfo* backend = selected_backend(state);
	if (backend == nullptr) {
		push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=20}, "No backend available", MutedTextColor);
		y += 26;
	} else {
		push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Backend", TextColor);
		y += 22;
		for (const BackendInfo& candidate : state.backends) {
			const Rect row = backend_row_rect(rect, y);
			const bool selected = candidate.id == backend->id;
			if (selected) {
				push_rect(out, row, ActiveColor);
				push_border(out, row, AccentColor);
			}
			const Color color = candidate.capabilities.available ? TextColor : Color{.r=0.95f, .g=0.45f, .b=0.35f, .a=1.0f};
			push_text(out, Rect{.x=row.x + 6.0f, .y=row.y + 3.0f, .w=row.w - 12.0f, .h=16.0f}, backend_display_name(candidate), color);
			y += 24;
		}
		y += 8;
		const std::string implementation = backend->capabilities.implementation.empty() ? backend->name : backend->capabilities.implementation;
		push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, implementation, backend->capabilities.available ? MutedTextColor : Color{.r=0.95f, .g=0.45f, .b=0.35f, .a=1.0f});
		y += 22;
		if (!backend->capabilities.available) {
			push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=34}, backend->capabilities.error, Color{.r=0.95f, .g=0.45f, .b=0.35f, .a=1.0f});
			y += 38;
		}

		std::string currentGroup;
		for (const EncoderParamInfo& param : backend->params) {
			if (!param.relevantForStillImage) {
				continue;
			}
			if (param.group != currentGroup) {
				currentGroup = param.group;
				push_text(out, Rect{.x=rect.x + 12, .y=y + 4, .w=rect.w - 24, .h=18}, currentGroup.empty() ? "Parameters" : currentGroup, TextColor);
				y += 24;
			}
			const bool parameterEnabled = parameter_is_enabled(state, *backend, param);
			const Color parameterColor = parameterEnabled ? MutedTextColor : Color{.r=0.42f, .g=0.43f, .b=0.45f, .a=1.0f};
			push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w * 0.46f, .h=18}, param.label, parameterColor);
			Rect const control{.x=rect.x + rect.w * 0.50f, .y=y - 2, .w=rect.w * 0.44f, .h=20};
			push_rect(out, control, parameterEnabled ? Color{.r=0.11f, .g=0.12f, .b=0.13f, .a=1.0f} : Color{.r=0.07f, .g=0.075f, .b=0.08f, .a=1.0f});
			push_border(out, control, BorderColor);
			const ParamValue currentValue = current_param_value(state, backend->id, param);
			std::string value = param_value_to_string(currentValue);
			if (param.automaticIntValue) {
				if (const int64_t* integer = std::get_if<int64_t>(&currentValue); integer != nullptr && *integer == *param.automaticIntValue) {
					value = param.automaticLabel.empty() ? "Auto" : param.automaticLabel;
				}
			}
			if (param.kind == ParamKind::Bool) {
				value = value.empty() ? "off" : value;
				const bool enabled = std::get_if<bool>(&currentValue) != nullptr && *std::get_if<bool>(&currentValue);
				const Rect checkbox{.x=control.x + 4, .y=control.y + 3, .w=14, .h=14};
				push_rect(out, checkbox, enabled ? AccentColor : Color{.r=0.07f, .g=0.075f, .b=0.08f, .a=1.0f});
				push_border(out, checkbox, BorderColor);
				push_text(out, Rect{.x=control.x + 24, .y=control.y + 3, .w=control.w - 30, .h=16}, enabled ? "on" : "off", TextColor);
				y += 24;
				continue;
			} else if (param.kind == ParamKind::Enum && !param.enumValues.empty()) {
				const auto it = std::ranges::find_if(param.enumValues, [&](const EnumValue& enumValue) {
					return enumValue.value == value;
				});
				if (it != param.enumValues.end()) {
					const auto index = static_cast<std::size_t>(std::distance(param.enumValues.begin(), it));
					const float segmentW = control.w / static_cast<float>(param.enumValues.size());
					push_rect(out, Rect{.x=control.x + segmentW * static_cast<float>(index), .y=control.y, .w=segmentW, .h=control.h}, ActiveColor);
					for (std::size_t i = 1; i < param.enumValues.size(); ++i) {
						const float sx = control.x + segmentW * static_cast<float>(i);
						push_rect(out, Rect{.x=sx, .y=control.y + 2, .w=1, .h=control.h - 4}, BorderColor);
					}
				}
				value = it == param.enumValues.end() ? param.enumValues.front().label : it->label;
				if (param.name == "implementation") {
					value += " (identity only)";
				}
			} else if (param.kind == ParamKind::Enum && param.enumValues.empty()) {
				value = "unavailable";
			} else if (param.kind == ParamKind::Int && param.intRange && !param.directNumericInput) {
				const int64_t current = std::get_if<int64_t>(&currentValue) == nullptr ? param.intRange->min : *std::get_if<int64_t>(&currentValue);
				const auto denom = static_cast<double>(param.intRange->max - param.intRange->min);
				const float ratio = denom <= 0.0 ? 0.0f : static_cast<float>(std::clamp((static_cast<double>(current - param.intRange->min)) / denom, 0.0, 1.0));
				push_rect(out, Rect{.x=control.x + 4, .y=control.y + 8, .w=std::max(0.0f, (control.w - 8) * ratio), .h=4}, AccentColor);
			} else if (param.kind == ParamKind::Float && param.floatRange && !param.directNumericInput) {
				const double current = std::get_if<double>(&currentValue) == nullptr ? param.floatRange->min : *std::get_if<double>(&currentValue);
				const double denom = param.floatRange->max - param.floatRange->min;
				const float ratio = denom <= 0.0 ? 0.0f : static_cast<float>(std::clamp((current - param.floatRange->min) / denom, 0.0, 1.0));
				push_rect(out, Rect{.x=control.x + 4, .y=control.y + 8, .w=std::max(0.0f, (control.w - 8) * ratio), .h=4}, AccentColor);
			} else if (param.kind == ParamKind::String || param.directNumericInput) {
				const std::string widgetId = "param:" + std::to_string(backend->id.value) + ":" + param.name;
				if (state.interaction.focusedWidget == widgetId) {
					push_border(out, control, FocusColor);
				}
			} else if (value.empty()) {
				value = "-";
			}
			push_text(out, Rect{.x=control.x + 6, .y=control.y + 3, .w=control.w - 12, .h=16}, std::move(value), parameterEnabled ? TextColor : parameterColor);
			y += 24;
		}
		if (backend->params.empty() && backend->capabilities.available) {
			push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Capability query pending", MutedTextColor);
			y += 22;
		}
	}

	const ImageObject* image = image_by_id(state, state.selection.selectedImage);
	if (const EncodeRun* run = run_by_id(state, state.selection.selectedRun)) {
		draw_run_details(out, state, *run, rect, y);
		y += 10;
	}
	draw_queue_panel(out, state, rect, y);
	const float debugReserve = state.debug.enabled ? 112.0f : 0.0f;
	constexpr float SelectedImageReserve = 240.0f;
	if (image != nullptr && y + 76 < rect.y + rect.h - debugReserve) {
		y = std::max(y + 10, rect.y + rect.h - SelectedImageReserve - debugReserve);
		push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=20}, "Selected image", TextColor);
		y += 22;
		push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, image->displayName + "  " + std::to_string(image->width) + "x" + std::to_string(image->height), MutedTextColor);
		y += 20;
		if(!image->encoded && image->decoded && !image->decoded->importNote.empty()) {
			for(const auto& line:wrap_tooltip(image->decoded->importNote,std::max<std::size_t>(24,(rect.w-24)/9))) {
				if(y+18>=rect.y+rect.h-debugReserve) break;
				push_text(out,Rect{.x=rect.x+12,.y=y,.w=rect.w-24,.h=18},line,MutedTextColor); y+=18;
			}
		}
		if (image->encoded) {
			push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Encoded size: " + format_bytes_both(image->encoded->byteSize), TextColor);
			y += 20;
			const std::string bpp = format_bpp(image->encoded->byteSize, image->width, image->height);
			if (!bpp.empty() && y + 18 < rect.y + rect.h) {
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Density: " + bpp, MutedTextColor);
				y += 20;
			}
			if (!image->encoded->backendName.empty() && y + 18 < rect.y + rect.h) {
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Backend: " + image->encoded->backendName, MutedTextColor);
				y += 20;
			}
			const uint64_t sourceSize = source_decoded_size(state);
			if (sourceSize > 0) {
				push_text(
					out,
					Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18},
					"Encoded/source ratio: " + format_encoded_source_ratio(image->encoded->byteSize, sourceSize) + "x",
					MutedTextColor
				);
				y += 20;
			}
			if (image->encoded->encodeSeconds > 0.0) {
				std::ostringstream duration;
				duration << std::fixed << std::setprecision(3) << image->encoded->encodeSeconds << " s";
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Encode duration: " + duration.str(), MutedTextColor);
				y += 20;
			}
			if (image->encoded->decodeSeconds > 0.0) {
				std::ostringstream duration;
				duration << std::fixed << std::setprecision(3) << image->encoded->decodeSeconds << " s";
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Decode duration: " + duration.str(), MutedTextColor);
				y += 20;
			}
			if (image->encoded->metricSeconds > 0.0) {
				std::ostringstream duration;
				duration << std::fixed << std::setprecision(3) << image->encoded->metricSeconds << " s";
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Metric duration: " + duration.str(), MutedTextColor);
				y += 20;
			}
			if (!image->encoded->outputPath.empty() && y + 18 < rect.y + rect.h) {
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Output: " + image->encoded->outputPath.string(), MutedTextColor);
				y += 20;
			}
			if (!image->encoded->metrics.empty() && y + 18 < rect.y + rect.h) {
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Quality: " + image->encoded->metricReference, TextColor);
				y += 20;
				for (const QualityMetricRecord& metric : image->encoded->metrics) {
					if (y + 18 >= rect.y + rect.h) {
						break;
					}
					push_text(
						out,
						Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18},
						format_metric_value(metric),
						metric.value ? MutedTextColor : Color{.r=0.95f, .g=0.72f, .b=0.35f, .a=1.0f}
					);
					y += 20;
				}
			} else if (!image->encoded->metricError.empty() && y + 18 < rect.y + rect.h) {
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Metrics unavailable: " + image->encoded->metricError, Color{.r=0.95f, .g=0.72f, .b=0.35f, .a=1.0f});
				y += 20;
			}
			if (!image->encoded->previewError.empty() && y + 18 < rect.y + rect.h) {
				push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, "Preview unavailable: " + image->encoded->previewError, Color{.r=0.95f, .g=0.72f, .b=0.35f, .a=1.0f});
				y += 20;
			}
		}
		if (!image->parents.empty() && y + 18 < rect.y + rect.h) {
			std::string parents = "Parents:";
			for (ImageId const parent : image->parents) {
				parents += " " + std::to_string(parent.value);
			}
			push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, std::move(parents), MutedTextColor);
			y += 20;
		}
		if (image->derived && y + 18 < rect.y + rect.h) {
			std::ostringstream derived;
			derived << image->derived->kind << "  gain " << std::fixed << std::setprecision(2) << image->derived->gain;
			push_text(out, Rect{.x=rect.x + 12, .y=y, .w=rect.w - 24, .h=18}, derived.str(), MutedTextColor);
		}
	}
	if (state.debug.enabled) {
		const float panelY = std::max(y + 12, rect.y + rect.h - 110.0f);
		push_text(out, Rect{.x=rect.x + 12, .y=panelY, .w=rect.w - 24, .h=18}, "Debug log", TextColor);
		float logY = panelY + 22;
		const std::size_t count = std::min<std::size_t>(4, state.debug.recent.size());
		for (std::size_t i = 0; i < count; ++i) {
			const DebugLogEntry& entry = state.debug.recent[state.debug.recent.size() - count + i];
			push_text(out, Rect{.x=rect.x + 12, .y=logY, .w=rect.w - 24, .h=18}, entry.message, MutedTextColor);
			logY += 18;
		}
		const BackendInfo* debugBackend = selected_backend(state);
		if (debugBackend != nullptr && logY + 18 < rect.y + rect.h) {
			std::string capability = "Capabilities: ";
			capability += debugBackend->capabilities.available ? "available " : "unavailable ";
			capability += debugBackend->capabilities.implementation.empty() ? debugBackend->name : debugBackend->capabilities.implementation;
			push_text(out, Rect{.x=rect.x + 12, .y=logY, .w=rect.w - 24, .h=18}, std::move(capability), MutedTextColor);
			logY += 18;
			const std::size_t detailCount = std::min<std::size_t>(3, debugBackend->capabilities.details.size());
			for (std::size_t i = 0; i < detailCount && logY + 18 < rect.y + rect.h; ++i) {
				push_text(out, Rect{.x=rect.x + 12, .y=logY, .w=rect.w - 24, .h=18}, debugBackend->capabilities.details[i], MutedTextColor);
				logY += 18;
			}
		}
	}
	const float contentHeight = std::max(content.h, y + state.layout.inspectorScrollOffset - content.y + 12.0f);
	push_scissor(out, DrawCommandKind::ScissorEnd, content);
	draw_scrollbar(out, content, contentHeight, state.layout.inspectorScrollOffset);
}

void draw_parameter_tooltip(std::vector<DrawCommand>& out, const AppState& state, const LayoutResult& layout) {
    if(state.decoderInspector)return;
	const BackendInfo* backend = selected_backend(state);
	if (backend == nullptr || !contains(layout.inspector, state.interaction.lastPointer)) {
		return;
	}
	float y = layout.inspector.y + 42.0f + 22.0f - state.layout.inspectorScrollOffset;
	y += static_cast<float>(state.backends.size()) * 24.0f + 8.0f + 22.0f;
	std::string currentGroup;
	const EncoderParamInfo* hovered = nullptr;
	for (const EncoderParamInfo& param : backend->params) {
		if (!param.relevantForStillImage) continue;
		if (param.group != currentGroup) {
			currentGroup = param.group;
			y += 24.0f;
		}
		const Rect row{.x=layout.inspector.x + 8.0f, .y=y - 3.0f, .w=layout.inspector.w - 16.0f, .h=22.0f};
		if (contains(row, state.interaction.lastPointer) &&
		    state.interaction.lastPointer.y >= layout.inspector.y + 40.0f) {
			hovered = &param;
			break;
		}
		y += 24.0f;
	}
	if (hovered == nullptr) return;
	const std::size_t overlayStart = out.size();

	std::string defaultText = std::visit([](const auto& value) -> std::string {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, std::monostate>) {
			return "not specified";
		} else if constexpr (std::is_same_v<T, bool>) {
			return value ? "on" : "off";
		} else if constexpr (std::is_same_v<T, double>) {
			std::ostringstream out;
			out << value;
			return out.str();
		} else if constexpr (std::is_same_v<T, int64_t>) {
			return std::to_string(value);
		} else {
			return value.empty() ? "explicitly unset" : value;
		}
	}, hovered->defaultValue);
	if (hovered->automaticIntValue) {
		if (const int64_t* integer = std::get_if<int64_t>(&hovered->defaultValue); integer != nullptr && *integer == *hovered->automaticIntValue) {
			defaultText = (hovered->automaticLabel.empty() ? "Auto" : hovered->automaticLabel) + " (application sentinel " + std::to_string(*integer) + "; not written to the bitstream)";
		}
	}
	std::vector<std::string> lines;
	lines.push_back(hovered->label + " (" + hovered->name + ")");
	lines.push_back("Application default: " + defaultText);
	if (const std::string reason = parameter_disabled_reason(state, *backend, *hovered); !reason.empty()) {
		lines.push_back("Disabled: " + reason);
	}
	const std::vector<std::string> helpLines = wrap_tooltip(hovered->help.empty() ? "No description is available for this option." : hovered->help);
	lines.insert(lines.end(), helpLines.begin(), helpLines.end());
	const float width = std::min(560.0f, static_cast<float>(state.interaction.framebufferWidth) - 32.0f);
	const float height = 20.0f + static_cast<float>(lines.size()) * 24.0f;
	float x = layout.inspector.x - width - 12.0f;
	if (x < 12.0f) x = std::min(layout.inspector.x + layout.inspector.w + 12.0f, static_cast<float>(state.interaction.framebufferWidth) - width - 12.0f);
	float tooltipY = std::min(state.interaction.lastPointer.y + 18.0f, static_cast<float>(state.interaction.framebufferHeight) - height - 12.0f);
	x = std::max(12.0f, x);
	tooltipY = std::max(12.0f, tooltipY);
	const Rect box{.x=x, .y=tooltipY, .w=width, .h=height};
	push_rect(out, box, Color{.r=0.025f, .g=0.028f, .b=0.034f, .a=1.0f});
	push_border(out, box, FocusColor);
	float lineY = box.y + 8.0f;
	for (std::size_t i = 0; i < lines.size(); ++i) {
		push_text(out, Rect{.x=box.x + 10.0f, .y=lineY, .w=box.w - 20.0f, .h=18.0f}, lines[i], i < 2 ? TextColor : MutedTextColor);
		lineY += 24.0f;
	}
	for (std::size_t i = overlayStart; i < out.size(); ++i) out[i].layer = 1;
}

void draw_image_tooltip(std::vector<DrawCommand>& out, const AppState& state, const LayoutResult& layout) {
	if (!contains(layout.imageList, state.interaction.lastPointer) || state.interaction.lastPointer.y < layout.imageList.y + ImageListHeaderHeight) return;
	float y = layout.imageList.y + ImageListHeaderHeight + 4.0f - state.imageList.scrollOffset;
	const ImageObject* hovered = nullptr;
	for (const ImageObject* image : ordered_images(state)) {
		const Rect row = image_list_row_rect(layout.imageList, y);
		if (contains(row, state.interaction.lastPointer)) {
			const std::size_t paneButtons = std::min<std::size_t>(state.panes.size(), 4);
			for (std::size_t i = 0; i < paneButtons; ++i) {
				if (contains(image_list_pane_button_rect(row, i), state.interaction.lastPointer)) return;
			}
			if (valid(state.selection.selectedImage) && state.selection.selectedImage != image->id &&
			    contains(image_list_difference_button_rect(row, paneButtons), state.interaction.lastPointer)) return;
			hovered = image;
			break;
		}
		y += ImageListRowStep;
	}
	if (hovered == nullptr) return;
	const std::size_t overlayStart = out.size();
	std::vector<std::string> lines{hovered->displayName, std::to_string(hovered->width) + " x " + std::to_string(hovered->height) + "  " + pixel_format_label(hovered->pixelFormat)};
	if(hovered->decoded && !hovered->decoded->importNote.empty()) {
		const auto note=wrap_tooltip(hovered->decoded->importNote,52);
		lines.insert(lines.end(),note.begin(),note.end());
	}
	if (hovered->encoded) {
		lines.push_back("Encoded size: " + format_bytes_both(hovered->encoded->byteSize));
		if (hovered->encoded->encodeSeconds > 0.0) {
			std::ostringstream duration;
			duration << std::fixed << std::setprecision(3) << hovered->encoded->encodeSeconds << " s";
			lines.push_back("Encode time: " + duration.str());
		}
		lines.push_back("Backend: " + hovered->encoded->backendName + "  codec: " + hovered->encoded->codecName);
		for (const ParamSummary& param : hovered->encoded->keyParams) {
			if (lines.size() >= 8) break;
			lines.push_back(param.name + " = " + param.value);
		}
		lines.push_back("Quality reference: " + hovered->encoded->metricReference);
		lines.push_back("Metrics: " + metric_state_text(hovered->encoded->metricState));
		if(!hovered->encoded->metricError.empty()) {
			const auto error=wrap_tooltip(hovered->encoded->metricError,52);lines.insert(lines.end(),error.begin(),error.end());
		}
		lines.push_back("Metric space: linear BT.2020 FP32; reference white 203 cd/m²");
		if(hovered->encoded->metrics.empty()) {
			std::string offered="Available: ";
			for(const auto& d:metric_catalog()) {if(offered.size()>11) offered+=", ";offered+=d.label;}
			const auto wrapped=wrap_tooltip(offered,52);lines.insert(lines.end(),wrapped.begin(),wrapped.end());
		}
		for (const QualityMetricRecord& metric : hovered->encoded->metrics) {
			lines.push_back(format_metric_value(metric));
			if(!metric.accuracyNote.empty()) {
				const auto note=wrap_tooltip(metric.accuracyNote,52);lines.insert(lines.end(),note.begin(),note.end());
			}
			if (!metric.value && !metric.unavailableReason.empty()) {
				const auto reason = wrap_tooltip(metric.label + ": " + metric.unavailableReason, 52);
				lines.insert(lines.end(), reason.begin(), reason.end());
			}
		}
		if (!hovered->encoded->previewError.empty()) {
			const auto reason = wrap_tooltip("Preview: " + hovered->encoded->previewError, 52);
			lines.insert(lines.end(), reason.begin(), reason.end());
		}
	} else if (hovered->type == ImageObjectType::Source) {
		if (hovered->sourceByteSize) {
			lines.push_back("File size: " + format_bytes_both(*hovered->sourceByteSize));
		}
		if (hovered->decoded) {
			lines.push_back("Uncompressed buffer: " + format_bytes_both(raw_image_bytes(*hovered->decoded)));
		}
	}
	lines.emplace_back("Select then use Delete to remove this entry.");
	const float width = std::min(520.0f, static_cast<float>(state.interaction.framebufferWidth) - 32.0f);
	const float height = 16.0f + static_cast<float>(lines.size()) * 20.0f;
	float const x = std::min(state.interaction.lastPointer.x + 18.0f, static_cast<float>(state.interaction.framebufferWidth) - width - 12.0f);
	float const tooltipY = std::min(state.interaction.lastPointer.y + 18.0f, static_cast<float>(state.interaction.framebufferHeight) - height - 12.0f);
	const Rect box{.x=std::max(12.0f, x), .y=std::max(12.0f, tooltipY), .w=width, .h=height};
	push_rect(out, box, Color{.r=0.025f, .g=0.028f, .b=0.034f, .a=1.0f});
	push_border(out, box, FocusColor);
	float lineY = box.y + 8.0f;
	for (std::size_t i = 0; i < lines.size(); ++i) {
		push_text(out, Rect{.x=box.x + 10.0f, .y=lineY, .w=box.w - 20.0f, .h=18.0f}, lines[i], i == 0 ? TextColor : MutedTextColor);
		lineY += 20.0f;
	}
	for (std::size_t i = overlayStart; i < out.size(); ++i) out[i].layer = 1;
}

Rect empty_viewer_import_rect(Rect viewer) {
	constexpr float width = 150.0f;
	constexpr float height = 34.0f;
	return Rect{
		.x=viewer.x + (viewer.w - width) * 0.5f,
		.y=viewer.y + viewer.h * 0.5f + 28.0f,
		.w=width,
		.h=height,
	};
}

void draw_empty_pane(std::vector<DrawCommand>& out, const AppState& state, Rect rect, std::string title) {
	push_scissor(out, DrawCommandKind::ScissorBegin, rect);
	push_rect(out, rect, Color{.r=0.04f, .g=0.045f, .b=0.05f, .a=1.0f});
	const float width = std::min(460.0f, std::max(180.0f, rect.w - 48.0f));
	const Rect card{.x=rect.x + (rect.w - width) * 0.5f, .y=rect.y + rect.h * 0.5f - 68.0f, .w=width, .h=136.0f};
	push_rect(out, card, PanelBg);
	push_border(out, card, BorderColor);
	push_text(out, Rect{.x=card.x + 18.0f, .y=card.y + 18.0f, .w=card.w - 36.0f, .h=22.0f}, std::move(title), TextColor);
	if (state.images.empty()) {
		push_text(out, Rect{.x=card.x + 18.0f, .y=card.y + 45.0f, .w=card.w - 36.0f, .h=18.0f}, "Open an image to start comparing codecs.", MutedTextColor);
		const Rect button = empty_viewer_import_rect(rect);
		const bool importEnabled = command_is_enabled(state, "import");
		push_rect(out, button, importEnabled ? AccentColor : DisabledColor);
		push_border(out, button, importEnabled ? FocusColor : BorderColor);
		push_text(out, Rect{.x=button.x + 18.0f, .y=button.y + 8.0f, .w=button.w - 36.0f, .h=18.0f}, importEnabled ? "Import image" : "Opening...", importEnabled ? TextColor : MutedTextColor);
	} else {
		push_text(out, Rect{.x=card.x + 18.0f, .y=card.y + 48.0f, .w=card.w - 36.0f, .h=18.0f}, "Choose this pane's P button in Results.", MutedTextColor);
		push_text(out, Rect{.x=card.x + 18.0f, .y=card.y + 72.0f, .w=card.w - 36.0f, .h=18.0f}, "Example: P2 assigns an image to Pane 2.", MutedTextColor);
	}
	push_scissor(out, DrawCommandKind::ScissorEnd, rect);
	push_border(out, rect, BorderColor);
}

void draw_viewer(std::vector<DrawCommand>& out, const AppState& state, const LayoutResult& layout, const ResourceSnapshot& resources, double timeSeconds) {
	push_rect(out, layout.viewer, ViewerBg);
	if (state.images.empty()) {
		draw_empty_pane(out, state, layout.viewer, "No source image");
		return;
	}
	if (state.viewMode.kind == ViewModeKind::Difference && state.viewMode.generatedImage) {
		const ImageObject* image = image_by_id(state, *state.viewMode.generatedImage);
		if (image != nullptr) {
			push_scissor(out, DrawCommandKind::ScissorBegin, layout.viewer);
			Pane synthetic;
			synthetic.id = PaneId{};
			synthetic.image = image->id;
			synthetic.transform = fit_transform(image->width, image->height, layout.viewer, state.interaction.outputScale);
			DrawCommand command;
			command.kind = DrawCommandKind::Image;
			command.rect = image_rect_in_pane(synthetic, *image, layout.viewer);
			command.image = image->id;
			command.texture = resolve_texture(resources, image->id);
			command.color = Color{.r=1, .g=1, .b=1, .a=1};
			out.push_back(std::move(command));
			if (!valid(out.back().texture)) {
				push_text(out, Rect{.x=layout.viewer.x + 12, .y=layout.viewer.y + 12, .w=layout.viewer.w - 24, .h=20}, "Difference texture pending", MutedTextColor);
			}
			push_text(out, Rect{.x=layout.viewer.x + 12, .y=layout.viewer.y + 12, .w=layout.viewer.w - 24, .h=20}, image->displayName, TextColor);
			push_scissor(out, DrawCommandKind::ScissorEnd, layout.viewer);
			push_border(out, layout.viewer, BorderColor);
			return;
		}
	}
	if (state.viewMode.kind == ViewModeKind::Blink && state.viewMode.paneOrder.size() >= 2) {
		const double interval = std::max(0.05, state.viewMode.blinkIntervalSeconds);
		const std::size_t index = static_cast<std::size_t>(std::floor(timeSeconds / interval)) % 2u;
		const Pane* pane = find_pane(state, state.viewMode.paneOrder[index]);
		const ImageObject* image = pane != nullptr && pane->image ? image_by_id(state, *pane->image) : nullptr;
		if (pane != nullptr && image != nullptr) {
			if (image->decoded == nullptr) {
				draw_pane_unavailable(out, *image, layout.viewer);
			} else {
				draw_pane_image(out, resources, *pane, *image, layout.viewer, layout.viewer);
			}
			push_text(out, Rect{.x=layout.viewer.x + 12, .y=layout.viewer.y + 12, .w=layout.viewer.w - 24, .h=20}, "Blink: " + image->displayName, TextColor);
			push_border(out, layout.viewer, BorderColor);
			return;
		}
	}
	if (state.viewMode.kind == ViewModeKind::Split && state.viewMode.paneOrder.size() >= 2) {
		const Pane* leftPane = find_pane(state, state.viewMode.paneOrder[0]);
		const Pane* rightPane = find_pane(state, state.viewMode.paneOrder[1]);
		const ImageObject* leftImage = leftPane != nullptr && leftPane->image ? image_by_id(state, *leftPane->image) : nullptr;
		const ImageObject* rightImage = rightPane != nullptr && rightPane->image ? image_by_id(state, *rightPane->image) : nullptr;
		const float split = layout.viewer.x + layout.viewer.w * static_cast<float>(std::clamp(state.viewMode.splitPosition, 0.0, 1.0));
		const Rect leftClip{.x=layout.viewer.x, .y=layout.viewer.y, .w=std::max(0.0f, split - layout.viewer.x), .h=layout.viewer.h};
		const Rect rightClip{.x=split, .y=layout.viewer.y, .w=std::max(0.0f, layout.viewer.x + layout.viewer.w - split), .h=layout.viewer.h};
		if (leftPane != nullptr && leftImage != nullptr) {
			if (leftImage->decoded == nullptr) {
				draw_pane_unavailable(out, *leftImage, leftClip);
			} else {
				draw_pane_image(out, resources, *leftPane, *leftImage, leftClip, layout.viewer);
			}
		} else {
			draw_empty_pane(out, state, leftClip, "Pane 1 is empty");
		}
		if (rightPane != nullptr && rightImage != nullptr) {
			if (rightImage->decoded == nullptr) {
				draw_pane_unavailable(out, *rightImage, rightClip);
			} else {
				draw_pane_image(out, resources, *rightPane, *rightImage, rightClip, layout.viewer);
			}
		} else {
			draw_empty_pane(out, state, rightClip, "Pane 2 is empty");
		}
		push_rect(out, Rect{.x=split - 1.0f, .y=layout.viewer.y, .w=2.0f, .h=layout.viewer.h}, AccentColor);
		push_border(out, layout.viewer, BorderColor);
		return;
	}
	const std::vector<PaneRect> paneRects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
	for (const PaneRect& paneRect : paneRects) {
		const Pane* pane = find_pane(state, paneRect.pane);
		const ImageObject* image = pane != nullptr && pane->image ? image_by_id(state, *pane->image) : nullptr;
		if (image == nullptr) {
			draw_empty_pane(out, state, paneRect.rect, pane_label(state, paneRect.pane) + " is empty");
		} else {
			if (image->decoded == nullptr) {
				draw_pane_unavailable(out, *image, paneRect.rect);
			} else {
				draw_pane_image(out, resources, *pane, *image, paneRect.rect, paneRect.rect);
			}
			push_text(out, Rect{.x=paneRect.rect.x + 12, .y=paneRect.rect.y + 12, .w=paneRect.rect.w - 24, .h=20}, pane_label(state, paneRect.pane) + "  " + image->displayName, TextColor);
		}
		if (image != nullptr) push_border(out, paneRect.rect, BorderColor);
	}
}

} // namespace

TextureId resolve_texture(const ResourceSnapshot& resources, ImageId image) {
	const auto it = std::ranges::find_if(resources.textures, [image](const TextureBinding& binding) {
		return binding.image == image;
	});
	return it == resources.textures.end() ? TextureId{} : it->texture;
}

std::vector<DrawCommand> build_draw_list(
	const AppState& state,
	const LayoutResult& layout,
	const ResourceSnapshot& resources,
	double timeSeconds
) {
	std::vector<DrawCommand> out;
	push_rect(out, layout.commandBar, PanelBg);
	push_text(out, Rect{.x=layout.commandBar.x + 12, .y=layout.commandBar.y + 10, .w=120, .h=20}, "codec_vis", TextColor);
	for (const CommandButton& button : CommandButtons) {
		const Rect buttonRect{.x=layout.commandBar.x + button.x, .y=layout.commandBar.y + 4.0f, .w=button.w, .h=layout.commandBar.h - 8.0f};
		const bool enabled = command_is_enabled(state, button.name);
		const bool active = command_active(state, button.name);
		push_rect(out, buttonRect, enabled ? Color{.r=0.105f, .g=0.112f, .b=0.124f, .a=1.0f} : Color{.r=0.065f, .g=0.068f, .b=0.074f, .a=1.0f});
		push_border(out, buttonRect, enabled ? BorderColor : Color{.r=0.12f, .g=0.125f, .b=0.135f, .a=1.0f});
		if (active) {
			push_rect(out, buttonRect, ActiveColor);
			push_border(out, buttonRect, AccentColor);
		}
		const char* label = button.label;
		if (std::string_view{button.name} == "scratch") label = state.scratchResults ? "Replace latest" : "Keep all results";
		if (std::string_view{button.name} == "import" && state.interaction.importPending) label = "Wait";
		if (std::string_view{button.name} == "cancel") {
			if (const EncodeRun* run = active_encode_run(state); run != nullptr && run->state == EncodeRunState::CancelRequested) label = "Canceling...";
		}
		const Color labelColor = enabled ? (active ? TextColor : Color{.r=0.76f, .g=0.79f, .b=0.82f, .a=1.0f}) : Color{.r=0.36f, .g=0.38f, .b=0.40f, .a=1.0f};
		push_text(out, Rect{.x=buttonRect.x + 8.0f, .y=buttonRect.y + 6.0f, .w=buttonRect.w - 16.0f, .h=buttonRect.h - 10.0f}, label, labelColor);
	}
	draw_image_list(out, state, layout.imageList);
	draw_viewer(out, state, layout, resources, timeSeconds);
	draw_inspector(out, state, layout.inspector);
	draw_widget_state(out, state, layout);
	push_rect(out, layout.statusBar, PanelBg);
	push_border(out, layout.statusBar, BorderColor);
	std::string status = "Ready";
	if (const EncodeRun* run = active_encode_run(state)) {
		status = "Run " + std::to_string(run->id.value) + " " + encode_run_state_text(run->state);
		if (const BackendInfo* backend = backend_by_id(state, run->backend)) {
			status += "  " + backend->name;
		}
	}
	const PaneId statusPaneId = valid(state.interaction.hoveredPane) ? state.interaction.hoveredPane : state.selection.activePane;
	if (const Pane* pane = find_pane(state, statusPaneId)) {
		status += "  " + pane_label(state, pane->id);
		if (pane->image) {
			if (const ImageObject* image = image_by_id(state, *pane->image)) {
				const std::vector<PaneRect> paneRects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
				const auto rect = std::ranges::find_if(paneRects, [pane](const PaneRect& paneRect) {
					return paneRect.pane == pane->id;
				});
				status += "  " + image->displayName;
				if (rect != paneRects.end()) {
					if (std::optional<ImagePixelCoord> coord = pane_to_image_coord(*pane, *image, rect->rect, state.interaction.lastPointer)) {
						status += "  x=" + std::to_string(coord->x) + " y=" + std::to_string(coord->y);
						const std::string sample = format_pixel_sample(*image, *coord);
						if (!sample.empty()) {
							status += "  " + sample;
						}
					}
				}
				std::ostringstream zoom;
				zoom << std::fixed << std::setprecision(1) << (pane->transform.scale * 100.0) << '%';
				status += "  zoom " + zoom.str();
				if (image->encoded) {
					status += "  size " + format_bytes(image->encoded->byteSize);
					const std::string metric = primary_metric_text(*image->encoded);
					if (!metric.empty()) {
						status += "  " + metric;
					}
				}
				if (state.viewMode.kind != ViewModeKind::Single) {
					std::vector<std::pair<PaneId, int>> samples;
					const std::vector<PaneRect> visibleRects = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
					for (const PaneRect& visible : visibleRects) {
						const Pane* otherPane = find_pane(state, visible.pane);
						const ImageObject* otherImage = otherPane != nullptr && otherPane->image ? image_by_id(state, *otherPane->image) : nullptr;
						if (otherPane == nullptr || otherImage == nullptr) {
							continue;
						}
						if (std::optional<ImagePixelCoord> otherCoord = pane_to_image_coord(*otherPane, *otherImage, visible.rect, state.interaction.lastPointer)) {
							if (std::optional<int> sample = luma_sample_value(*otherImage, *otherCoord)) {
								samples.emplace_back(otherPane->id, *sample);
							}
						}
					}
					if (samples.size() >= 2) {
						status += "  compare";
						for (const auto& sample : samples) {
							status += " p" + std::to_string(sample.first.value) + "=" + std::to_string(sample.second);
						}
						status += " d=" + std::to_string(samples[1].second - samples[0].second);
					}
				}
			}
		}
	}
	if (!state.errors.empty()) {
		const AppError& error = state.errors.front();
		status = "ERROR [" + error.subsystem + "/" + error.operation + "]: " + error.message + "  (click to dismiss)";
	}
	push_text(out, Rect{.x=layout.statusBar.x + 12, .y=layout.statusBar.y + 6, .w=layout.statusBar.w - 24, .h=18}, std::move(status), state.errors.empty() ? MutedTextColor : Color{.r=0.98f, .g=0.55f, .b=0.4f, .a=1.0f});
	draw_parameter_tooltip(out, state, layout);
	draw_image_tooltip(out, state, layout);
	return out;
}

} // namespace codec_gui::gui
