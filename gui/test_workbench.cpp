#include "metrics.hpp"
#include "test_check.hpp"
#include "workbench.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
using namespace codec_gui;
using namespace codec_gui::gui;
#define CHECK CODEC_VIS_CHECK
int main() {
	{
		AppState state;
		auto pixels = std::make_shared<RawImage>();
		pixels->width = 4; pixels->height = 3;
		pixels->planes[0].bytes = {1, 2, 3, 4};
		Action loaded; loaded.kind = ActionKind::SourceLoaded; loaded.sourceLoaded.image = pixels;
		state = update(state, loaded).state;
		const auto source = state.images.front().id;
		Workbench ui([](std::string_view text, float size, FontRole) { return float(text.size()) * size * .5f; });
		auto frame = [&] {
			for (const auto& action : ui.build(state, {0, 0, 1920, 1080}, 0)) state = update(state, action).state;
		};
		auto input = [&](Action a) {
			for (const auto& action : ui.handle(a, state)) state = update(state, action).state;
			frame();
		};
		frame();
		const auto widgets = ui.context().widgets();
		const auto field = std::ranges::find_if(widgets, [](const auto& w) { return w.id == "import.pixel-aspect"; });
		CHECK(field != widgets.end());
		Action click; click.kind = ActionKind::PointerPressed;
		click.point = {field->rect.x + 10, field->rect.y + 10}; input(click);
		click.kind = ActionKind::PointerReleased; input(click);
		Action key; key.kind = ActionKind::KeyPressed; key.text = "a"; key.control = true; input(key);
		key.control = false; key.text = "4:3"; input(key);
		// No Enter or re-import: the visible source and future imports must agree.
		CHECK(state.importPixelAspectRatio == PixelAspectRatio(4, 3));
		CHECK(find_image(state, source)->decoded->pixelAspectRatio == PixelAspectRatio(4, 3));
		CHECK(find_image(state, source)->decoded->width == 4 && find_image(state, source)->decoded->height == 3);
		CHECK(find_image(state, source)->decoded->planes[0].bytes == pixels->planes[0].bytes);
		CHECK(pixels->pixelAspectRatio.square()); // Immutable snapshots are preserved.
		frame();
		CHECK(std::ranges::any_of(ui.commands(), [](const auto& cmd) { return cmd.text.find("SAR 4:3") != std::string::npos; }));
		CHECK(std::ranges::any_of(ui.commands(), [&](const auto& cmd) {
			return cmd.kind == DrawCommandKind::Image && cmd.image == source &&
				std::abs(cmd.rect.w / cmd.rect.h - 16.0 / 9.0) < 0.001;
		}));
		Action chosen; chosen.kind = ActionKind::SourcePathChosen; chosen.path = "another.nef";
		CHECK(update(state, chosen).commands.front().importPixelAspectRatio == PixelAspectRatio(4, 3));
		key.control = true; key.text = "a"; input(key);
		key.control = false; key.text = "4:"; input(key);
		CHECK(state.errors.empty());
		CHECK(find_image(state, source)->decoded->pixelAspectRatio == PixelAspectRatio(4, 3));
		key.control = true; key.text = "a"; input(key);
		key.control = false; key.text = "auto"; input(key);
		CHECK(find_image(state, source)->decoded->pixelAspectRatio.square());
		CHECK(!state.importPixelAspectRatio);
	}
	AppState s;
	auto     raw = std::make_shared<RawImage>();
	raw->width   = 5600;
	raw->height  = 3728;
	Action load;
	load.kind               = ActionKind::SourceLoaded;
	load.sourceLoaded.path  = "one.266";
	load.sourceLoaded.image = raw;
	s                       = update(std::move(s), load).state;
	ImageId     source      = s.selection.selectedImage;
	BackendInfo backend;
	backend.id   = {10};
	backend.name = "Long encoder implementation";
	// The same group contains both a first-class field and a specialist setting.
	EncoderParamInfo depth;
	depth.name             = "bit-depth";
	depth.label            = "Bit depth";
	depth.group            = "Output Format";
	depth.kind             = ParamKind::Int;
	depth.defaultValue     = int64_t(10);
	depth.intRange         = IntRange{8, 16};
	EncoderParamInfo other = depth;
	other.name             = "matrix";
	other.label            = "Matrix";
	backend.params         = {depth, other};
	EncoderParamInfo note  = depth;
	note.name              = "note";
	note.label             = "Note";
	note.group             = "Specialist";
	note.kind              = ParamKind::String;
	note.defaultValue      = std::string("initial");
	backend.params.push_back(note);
	s.backends                  = {backend};
	s.selection.selectedBackend = backend.id;
	ImageObject candidate       = s.images.front();
	candidate.id                = next_image_id(s);
	candidate.type              = ImageObjectType::EncodedResult;
	candidate.parents           = {source};
	candidate.encoded.emplace();
	candidate.encoded->run = {20};
	candidate.displayName  = "Candidate one";
	s.images.push_back(candidate);
	EncodeRun run;
	run.id                    = {20};
	run.source                = source;
	run.backend               = backend.id;
	run.producedImage         = candidate.id;
	run.state                 = EncodeRunState::Completed;
	s.encodeRuns              = {run};
	s.selection.selectedImage = candidate.id;
	Workbench view([](std::string_view text, float size, FontRole) { return float(text.size()) * size * .5f; });
	auto      build = [&] { return view.build(s, {0, 0, 2880, 1800}, 0); };
	auto      act   = [&](const Action &a) {
		for (auto &produced: view.handle(a, s)) s = update(std::move(s), produced).state;
		for (auto &produced: build()) s = update(std::move(s), produced).state;
	};
	auto click = [&](std::string id) {
		auto ws = view.context().widgets();
		auto it = std::ranges::find_if(ws, [&](const auto &w) { return w.id == id; });
		CHECK(it != ws.end());
		Action a;
		a.kind  = ActionKind::PointerPressed;
		a.point = {it->rect.x + it->rect.w * .5f, it->rect.y + it->rect.h * .5f};
		act(a);
		a.kind = ActionKind::PointerReleased;
		act(a);
	};
	build();
	CHECK(view.reference() == source && view.candidate() == candidate.id);
	CHECK(view.canvas().w > 1800);
	Action zoom;
	zoom.kind  = ActionKind::PointerScrolled;
	zoom.point = {1400, 600};
	zoom.value = -4;
	act(zoom);
	const auto crop = view.viewport();
	CHECK(crop.scale > 0);
	click("selection.assign.0");
	build();
	CHECK(view.reference() == candidate.id);
	ImageObject second  = candidate;
	second.id           = next_image_id(s);
	second.encoded->run = {21};
	second.displayName  = "Candidate two";
	s.images.push_back(second);
	s.selection.selectedImage = second.id;
	run.id                    = {21};
	run.producedImage         = second.id;
	s.encodeRuns.push_back(run);
	build();
	CHECK(view.reference() == candidate.id && view.candidate() == candidate.id);
	click("selection.assign.1");
	CHECK(view.candidate() == second.id);
	CHECK(view.viewport().scale == crop.scale);
	CHECK(view.viewport().centerX == crop.centerX);
	click("detail.2");
	build();
	CHECK(std::ranges::any_of(view.context().widgets(), [](const auto &w) { return w.id == "configuration"; }));
	click("detail.0");
	click("task.2");
	auto selected = view.context().table_state("experiments").selection;
	CHECK(!selected.empty());
	auto widget =
			std::ranges::find_if(view.context().widgets(), [](const auto &w) { return w.id == "metrics.compute"; });
	CHECK(widget != view.context().widgets().end());
	Action press;
	press.kind  = ActionKind::PointerPressed;
	press.point = {widget->rect.x + 10, widget->rect.y + 10};
	act(press);
	press.kind        = ActionKind::PointerReleased;
	auto inputActions = view.handle(press, s);
	CHECK(inputActions.empty());
	auto requests = build();
	CHECK(requests.size() == 1);
	CHECK(requests[0].kind == ActionKind::RequestMetrics);
	CHECK(requests[0].metricIds.size() == metric_catalog().size());
	auto requested = update(s, requests[0]);
	CHECK(requested.commands.size() >= 1);
	CHECK(requested.commands[0].metricIds == requests[0].metricIds);
	s           = std::move(requested.state);
	auto *image = find_image(s, second.id);
	CHECK(image && image->encoded);
	image->encoded->metrics.push_back({.id = "psnr-all", .label = "PSNR", .value = 40.});
	image->encoded->metricState = MetricState::Ready;
	Action more                 = requests[0];
	more.metricIds              = {"psnr-all", "ssim"};
	auto incremental            = update(s, more);
	CHECK(incremental.commands[0].metricIds == std::vector<std::string>{"ssim"});
	CHECK(find_image(incremental.state, second.id)->encoded->metrics.front().value == 40.);
	// Importing another source retains prior records, jobs, and their parent links.
	auto imported = update(s, load).state;
	CHECK(imported.images.size() == s.images.size() + 1);
	CHECK(imported.encodeRuns.size() == s.encodeRuns.size());
	CHECK(find_image(imported, source));
	CHECK(find_image(imported, second.id)->parents.front() == source);
	// An asynchronous difference must not steal a later selection.
	Action diff;
	diff.kind                        = ActionKind::DerivedImageComputed;
	diff.derivedImage.image          = raw;
	diff.derivedImage.first          = source;
	diff.derivedImage.second         = candidate.id;
	diff.derivedImage.firstRevision  = 7;
	diff.derivedImage.secondRevision = 8;
	auto finished                    = update(s, diff).state;
	CHECK(finished.selection.selectedImage == s.selection.selectedImage);
	CHECK(find_image(finished, *finished.viewMode.generatedImage)->derived->firstRevision == 7);
	CHECK(find_image(finished, *finished.viewMode.generatedImage)->derived->secondRevision == 8);
	click("task.1");
	build();
	CHECK(s.decoderInspector);
	click("task.0");
	auto key = [&](std::string text, bool control = false) {
		Action a;
		a.kind    = ActionKind::KeyPressed;
		a.text    = std::move(text);
		a.control = control;
		act(a);
	};
	click("parameters.search");
	key("a", true);
	key("note");
	build();
	click("param.10.note");
	key("a", true);
	key("revised text");
	CHECK(!s.encoderConfigs.empty() && !s.encoderConfigs.back().params.empty());
	CHECK(std::get<std::string>(s.encoderConfigs.back().params.back().value) == "revised text");
	click("parameters.search");
	key("a", true);
	key("bit-depth");
	build();
	click("param.10.bit-depth");
	key("a", true);
	key("99");
	key("enter");
	auto encode = std::ranges::find_if(view.context().widgets(), [](const auto &w) { return w.id == "encode.run"; });
	CHECK(encode != view.context().widgets().end() && !encode->enabled);
	key("a", true);
	key("12");
	key("enter");
	encode = std::ranges::find_if(view.context().widgets(), [](const auto &w) { return w.id == "encode.run"; });
	CHECK(encode->enabled);
	CHECK(view.context().edit_error("param.10.bit-depth").empty());
	auto another = update(s, load).state;
	view.build(another, {0, 0, 2880, 1800}, 0);
	CHECK(view.reference() == candidate.id); // Importing a source preserves explicit pane assignments.
	CHECK(view.context().table_state("experiments").selection.empty());

	std::cout << "Workbench: stable A/B and crop, grouped settings, configuration, chosen incremental metrics, "
				 "retained imports and independent Decode passed\n";
}
