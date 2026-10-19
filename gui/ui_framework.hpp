#pragma once
#include "draw_list.hpp"
#include "ui_geometry.hpp"
#include <functional>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_set>
namespace codec_gui::gui::ui {
	enum class Axis { Horizontal, Vertical };
	enum class SliderScale { Linear, Logarithmic };
	struct Theme {
		Color background{.025f, .028f, .033f, 1}, surface{.06f, .065f, .075f, 1}, elevated{.1f, .11f, .125f, 1};
		Color text{.84f, .86f, .89f, 1}, muted{.4f, .44f, .49f, 1}, border{.17f, .19f, .22f, 1};
		Color accent{.12f, .43f, .65f, 1}, focus{.25f, .65f, .9f, 1}, danger{.95f, .25f, .2f, 1};
		float fontSize = 16, lineHeight = 23, rowHeight = 32, padding = 8, gap = 8;
	};
	struct DockSizes {
		float left = 260, right = 400, bottom = 340, bar = 44, status = 28;
		bool  focus = false;
	};
	struct DockLayout {
		Rect bar, status, left, right, bottom, canvas, leftHandle, rightHandle, bottomHandle;
	};
	[[nodiscard]] DockLayout dock_layout(Rect bounds, DockSizes sizes, float minimumCanvasWidth = 480,
										 float minimumCanvasHeight = 240);
	struct Track {
		float minimum = 0, preferred = 0, maximum = 1e6f, flex = 0;
	};
	[[nodiscard]] std::vector<Rect> layout_tracks(Rect bounds, Axis axis, std::span<const Track> tracks, float gap = 8);
	struct VisibleRows {
		std::size_t begin = 0, end = 0;
	};
	[[nodiscard]] VisibleRows visible_rows(std::size_t count, float scroll, float viewportHeight, float rowHeight,
										   std::size_t overscan = 1);
	enum class InputKind { Move, Press, Release, Scroll, Key, KeyRelease, Text, FocusLost };
	struct Input {
		InputKind   kind = InputKind::Move;
		Point       point{}, delta{};
		std::string text;
		uint32_t    button = 272;
		bool        shift = false, control = false, alt = false;
	};
	[[nodiscard]] std::optional<Input> input_from_action(const Action &action);
	struct NumberOptions {
		double minimum = -1e12, maximum = 1e12, step = 1;
		bool   integer  = false;
		int    decimals = 3; // Explicit fixed-point display precision; independent of parsing.
	};
	struct EditResult {
		bool        changed = false, committed = false, valid = true;
		std::string error;
	};
	struct ClipboardRequest {
		enum Kind { Copy, Paste } kind;
		std::string widget, text;
	};
	struct Choice {
		std::string label;
		bool        enabled = true;
	};
	struct Column {
		std::string id, title;
		float       width = 150, minimum = 60;
		TextAlign   align    = TextAlign::Left;
		bool        visible  = true;
		bool        sortable = true;
	};
	struct TableModel {
		std::size_t                                          count = 0;
		std::function<uint64_t(std::size_t)>                 rowId;
		std::function<std::optional<std::size_t>(uint64_t)>  indexOf;
		std::function<std::string(std::size_t, std::size_t)> cell;
		std::function<bool(std::size_t, std::size_t, Rect)>  drawCell;
	};
	struct TableState {
		std::set<uint64_t>      selection;
		std::optional<uint64_t> active, anchor;
		std::string             sortColumn;
		bool                    ascending = true;
	};
	struct TableResult {
		bool        selectionChanged = false, sortChanged = false;
		VisibleRows visible;
	};
	struct PlotPoint {
		uint64_t id;
		double   x, y;
		std::string label;
	};
	struct PlotAxes {
		double      xMin = 0, xMax = 1, yMin = 0, yMax = 1;
		bool        logarithmicX = false, logarithmicY = false;
		std::string xLabel, yLabel;
	};
	struct ScrollState {
		float x = 0, y = 0;
	};
	struct ScrollRegion {
		Rect        viewport, content;
		VisibleRows rows(std::size_t count, float rowHeight) const;
	};
	enum class Role {
		Button,
		CheckBox,
		TextBox,
		Number,
		Slider,
		ComboBox,
		Table,
		Row,
		ColumnHeader,
		Splitter,
		ScrollBar,
		ScrollArea,
		Plot,
		MenuItem
	};
	struct Widget {
		std::string         id, label;
		Role                role = Role::Button;
		Rect                rect, clip;
		int                 layer   = 0;
		bool                enabled = true, focusable = true;
		std::string         owner;
		std::size_t         index   = 0;
		uint64_t            rowId   = 0;
		Axis                axis    = Axis::Horizontal;
		float               minimum = 0, maximum = 0, value = 0;
		NumberOptions       number;
		SliderScale         sliderScale = SliderScale::Linear;
		std::vector<Choice> choices;
	};
	// Frame-local declarations; input is routed against the last committed frame.
	// Widgets never retain pointers to application model fields between frames.
	class Context {
	public:
		using Measure = std::function<float(std::string_view, float, FontRole)>;
		explicit Context(Measure measure);
		Theme                                         theme;
		void                                          begin_frame(Rect bounds);
		void                                          end_frame();
		bool                                          handle(const Input &input);
		[[nodiscard]] std::vector<ClipboardRequest>   take_clipboard_requests();
		bool                                          paste(std::string_view widget, std::string text);
		[[nodiscard]] const std::vector<DrawCommand> &commands() const { return commands_; }
		[[nodiscard]] const std::vector<Widget>      &widgets() const { return widgets_; }
		[[nodiscard]] const std::string              &focus() const { return focus_; }
		[[nodiscard]] const std::string              &capture() const { return capture_; }
		[[nodiscard]] std::string                     edit_error(std::string_view id) const;
		void                                          push_clip(Rect rect);
		void                                          pop_clip();
		void                                          fill(Rect rect, Color color, float radius = 0, int layer = -1);
		void                                          border(Rect rect, Color color, float width = 1, float radius = 0);
		void                                          line(Point from, Point to, Color color, float width = 1);
		void                                          point(Point center, Color color, float radius = 3);
		void                                          image(Rect rect, ImageId image, Rect uv = {0, 0, 1, 1});
		void         image_difference(Rect rect, ImageId first, ImageId second, float gain);
		void         label(Rect rect, std::string text, Color color, float size = 0, TextAlign align = TextAlign::Left,
						   TextOverflow overflow = TextOverflow::Ellipsis, FontRole font = FontRole::Interface);
		void         tooltip(Rect target, std::string text);
		bool         button(std::string id, std::string label, Rect rect, bool enabled = true, bool selected = false);
		bool         checkbox(std::string id, std::string label, Rect rect, bool &value, bool enabled = true);
		EditResult   text_input(std::string id, Rect rect, std::string &value, bool enabled = true,
								std::string_view placeholder = {});
		EditResult   number_input(std::string id, Rect rect, double &value, NumberOptions options, bool enabled = true);
		bool         slider(std::string id, Rect rect, double &value, NumberOptions options, bool enabled = true,
							SliderScale scale = SliderScale::Linear);
		bool         combo(std::string id, Rect rect, std::span<const Choice> choices, std::size_t &selected,
						   bool enabled = true);
		bool         disclosure(std::string id, std::string label, Rect rect, bool &expanded);
		bool         splitter(std::string id, Rect rect, float &value, Axis axis, float minimum, float maximum,
							  bool reversed = false);
		ScrollRegion begin_scroll(std::string id, Rect viewport, Point contentSize);
		void         end_scroll();
		ScrollState &scroll_state(const std::string &id) { return scrolls_[id]; }
		TableState  &table_state(const std::string &id) { return tables_[id]; }
		TableResult  table(std::string id, Rect rect, std::vector<Column> &columns, const TableModel &model);
		std::optional<uint64_t> plot(std::string id, Rect rect, std::span<const PlotPoint> points, const PlotAxes &axes,
									 std::optional<uint64_t> selected = {});

