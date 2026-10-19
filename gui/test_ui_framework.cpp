#include "test_check.hpp"
#include "ui_framework.hpp"
#include "ui_render_frame.hpp"
#include <cmath>
#include <iostream>
#include <numeric>
#include <thread>
using namespace codec_gui::gui;
using namespace codec_gui::gui::ui;
#define CHECK CODEC_VIS_CHECK
static Context context() {
	return Context([](std::string_view text, float size, FontRole) { return float(text.size()) * size / 2; });
}
static void click(Context &ui, Point p, bool control = false, bool shift = false) {
	Input e;
	e.kind    = InputKind::Press;
	e.point   = p;
	e.control = control;
	e.shift   = shift;
	CHECK(ui.handle(e));
	e.kind = InputKind::Release;
	CHECK(ui.handle(e));
}
static void key(Context &ui, std::string key, bool control = false, bool shift = false) {
	Input e;
	e.kind    = InputKind::Key;
	e.text    = std::move(key);
	e.control = control;
	e.shift   = shift;
	CHECK(ui.handle(e));
}
static void text(Context &ui, std::string text) {
	Input e;
	e.kind = InputKind::Text;
	e.text = std::move(text);
	CHECK(ui.handle(e));
}
int main() {
    const Rect native{0, 0, 2880, 1800};
    {
        auto ui=context();
        ui.begin_frame(native);
        ui.begin_scroll("background",{0,0,400,400},{400,800});
        ui.button("active","Active",{20,20,100,32});
        ui.button("inactive","Inactive",{20,70,100,32},false);
        ui.end_scroll();
        std::vector<Column> columns{{.id="name",.title="Name",.width=200}};
        TableModel empty;
        empty.rowId=[](std::size_t row){return uint64_t(row);};
        empty.indexOf=[](uint64_t)->std::optional<std::size_t>{return std::nullopt;};
        empty.cell=[](std::size_t,std::size_t){return std::string{};};
        ui.table("empty",{500,0,400,300},columns,empty);
        ui.end_frame();
        Input press;press.kind=InputKind::Press;
        for(auto point:{Point{200,200},Point{30,80},Point{550,150}}){
            press.point=point;CHECK(!ui.handle(press));CHECK(ui.capture().empty());
        }
        press.point={30,30};CHECK(ui.handle(press));CHECK(ui.capture()=="active");
        Input release=press;release.kind=InputKind::Release;CHECK(ui.handle(release));
        Input scroll;scroll.kind=InputKind::Scroll;scroll.point={200,200};scroll.delta={0,40};
        CHECK(ui.handle(scroll));CHECK(ui.scroll_state("background").y==40);
    }
	{
		auto dock = dock_layout(native, {});
		CHECK(dock.canvas.w >= 480 && dock.canvas.h >= 240);
		CHECK(dock.bottom.y + dock.bottom.h == dock.status.y);
		CHECK(dock.canvas.x == dock.left.w);
		DockSizes sizes;
		sizes.focus = true;
		auto focus  = dock_layout(native, sizes);
		CHECK(focus.canvas.w == 2880 && focus.left.w == 0 && focus.bottom.h == 0);
		auto small = dock_layout({17, 21, 120, 90}, {});
		CHECK(small.canvas.w >= 0 && small.canvas.h >= 0 && small.status.y >= 21);
		std::vector<Track> tracks{{30, 50, 100, 1}, {20, 40, 80, 2}, {10, 10, 10, 0}};
		auto               cells = layout_tracks({0, 0, 250, 32}, Axis::Horizontal, tracks, 8);
		CHECK(cells.size() == 3 && cells[0].w == 100 && cells[1].w == 80 && cells[2].w == 10);
		cells = layout_tracks({0, 0, 80, 32}, Axis::Horizontal, tracks, 8);
		CHECK(cells[0].w >= 30 && cells[1].w >= 20 && cells[2].w == 10);
		auto rows = visible_rows(100000, 320000, 320, 32);
		CHECK(rows.begin == 9999 && rows.end == 10011);
		CHECK(visible_rows(0, 0, 320, 32).end == 0);
	}
	{
		DrawCommand a;
		a.kind = DrawCommandKind::ScissorBegin;
		a.rect = {10, 10, 100, 100};
		auto b = a;
		b.rect = {50, 50, 100, 100};
		DrawCommand end;
		end.kind = DrawCommandKind::ScissorEnd;
		DrawCommand solid;
		solid.rect        = {0, 0, 300, 300};
		solid.color       = {1, 0, 0, 1};
		DrawCommand label = solid;
		label.kind        = DrawCommandKind::Text;
		label.layer       = 7;
		auto scene        = compile_paint_scene(std::vector<DrawCommand>{a, b, label, solid, end, end});
		CHECK(scene.size() == 2 && scene[0].commandIndex == 3 && scene[1].commandIndex == 2);
		CHECK(scene[0].clip.x == 50 && scene[0].clip.w == 60);
		bool rejected = false;
		try {
			compile_paint_scene(std::vector<DrawCommand>{end});
		} catch (const std::invalid_argument &) { rejected = true; }
		CHECK(rejected);
		std::vector<DrawCommand> boxes(10000, solid);
		auto                     batched = compile_ui_frame(boxes, nullptr);
		CHECK(batched.instances.size() == 10000 && batched.batches.size() == 1);
		DrawCommand txt = solid;
		txt.kind        = DrawCommandKind::Text;
		txt.text        = "text";
		TextAtlas atlas;
		atlas.quads.push_back({{0, 0, 10, 10}, {0, 0, 1, 1}, {1, 1, 1, 1}, false, 0, unlimited_clip, 0});
		auto ordered = compile_ui_frame(std::vector<DrawCommand>{txt, solid}, &atlas);
		CHECK(ordered.instances.size() == 2 && ordered.instances[0].shape[0] == 1 &&
			  ordered.instances[1].shape[0] == 0);
	}
	{
		auto ui        = context();
		bool activated = false;
		auto frame     = [&]() {
			ui.begin_frame(native);
			activated = ui.button("one", "One", {20, 20, 100, 32});
			ui.button("disabled", "Disabled", {20, 60, 100, 32}, false);
			ui.push_clip({0, 100, 200, 30});
			ui.button("clipped", "Clipped", {20, 90, 100, 32});
			ui.pop_clip();
			ui.button("two", "Two", {20, 140, 100, 32});
			ui.end_frame();
		};
		frame();
		Input down;
		down.kind  = InputKind::Press;
		down.point = {30, 30};
		CHECK(ui.handle(down));
		frame();
		CHECK(!activated && ui.capture() == "one");
		Input up = down;
		up.kind  = InputKind::Release;
		up.point = {500, 500};
		CHECK(ui.handle(up));
		frame();
		CHECK(!activated && ui.capture().empty());
		click(ui, {30, 30});
		frame();
		CHECK(activated);
		key(ui, "tab");
		CHECK(ui.focus() == "clipped");
		key(ui, "tab");
		CHECK(ui.focus() == "two");
		key(ui, "tab", false, true);
		CHECK(ui.focus() == "clipped");
		CHECK(!ui.handle({InputKind::Press, {30, 95}})); // Outside the effective clip.
		down.button = 273;
		CHECK(!ui.handle(down));
	}
	{
		auto        ui    = context();
		std::string name  = "AλB";
		double      value = 35;
		EditResult  number, stringEdit;
		auto        frame = [&]() {
			ui.begin_frame(native);
			stringEdit = ui.text_input("text", {20, 20, 240, 32}, name);
			number     = ui.number_input("number", {20, 70, 240, 32}, value, {0, 63, 1, true});
			ui.end_frame();
		};
		frame();
		click(ui, {60, 30});
		key(ui, "end");
		key(ui, "left");
		key(ui, "backspace");
		frame();
		CHECK(name == "AB");
		key(ui, "a", true);
		text(ui, "a");
		text(ui, "b");
		frame();
		CHECK(name == "ab" && stringEdit.changed);
		key(ui, "a", true);
		text(ui, "こんにちは");
		frame();
		CHECK(name == "こんにちは");
		key(ui, "escape");
		frame();
		CHECK(name == "AλB");
		key(ui, "a", true);
		key(ui, "c", true);
		auto copied = ui.take_clipboard_requests();
		CHECK(copied.size() == 1 && copied[0].text == name);
		key(ui, "x", true);
		frame();
		CHECK(name.empty());
		(void) ui.take_clipboard_requests();
		key(ui, "v", true);
		auto pendingPaste = ui.take_clipboard_requests();
		CHECK(pendingPaste.size() == 1 && pendingPaste[0].kind == ClipboardRequest::Paste);
		CHECK(!ui.paste("number", "123"));
		CHECK(ui.paste("text", "12\n3"));
		frame();
		CHECK(name == "12 3");
		key(ui, "escape");
		frame();
		CHECK(name == "AλB");
		click(ui, {60, 80});
		key(ui, "a", true);
		text(ui, "99");
		key(ui, "enter");
		frame();
		CHECK(value == 35 && !number.valid && !number.error.empty());
		key(ui, "a", true);
		text(ui, "17");
		key(ui, "enter");
		frame();
		CHECK(value == 17 && number.valid);
		key(ui, "up");
		frame();
		CHECK(value == 18);
		key(ui, "a", true);
		text(ui, "nan");
		key(ui, "enter");
		frame();
		CHECK(value == 18 && !number.valid);
	}
	{
		auto        ui    = context();
		double      value = 28.000000000000002;
		std::string search;
		auto        frame = [&] {
			ui.begin_frame(native);
			ui.number_input("fraction", {20, 20, 96, 32}, value, {0, 1000000000, .1, false, 1});
			ui.text_input("search", {20, 70, 240, 32}, search, true, "Search settings…");
			ui.end_frame();
		};
		auto label = [&](std::string_view text) {
			auto it = std::ranges::find_if(
					ui.commands(), [&](const auto &c) { return c.kind == DrawCommandKind::Text && c.text == text; });
			CHECK(it != ui.commands().end());
			return *it;
		};
		frame();
		CHECK(label("28.0").rect.x == 20 + ui.theme.padding);
		CHECK(label("Search settings…").rect.y == 70 + (32 - ui.theme.lineHeight) / 2);
		value = 999999999;
		frame();
		click(ui, {60, 30});
		key(ui, "end");
		frame();
		click(ui, {60, 80});
		value = 18.75;
		frame();
		CHECK(label("18.8").rect.x == 20 + ui.theme.padding); // A shorter value drops the old scroll offset.
		search = "a long setting search that requires horizontal scrolling";
		frame();
		key(ui, "end");
		frame();
		key(ui, "a", true);
		key(ui, "backspace");
		frame();
		CHECK(label("Search settings…").rect.x == 20 + ui.theme.padding);
	}
	{
		auto                ui = context();
		std::vector<Choice> choices{{"CQP"}, {"Unavailable", false}, {"ICQ"}};
		std::size_t         selected   = 0;
		bool                underlying = false;
		auto                frame      = [&]() {
			ui.begin_frame(native);
			ui.push_clip({0, 0, 500, 80});
			ui.combo("mode", {20, 20, 240, 32}, choices, selected);
			ui.pop_clip();
			underlying = ui.button("under", "Under", {20, 150, 240, 32});
			ui.end_frame();
		};
		frame();
		click(ui, {30, 30});
		frame();
		key(ui, "down");
		key(ui, "enter");
		frame();
		CHECK(selected == 2);
		click(ui, {30, 30});
		frame();
		click(ui, {30, 170});
		frame();
		CHECK(!underlying); // Click outside closes without activating the control below.
		click(ui, {30, 170});
		frame();
		CHECK(underlying);
		click(ui, {30, 30});
		frame();
		click(ui, {30, 130});
		frame();
		CHECK(selected == 2); // Menu portal extends beyond the parent clip.
		compile_paint_scene(ui.commands());
	}
	{
		auto  ui    = context();
		float width = 260;
		auto  frame = [&]() {
			ui.begin_frame(native);
			ui.splitter("split", {257, 40, 6, 1000}, width, Axis::Horizontal, 100, 600);
			ui.end_frame();
		};
		frame();
		Input e;
		e.kind  = InputKind::Press;
		e.point = {260, 100};
		CHECK(ui.handle(e));
		e.kind  = InputKind::Move;
		e.point = {900, 100};
		CHECK(ui.handle(e));
		frame();
		CHECK(width == 600);
		e.kind = InputKind::Release;
		CHECK(ui.handle(e));
		CHECK(ui.capture().empty());
		key(ui, "left");
		frame();
		CHECK(width == 592);
	}
	{
		auto                  ui = context();
		std::vector<Column>   columns{{"name", "Encoder", 200, 80}, {"size", "Size", 100, 60, TextAlign::Right}};
		std::size_t           accessed = 0;
		std::vector<uint64_t> order(100000);
		std::iota(order.begin(), order.end(), 1000);
		TableModel  model{order.size(), [&](auto i) { return order.at(i); },
						  [&](uint64_t id) -> std::optional<std::size_t> {
							 auto it = std::find(order.begin(), order.end(), id);
							 return it == order.end() ? std::nullopt : std::optional<std::size_t>(it - order.begin());
						  },
						  [&](auto row, auto col) {
							 ++accessed;
							 return col ? std::to_string(row * 43) : "Encoder " + std::to_string(order.at(row));
						  }};
		TableResult result;
		auto        frame = [&]() {
			accessed = 0;
			ui.begin_frame(native);
			result = ui.table("table", {20, 20, 700, 352}, columns, model);
			ui.end_frame();
		};
		frame();
		CHECK(accessed <= 24);
		click(ui, {40, 65});
		frame();
		CHECK(ui.table_state("table").active == 1000);
		click(ui, {40, 97}, true);
		frame();
		CHECK(ui.table_state("table").selection.size() == 2);
		click(ui, {40, 161}, false, true);
		frame();
		CHECK(ui.table_state("table").selection.size() == 3);
		click(ui, {40, 32});
		frame();
		CHECK(result.sortChanged && ui.table_state("table").sortColumn == "name");
		std::reverse(order.begin(), order.end());
		frame();
		CHECK(ui.table_state("table").active == 1003);
		Input scroll;
		scroll.kind  = InputKind::Scroll;
		scroll.point = {100, 100};
		scroll.delta = {0, 320000};
		CHECK(ui.handle(scroll));
		frame();
		CHECK(result.visible.begin == 9999 && accessed <= 26);
		CHECK(ui.widgets().size() < 60); // Scale with visible rows, not model size.
		Input press;
		press.kind  = InputKind::Press;
		press.point = {220, 30};
		CHECK(ui.handle(press));
		press.kind  = InputKind::Move;
		press.point = {280, 30};
		CHECK(ui.handle(press));
		frame();
		CHECK(columns[0].width == 260);
	}
	{
		auto ui    = context();
		auto frame = [&]() {
			ui.begin_frame(native);
			ui.begin_scroll("outer", {0, 0, 300, 300}, {300, 900});
			ui.begin_scroll("inner", {10, 10, 200, 100}, {200, 200});
			ui.end_scroll();
			ui.end_scroll();
			ui.end_frame();
		};
		frame();
		Input scroll;
		scroll.kind  = InputKind::Scroll;
		scroll.point = {30, 30};
		scroll.delta = {0, 150};
		CHECK(ui.handle(scroll));
		CHECK(ui.scroll_state("inner").y == 100 && ui.scroll_state("outer").y == 50);
	}
	{
		auto   ui      = context();
		double value   = 5;
		bool   changed = false;
		auto   frame   = [&](bool enabled = true) {
			ui.begin_frame(native);
			changed = ui.slider("quality", {100, 100, 214, 32}, value, {1, 51, 1, true}, enabled);
			ui.end_frame();
		};
		frame();
		Input e;
		e.kind  = InputKind::Press;
		e.point = {107, 116};
		CHECK(ui.handle(e));
		frame();
		CHECK(changed && value == 1);
		e.kind  = InputKind::Move;
		e.point = {207, 116};
		CHECK(ui.handle(e));
		frame();
		CHECK(changed && value == 26);
		e.point = {1000, 116};
		CHECK(ui.handle(e));
		frame();
		CHECK(value == 51); // Capture and bounds outside the control.
		e.kind = InputKind::Release;
		CHECK(ui.handle(e));
		frame();
		key(ui, "left");
		frame();
		CHECK(value == 50);
		key(ui, "home");
		frame();
		CHECK(value == 1);
		key(ui, "end");
		frame();
		CHECK(value == 51);
        frame(false);
        e.kind=InputKind::Press;e.point={107,116};CHECK(!ui.handle(e));
        e.kind=InputKind::Release;CHECK(!ui.handle(e));
		frame(false);
		CHECK(value == 51);
	}
	{
		auto   ui    = context();
		double value = 1000;
		auto   frame = [&](bool enabled = true) {
			ui.begin_frame(native);
			ui.slider("bitrate", {100, 100, 214, 32}, value, {1, 1000000, 1, true, 0}, enabled,
					  SliderScale::Logarithmic);
			ui.end_frame();
		};
		frame();
		// Inverse mapping puts 1000 at the geometric midpoint of 1..1,000,000.
		CHECK(std::ranges::any_of(ui.commands(), [](const auto &c) {
			return c.rect.w == 12 && c.rect.h == 26 && std::abs(c.rect.x - 201) < .01;
		}));
		Input input;
		input.kind  = InputKind::Press;
		input.point = {157, 116};
		CHECK(ui.handle(input));
		frame();
		CHECK(value == 32);
		input.kind    = InputKind::Move;
		input.point.x = 207;
		CHECK(ui.handle(input));
		frame();
		CHECK(value == 1000);
		input.point.x = 257;
		CHECK(ui.handle(input));
		frame();
		CHECK(value == 31623);
		input.point.x = 1000;
		CHECK(ui.handle(input));
		frame();
		CHECK(value == 1000000);
		input.point.x = 0;
		CHECK(ui.handle(input));
		frame();
		CHECK(value == 1);
		input.kind = InputKind::Release;
		CHECK(ui.handle(input));
		frame();
		key(ui, "end");
		frame();
		CHECK(value == 1000000);
		key(ui, "home");
		frame();
		CHECK(value == 1);
		value = 1000;
		frame();
		key(ui, "right");
		frame();
		CHECK(value == 1148);
		key(ui, "left");
		frame();
		CHECK(std::abs(value - 1000) <= 1);
        frame(false);
        input.kind=InputKind::Press;input.point={307,116};CHECK(!ui.handle(input));
        input.kind=InputKind::Release;CHECK(!ui.handle(input));
		frame(false);
		CHECK(value < 1001);
	}

	{
		auto ui = context();
		std::vector<PlotPoint> points{{7, .5, .5, "#7 · HEVC\n12345 bytes · PSNR 42.125 dB"}};
		auto frame = [&] {
			ui.begin_frame(native);
			ui.plot("audit.plot", {20, 20, 400, 300}, points, {});
			ui.end_frame();
		};
		frame();
		auto point = std::ranges::find_if(ui.widgets(), [](const auto& w) { return w.id == "audit.plot/point/7"; });
		CHECK(point != ui.widgets().end());
		Input move; move.kind = InputKind::Move; move.point = {point->rect.x + 6, point->rect.y + 6};
		ui.handle(move); frame();
		std::this_thread::sleep_for(std::chrono::milliseconds(450)); frame();
		auto tooltip = std::ranges::find_if(ui.commands(), [](const auto& c) { return c.kind == DrawCommandKind::Text && c.text.find("12345 bytes") != std::string::npos; });
		CHECK(tooltip != ui.commands().end() && tooltip->layer == 1000);
		CHECK(tooltip->rect.x >= 0 && tooltip->rect.y >= 0 && tooltip->rect.x + tooltip->rect.w <= native.w && tooltip->rect.y + tooltip->rect.h <= native.h);
		move.point = {1000, 1000}; ui.handle(move); frame();
		CHECK(!std::ranges::any_of(ui.commands(), [](const auto& c) { return c.kind == DrawCommandKind::Text && c.text.find("12345 bytes") != std::string::npos; }));
	}
	std::cout << "UI framework: ordered clipping/batching, focus/capture, Unicode editing, numeric validation, live "
				 "sliders, popover "
				 "portals, resizing, virtualized tables and nested scrolling passed\n";
}
