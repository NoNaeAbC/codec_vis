#include "metrics.hpp"
#include "paint_scene.hpp"
#include "test_check.hpp"
#include "workbench.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
using namespace codec_gui;
using namespace codec_gui::gui;
#define CHECK CODEC_VIS_CHECK
namespace {
	struct Fixture {
		AppState  state;
		Workbench view{[](std::string_view t, float size, FontRole) { return float(t.size()) * size * .5f; }};
		ImageId   source, first, second;
		Fixture() {
			auto raw    = std::make_shared<RawImage>();
			raw->width  = 5600;
			raw->height = 3728;
			Action load;
			load.kind               = ActionKind::SourceLoaded;
			load.sourceLoaded.image = raw;
			load.sourceLoaded.path  = "source.266";
			state                   = update(std::move(state), load).state;
			source                  = state.selection.selectedImage;
			BackendInfo backend;
			backend.id                      = {10};
			backend.name                    = "Controlled encoder";
			state.backends                  = {backend};
			state.selection.selectedBackend = backend.id;
			first                           = add(1000);
			second                          = add(1001);
			state.selection.selectedImage   = second;
			frame();
		}
		ImageId add(uint64_t runId) {
			ImageObject image = state.images.front();
			image.id          = next_image_id(state);
			image.type        = ImageObjectType::EncodedResult;
			image.parents     = {source};
			image.encoded.emplace();
			image.encoded->run     = {runId};
			image.encoded->backend = {10};
			image.displayName      = "Result " + std::to_string(runId);
			EncodeRun run;
			run.id            = {runId};
			run.source        = source;
			run.backend       = {10};
			run.producedImage = image.id;
			run.state         = EncodeRunState::Completed;
			state.encodeRuns.push_back(run);
			state.images.push_back(image);
			return image.id;
		}
		void apply(std::vector<Action> actions) {
			for (const auto &a: actions) state = update(std::move(state), a).state;
		}
		void frame() {
			for (int pass = 0; pass < 4; ++pass) {
				auto actions = view.build(state, {0, 0, 2880, 1800}, 0);
				bool again   = !actions.empty() || view.needs_rebuild();
				apply(std::move(actions));
				if (!again) return;
			}
			CHECK(false);
		}
		void send(Action a) {
			apply(view.handle(a, state));
			frame();
		}
		ui::Widget widget(const std::string &id) {
			const auto &ws = view.context().widgets();
			auto        i  = std::ranges::find_if(ws, [&](const auto &w) { return w.id == id; });
			CHECK(i != ws.end());
			return *i;
		}
		void click(Point p) {
			Action a;
			a.kind  = ActionKind::PointerPressed;
			a.point = p;
			send(a);
			a.kind = ActionKind::PointerReleased;
			send(a);
		}
		void click(const std::string &id) {
			auto w = widget(id);
			CHECK(w.enabled);
			click(Point{w.rect.x + w.rect.w * .5f, w.rect.y + w.rect.h * .5f});
		}
		void key(std::string key, bool control = false) {
			Action a;
			a.kind    = ActionKind::KeyPressed;
			a.text    = std::move(key);
			a.control = control;
			send(a);
		}
		void mode(int m) {
			click("view.mode");
			for (int i = 0; i < 6; ++i) key("up");
			for (int i = 0; i < m; ++i) key("down");
			key("enter");
		}
		struct Image {
			ImageId id;
			Rect    rect, clip;
		};
		std::vector<Image> images() {
			std::vector<Image> out;
			for (const auto &p: compile_paint_scene(view.commands())) {
				const auto &c = view.commands()[p.commandIndex];
				if (c.kind == DrawCommandKind::Image) out.push_back({c.image, c.rect, p.clip});
			}
			return out;
		}
		void zoom(Point p, double amount) {
			Action a;
			a.kind  = ActionKind::PointerScrolled;
			a.point = p;
			a.value = amount;
			send(a);
		}
		bool text(std::string_view part) {
			return std::ranges::any_of(view.commands(), [&](const auto &c) {
				return c.kind == DrawCommandKind::Text && c.text.find(part) != std::string::npos;
			});
		}
	};
	Point center(Rect r) { return {r.x + r.w * .5f, r.y + r.h * .5f}; }
	bool  near(double a, double b) { return std::abs(a - b) < 1e-5; }
} // namespace
int main() {
	{
		Fixture f;
		auto raw = std::make_shared<RawImage>();
		raw->width = 5;
		raw->height = 3;
		raw->format = PixelFormat::YUV420P10LE;
		for (auto& plane : raw->planes) {
			plane.strideBytes = 64;
			plane.bytes.resize(192);
		}
		auto* source = find_image(f.state, f.source);
		source->decoded = raw;
		source->sourceByteSize = 100;
		f.frame();
		// Visible 10-bit samples occupy 16-bit storage; chroma rounds up.
		// Allocator/stride padding and the imported file are separate sizes.
		CHECK(f.text("Uncompressed: 54 B"));
		CHECK(f.text("File: 100 B"));
	}
    {
        Fixture f;
        Action press;press.kind=ActionKind::PointerPressed;press.point={400,24};
        f.send(press);CHECK(!f.view.input_consumed()); // Source caption/window background.
        auto release=press;release.kind=ActionKind::PointerReleased;f.send(release);
        const auto control=f.widget("import");
        press.point={control.rect.x+10,control.rect.y+10};
        f.send(press);CHECK(f.view.input_consumed());
        release.point=press.point;f.send(release);
        const auto canvas=f.view.canvas();
        press.point={canvas.x+canvas.w*.25f,canvas.y+canvas.h*.5f};
        f.send(press);CHECK(f.view.input_consumed()); // Pan/select retains the gesture.
        release.point=press.point;f.send(release);
        press.point={400,24};f.send(press);CHECK(!f.view.input_consumed());
        release.point=press.point;f.send(release);
        f.click("view.mode"); // Dismissing a popup consumes a background press.
        press.point={400,24};f.send(press);CHECK(f.view.input_consumed());
        release.point=press.point;f.send(release);
        press.point={400,24};f.send(press);CHECK(!f.view.input_consumed());
    }

	{
		Fixture f;
		f.zoom(center(f.images()[1].clip), -4);
		auto crop = f.view.viewport();
		for (int mode: {0, 1, 2, 3, 5, 1}) {
			f.mode(mode);
			CHECK(near(f.view.viewport().scale, crop.scale));
			CHECK(near(f.view.viewport().centerX, crop.centerX));
			CHECK(near(f.view.viewport().centerY, crop.centerY));
		}
		// Unlinked A may contain exactly the same image as B; pane role still controls navigation.
		f.click("selection.assign.0");
		f.click("view.link");
		auto before = f.images();
		f.zoom(center(before[0].clip), -3);
		auto after = f.images();
		CHECK(before[0].id == before[1].id);
		CHECK(after[0].rect.w > before[0].rect.w);
		CHECK(near(after[1].rect.w, before[1].rect.w));
		CHECK(near(f.view.viewport().scale, crop.scale));
		// Bookmarks restore both independent crops and retain their names/IDs after another is removed.
		f.click("bookmark.add");
		f.click("bookmark.1.name");
		f.key("a", true);
		f.key("Text edges");
		f.key("enter");
		auto bookmarked = f.images();
		f.zoom(center(bookmarked[0].clip), -2);
		f.zoom(center(f.images()[1].clip), -2);
		f.click("bookmark.1.go");
		after = f.images();
		CHECK(near(after[0].rect.w, bookmarked[0].rect.w) && near(after[1].rect.w, bookmarked[1].rect.w));
		f.click("bookmark.add");
		f.click("bookmark.2.name");
		f.key("a", true);
		f.key("Highlights");
		f.key("enter");
		f.click("bookmark.1.remove");
		CHECK(f.text("Highlights"));
		CHECK(!f.text("Text edges"));
		CHECK(f.widget("bookmark.2.go").enabled);
	}
	{
		Fixture f;
		auto   &selected = f.view.context().table_state("experiments").selection;
		selected.clear();
		for (uint64_t id = 1100; id < 1200; ++id) {
			f.add(id);
			selected.insert(id);
		}
		f.frame();
		f.mode(5);
		auto first = f.images();
		CHECK(first.size() == 4);
		CHECK(f.text("Page 1 / 34"));
		CHECK(f.text("#1100 · "));
		f.click("grid.next");
		auto page = f.images();
		CHECK(page.size() == 4 && page[0].id == f.source);
		for (std::size_t i = 1; i < page.size(); ++i)
			CHECK(std::ranges::none_of(first, [&](const auto &p) { return p.id == page[i].id; }));
		CHECK(selected.size() == 100);
		f.click(center(page[2].clip));
		CHECK(f.state.selection.selectedImage == page[2].id);
		CHECK(f.view.candidate() == f.second); // Inspecting a tile does not reassign a pane.
		CHECK(selected.size() == 100);
		auto   target = f.images()[3];
		auto   point  = center(target.clip);
		double x = (point.x - target.rect.x) / target.rect.w, y = (point.y - target.rect.y) / target.rect.h;
		f.zoom(point, -2);
		auto zoomed = f.images()[3];
		CHECK(near((point.x - zoomed.rect.x) / zoomed.rect.w, x));
		CHECK(near((point.y - zoomed.rect.y) / zoomed.rect.h, y));
		for (int i = 1; i < 33; ++i) f.click("grid.next");
		CHECK(f.images().size() == 2);
		CHECK(!f.widget("grid.next").enabled);
		CHECK(selected.size() == 100);
	}
	{
		Fixture   f;
		auto     &selected = f.view.context().table_state("experiments").selection;
		EncodeRun failed;
		failed.id      = {2000};
		failed.source  = f.source;
		failed.backend = {10};
		failed.state   = EncodeRunState::Failed;
		f.state.encodeRuns.push_back(failed);
		selected = {failed.id.value};
		f.click("task.2");
		CHECK(!f.widget("metrics.compute").enabled);
		CHECK(f.text("without a decoded result"));
		selected                    = {1001};
		auto *image                 = find_image(f.state, f.second);
		image->encoded->metricState = MetricState::Queued;
		f.frame();
		CHECK(!f.widget("metrics.compute").enabled);
		CHECK(f.text("queued or running"));
		image                       = find_image(f.state, f.second);
		image->encoded->metricState = MetricState::NotRequested;
		image->decoder.running      = true;
		f.frame();
		CHECK(!f.widget("metrics.compute").enabled);
		CHECK(f.text("waiting for decode"));
		image                  = find_image(f.state, f.second);
		image->decoder.running = false;
		f.frame();
		CHECK(f.widget("metrics.compute").enabled);
		QualityMetricRecord score;
		score.id    = "psnr-all";
		score.label = "PSNR";
		score.value = 40.;
		for (const auto &metric: metric_catalog()) {
			score.id = std::string(metric.id);
			image->encoded->metrics.push_back(score);
		}
		f.frame();
		CHECK(!f.widget("metrics.compute").enabled);
		CHECK(f.text("already computed"));
		f.click("task.0");
		f.click("experiments.filter");
		f.key("missing encoder");
		CHECK(f.text("1 hidden by filter"));
		f.click("experiments.clear-selection");
		CHECK(f.text("0 selected"));
		CHECK(selected.empty());
	}
	{
		Fixture f;
		auto    sample = [](double seconds, double codec, std::string error = {}) {
			DecodeSample s;
			s.seconds      = seconds;
			s.codecSeconds = codec;
			s.error        = std::move(error);
			return s;
		};
		DecodeMeasurement old;
		old.decoderName = "Previous decoder";
		old.finished    = true;
		old.samples     = {sample(8, 8)};
		DecodeMeasurement last;
		last.decoderName                              = "Current decoder";
		last.finished                                 = true;
		last.requested                                = 4;
		last.samples                                  = {sample(.01, .001), sample(.02, .010), sample(.11, .100),
														 sample(.2, .020, "Controlled failure")};
		find_image(f.state, f.first)->decoder.history = {old, last};
		f.click("experiments.columns");
		f.key("down"); f.key("enter"); // Speed columns expose timing and failures without quality columns.
		f.frame();
		CHECK(f.text("32.75 ms"));
		CHECK(f.text("85.00 ms")); // Total includes every attempt, including the failed one.
		CHECK(f.text("1 failed"));
		CHECK(!f.text("8.000 s"));
		// Numeric decoder timing sorts by measured values; unmeasured runs remain last.
		f.click("experiments/header/decode");
		auto rows     = f.view.context().widgets();
		auto measured = std::ranges::find_if(rows, [](const auto &w) { return w.id == "experiments/row/1000"; });
		auto missing  = std::ranges::find_if(rows, [](const auto &w) { return w.id == "experiments/row/1001"; });
		CHECK(measured != rows.end() && missing != rows.end() && measured->rect.y < missing->rect.y);
		CHECK(find_image(f.state, f.first)->decoder.history.back().samples.size() == 4);
		f.click("experiments/header/decode-total");
		CHECK(f.text("85.00 ms"));
	}
	{
		Fixture f;
		auto   &backend = f.state.backends.front();
		backend.kind    = BackendKind::Hardware;
		EncoderParamInfo qp;
		qp.name         = "qp";
		qp.label        = "QP";
		qp.kind         = ParamKind::Int;
		qp.defaultValue = int64_t(35);
		qp.intRange     = IntRange{1, 51, 1};
		backend.params  = {qp};
		f.frame();
		auto generation = f.state.livePreview.requested;
		CHECK(generation > 0 && f.state.livePreview.busy);
		auto slider = f.widget("param.10.qp.slider");
		f.click(Point{slider.rect.x + slider.rect.w * .5f, slider.rect.y + slider.rect.h * .5f});
		CHECK(f.state.livePreview.requested > generation);
		CHECK(std::get<int64_t>(f.state.livePreview.requestedParams.front().value) == 26);
		CHECK(f.state.encodeRuns.size() == 2); // Preview edits never create experiment records.
		f.click("live.enabled");
		CHECK(!f.state.livePreview.enabled && !f.state.livePreview.busy);
		f.click("live.enabled");
		CHECK(f.state.livePreview.enabled && f.state.livePreview.busy);
		generation                             = f.state.livePreview.requested;
		find_image(f.state, f.source)->decoded = std::make_shared<RawImage>(*find_image(f.state, f.source)->decoded);
		f.frame();
		CHECK(f.state.livePreview.requested > generation);
	}
	{
		Fixture f;
		auto    live      = *find_image(f.state, f.first);
		live.id           = next_image_id(f.state);
		live.type         = ImageObjectType::LivePreview;
		live.encoded->run = {};
		auto id           = live.id;
		f.state.images.push_back(live);
		auto &selection = f.view.context().table_state("experiments").selection;
		selection       = {1000, 1001};
		f.add(2000);
		f.add(2001);
		selection.insert(2000);
		selection.insert(2001);
		f.state.selection.selectedImage = id;
		f.frame();
		CHECK(f.widget("selection.assign.0").enabled);
		f.mode(5);
		auto first = f.images();
		CHECK(first.size() == 4 && first[1].id == id);
		f.click("grid.next");
		auto last = f.images();
		CHECK(last.size() == 4 && last[1].id == id && selection.size() == 4);
		f.mode(4);
		CHECK(std::ranges::any_of(f.view.commands(), [&](const auto &c) {
			return c.kind == DrawCommandKind::Image && c.image == id && c.comparisonImage == f.source;
		}));
	}
	{
		Fixture f;
		f.click("pane.add");
		f.click("pane.add");
		CHECK(f.images().size() == 4);
		f.click("run.1000.assign.0");
		f.click("run.1000.assign.2");
		f.click("run.1001.assign.3");
		auto before = f.images();
		CHECK(before[0].id == f.first && before[1].id == f.second && before[2].id == f.first &&
			  before[3].id == f.second);
		f.click("experiments/row/1000");
		CHECK(f.state.selection.selectedImage == f.first);
		f.click("experiments/header/time");
		CHECK(f.state.selection.selectedImage == f.first); // Sorting must not select pane B's image.
		DecodeMeasurement measurement;
		measurement.requested = 1;
		measurement.samples.emplace_back();
		measurement.samples.back().seconds = .001;
		find_image(f.state, f.first)->decoder.history.push_back(measurement);
		f.click("detail.1");
		f.click("measurement.attempts." + std::to_string(f.first.value) + "/header/codec");
		CHECK(f.state.selection.selectedImage == f.first);
		f.click("detail.0");
		for (std::size_t p = 0; p < 4; ++p) CHECK(f.images()[p].id == before[p].id);
		f.click("view.link");
		f.zoom(center(f.images()[2].clip), -3);
		auto after = f.images();
		CHECK(after[2].rect.w > before[2].rect.w);
		for (std::size_t p: {0, 1, 3}) CHECK(near(after[p].rect.w, before[p].rect.w));
		// Replacement updates every assigned pane, preserves the record lineage and removes dead rows.
		find_image(f.state, f.first)->encoded->encoderInput = find_image(f.state, f.source)->decoded;
		Action inspectInput; inspectInput.kind = ActionKind::InspectEncoderInput; inspectInput.image = f.first;
		f.apply({inspectInput}); f.frame();
		const auto inputId = f.state.images.back().id;
		EncodeRun run;
		run.id                    = {1002};
		run.source                = f.source;
		run.backend               = {10};
		run.replacePreviousResult = true;
		f.state.encodeRuns.push_back(run);
		Action done;
		done.kind                             = ActionKind::EncodeRunCompleted;
		done.encodeCompleted.run              = run.id;
		done.encodeCompleted.metadata.backend = {10};
		done.encodeCompleted.metadata.run     = run.id;
		done.encodeCompleted.preview          = find_image(f.state, f.source)->decoded;
		f.state                               = update(std::move(f.state), done).state;
		f.frame();
		auto replacement = *f.state.encodeRuns.back().producedImage;
		CHECK(!find_image(f.state, inputId));
		for (const auto &pane: f.images()) CHECK(pane.id == replacement);
		CHECK(f.text("1 visible"));
		CHECK(f.state.encodeRuns[0].replacedBy == run.id && f.state.encodeRuns[1].replacedBy == run.id);
	}

	{
		Fixture f;
		auto   &backend = f.state.backends.front();
		backend.kind    = BackendKind::Hardware;
		EncoderParamInfo bitrate;
		bitrate.name         = "bitrate-kbps";
		bitrate.label        = "Bitrate (kbps)";
		bitrate.kind         = ParamKind::Int;
		bitrate.defaultValue = int64_t(10000);
		bitrate.intRange     = IntRange{1, 1000000, 1000};
		backend.params       = {bitrate};
		f.frame();
		auto track = f.widget("param.10.bitrate-kbps.slider");
		CHECK(track.sliderScale == ui::SliderScale::Logarithmic && track.number.step == 1);
		CHECK(f.widget("param.10.bitrate-kbps").number.step == 1000);
		f.click(Point{track.rect.x + track.rect.w / 2, track.rect.y + track.rect.h / 2});
		CHECK(std::get<int64_t>(f.state.livePreview.requestedParams.front().value) == 1000);
		f.click("param.10.bitrate-kbps");
		f.key("a", true);
		f.key("12345");
		f.key("enter");
		CHECK(std::get<int64_t>(f.state.livePreview.requestedParams.front().value) == 12345);
		auto live              = *find_image(f.state, f.first);
		live.id                = next_image_id(f.state);
		live.type              = ImageObjectType::LivePreview;
		live.displayName       = "Live preview";
		live.encoded->run      = {};
		live.encoded->byteSize = 169518;
		auto id                = live.id;
		f.state.images.push_back(live);
		f.state.livePreview.image       = id;
		f.state.livePreview.displayed   = f.state.livePreview.requested;
		f.state.livePreview.shownParams = {{"bitrate-kbps", int64_t(1000)}};
		f.state.selection.selectedImage = id;
		f.frame();
		CHECK(f.text("Size 169.518 kB · 0.06 bpp"));
		CHECK(f.text("Live · ") && f.text("169.518 kB"));
		CHECK(f.text("Shown: bitrate-kbps 1000")); // Shown size/rate describe the frame, not the newer draft.
		auto header                                = f.widget("group.10/Core settings").rect;
		find_image(f.state, id)->encoded->byteSize = 3000000;
		++f.state.livePreview.requested;
		f.state.livePreview.busy = true;
		f.frame();
		CHECK(f.text("Size 3000.000 kB"));
		CHECK(f.widget("group.10/Core settings").rect.y == header.y);
	}
	{
		Fixture f;
		auto   &backend = f.state.backends.front();
		backend.codec   = "JPEG XS";
		EncoderParamInfo bpp;
		bpp.name         = "bpp";
		bpp.label        = "Bits per pixel";
		bpp.kind         = ParamKind::Float;
		bpp.defaultValue = 6.;
		bpp.floatRange   = FloatRange{1., 36., .25};
		backend.params   = {bpp};
		f.frame();
		auto track = f.widget("param.10.bpp.slider");
		CHECK(track.sliderScale == ui::SliderScale::Logarithmic);
		f.click(Point{track.rect.x + track.rect.w / 2, track.rect.y + track.rect.h / 2});
		CHECK(std::get<double>(f.state.livePreview.requestedParams.front().value) == 6.);
	}

	{
		Fixture f;
		auto   &backend = f.state.backends.front();
		backend.kind    = BackendKind::Hardware;
		EncoderParamInfo target;
		target.name         = "target-size-kb";
		target.label        = "Target image size (kB)";
		target.kind         = ParamKind::Float;
		target.defaultValue = 250.;
		target.floatRange   = FloatRange{.125, 125000., .125};
		backend.params      = {target};
		f.frame();
		CHECK(f.widget("param.10.target-size-kb.slider").sliderScale == ui::SliderScale::Logarithmic);
		CHECK(f.widget("param.10.target-size-kb.slider").number.minimum == 62.5);
		CHECK(f.widget("param.10.target-size-kb.slider").number.maximum == 1000.);
		auto revision = f.state.livePreview.requested;
		f.click("param.10.target-size-kb.range");
		CHECK(f.widget("param.10.target-size-kb.slider").number.minimum == .125);
		CHECK(f.widget("param.10.target-size-kb.slider").number.maximum == 125000.);
		CHECK(f.state.livePreview.requested == revision);
		f.click("param.10.target-size-kb.range");
		CHECK(f.widget("param.10.target-size-kb.slider").number.minimum == 62.5);
		Action old;
		old.kind    = ActionKind::SetEncoderParam;
		old.backend = backend.id;
		old.param   = {"bitrate-kbps", int64_t(2000)};
		f.state     = update(std::move(f.state), old).state;
		f.frame();
		CHECK(f.state.encoderConfigs.back().params.back().name == "target-size-kb");
		CHECK(std::get<double>(f.state.livePreview.requestedParams.front().value) == 250.);
		f.click("param.10.target-size-kb");
		f.key("a", true);
		f.key("375.125");
		f.key("enter");
		CHECK(std::get<double>(f.state.livePreview.requestedParams.front().value) == 375.125);
	}

	{
		Fixture f;
		CHECK(!f.text("codec_vis") && !f.text("Scroll to zoom"));
		auto initial = f.view.canvas();
		f.zoom(center(f.images()[1].clip), -2);
		auto crop = f.view.viewport();
		f.click("panels.sources");
		CHECK(f.view.canvas().w > initial.w);
		CHECK(near(f.view.viewport().scale, crop.scale));
		f.click("panels.tools");
		CHECK(f.view.canvas().w > initial.w + 260);
		f.click("panels.sources"); f.click("panels.tools");
		CHECK(near(f.view.canvas().w, initial.w));
		CHECK(!std::ranges::any_of(f.view.context().widgets(), [](const auto& w) { return w.id == "experiments/header/decode"; }));
		f.click("experiments.columns"); f.key("down"); f.key("enter");
		CHECK(f.widget("experiments/header/decode").enabled);
		CHECK(!std::ranges::any_of(f.view.context().widgets(), [](const auto& w) { return w.id == "experiments/header/butteraugli"; }));
		f.click("detail.3");
		const float compact = f.view.canvas().h;
		f.click("plot.expand"); CHECK(f.view.canvas().h < compact);
		f.click("plot.expand"); CHECK(near(f.view.canvas().h, compact));
	}
	{
		Fixture f;
		auto input = std::make_shared<RawImage>(*find_image(f.state, f.source)->decoded);
		input->format = PixelFormat::YUV444P10LE;
		auto* result = find_image(f.state, f.second);
		result->encoded->encoderInput = input;
		result->encoded->metrics = {{.id="psnr-all", .label="PSNR linear RGB", .value=42.125}};
		f.frame(); f.click("task.2");
		CHECK(f.text("42.12500"));
		CHECK(f.text("Scores vs original: source.266"));
		CHECK(!f.text("fixed peak")); // Methodology stays available without occupying the score view.
		const auto reference = result->encoded->metricReference;
		f.click("metrics.input");
		const auto* prepared = find_image(f.state, f.view.reference());
		CHECK(prepared && prepared->derived && prepared->derived->kind == "encoder-input");
		CHECK(prepared->decoded == input && prepared->pixelFormat == PixelFormat::YUV444P10LE);
		CHECK(f.view.candidate() == f.second && f.state.selection.selectedImage == f.second);
		CHECK(find_image(f.state, f.second)->encoded->metricReference == reference);
		const auto count = f.state.images.size();
		f.click("metrics.input"); CHECK(f.state.images.size() == count);
		f.click("metrics.methodology"); CHECK(f.text("fixed peak"));
		const auto preparedId = f.view.reference();
		Action remove; remove.kind = ActionKind::RemoveImage; remove.image = f.second;
		f.apply({remove}); f.frame();
		CHECK(!find_image(f.state, preparedId) && find_image(f.state, f.source));
	}
	{
		Fixture f;
		auto& backend = f.state.backends.front();
		EncoderParamInfo scc;
		scc.name="scc"; scc.label="SCC"; scc.kind=ParamKind::Bool; scc.defaultValue=true;
		scc.enabledWhen={{.parameter="chroma-subsampling", .acceptedValues={"420","444"}}};
		EncoderParamInfo chroma;
		chroma.name="chroma-subsampling"; chroma.label="Chroma"; chroma.kind=ParamKind::Enum;
		chroma.defaultValue=std::string("444");
		chroma.enumValues={{"420","4:2:0"},{"422","4:2:2"},{"444","4:4:4"}};
		backend.params={scc,chroma}; f.frame();
		CHECK(!f.widget("param.10.chroma-subsampling.option.1").enabled);
		f.click("param.10.scc");
		CHECK(f.widget("param.10.chroma-subsampling.option.1").enabled);
	}
	std::cout
			<< "Workbench navigation: preserved crops, independent same-image A/B, stable editable bookmarks, bounded "
			   "paged grid, cursor-anchored zoom, explicit metric eligibility and decoder statistics passed\n";
}