	private:
		struct Edit {
			std::string text, original, error;
			std::size_t caret = 0, anchor = 0;
			float       offset = 0;
			bool        dirty  = false;
		};
		struct Signal {
			std::string id;
			enum Kind { Activate, Commit, Change, Drag, Choose, Sort, Select, Step, Navigate } kind;
			double   value = 0;
			uint64_t row   = 0;
			bool     shift = false, control = false;
		};
		struct ScrollScope {
			std::string id;
			Rect        viewport;
			Point       contentSize;
		};
		Measure                            measure_;
		Rect                               bounds_;
		std::vector<DrawCommand>           commands_;
		std::vector<Widget>                widgets_, building_;
		std::vector<Rect>                  clips_;
		std::vector<ScrollScope>           scrollScopes_;
		std::vector<Signal>                signals_;
		std::vector<ClipboardRequest>      clipboard_;
		std::map<std::string, Edit>        edits_;
		std::map<std::string, ScrollState> scrolls_;
		std::map<std::string, TableState>  tables_;
		std::unordered_set<std::string>    ids_;
		std::string                        focus_, capture_, popup_, tooltip_;
		std::string                        tooltipHover_;
		std::chrono::steady_clock::time_point tooltipSince_{};
		bool tooltipSuppressed_ = false;
		Point                              pointer_, pressed_;
		float                              capturedValue_ = 0;
		int                                layer_ = 0, popupIndex_ = 0;
		bool                               capturedReverse_ = false, suppressRelease_ = false;
		const Widget                      *find(std::string_view id) const;
		void                               widget(Widget info);
		bool                               activated(std::string_view id) const;
		void                               set_focus(std::string id);
		void                               edit_key(const Input &, const Widget &);
		void                               position_caret(Edit &, const Widget &, float x);
		EditResult editor(std::string id, Rect rect, std::string &value, Role role, NumberOptions options, bool enabled,
						  std::string_view placeholder = {});
		void       scrollbar(const std::string &id, Rect viewport, Point contentSize, Axis axis);
	};
} // namespace codec_gui::gui::ui
