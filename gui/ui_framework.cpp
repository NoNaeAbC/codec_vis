#include "ui_framework.hpp"
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
namespace codec_gui::gui::ui {
	std::optional<Input> input_from_action(const Action &action) {
		Input input;
		input.point   = action.point;
		input.button  = action.button;
		input.shift   = action.shift;
		input.control = action.control;
		input.alt     = action.alt;
		switch (action.kind) {
			case ActionKind::PointerMoved:
				input.kind = InputKind::Move;
				break;
			case ActionKind::PointerPressed:
				input.kind = InputKind::Press;
				break;
			case ActionKind::PointerReleased:
				input.kind = InputKind::Release;
				break;
			case ActionKind::PointerScrolled:
				input.kind  = InputKind::Scroll;
				input.delta = {action.scrollX * 3, float(action.value) * 3};
				break;
			case ActionKind::WindowFocusLost:
				input.kind = InputKind::FocusLost;
				break;
			case ActionKind::KeyReleased:
				input.kind = InputKind::KeyRelease;
				input.text = action.text;
				break;
			case ActionKind::KeyPressed: {
				static const std::set<std::string> keys = {"tab",  "enter",  "escape",  "backspace", "delete",
														   "left", "right",  "up",      "down",      "home",
														   "end",  "pageup", "pagedown"};
				input.kind =
						keys.contains(action.text) || action.control || action.alt ? InputKind::Key : InputKind::Text;
				input.text = action.text;
				if (action.text == " ") {
					input.kind = InputKind::Key;
					input.text = "space";
				}
				break;
			}
			default:
				return {};
		}
		return input;
	}

	namespace {
		float bounded(float value, float low, float high) {
			return std::clamp(std::isfinite(value) ? value : low, low, std::max(low, high));
		}
		std::size_t previous_utf8(std::string_view text, std::size_t at) {
			if (!at) return 0;
			--at;
			while (at && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) --at;
			return at;
		}
		std::size_t next_utf8(std::string_view text, std::size_t at) {
			if (at >= text.size()) return text.size();
			++at;
			while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) ++at;
			return at;
		}
		double slider_position(double value, NumberOptions options, SliderScale scale) {
			if (options.maximum == options.minimum) return 0;
			value = std::clamp(value, options.minimum, options.maximum);
			if (scale == SliderScale::Logarithmic)
				return (std::log(value) - std::log(options.minimum)) /
					   (std::log(options.maximum) - std::log(options.minimum));
			return (value - options.minimum) / (options.maximum - options.minimum);
		}
		double slider_value(double position, NumberOptions options, SliderScale scale) {
			if (position <= 0) return options.minimum;
			if (position >= 1) return options.maximum;
			return scale == SliderScale::Logarithmic
						   ? std::exp(std::lerp(std::log(options.minimum), std::log(options.maximum), position))
						   : std::lerp(options.minimum, options.maximum, position);
		}
		double slider_pointer_value(const Widget &w, Point point) {
			return slider_value(double((point.x - w.rect.x - 7) / std::max(1.f, w.rect.w - 14)), w.number,
								w.sliderScale);
		}
		std::string number_text(double value, NumberOptions options) {
			char buffer[80];
			auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed,
											  options.integer ? 0 : std::clamp(options.decimals, 0, 12));
			return error == std::errc{} ? std::string(buffer, end) : std::string{};
		}
		std::optional<double> parse_number(std::string_view text, NumberOptions options) {
			while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
			while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
			if (!text.empty() && text.front() == '+') text.remove_prefix(1);
			double value      = 0;
			auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (text.empty() || error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) ||
				value < options.minimum || value > options.maximum || (options.integer && std::trunc(value) != value))
				return {};
			return value;
		}
	} // namespace
	DockLayout dock_layout(Rect bounds, DockSizes s, float minW, float minH) {
		DockLayout out;
		if (!valid_rect(bounds)) return out;
		const float bar = bounded(s.bar, 0, bounds.h), status = bounded(s.status, 0, bounds.h - bar);
		out.bar    = {bounds.x, bounds.y, bounds.w, bar};
		out.status = {bounds.x, bounds.y + bounds.h - status, bounds.w, status};
		Rect        content{bounds.x, bounds.y + bar, bounds.w, bounds.h - bar - status};
		const float bottom = s.focus ? 0 : bounded(s.bottom, 0, content.h - std::min(minH, content.h));
		const float left   = s.focus ? 0 : bounded(s.left, 0, content.w - std::min(minW, content.w));
		const float right  = s.focus ? 0 : bounded(s.right, 0, content.w - left - std::min(minW, content.w));
		const float h      = content.h - bottom;
		out.left           = {content.x, content.y, left, h};
		out.right          = {content.x + content.w - right, content.y, right, h};
		out.bottom         = {content.x, content.y + h, content.w, bottom};
		out.canvas         = {content.x + left, content.y, content.w - left - right, h};
		if (left) out.leftHandle = {content.x + left - 3, content.y, 6, h};
		if (right) out.rightHandle = {out.right.x - 3, content.y, 6, h};
		if (bottom) out.bottomHandle = {content.x, content.y + h - 3, content.w, 6};
		return out;
	}
	std::vector<Rect> layout_tracks(Rect bounds, Axis axis, std::span<const Track> tracks, float gap) {
		if (!std::isfinite(gap) || gap < 0) throw std::invalid_argument("Invalid layout gap");
		const float        extent = axis == Axis::Horizontal ? bounds.w : bounds.h;
		std::vector<float> sizes;
		float              used = gap * float(tracks.empty() ? 0 : tracks.size() - 1);
		for (auto t: tracks) {
			if (t.minimum < 0 || t.maximum < t.minimum || t.flex < 0)
				throw std::invalid_argument("Invalid layout track");
			sizes.push_back(bounded(t.preferred, t.minimum, t.maximum));
			used += sizes.back();
		}
		float remaining = extent - used;
		for (std::size_t pass = 0; pass < tracks.size() + 1 && std::abs(remaining) > .01f; ++pass) {
			float weights = 0;
			for (std::size_t i = 0; i < tracks.size(); ++i)
				weights += remaining > 0 ? (sizes[i] < tracks[i].maximum ? tracks[i].flex : 0)
										 : (sizes[i] > tracks[i].minimum ? 1 : 0);
			if (weights <= 0) break;
			const float before = remaining;
			for (std::size_t i = 0; i < tracks.size(); ++i) {
				const float weight = before > 0 ? (sizes[i] < tracks[i].maximum ? tracks[i].flex : 0)
												: (sizes[i] > tracks[i].minimum ? 1 : 0);
				float       next = bounded(sizes[i] + before * weight / weights, tracks[i].minimum, tracks[i].maximum);
				remaining -= next - sizes[i];
				sizes[i] = next;
			}
		}
		std::vector<Rect> result;
		float             cursor = axis == Axis::Horizontal ? bounds.x : bounds.y;
		for (auto size: sizes) {
			result.push_back(axis == Axis::Horizontal ? Rect{cursor, bounds.y, size, bounds.h}
													  : Rect{bounds.x, cursor, bounds.w, size});
			cursor += size + gap;
		}
		return result;
	}
	VisibleRows visible_rows(std::size_t count, float scroll, float height, float rowHeight, std::size_t overscan) {
		if (!count || !std::isfinite(rowHeight) || rowHeight <= 0 || !std::isfinite(height) || height <= 0) return {};
		const double start = std::floor(std::max(0.f, std::isfinite(scroll) ? scroll : 0.f) / double(rowHeight));
		auto         begin = std::size_t(std::min(start, double(count)));
		auto         end =
				std::size_t(std::min(double(count), std::ceil((std::max(0.f, scroll) + height) / double(rowHeight))));
		return {begin > overscan ? begin - overscan : 0, std::min(count, end + std::min(overscan, count - end))};
	}
	VisibleRows ScrollRegion::rows(std::size_t count, float rowHeight) const {
		return visible_rows(count, viewport.y - content.y, viewport.h, rowHeight);
	}
	Context::Context(Measure measure) : measure_(std::move(measure)) {
		if (!measure_) throw std::invalid_argument("A text measurer is required");
	}
	void Context::begin_frame(Rect bounds) {
		bounds_ = bounds;
		commands_.clear();
		building_.clear();
		ids_.clear();
		clips_ = {bounds};
		scrollScopes_.clear();
		layer_ = 0;
		tooltip_.clear();
	}
	void Context::end_frame() {
		if (clips_.size() != 1 || !scrollScopes_.empty()) throw std::logic_error("Unbalanced UI scope");
		const auto now = std::chrono::steady_clock::now();
		if (tooltip_ != tooltipHover_) { tooltipHover_ = tooltip_; tooltipSince_ = now; }
		if (!tooltip_.empty() && !tooltipSuppressed_ && capture_.empty() && popup_.empty() && now - tooltipSince_ >= std::chrono::milliseconds(400)) {
			const float width = std::min(560.f, bounds_.w - 24);
			std::istringstream lines(tooltip_);
			std::string line;
			int count = 0;
			while (std::getline(lines, line)) count += 1 + int(std::ceil(measure_(line, 14, FontRole::Interface) / std::max(1.f, width - 20)));
			const float height = std::min(bounds_.h - 24, 16.f + count * theme.lineHeight);
			Rect box{bounded(pointer_.x + 16, bounds_.x + 12, bounds_.x + bounds_.w - width - 12),
				bounded(pointer_.y + 20, bounds_.y + 12, bounds_.y + bounds_.h - height - 12), width, height};
			const int previous = layer_;
			layer_ = 1000;
			fill(box, theme.elevated, 4); border(box, theme.border, 1, 4);
			label(inset(box, 10), tooltip_, theme.text, 14, TextAlign::Left, TextOverflow::Wrap);
			layer_ = previous;
		}
		widgets_ = std::move(building_);
		if (!focus_.empty() && !find(focus_)) focus_.clear();
		if (!capture_.empty() && !find(capture_)) capture_.clear();
		if (!popup_.empty() && !find(popup_)) popup_.clear();
		// Reclaim state for controls no longer declared; large changing tables do not
		// grow a permanent state entry for every row ever visited.
		std::erase_if(edits_, [&](const auto &p) { return !ids_.contains(p.first); });
		signals_.clear();
	}
	const Widget *Context::find(std::string_view id) const {
		auto it = std::find_if(widgets_.begin(), widgets_.end(), [&](const auto &w) { return w.id == id; });
		return it == widgets_.end() ? nullptr : &*it;
	}
	void Context::widget(Widget w) {
		if (w.id.empty() || !ids_.insert(w.id).second) throw std::logic_error("Duplicate/empty UI id: " + w.id);
		w.clip  = intersect(clips_.back(), w.rect);
		w.layer = layer_;
		if (!valid_rect(w.clip)) w.focusable = false;
		building_.push_back(std::move(w));
	}
	bool Context::activated(std::string_view id) const {
		return std::any_of(signals_.begin(), signals_.end(),
						   [&](const auto &s) { return s.id == id && s.kind == Signal::Activate; });
	}
	void Context::push_clip(Rect rect) {
		clips_.push_back(intersect(clips_.back(), rect));
		DrawCommand c;
		c.kind = DrawCommandKind::ScissorBegin;
		c.rect = clips_.back();
		commands_.push_back(c);
	}
	void Context::pop_clip() {
		if (clips_.size() < 2) throw std::logic_error("Unmatched UI clip");
		clips_.pop_back();
		DrawCommand c;
		c.kind = DrawCommandKind::ScissorEnd;
		commands_.push_back(c);
	}
	void Context::fill(Rect rect, Color color, float radius, int layer) {
		DrawCommand c;
		c.kind   = radius > 0 ? DrawCommandKind::RoundedRect : DrawCommandKind::Rect;
		c.rect   = rect;
		c.color  = color;
		c.radius = radius;
		c.layer  = layer < 0 ? layer_ : layer;
		commands_.push_back(c);
	}
	void Context::border(Rect rect, Color color, float width, float radius) {
		DrawCommand c;
		c.kind        = DrawCommandKind::Border;
		c.rect        = rect;
		c.color       = color;
		c.radius      = radius;
		c.strokeWidth = width;
		c.layer       = layer_;
		commands_.push_back(c);
	}
	void Context::line(Point from, Point to, Color color, float width) {
		DrawCommand c;
		c.kind        = DrawCommandKind::Line;
		c.rect        = {from.x, from.y, 0, 0};
		c.endPoint    = to;
		c.color       = color;
		c.strokeWidth = width;
		c.layer       = layer_;
		commands_.push_back(c);
	}
	void Context::point(Point p, Color color, float radius) {
		DrawCommand c;
		c.kind  = DrawCommandKind::Circle;
		c.rect  = {p.x - radius, p.y - radius, radius * 2, radius * 2};
		c.color = color;
		c.layer = layer_;
		commands_.push_back(c);
	}
	void Context::image(Rect rect, ImageId imageId, Rect uv) {
		DrawCommand c;
		c.kind   = DrawCommandKind::Image;
		c.rect   = rect;
		c.image  = imageId;
		c.uvRect = uv;
		c.color  = {1, 1, 1, 1};
		c.layer  = layer_;
		commands_.push_back(c);
	}
	void Context::image_difference(Rect rect, ImageId first, ImageId second, float gain) {
		image(rect, first);
		commands_.back().comparisonImage = second;
		commands_.back().differenceGain  = gain;
	}
	void Context::tooltip(Rect target, std::string text) {
		if (contains(intersect(target, clips_.back()), pointer_)) tooltip_ = std::move(text);
	}
	void Context::label(Rect rect, std::string text, Color color, float size, TextAlign align, TextOverflow overflow,
						FontRole font) {
		DrawCommand c;
		c.kind         = DrawCommandKind::Text;
		c.rect         = rect;
		c.text         = std::move(text);
		c.color        = color;
		c.fontSize     = size > 0 ? size : theme.fontSize;
		c.fontRole     = font;
		c.textAlign    = align;
		c.textOverflow = overflow;
		c.lineHeight   = theme.lineHeight;
		c.layer        = layer_;
		commands_.push_back(std::move(c));
	}
	void Context::set_focus(std::string id) {
		if (focus_ == id) return;
		if (auto it = edits_.find(focus_); it != edits_.end() && it->second.dirty)
			signals_.push_back({focus_, Signal::Commit});
		focus_ = std::move(id);
	}
	bool Context::button(std::string id, std::string text, Rect rect, bool enabled, bool selected) {
		Widget w;
		w.id      = id;
		w.label   = text;
		w.rect    = rect;
		w.enabled = enabled;
		widget(w);
		const bool hover = contains(intersect(rect, clips_.back()), pointer_);
		fill(rect,
			 (selected || capture_ == id) && enabled ? theme.accent
			 : hover && enabled                      ? theme.elevated
													 : theme.surface,
			 3);
		border(rect, focus_ == id ? theme.focus : theme.border, focus_ == id ? 2 : 1, 3);
		label({rect.x + theme.padding, rect.y + (rect.h - theme.lineHeight) / 2, rect.w - 2 * theme.padding,
			   theme.lineHeight},
			  text, enabled ? theme.text : theme.muted, 0, TextAlign::Center);
		return enabled && activated(id);
	}
	bool Context::checkbox(std::string id, std::string text, Rect rect, bool &value, bool enabled) {
		Widget w;
		w.id      = id;
		w.label   = text;
		w.rect    = rect;
		w.role    = Role::CheckBox;
		w.enabled = enabled;
		widget(w);
		bool changed = enabled && activated(id);
		if (changed) value = !value;
		Rect box{rect.x, rect.y + (rect.h - 18) / 2, 18, 18};
		fill(box, value ? theme.accent : theme.surface, 2);
		border(box, focus_ == id ? theme.focus : theme.border, 1, 2);
		if (value) {
			line({box.x + 4, box.y + 9}, {box.x + 8, box.y + 13}, theme.text, 2);
			line({box.x + 8, box.y + 13}, {box.x + 14, box.y + 5}, theme.text, 2);
		}
		label({rect.x + 26, rect.y + (rect.h - theme.lineHeight) / 2, rect.w - 26, theme.lineHeight}, text,
			  enabled ? theme.text : theme.muted);
		return changed;
	}
	bool Context::slider(std::string id, Rect rect, double &value, NumberOptions options, bool enabled,
						 SliderScale scale) {
		if (!std::isfinite(options.minimum) || !std::isfinite(options.maximum) || options.maximum < options.minimum ||
			(scale == SliderScale::Logarithmic && options.minimum <= 0))
			throw std::invalid_argument("Invalid slider limits");
		Widget w;
		w.id          = id;
		w.label       = id;
		w.role        = Role::Slider;
		w.sliderScale = scale;
		w.rect        = rect;
		w.enabled     = enabled;
		w.number      = options;
		w.value       = float(value);
		widget(w);
		bool changed = false;
		for (const auto &s: signals_)
			if (s.id == id && enabled) {
				double next = value;
				if (s.kind == Signal::Drag) next = s.value;
				if (s.kind == Signal::Step) {
					if (scale == SliderScale::Logarithmic)
						next = slider_value(slider_position(value, options, scale) + s.value / 100., options, scale);
					else
						next += s.value * options.step;
				}
				if (s.kind == Signal::Drag || s.kind == Signal::Step) {
					if (options.step > 0)
						next = options.minimum + std::round((next - options.minimum) / options.step) * options.step;
					next = std::clamp(next, options.minimum, options.maximum);
					if (options.integer) next = std::round(next);
					changed |= next != value;
					value = next;
				}
			}
		float fraction = float(slider_position(value, options, scale));
		Rect  track{rect.x + 7, rect.y + rect.h * .5f - 2, std::max(0.f, rect.w - 14), 4};
		fill(track, theme.border, 2);
		fill({track.x, track.y, track.w * std::clamp(fraction, 0.f, 1.f), 4}, enabled ? theme.accent : theme.muted, 2);
		fill({track.x + track.w * std::clamp(fraction, 0.f, 1.f) - 6, rect.y + 3, 12, rect.h - 6},
			 enabled ? theme.text : theme.muted, 3);
		if (focus_ == id) border(rect, theme.focus);
		return changed;
	}
	bool Context::disclosure(std::string id, std::string text, Rect rect, bool &expanded) {
		bool changed = button(std::move(id), (expanded ? "−  " : "+  ") + text, rect);
		if (changed) expanded = !expanded;
		return changed;
	}
	void Context::edit_key(const Input &input, const Widget &w) {
		auto &e               = edits_[w.id];
		auto  erase_selection = [&]() {
			auto [a, b] = std::minmax(e.caret, e.anchor);
			if (a == b) return false;
			e.text.erase(a, b - a);
			e.caret = e.anchor = a;
			return true;
		};
		if (input.kind == InputKind::Text) {
			if (input.text.size() + e.text.size() > 65536) return;
			if (std::any_of(input.text.begin(), input.text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
				return;
			erase_selection();
			e.text.insert(e.caret, input.text);
			e.caret += input.text.size();
			e.anchor = e.caret;
			e.dirty  = true;
			signals_.push_back({w.id, Signal::Change});
			return;
		}
		const auto &key = input.text;
		if (key == "space") {
			auto text = input;
			text.kind = InputKind::Text;
			text.text = " ";
			edit_key(text, w);
			return;
		}
		if (input.control && (key == "c" || key == "C" || key == "x" || key == "X")) {
			auto [a, b] = std::minmax(e.caret, e.anchor);
			if (a != b) clipboard_.push_back({ClipboardRequest::Copy, w.id, e.text.substr(a, b - a)});
			if ((key == "x" || key == "X") && erase_selection()) {
				e.dirty = true;
				signals_.push_back({w.id, Signal::Change});
			}
			return;
		}
		if (input.control && (key == "v" || key == "V")) {
			clipboard_.push_back({ClipboardRequest::Paste, w.id, {}});
			return;
		}
		if (input.control && (key == "a" || key == "A")) {
			e.anchor = 0;
			e.caret  = e.text.size();
			return;
		}
		if (key == "enter") {
			signals_.push_back({w.id, Signal::Commit});
			return;
		}
		if (key == "escape") {
			e.text  = e.original;
			e.caret = e.anchor = e.text.size();
			e.error.clear();
			e.dirty = false;
			signals_.push_back({w.id, Signal::Change});
			return;
		}
		if (key == "left" || key == "right" || key == "home" || key == "end") {
			if (key == "home") e.caret = 0;
			else if (key == "end")
				e.caret = e.text.size();
			else if (!input.shift && e.caret != e.anchor)
				e.caret = key == "left" ? std::min(e.caret, e.anchor) : std::max(e.caret, e.anchor);
			else
				e.caret = key == "left" ? previous_utf8(e.text, e.caret) : next_utf8(e.text, e.caret);
			if (!input.shift) e.anchor = e.caret;
			return;
		}
		if ((key == "up" || key == "down") && w.role == Role::Number) {
			signals_.push_back({w.id, Signal::Step, key == "up" ? 1.0 : -1.0});
			return;
		}
		if (key == "backspace" || key == "delete") {
			if (!erase_selection()) {
				auto a = key == "backspace" ? previous_utf8(e.text, e.caret) : e.caret;
				auto b = key == "backspace" ? e.caret : next_utf8(e.text, e.caret);
				e.text.erase(a, b - a);
				e.caret = e.anchor = a;
			}
			e.dirty = true;
			signals_.push_back({w.id, Signal::Change});
		}
	}
	std::string Context::edit_error(std::string_view id) const {
		auto it = edits_.find(std::string(id));
		return it == edits_.end() ? "" : it->second.error;
	}
	EditResult Context::editor(std::string id, Rect rect, std::string &value, Role role, NumberOptions options,
							   bool enabled, std::string_view placeholder) {
		auto [it, inserted] = edits_.try_emplace(id);
		auto &e             = it->second;
		if (inserted || (focus_ != id && !e.dirty && e.error.empty())) {
			e.text     = value;
			e.original = value;
			e.caret = e.anchor = value.size();
		}
		Widget w;
		w.id      = id;
		w.rect    = rect;
		w.role    = role;
		w.label   = id;
		w.enabled = enabled;
		w.number  = options;
		widget(w);
		EditResult result;
		for (const auto &signal: signals_)
			if (signal.id == id) {
				if (signal.kind == Signal::Step) {
					auto   parsed  = parse_number(e.text, options);
					double current = parsed.value_or(parse_number(value, options).value_or(0));
					e.text         = number_text(
							std::clamp(current + signal.value * options.step, options.minimum, options.maximum),
							options);
					e.caret = e.anchor = e.text.size();
					e.dirty            = true;
					result.committed   = true;
				}
				if (signal.kind == Signal::Commit) result.committed = true;
				if (signal.kind == Signal::Change && role == Role::TextBox) {
					result.changed |= e.text != value;
					value   = e.text;
					e.dirty = false;
				}
			}
		if (result.committed) {
			if (role == Role::Number && !parse_number(e.text, options))
				e.error = "Enter " + std::string(options.integer ? "an integer" : "a number") + " from " +
						  number_text(options.minimum, options) + " to " + number_text(options.maximum, options) + ".";
			else {
				result.changed |= value != e.text;
				value      = e.text;
				e.original = e.text;
				e.error.clear();
				e.dirty = false;
			}
		}
		result.valid = e.error.empty();
		result.error = e.error;
		fill(rect, theme.background, 3);
		border(rect, !result.valid ? theme.danger : focus_ == id ? theme.focus : theme.border, focus_ == id ? 2 : 1, 3);
		auto inside        = inset(rect, theme.padding);
		inside.y           = rect.y + (rect.h - theme.lineHeight) / 2;
		inside.h           = theme.lineHeight;
		const float caretX = measure_(std::string_view(e.text).substr(0, e.caret), theme.fontSize, FontRole::Interface);
		const float textWidth = measure_(e.text, theme.fontSize, FontRole::Interface);
		if (focus_ == id) {
			e.offset = std::clamp(e.offset, 0.f, std::max(0.f, textWidth - inside.w + 2));
			if (caretX < e.offset) e.offset = caretX;
			if (caretX > e.offset + inside.w - 2) e.offset = std::max(0.f, caretX - inside.w + 2);
		} else
			e.offset = 0;
		push_clip(inside);
		auto [a, b] = std::minmax(e.caret, e.anchor);
		if (focus_ == id && a != b) {
			const float left  = measure_(std::string_view(e.text).substr(0, a), theme.fontSize, FontRole::Interface);
			const float right = measure_(std::string_view(e.text).substr(0, b), theme.fontSize, FontRole::Interface);
			fill({inside.x + left - e.offset, inside.y, right - left, inside.h}, theme.accent);
		}
		if (e.text.empty() && !placeholder.empty()) label(inside, std::string(placeholder), theme.muted);
		label({inside.x - e.offset, inside.y, std::max(inside.w + e.offset, textWidth + 2), inside.h}, e.text,
			  enabled ? theme.text : theme.muted, 0, TextAlign::Left, TextOverflow::Clip);
		if (focus_ == id)
			line({inside.x + caretX - e.offset, inside.y + 2}, {inside.x + caretX - e.offset, inside.y + inside.h - 2},
				 theme.text, 1);
		pop_clip();
		return result;
	}
	EditResult Context::text_input(std::string id, Rect rect, std::string &value, bool enabled,
								   std::string_view placeholder) {
		return editor(std::move(id), rect, value, Role::TextBox, {}, enabled, placeholder);
	}
	EditResult Context::number_input(std::string id, Rect rect, double &value, NumberOptions options, bool enabled) {
		if (!std::isfinite(options.minimum) || !std::isfinite(options.maximum) || options.minimum > options.maximum ||
			!std::isfinite(options.step) || options.step <= 0)
			throw std::invalid_argument("Invalid numeric limits");
		auto text   = number_text(value, options);
		auto result = editor(std::move(id), rect, text, Role::Number, options, enabled);
		if (result.committed && result.valid) value = *parse_number(text, options);
		return result;
	}
	bool Context::splitter(std::string id, Rect rect, float &value, Axis axis, float minimum, float maximum,
						   bool reversed) {
		Widget w;
		w.id      = id;
		w.rect    = rect;
		w.role    = Role::Splitter;
		w.axis    = axis;
		w.minimum = minimum;
		w.maximum = std::max(minimum, maximum);
		w.value   = value;
		w.label   = reversed ? "reverse" : "forward";
		widget(w);
		bool changed = false;
		for (const auto &signal: signals_)
			if (signal.id == id && (signal.kind == Signal::Drag || signal.kind == Signal::Step)) {
				value   = bounded(signal.kind == Signal::Drag ? float(signal.value) : value + float(signal.value) * 8,
								  minimum, maximum);
				changed = true;
			}
		if (capture_ == id || contains(intersect(rect, clips_.back()), pointer_)) fill(rect, theme.accent);
		if (focus_ == id) border(rect, theme.focus, 1);
		return changed;
	}
	bool Context::combo(std::string id, Rect rect, std::span<const Choice> choices, std::size_t &selected,
						bool enabled) {
		bool changed = false;
		for (const auto &signal: signals_)
			if (signal.id == id && signal.kind == Signal::Choose && signal.value >= 0 &&
				signal.value < choices.size() && choices[std::size_t(signal.value)].enabled) {
				selected = std::size_t(signal.value);
				changed  = true;
			}
		Widget w;
		w.id      = id;
		w.rect    = rect;
		w.role    = Role::ComboBox;
		w.label   = selected < choices.size() ? choices[selected].label : "Choose...";
		w.enabled = enabled;
		w.choices.assign(choices.begin(), choices.end());
		widget(w);
		fill(rect, theme.surface, 3);
		border(rect, focus_ == id ? theme.focus : theme.border, focus_ == id ? 2 : 1, 3);
		label({rect.x + theme.padding, rect.y + (rect.h - theme.lineHeight) / 2, rect.w - 32, theme.lineHeight},
			  w.label, enabled ? theme.text : theme.muted);
		label({rect.x + rect.w - 24, rect.y + (rect.h - theme.lineHeight) / 2, 20, theme.lineHeight}, "▾", theme.muted);
		if (popup_ != id || !enabled || choices.empty()) return changed;
		const int oldLayer = layer_;
		auto      oldClips = clips_;
		// A portal intentionally escapes parent scrolling clips, but stays within the window.
		for (std::size_t i = 1; i < oldClips.size(); ++i) {
			DrawCommand end;
			end.kind = DrawCommandKind::ScissorEnd;
			commands_.push_back(end);
		}
		clips_                     = {bounds_};
		layer_                     = oldLayer + 100;
		const std::size_t capacity = std::min<std::size_t>(8, choices.size());
		const float       height   = std::min(float(capacity) * theme.rowHeight + 4, bounds_.h);
		const float       width    = std::min(std::max(rect.w, 220.f), bounds_.w);
		const float       x        = bounded(rect.x, bounds_.x, bounds_.x + bounds_.w - width);
		const float       below    = rect.y + rect.h + 3;
		const float       y        = below + height <= bounds_.y + bounds_.h
											 ? below
											 : bounded(rect.y - height - 3, bounds_.y, bounds_.y + bounds_.h - height);
		Rect              menu{x, y, width, height};
		fill(menu, theme.elevated, 3);
		border(menu, theme.border, 1, 3);
		popupIndex_       = std::clamp(popupIndex_, 0, int(choices.size() - 1));
		std::size_t first = popupIndex_ >= int(capacity) ? std::size_t(popupIndex_) - capacity + 1 : 0;
		push_clip(inset(menu, 2));
		for (std::size_t i = first; i < std::min(choices.size(), first + capacity); ++i) {
			Rect   row{x + 2, y + 2 + float(i - first) * theme.rowHeight, width - 4, theme.rowHeight};
			Widget item;
			item.id        = id + "/choice/" + std::to_string(i);
			item.owner     = id;
			item.index     = i;
			item.rect      = row;
			item.role      = Role::MenuItem;
			item.label     = choices[i].label;
			item.enabled   = choices[i].enabled;
			item.focusable = false;
			widget(item);
			if (int(i) == popupIndex_ || contains(row, pointer_)) fill(row, theme.accent);
			label(inset(row, theme.padding), choices[i].label, choices[i].enabled ? theme.text : theme.muted);
		}
		pop_clip();
		clips_ = std::move(oldClips);
		layer_ = oldLayer;
		for (std::size_t i = 1; i < clips_.size(); ++i) {
			DrawCommand begin;
			begin.kind = DrawCommandKind::ScissorBegin;
			begin.rect = clips_[i];
			commands_.push_back(begin);
		}
		return changed;
	}
	void Context::scrollbar(const std::string &id, Rect viewport, Point content, Axis axis) {
		const float visible = axis == Axis::Horizontal ? viewport.w : viewport.h,
					total   = axis == Axis::Horizontal ? content.x : content.y;
		if (total <= visible || visible <= 0) return;
		auto       &state  = scrolls_[id];
		float      &scroll = axis == Axis::Horizontal ? state.x : state.y;
		const float thumb  = std::min(visible, std::max(24.f, visible * visible / total));
		const float travel = visible - thumb, offset = scroll / (total - visible) * travel;
		Rect        r = axis == Axis::Horizontal ? Rect{viewport.x + offset, viewport.y + viewport.h - 8, thumb, 7}
												 : Rect{viewport.x + viewport.w - 8, viewport.y + offset, 7, thumb};
		Widget      w;
		w.id        = id + (axis == Axis::Horizontal ? "/scroll-x" : "/scroll-y");
		w.owner     = id;
		w.role      = Role::ScrollBar;
		w.rect      = r;
		w.axis      = axis;
		w.minimum   = travel;
		w.maximum   = total - visible;
		w.value     = scroll;
		w.focusable = false;
		widget(w);
		fill(r, capture_ == w.id ? theme.accent : theme.border, 3);
	}
	ScrollRegion Context::begin_scroll(std::string id, Rect viewport, Point content) {
		auto &state = scrolls_[id];
		content.x   = std::max(viewport.w, content.x);
		content.y   = std::max(viewport.h, content.y);
		state.x     = bounded(state.x, 0, content.x - viewport.w);
		state.y     = bounded(state.y, 0, content.y - viewport.h);
		Widget w;
		w.id        = id;
		w.role      = Role::ScrollArea;
		w.rect      = viewport;
		w.focusable = false;
		w.minimum   = content.x - viewport.w;
		w.maximum   = content.y - viewport.h;
		widget(w);
		scrollScopes_.push_back({id, viewport, content});
		push_clip(viewport);
		return {viewport, {viewport.x - state.x, viewport.y - state.y, content.x, content.y}};
	}
	void Context::end_scroll() {
		if (scrollScopes_.empty()) throw std::logic_error("Unmatched scrolling region");
		const auto scope = scrollScopes_.back();
		scrollScopes_.pop_back();
		pop_clip();
		scrollbar(scope.id, scope.viewport, scope.contentSize, Axis::Horizontal);
		scrollbar(scope.id, scope.viewport, scope.contentSize, Axis::Vertical);
	}
	TableResult Context::table(std::string id, Rect rect, std::vector<Column> &columns, const TableModel &model) {
		if (!model.rowId || !model.cell || !model.indexOf) throw std::invalid_argument("Incomplete table model");
		TableResult result;
		auto       &state  = tables_[id];
		auto       &scroll = scrolls_[id + "/body"];
		for (auto &column: columns)
			for (const auto &s: signals_)
				if (s.id == id + "/resize/" + column.id && s.kind == Signal::Drag)
					column.width = std::max(column.minimum, float(s.value));
		for (const auto &s: signals_)
			if (s.id == id) {
				if (s.kind == Signal::Sort) {
					const auto index = std::size_t(s.value);
					if (index < columns.size()) {
						const auto &key = columns[index].id;
						if (state.sortColumn == key) state.ascending = !state.ascending;
						else {
							state.sortColumn = key;
							state.ascending  = true;
						}
						result.sortChanged = true;
					}
				}
				if (s.kind == Signal::Select) {
					auto index = model.indexOf(s.row);
					if (!index) continue;
					if (!s.control) state.selection.clear();
					if (s.shift && state.anchor) {
						if (auto anchor = model.indexOf(*state.anchor))
							for (std::size_t n = std::min(*anchor, *index); n <= std::max(*anchor, *index); ++n)
								state.selection.insert(model.rowId(n));
					} else {
						if (s.control && state.selection.contains(s.row)) state.selection.erase(s.row);
						else
							state.selection.insert(s.row);
						state.anchor = s.row;
					}
					state.active            = s.row;
					result.selectionChanged = true;
				}
				if (s.kind == Signal::Navigate && model.count) {
					auto       current = state.active ? model.indexOf(*state.active).value_or(0) : 0;
					const auto next = std::size_t(std::clamp(double(current) + s.value, 0.0, double(model.count - 1)));
					state.active    = model.rowId(next);
					state.anchor    = state.active;
					state.selection = {*state.active};
					result.selectionChanged = true;
					if (next * theme.rowHeight < scroll.y) scroll.y = next * theme.rowHeight;
					if ((next + 1) * theme.rowHeight > scroll.y + rect.h - theme.rowHeight)
						scroll.y = (next + 1) * theme.rowHeight - (rect.h - theme.rowHeight);
				}
			}
		float totalWidth = 0;
		for (auto &column: columns)
			if (column.visible) {
				column.width = std::max(column.width, column.minimum);
				totalWidth += column.width;
			}
		const float header = theme.rowHeight;
		Rect        body{rect.x, rect.y + header, rect.w, std::max(0.f, rect.h - header)};
		fill(rect, theme.background);
		border(rect, focus_ == id ? theme.focus : theme.border);
		Widget tableWidget;
		tableWidget.id    = id;
		tableWidget.rect  = rect;
		tableWidget.role  = Role::Table;
		tableWidget.label = "Results table";
		widget(tableWidget);
		scroll.x = bounded(scroll.x, 0, totalWidth - body.w);
		push_clip({rect.x, rect.y, rect.w, header});
		float x = rect.x - scroll.x;
		for (std::size_t col = 0; col < columns.size(); ++col) {
			auto &column = columns[col];
			if (!column.visible) continue;
			Rect cell{x, rect.y, column.width, header};
			if (valid_rect(intersect(cell, {rect.x, rect.y, rect.w, header}))) {
				Widget w;
				w.id        = id + "/header/" + column.id;
				w.owner     = id;
				w.index     = col;
				w.rect      = cell;
				w.role      = Role::ColumnHeader;
				w.label     = column.title;
				w.enabled   = column.sortable;
				w.focusable = column.sortable;
				widget(w);
				fill(cell, theme.surface);
				label(inset(cell, theme.padding),
					  column.title + (state.sortColumn == column.id ? (state.ascending ? " ↑" : " ↓") : ""), theme.text,
					  0, column.align);
				const std::string resizeId = id + "/resize/" + column.id;
				++layer_;
				splitter(resizeId, {x + column.width - 4, rect.y, 8, header}, column.width, Axis::Horizontal,
						 column.minimum, 2000);
				--layer_;
			}
			x += column.width;
		}
		pop_clip();
		const float contentHeight =
				std::min(double(std::numeric_limits<float>::max() / 2), double(model.count) * theme.rowHeight);
		auto region    = begin_scroll(id + "/body", body, {totalWidth, contentHeight});
		result.visible = region.rows(model.count, theme.rowHeight);
		for (std::size_t row = result.visible.begin; row < result.visible.end; ++row) {
			const auto key = model.rowId(row);
			Rect       area{body.x, region.content.y + float(row) * theme.rowHeight, body.w, theme.rowHeight};
			Widget     w;
			w.id        = id + "/row/" + std::to_string(key);
			w.owner     = id;
			w.rowId     = key;
			w.role      = Role::Row;
			w.rect      = area;
			w.focusable = false;
			widget(w);
			if (state.selection.contains(key)) fill(area, theme.accent);
			else if (row % 2)
				fill(area, theme.surface);
			else if (contains(intersect(area, body), pointer_))
				fill(area, theme.elevated);
			x = region.content.x;
			for (std::size_t col = 0; col < columns.size(); ++col) {
				const auto &column = columns[col];
				if (!column.visible) continue;
				Rect cell{x, area.y, column.width, area.h};
				if (valid_rect(intersect(cell, body))) {
					push_clip(cell);
					if (!model.drawCell || !model.drawCell(row, col, cell))
						label(inset(cell, theme.padding), model.cell(row, col), theme.text, 0, column.align);
					pop_clip();
				}
				x += column.width;
			}
		}
		end_scroll();
		return result;
	}
	std::optional<uint64_t> Context::plot(std::string id, Rect rect, std::span<const PlotPoint> points,
										  const PlotAxes &axes, std::optional<uint64_t> selected) {
		auto transform = [](double v, bool log) { return log ? std::log10(v) : v; };
		if (!std::isfinite(axes.xMin) || !std::isfinite(axes.xMax) || !std::isfinite(axes.yMin) ||
			!std::isfinite(axes.yMax) || axes.xMin >= axes.xMax || axes.yMin >= axes.yMax ||
			(axes.logarithmicX && axes.xMin <= 0) || (axes.logarithmicY && axes.yMin <= 0))
			throw std::invalid_argument("Invalid plot axes");
		fill(rect, theme.background);
		border(rect, theme.border);
		Rect         area{rect.x + 72, rect.y + 16, std::max(0.f, rect.w - 92), std::max(0.f, rect.h - 68)};
		const double xmin = transform(axes.xMin, axes.logarithmicX), xmax = transform(axes.xMax, axes.logarithmicX),
					 ymin = transform(axes.yMin, axes.logarithmicY), ymax = transform(axes.yMax, axes.logarithmicY);
		auto         tick = [](double value, double step) {
			std::ostringstream out;
			int                places = step > 0 ? std::clamp(int(std::ceil(-std::log10(step))) + 1, 0, 6) : 3;
			out << std::fixed << std::setprecision(places) << value;
			auto text = out.str();
			if (text.size() > 9) {
				out.str("");
				out.clear();
				out << std::scientific << std::setprecision(2) << value;
				text = out.str();
			}
			return text;
		};
		for (int i = 0; i <= 4; ++i) {
			const float t = float(i) / 4, x = area.x + t * area.w, y = area.y + area.h - t * area.h;
			line({x, area.y}, {x, area.y + area.h}, theme.border);
			line({area.x, y}, {area.x + area.w, y}, theme.border);
			const double xv = axes.logarithmicX ? std::pow(10, xmin + t * (xmax - xmin)) : xmin + t * (xmax - xmin);
			const double yv = axes.logarithmicY ? std::pow(10, ymin + t * (ymax - ymin)) : ymin + t * (ymax - ymin);
			label({x - 55, area.y + area.h + 5, 110, theme.lineHeight}, tick(xv, (axes.xMax - axes.xMin) / 4),
				  theme.muted, 13, TextAlign::Center);
			label({rect.x + 2, y - 10, 62, theme.lineHeight}, tick(yv, (axes.yMax - axes.yMin) / 4), theme.muted, 13,
				  TextAlign::Right);
		}
		label({area.x, rect.y + rect.h - 24, area.w, 22}, axes.xLabel, theme.muted, 0, TextAlign::Center);
		label({area.x + 4, area.y + 4, area.w - 8, 22}, axes.yLabel, theme.muted);
		push_clip(area);
		for (const auto &p: points) {
			if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < axes.xMin || p.x > axes.xMax || p.y < axes.yMin ||
				p.y > axes.yMax)
				continue;
			Point  screen{area.x + float((transform(p.x, axes.logarithmicX) - xmin) / (xmax - xmin)) * area.w,
						  area.y + area.h - float((transform(p.y, axes.logarithmicY) - ymin) / (ymax - ymin)) * area.h};
			Widget w;
			w.id        = id + "/point/" + std::to_string(p.id);
			w.owner     = id;
			w.rowId     = p.id;
			w.role      = Role::Plot;
			w.rect      = {screen.x - 6, screen.y - 6, 12, 12};
			w.focusable = false;
			widget(w);
			point(screen, selected == p.id ? theme.focus : theme.accent, selected == p.id ? 5 : 3);
			if (!p.label.empty()) tooltip(w.rect, p.label);
		}
		pop_clip();
		for (const auto &s: signals_)
			if (s.id == id && s.kind == Signal::Select) return s.row;
		return {};
	}
	void Context::position_caret(Edit &e, const Widget &w, float pointerX) {
		const float x     = pointerX - w.rect.x - theme.padding + e.offset;
		std::size_t caret = 0;
		float       prior = 0;
		while (caret < e.text.size()) {
			const auto  next = next_utf8(e.text, caret);
			const float advance =
					measure_(std::string_view(e.text).substr(0, next), theme.fontSize, FontRole::Interface);
			if (x < (prior + advance) / 2) break;
			caret = next;
			prior = advance;
		}
		e.caret = caret;
	}
	std::vector<ClipboardRequest> Context::take_clipboard_requests() {
		auto result = std::move(clipboard_);
		clipboard_.clear();
		return result;
	}
	bool Context::paste(std::string_view id, std::string text) {
		const auto *w = find(id);
		if (!w || !w->enabled || focus_ != id || (w->role != Role::TextBox && w->role != Role::Number)) return false;
		for (auto &c: text)
			if (c == '\n' || c == '\r' || c == '\t') c = ' ';
		Input input;
		input.kind = InputKind::Text;
		input.text = std::move(text);
		edit_key(input, *w);
		return true;
	}
	bool Context::handle(const Input &input) {
		if (input.kind == InputKind::FocusLost) {
			set_focus({});
			capture_.clear();
			popup_.clear();
			suppressRelease_ = false;
			return true;
		}
		if (input.kind == InputKind::Press || input.kind == InputKind::Scroll || input.kind == InputKind::Key) tooltipSuppressed_ = true;
		if (input.kind == InputKind::Move && (std::abs(input.point.x - pointer_.x) > 1 || std::abs(input.point.y - pointer_.y) > 1)) {
			tooltipSuppressed_ = false; tooltipSince_ = std::chrono::steady_clock::now();
		}
		if (input.kind == InputKind::Move || input.kind == InputKind::Press || input.kind == InputKind::Release ||
			input.kind == InputKind::Scroll)
			pointer_ = input.point;
		auto top = [&]() -> const Widget * {
			const Widget *hit = nullptr;
			for (const auto &w: widgets_)
				if (contains(w.clip, input.point) && (!hit || w.layer >= hit->layer)) hit = &w;
			return hit;
		};
		if (input.kind == InputKind::Move) {
			if (const auto *w = find(capture_)) {
				if (w->role == Role::Slider)
					signals_.push_back({w->id, Signal::Drag, slider_pointer_value(*w, input.point)});
				if (w->role == Role::Splitter || w->role == Role::ScrollBar) {
					float delta = w->axis == Axis::Horizontal ? input.point.x - pressed_.x : input.point.y - pressed_.y;
					if (w->role == Role::ScrollBar) {
						auto &s     = scrolls_[w->owner];
						float value = bounded(capturedValue_ + (w->minimum > 0 ? delta * w->maximum / w->minimum : 0),
											  0, w->maximum);
						(w->axis == Axis::Horizontal ? s.x : s.y) = value;
					} else
						signals_.push_back({w->id, Signal::Drag, capturedValue_ + (capturedReverse_ ? -delta : delta)});
				}
				if (w->role == Role::TextBox || w->role == Role::Number)
					position_caret(edits_[w->id], *w, input.point.x);
				return true;
			}
			return top() != nullptr;
		}
		if (input.kind == InputKind::Scroll) {
			if (!popup_.empty()) {
				if (const auto *w = find(popup_))
					popupIndex_ = std::clamp(popupIndex_ + (input.delta.y > 0 ? 1 : -1), 0, int(w->choices.size()) - 1);
				return true;
			}
			// Deepest scroll regions consume what they can; the remainder bubbles.
			Point remaining = input.delta;
			bool  used      = false;
			for (auto it = widgets_.rbegin(); it != widgets_.rend(); ++it)
				if (it->role == Role::ScrollArea && contains(it->clip, input.point)) {
					auto       &s = scrolls_[it->id];
					const float x = s.x, y = s.y;
					s.x = bounded(s.x + remaining.x, 0, it->minimum);
					s.y = bounded(s.y + remaining.y, 0, it->maximum);
					remaining.x -= s.x - x;
					remaining.y -= s.y - y;
					used |= s.x != x || s.y != y;
				}
			return used;
		}
		if (input.kind == InputKind::Press) {
			if (input.button != 272) return false;
			const auto *w = top();
			if (!popup_.empty() && (!w || (w->id != popup_ && w->owner != popup_))) {
				popup_.clear();
				capture_.clear();
				suppressRelease_ = true;
				return true;
			}
			if (!w) {
				set_focus({});
				return false;
			}
            if (!w->enabled && !popup_.empty()) {
                suppressRelease_ = true;
                return true;
            }
            // Scroll/table backgrounds only react to scrolling or keys. A left
            // press on them (or on disabled controls) is available to the window.
            if(!w->enabled || w->role==Role::ScrollArea || w->role==Role::Table){
                set_focus({});
                return false;
            }
			if (w->role == Role::Row) set_focus(w->owner);
			else if (w->focusable)
				set_focus(w->id);
			capture_         = w->id;
			pressed_         = input.point;
			capturedValue_   = w->value;
			capturedReverse_ = w->label == "reverse";
			if (w->role == Role::Slider)
				signals_.push_back({w->id, Signal::Drag, slider_pointer_value(*w, input.point)});
			if (w->role == Role::TextBox || w->role == Role::Number) {
				auto &e = edits_[w->id];
				position_caret(e, *w, input.point.x);
				if (!input.shift) e.anchor = e.caret;
			}
			return true;
		}
		if (input.kind == InputKind::Release) {
			if (input.button != 272) return false;
			if (suppressRelease_) {
				suppressRelease_ = false;
				return true;
			}
			const auto *w = find(capture_);
			capture_.clear();
			if (!w) return false;
			if (!contains(w->clip, input.point) || !w->enabled) return true;
			if (w->role == Role::MenuItem) {
				signals_.push_back({w->owner, Signal::Choose, double(w->index)});
				popup_.clear();
			} else if (w->role == Role::ComboBox) {
				popup_      = popup_ == w->id ? "" : w->id;
				popupIndex_ = 0;
			} else if (w->role == Role::ColumnHeader)
				signals_.push_back({w->owner, Signal::Sort, double(w->index)});
			else if (w->role == Role::Row || w->role == Role::Plot)
				signals_.push_back({w->owner, Signal::Select, 0, w->rowId, input.shift, input.control});
			else if (w->role == Role::Button || w->role == Role::CheckBox)
				signals_.push_back({w->id, Signal::Activate});
			return true;
		}
		if (input.kind == InputKind::Key || input.kind == InputKind::Text) {
			if (!popup_.empty()) {
				const auto *w = find(popup_);
				if (!w) {
					popup_.clear();
					return true;
				}
				if (input.kind == InputKind::Key) {
					if (input.text == "escape") {
						popup_.clear();
						return true;
					}
					if (input.text == "enter" || input.text == "space") {
						if (popupIndex_ >= 0 && popupIndex_ < int(w->choices.size()) &&
							w->choices[popupIndex_].enabled) {
							signals_.push_back({w->id, Signal::Choose, double(popupIndex_)});
							popup_.clear();
						}
						return true;
					}
					if (input.text == "up" || input.text == "down") {
						const int direction = input.text == "up" ? -1 : 1;
						for (int i = popupIndex_ + direction; i >= 0 && i < int(w->choices.size()); i += direction)
							if (w->choices[i].enabled) {
								popupIndex_ = i;
								break;
							}
					}
					if (input.text == "tab") popup_.clear();
					else
						return true;
				} else
					return true;
			}
			if (input.kind == InputKind::Key && input.text == "tab") {
				std::vector<const Widget *> order;
				for (const auto &w: widgets_)
					if (w.enabled && w.focusable && valid_rect(w.clip)) order.push_back(&w);
				if (order.empty()) return false;
				auto it    = std::find_if(order.begin(), order.end(), [&](auto *w) { return w->id == focus_; });
				auto index = it == order.end() ? (input.shift ? 0 : -1) : int(it - order.begin());
				index      = (index + (input.shift ? -1 : 1) + int(order.size())) % int(order.size());
				set_focus(order[index]->id);
				return true;
			}
			const auto *w = find(focus_);
			if (!w || !w->enabled) return false;
			if (w->role == Role::TextBox || w->role == Role::Number) {
				edit_key(input, *w);
				return true;
			}
			if (input.kind != InputKind::Key) return false;
			if (w->role == Role::Slider) {
				if (input.text == "left" || input.text == "down" || input.text == "right" || input.text == "up")
					signals_.push_back(
							{w->id, Signal::Step, (input.text == "left" || input.text == "down") ? -1. : 1.});
				else if (input.text == "home" || input.text == "end")
					signals_.push_back(
							{w->id, Signal::Drag, input.text == "home" ? w->number.minimum : w->number.maximum});
				return true;
			}
			if (w->role == Role::Table) {
				if (input.text == "up" || input.text == "down" || input.text == "pageup" || input.text == "pagedown")
					signals_.push_back({w->id, Signal::Navigate,
										input.text == "up"       ? -1.0
										: input.text == "down"   ? 1.0
										: input.text == "pageup" ? -10.0
																 : 10.0});
				return true;
			}
			if (w->role == Role::Splitter &&
				(input.text == "left" || input.text == "right" || input.text == "up" || input.text == "down")) {
				const float sign = (input.text == "left" || input.text == "up") ? -1 : 1;
				signals_.push_back({w->id, Signal::Step, sign * (w->label == "reverse" ? -1 : 1)});
				return true;
			}
			if (input.text == "space" || input.text == "enter") {
				if (w->role == Role::ComboBox) {
					popup_      = w->id;
					popupIndex_ = 0;
				} else if (w->role == Role::ColumnHeader)
					signals_.push_back({w->owner, Signal::Sort, double(w->index)});
				else
					signals_.push_back({w->id, Signal::Activate});
				return true;
			}
		}
		return false;
	}
} // namespace codec_gui::gui::ui
