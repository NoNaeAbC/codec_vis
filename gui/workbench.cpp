#include "workbench.hpp"
#include "encoder_param_state.hpp"
#include "image_size_target.hpp"
#include "metrics.hpp"
#include "raw_image_utils.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace codec_gui::gui {
	namespace {
		const BackendInfo *find_backend(const AppState &s, BackendId id) {
			auto i = std::ranges::find_if(s.backends, [&](const auto &b) { return b.id == id; });
			return i == s.backends.end() ? nullptr : &*i;
		}
		std::string number(double x, int precision) {
			std::ostringstream s;
			s << std::fixed << std::setprecision(precision) << x;
			return s.str();
		}
		std::string duration(double seconds) {
			return seconds < 1 ? number(seconds * 1000, 2) + " ms" : number(seconds, 3) + " s";
		}
		std::string bytes(uint64_t n) {
			return n >= 1000000 ? number(double(n) / 1000000, 3) + " MB"
				   : n >= 1000  ? number(double(n) / 1000, 3) + " kB"
								: std::to_string(n) + " B";
		}
		std::string format(PixelFormat f) {
			return std::string(is_rgb(f)    ? "RGB "
							   : is_gray(f) ? "Gray "
							   : is_420(f)  ? "4:2:0 "
							   : is_422(f)  ? "4:2:2 "
											: "4:4:4 ") +
				   std::to_string(bit_depth(f)) + "-bit";
		}
		std::string uncompressed_size(const RawImage* image, bool exact = false) {
			if (!image || image->width <= 0 || image->height <= 0) return "Unavailable";
			uint64_t size = 0;
			for (int p = 0; p < plane_count(image->format); ++p)
				size += uint64_t(plane_width(*image, p)) * plane_height(*image, p) * bytes_per_sample(image->format);
			return bytes(size) + (exact && size >= 1000 ? " (" + std::to_string(size) + " B)" : "");
		}
		std::string lower(std::string s) {
			for (char &c: s) c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}
		std::string sample_description(const RawImage& image) {
			const auto& c = image.color;
			return format(image.format) + " · CICP " + std::to_string(int(c.primaries)) + "/" +
				std::to_string(int(c.transfer)) + "/" + std::to_string(int(c.matrix)) +
				(c.range == ColorRange::Full ? " · full range" : " · limited range");
		}
		bool matches(std::string_view value, const std::string &query) {
			return query.empty() || lower(std::string(value)).find(lower(query)) != std::string::npos;
		}
		const ImageObject *root(const AppState &s, const ImageObject *p) {
			for (std::size_t n = 0; p && n < s.images.size(); ++n) {
				if (p->type == ImageObjectType::Source) return p;
				if (p->parents.empty()) return nullptr;
				p = find_image(s, p->parents.front());
			}
			return nullptr;
		}
		std::string state_text(EncodeRunState s) {
			switch (s) {
				case EncodeRunState::Queued:
					return "Queued";
				case EncodeRunState::Running:
					return "Encoding / preview";
				case EncodeRunState::Completed:
					return "Complete";
				case EncodeRunState::Failed:
					return "Failed";
				case EncodeRunState::Canceled:
					return "Canceled";
				case EncodeRunState::CancelRequested:
					return "Cancel requested";
			}
			return {};
		}
		std::string parameter_text(const ParamValue &value, int decimals) {
			if (const auto *v = std::get_if<double>(&value)) return number(*v, decimals);
			return param_value_text(value);
		}
		std::string summary(const EncodeRun &run) {
			std::string s;
			for (const auto &p: run.params) {
				if (!s.empty()) s += "  ·  ";
				s += p.name + "=" + parameter_text(p.value, 3);
			}
			return s.empty() ? "Defaults" : s;
		}
		const EncodeRun *run_by_id(const AppState &s, uint64_t id) {
			auto i = std::ranges::find_if(s.encodeRuns, [&](const auto &r) { return r.id.value == id; });
			return i == s.encodeRuns.end() ? nullptr : &*i;
		}
		std::string decisive_settings(const EncodeRun& run) {
			std::string text;
			for (const auto& p : run.params) {
				static const std::set<std::string> keys{"rate-control","rate-control-mode","qp","qpi","crf","quality","lossless","near","scc","preset","speed","bpp","target-bpp","target-size-kb","bit-depth","chroma-subsampling"};
				if (!keys.contains(p.name)) continue;
				if (!text.empty()) text += " · ";
				text += p.name + "=" + parameter_text(p.value, 3);
			}
			return text.empty() ? "Defaults" : text;
		}
		std::string image_context(const AppState& s, const ImageObject* image, bool settings = true) {
			if (!image) return "Empty";
			if (!image->encoded) return image->displayName + " · " + format(image->pixelFormat);
			const auto& encoded = *image->encoded;
			std::string text = image->type == ImageObjectType::LivePreview ? "Live" : "#" + std::to_string(encoded.run.value);
			text += " · " + encoded.codecName + " · " + bytes(encoded.byteSize) + " · " + format(image->pixelFormat);
			if (const auto* run = run_by_id(s, encoded.run.value); settings && run) text += " · " + decisive_settings(*run);
			return text;
		}

		std::string metric_text(const EncodedMetadata &m, std::string_view id) {
			if (std::ranges::find(m.requestedMetrics, id) != m.requestedMetrics.end()) {
				if (m.metricState == MetricState::Queued) return "Queued";
				if (m.metricState == MetricState::Running) return "Running";
			}
			if (const auto *v = metric_by_id(m, id)) return v->value ? number(*v->value, 5) : "Failed";
			if (m.metricState == MetricState::Failed &&
				std::ranges::find(m.requestedMetrics, id) != m.requestedMetrics.end())
				return "Failed";
			return "Not requested";
		}

		struct DecodeSummary {
			const DecodeMeasurement *session = nullptr;
			SampleStatistics         codec, total, energy;
			std::size_t              failures = 0;
		};
		DecodeSummary decode_summary(const ImageObject *image) {
			DecodeSummary out;
			if (!image || image->decoder.history.empty()) return out;
			out.session = &image->decoder.history.back();
			std::vector<double> codec, total, energy;
			for (const auto &sample: out.session->samples) {
				total.push_back(sample.seconds);
				if (sample.codecSeconds) codec.push_back(*sample.codecSeconds);
				if (sample.joules) energy.push_back(*sample.joules);
				if (!sample.error.empty()) ++out.failures;
			}
			out.codec  = sample_statistics(codec);
			out.total  = sample_statistics(total);
			out.energy = sample_statistics(energy);
			return out;
		}

		Rect row(Rect area, float y, float h = 32) { return {area.x + 14, y, area.w - 28, h}; }
		void paragraph(ui::Context &u, Rect r, std::string t, bool error = false) {
			u.label(r, std::move(t), error ? u.theme.danger : u.theme.muted, 0, TextAlign::Left, TextOverflow::Wrap);
		}
	} // namespace
	Workbench::Workbench(ui::Context::Measure measure) : ui_(std::move(measure)) {
		ui_.theme.muted = {.57f, .61f, .65f, 1};
		columns_        = {{"panes", "Assign", 156, 156, TextAlign::Left, true, false},
						   {"run", "Run", 85},
						   {"encoder", "Encoder implementation", 320},
						   {"settings", "Settings", 360},
						   {"size", "Encoded size", 140, 100, TextAlign::Right},
		};
		for (const auto &m: metric_catalog())
			columns_.push_back({std::string(m.id),
								std::string(m.label) + (m.unit.empty() ? "" : " (" + std::string(m.unit) + ")"), 180,
								100, TextAlign::Right, false});
		for (const auto &m: metric_catalog()) chosenMetrics_.insert(std::string(m.id));
		for (auto column : std::vector<ui::Column>{
			{"time", "Encode time", 140, 100, TextAlign::Right},
			{"state", "Encode state", 160}, {"decoder", "Measured decoder", 240},
			{"decode", "Decode codec mean", 180, 120, TextAlign::Right},
			{"decode-total", "Decode total mean", 180, 120, TextAlign::Right},
			{"decode-sd", "Decode sample SD", 180, 120, TextAlign::Right}, {"attempts", "Decode attempts", 190}})
			columns_.push_back(std::move(column));
		measurementColumns_ = {{"attempt", "Attempt", 100},
							   {"decoder", "Decoder", 320},
							   {"codec", "Codec time", 160, 100, TextAlign::Right},
							   {"total", "Total time", 160, 100, TextAlign::Right},
							   {"energy", "Raw energy", 160, 100, TextAlign::Right},
							   {"window", "Energy interval", 170, 100, TextAlign::Right},
							   {"outcome", "Outcome", 500}};
	}
	void Workbench::emit(ActionKind k, ImageId image) {
		Action a;
		a.kind  = k;
		a.image = image;
		actions_.push_back(std::move(a));
	}
	ImageId Workbench::pane_image(std::size_t pane) const {
		return pane == 0 ? reference_ : pane == 1 ? candidate_ : extraPanes_[pane - 2];
	}
	void Workbench::assign_pane(const AppState &s, std::size_t pane, ImageId id) {
		const auto *image = find_image(s, id);
		if (!image || pane >= paneCount_) return;
		if (pane == 0) reference_ = id;
		else if (pane == 1)
			candidate_ = id;
		else
			extraPanes_[pane - 2] = id;
		paneRuns_[pane] = image->encoded ? image->encoded->run : EncodeRunId{};
		rebuild_        = true;
	}
	void Workbench::assignment_buttons(const AppState &s, std::string id, ImageId image, Rect rect) {
		for (std::size_t p = 0; p < paneCount_; ++p) {
			Rect button{rect.x + float(p) * 36, rect.y + 2, 32, std::min(28.f, rect.h - 4)};

			if (ui_.button(id + ".assign." + std::to_string(p), std::string(1, char('A' + p)), button,
						   find_image(s, image) != nullptr, pane_image(p) == image && valid(image)))
				assign_pane(s, p, image);
		}
	}
	void Workbench::choose(const AppState &s, ImageId id) {
		const auto *image = find_image(s, id);
		if (!image) return;
		const auto *parent = root(s, image);
		if (parent && parent->id != source_) source_ = parent->id;
		if (image->type == ImageObjectType::Source) {
			auto &t = ui_.table_state("experiments");
			t.selection.clear();
			t.active.reset();
			t.anchor.reset();
		}
		selected_ = observedSelection_ = id;
		emit(ActionKind::SelectImage, id);
	}
	void Workbench::reconcile(const AppState &s) {
		if (pendingEncoderInput_) {
			for (const auto& image : s.images)
				if (image.derived && image.derived->kind == "encoder-input" &&
					std::ranges::find(image.parents, *pendingEncoderInput_) != image.parents.end()) {
					assign_pane(s, 0, image.id);
					assign_pane(s, 1, *pendingEncoderInput_);
					pendingEncoderInput_.reset();
					break;
				}
		}
		if (s.selection.selectedImage != observedSelection_ && find_image(s, s.selection.selectedImage)) {
			const auto *selected = find_image(s, s.selection.selectedImage);
			const auto *parent   = root(s, selected);
			if (parent && parent->id != source_) {
				source_ = parent->id;
				auto &t = ui_.table_state("experiments");
				t.selection.clear();
				t.active.reset();
				t.anchor.reset();
			}
			const bool initial = !valid(observedSelection_);
			selected_ = observedSelection_ = selected->id;
			if (initial) {
				assign_pane(s, 0, source_);
				assign_pane(s, 1, selected_);
			}
			// The first live frame enters the active pane; subsequent frames update its stable image ID.
			if (selected->type == ImageObjectType::LivePreview) {
				bool assigned = false;
				for (std::size_t p = 0; p < paneCount_; ++p) assigned |= pane_image(p) == selected_;
				if (!assigned) assign_pane(s, activePane_, selected_);
			}
			if (selected->encoded && selected->type != ImageObjectType::LivePreview) {
				auto &t     = ui_.table_state("experiments");
				t.active    = selected->encoded->run.value;
				t.selection = {*t.active};
				t.anchor    = t.active;
			}
		}
		for (const auto &run: s.encodeRuns) {
			if (!run.producedImage || !find_image(s, *run.producedImage)) continue;
			const bool fresh = observedResults_.insert(run.id.value).second;
			if (fresh && s.selection.selectedRun == run.id && s.selection.selectedImage == *run.producedImage)
				assign_pane(s, activePane_, *run.producedImage);
		}
		for (std::size_t p = 0; p < paneCount_; ++p) {
			if (find_image(s, pane_image(p))) continue;
			const auto *run = run_by_id(s, paneRuns_[p].value);
			for (std::size_t n = 0; run && run->replacedBy && n < s.encodeRuns.size(); ++n)
				run = run_by_id(s, run->replacedBy->value);
			assign_pane(s, p, run && run->producedImage ? *run->producedImage : source_);
		}
		if (!find_image(s, selected_)) selected_ = source_;
	}
	void Workbench::fit_view(const AppState &s) {
		const auto *image = find_image(s, mode_ == 0 ? pane_image(activePane_) : candidate_);
		if (!image) return;
		Rect r = imageArea_;
		if (mode_ == 1) {
			r.w *= .5f;
			if (paneCount_ > 2) r.h *= .5f;
		}
		if (mode_ == 5) {
			auto cols = std::max(1., std::ceil(std::sqrt(double(gridCount_))));
			auto rows = std::ceil(double(gridCount_) / cols);
			r.w /= float(cols);
			r.h /= float(std::max(1., rows));
		}
		transform_          = fit_transform(image->width, image->height, r, 1);
		referenceTransform_ = transform_;
		extraTransforms_.fill(transform_);
	}

	const Workbench::DisplayedPane *Workbench::pane_at(Point p) const {
		for (const auto &pane: displayed_)
			if (ui::contains(pane.clip, p)) return &pane;
		return nullptr;
	}
	void Workbench::navigate(const DisplayedPane &pane, ViewportTransform view) {
		if (!linked_ && pane.reference && !holdingA_) referenceTransform_ = view;
		else if (!linked_ && pane.paneIndex >= 2)
			extraTransforms_[pane.paneIndex - 2] = view;
		else {
			transform_ = view;
			if (linked_) {
				referenceTransform_ = view;
				extraTransforms_.fill(view);
			}
		}
		fit_ = false;
	}
	std::vector<ImageId> Workbench::grid_candidates(const AppState &s) {
		std::vector<ImageId> images;
		for (auto id: ui_.table_state("experiments").selection) {
			const auto *r = run_by_id(s, id);
			if (!r || r->source != source_ || !r->producedImage || *r->producedImage == reference_) continue;
			if (const auto *image = find_image(s, *r->producedImage); image && image->decoded)
				images.push_back(image->id);
		}
		if (images.empty() && candidate_ != reference_)
			if (const auto *image = find_image(s, candidate_); image && image->decoded) images.push_back(candidate_);
		return images;
	}
	std::vector<Action> Workbench::handle(const Action &a, const AppState &s) {
		actions_.clear();
		auto input = ui::input_from_action(a);
		if (a.kind == ActionKind::WindowFocusLost) {
			dragging_ = splitDragging_ = holdingA_ = false;
			dragPane_.reset();
		}
		if (a.kind == ActionKind::KeyReleased && a.text == " ") holdingA_ = false;
		bool consumed = input && ui_.handle(*input), editing = false;
		for (const auto &w: ui_.widgets())
			if (w.id == ui_.focus() && (w.role == ui::Role::TextBox || w.role == ui::Role::Number)) editing = true;
		if (a.kind == ActionKind::KeyPressed && !consumed && !editing && !a.control && !a.alt) {
			if (a.text == " ") {
				holdingA_ = true;
				consumed  = true;
			}
			if (a.text == "f") {
				sizes_.focus = !sizes_.focus;
				consumed     = true;
			}
			if (a.text == "0") {
				fit_     = true;
				consumed = true;
			}
			if (a.text == "1") {
				transform_.scale    = 1 / deviceScale_;
				referenceTransform_ = transform_;
				extraTransforms_.fill(transform_);
				fit_     = false;
				consumed = true;
			}
			if (a.text == "escape") {
				sizes_.focus = false;
				holdingA_    = false;
				consumed     = true;
			}
		}
		if (a.kind == ActionKind::PointerMoved) {
			pointer_ = a.point;
			if (splitDragging_) split_ = std::clamp(double((a.point.x - imageArea_.x) / imageArea_.w), .02, .98);
			if (dragging_ && dragPane_) {
				if (std::hypot(a.point.x - pressPoint_.x, a.point.y - pressPoint_.y) >= 4) moved_ = true;
				if (moved_)
					if (const auto *image = find_image(s, dragPane_->image)) {
						auto view = pan_transform(dragPane_->transform, *image, dragPane_->transformRect,
												  a.point.x - dragPoint_.x, a.point.y - dragPoint_.y);
						navigate(*dragPane_, view);
						dragPane_->transform = view;
						dragPoint_           = a.point;
					}
			}
		}
		if (a.kind == ActionKind::PointerReleased && a.button == 272) {
			if (!consumed && dragging_ && dragPane_ && !moved_ && !holdingA_ &&
				ui::contains(dragPane_->clip, a.point)) {
				const auto *clicked = find_image(s, dragPane_->image);
				if (clicked) {
					if (mode_ == 1) activePane_ = dragPane_->paneIndex;
					choose(s, clicked->id);
					if (clicked->encoded && clicked->type != ImageObjectType::LivePreview)
						ui_.table_state("experiments").active = clicked->encoded->run.value;
				}
			}
			dragging_ = splitDragging_ = false;
			dragPane_.reset();
		}
		if (!consumed && ui::contains(imageArea_, a.point)) {
			if (a.kind == ActionKind::PointerPressed && a.button == 272) {
				splitDragging_ = mode_ == 2 && !holdingA_ &&
								 std::abs(a.point.x - imageArea_.x - imageArea_.w * float(split_)) < 12;
				if (const auto *pane = pane_at(a.point); pane && !splitDragging_) {
					dragPane_   = *pane;
					dragging_   = true;
					pressPoint_ = dragPoint_ = a.point;
					moved_                   = false;
				}
                consumed = splitDragging_ || dragging_;
			}
			if (a.kind == ActionKind::PointerScrolled)
				if (const auto *pane = pane_at(a.point))
					if (const auto *image = find_image(s, pane->image))
						navigate(*pane, zoom_transform(pane->transform, *image, pane->transformRect, a.point,
													   std::exp(-a.value * .1)));
		}
        inputConsumed_ = consumed;
		return std::move(actions_);
	}

	std::vector<Action> Workbench::build(const AppState &s, Rect bounds, double seconds, float deviceScale) {
		actions_.clear();
		rebuild_     = false;
		deviceScale_ = deviceScale;
		reconcile(s);
		auto visibleSizes = sizes_;
		if (!sourcesVisible_) visibleSizes.left = 0;
		if (!toolsVisible_) visibleSizes.right = 0;
		if (!resultsVisible_) visibleSizes.bottom = 0;
		if (resultsVisible_ && detail_ == 3 && plotExpanded_)
			visibleSizes.bottom = std::max(sizes_.bottom, bounds.h * .52f);
		dock_      = ui::dock_layout(bounds, visibleSizes);
		imageArea_ = {dock_.canvas.x, dock_.canvas.y + 88, dock_.canvas.w, std::max(0.f, dock_.canvas.h - 88)};
		ui_.begin_frame(bounds);
		ui_.fill(bounds, ui_.theme.background);
		ui_.fill(dock_.bar, ui_.theme.surface);
		if (ui_.button("import", s.interaction.importPending ? "Importing…" : "Import image…", {14, 10, 155, 32},
					   !s.interaction.importPending))
			emit(ActionKind::RequestOpenSource);
		const auto *source = find_image(s, source_);
		ui_.label({185, 10, std::max(0.f, bounds.w - 870), 32},
				  source ? source->displayName + "   /   " + std::to_string(source->width) + " × " +
								   std::to_string(source->height) + "   " + format(source->pixelFormat)
						 : "Image comparison workbench",
				  ui_.theme.muted);
		std::size_t jobs = 0;
		if (s.livePreview.busy) ++jobs;
		for (const auto &r: s.encodeRuns)
			if (r.state == EncodeRunState::Queued || r.state == EncodeRunState::Running ||
				r.state == EncodeRunState::CancelRequested)
				++jobs;
		for (const auto &i: s.images)
			if (i.decoder.running || (i.encoded && (i.encoded->metricState == MetricState::Queued ||
													i.encoded->metricState == MetricState::Running)))
				++jobs;
		ui_.label({bounds.w - 680, 10, 180, 32}, jobs ? std::to_string(jobs) + " active jobs" : "",
				  ui_.theme.muted, 0, TextAlign::Right);
		auto panelToggle = [&](const char* id, const char* name, float x, float width, bool& shown) {
			if (ui_.button(id, name, {x, 10, width, 32}, true, shown && !sizes_.focus)) {
				shown = sizes_.focus ? true : !shown; sizes_.focus = false; rebuild_ = true;
			}
		};
		panelToggle("panels.sources", "Sources", bounds.w - 482, 104, sourcesVisible_);
		panelToggle("panels.tools", "Tools", bounds.w - 370, 96, toolsVisible_);
		panelToggle("panels.results", "Results", bounds.w - 266, 112, resultsVisible_);
		if (ui_.button("focus", sizes_.focus ? "Exit focus · F" : "Focus · F", {bounds.w - 145, 10, 130, 32}))
			sizes_.focus = !sizes_.focus;
		if (!sizes_.focus) {
			if (dock_.left.w > 0) sources(s);
			if (dock_.right.w > 0) tools(s);
			if (dock_.bottom.h > 0) experiments(s);
			if (dock_.left.w > 0) ui_.splitter("dock.left", dock_.leftHandle, sizes_.left, ui::Axis::Horizontal, 190, 500);
			if (dock_.right.w > 0) ui_.splitter("dock.right", dock_.rightHandle, sizes_.right, ui::Axis::Horizontal, 380, 760, true);
			if (dock_.bottom.h > 0) ui_.splitter("dock.bottom", dock_.bottomHandle, sizes_.bottom, ui::Axis::Vertical, 180,
						 std::max(180.f, bounds.h - 400), true);
		}
		canvas_view(s, seconds);
		ui_.fill(dock_.status, ui_.theme.surface);
		std::string status;
		for (const auto &displayed: displayed_) {
			if (!ui::contains(displayed.clip, pointer_)) continue;
			const auto *image = find_image(s, displayed.image);
			if (image) {
				Pane pane;
				pane.transform = displayed.transform;
				if (auto pixel = pane_to_image_coord(pane, *image, displayed.transformRect, pointer_)) {
					status = std::string(mode_ == 4 && !holdingA_ ? "B samples under difference · " : "") + "x " +
							 std::to_string(pixel->x) + "  y " + std::to_string(pixel->y) + "   " +
							 format(image->pixelFormat);
					if (image->decoded) {
						status += "   codes [";
						for (int p = 0; p < plane_count(image->pixelFormat); ++p) {
							if (p) status += ", ";
							int x = p && !is_rgb(image->pixelFormat)
											? pixel->x /
													  (is_420(image->pixelFormat) || is_422(image->pixelFormat) ? 2 : 1)
											: pixel->x;
							int y = p && is_420(image->pixelFormat) ? pixel->y / 2 : pixel->y;
							const auto &plane  = image->decoded->planes[p];
							std::size_t offset = std::size_t(y) * plane.strideBytes +
												 std::size_t(x) * bytes_per_sample(image->pixelFormat);
							if (offset + bytes_per_sample(image->pixelFormat) <= plane.bytes.size()) {
								unsigned value = plane.bytes[offset];
								if (bytes_per_sample(image->pixelFormat) == 2)
									value |= unsigned(plane.bytes[offset + 1]) << 8;
								status += std::to_string(value);
							}
						}
						status += "]";
					}
				}
			}
		}
		ui_.label({14, dock_.status.y, bounds.w - 420, dock_.status.h}, status, ui_.theme.muted, 14);
		ui_.label({bounds.w - 400, dock_.status.y, 386, dock_.status.h}, "Linear BT.2020",
				  ui_.theme.muted, 14, TextAlign::Right);
		// A single dismissible diagnostic, with the full message available in a wide row.
		if (!s.errors.empty()) {
			const auto &e = s.errors.back();
			Rect        r = {dock_.canvas.x + 12, dock_.canvas.y + 94, dock_.canvas.w - 24, 76};
			ui_.fill(r, ui_.theme.elevated);
			ui_.label({r.x + 10, r.y + 6, r.w - 112, 64}, e.operation + ": " + e.message, ui_.theme.danger, 0,
					  TextAlign::Left, TextOverflow::Wrap);
			if (ui_.button("error.dismiss", "Dismiss", {r.x + r.w - 96, r.y + 20, 86, 32})) {
				Action a;
				a.kind  = ActionKind::ErrorDismissed;
				a.value = double(e.id);
				actions_.push_back(a);
			}
		}
		ui_.end_frame();
		return std::move(actions_);
	}
	void Workbench::sources(const AppState &s) {
		ui_.fill(dock_.left, ui_.theme.surface);
		ui_.label(row(dock_.left, dock_.left.y + 12), "SOURCES", ui_.theme.muted, 13);
		std::vector<const ImageObject *> sources;
		for (const auto &i: s.images)
			if (i.type == ImageObjectType::Source) sources.push_back(&i);
		const auto bookmarkCount = std::ranges::count_if(bookmarks_, [&](const auto& b) { return b.source == source_; });
		const float bookmarkHeight = std::min(dock_.left.h * .45f, 90.f + float(bookmarkCount) * 78);
		auto scroll = ui_.begin_scroll(
				"sources", {dock_.left.x, dock_.left.y + 52, dock_.left.w, std::max(0.f, dock_.left.h - bookmarkHeight - 62)},
				{dock_.left.w, float(sources.size()) * 156});
		float y = scroll.content.y;
		for (const auto *image: sources) {
			Rect r = row(dock_.left, y, 148);
			if (image->id == source_) ui_.fill(r, ui_.theme.elevated);
			if (ui_.button("source." + std::to_string(image->id.value), image->displayName,
						   {r.x + 4, r.y + 2, r.w - 8, 32}))
				choose(s, image->id);
			ui_.label({r.x + 10, r.y + 36, r.w - 20, 24},
					  std::to_string(image->width) + " × " + std::to_string(image->height) + "  " +
							  format(image->pixelFormat),
					  ui_.theme.muted, 13);
			const Rect sizeRect{r.x + 10, r.y + 60, r.w - 20, 24};
			ui_.label(sizeRect, "Uncompressed: " + uncompressed_size(image->decoded.get()), ui_.theme.text, 13);
			ui_.tooltip(sizeRect, "Native pixel storage excluding row padding and file headers. Samples above 8 bits occupy 16 bits.\n" +
				uncompressed_size(image->decoded.get(), true));
			ui_.label({r.x + 10, r.y + 84, r.w - 20, 24},
				"File: " + (image->sourceByteSize ? bytes(*image->sourceByteSize) : "Unavailable"), ui_.theme.muted, 13);
			assignment_buttons(s, "source." + std::to_string(image->id.value), image->id,
						   {r.x + 8, r.y + 114, r.w - 16, 32});
			y += 156;
		}
		if (sources.empty())
			paragraph(ui_, row(dock_.left, y, 90), "Import an image to begin. Each source keeps its own encodes.");
		ui_.end_scroll();

		y = dock_.left.y + std::max(100.f, dock_.left.h - bookmarkHeight);
		ui_.label(row(dock_.left, y), "INSPECTION VIEWS", ui_.theme.muted, 13);
		y += 38;
		if (ui_.button("bookmark.add", "Bookmark this view", row(dock_.left, y), valid(candidate_))) {
			auto id = nextBookmark_++;
			bookmarks_.push_back({id, "View " + std::to_string(id), source_, transform_, referenceTransform_, linked_});
			rebuild_ = true;
		}
		y += 42;
		float height = std::max(0.f, dock_.left.y + dock_.left.h - y - 12);
		auto  count  = std::ranges::count_if(bookmarks_, [&](const auto &b) { return b.source == source_; });
		auto  region = ui_.begin_scroll("bookmarks", {dock_.left.x, y, dock_.left.w, height},
										{dock_.left.w, float(count) * 78});
		y            = region.content.y;
		std::optional<uint64_t> remove;
		for (auto &bookmark: bookmarks_)
			if (bookmark.source == source_) {
				auto id = "bookmark." + std::to_string(bookmark.id);
				auto edited =
						ui_.text_input(id + ".name", {dock_.left.x + 14, y, dock_.left.w - 126, 32}, bookmark.name);
				if (edited.committed && bookmark.name.empty()) bookmark.name = "View " + std::to_string(bookmark.id);
				if (ui_.button(id + ".remove", "Remove", {dock_.left.x + dock_.left.w - 104, y, 90, 32}))
					remove = bookmark.id;
				if (ui_.button(id + ".go", "Go", {dock_.left.x + 14, y + 36, 64, 30})) {
					transform_          = bookmark.transform;
					referenceTransform_ = bookmark.referenceTransform;
					linked_             = bookmark.linked;
					fit_                = false;
				}
				ui_.label({dock_.left.x + 88, y + 36, dock_.left.w - 106, 30},
						  number(bookmark.transform.scale * deviceScale_ * 100, 1) + "% · " +
								  (bookmark.linked ? "linked" : "unlinked"),
						  ui_.theme.muted, 13);
				y += 78;
			}
		ui_.end_scroll();
		if (remove) {
			std::erase_if(bookmarks_, [&](const auto &b) { return b.id == *remove; });
			rebuild_ = true;
		}
	}

	void Workbench::canvas_view(const AppState &s, double seconds) {
		displayed_.clear();
		Rect area = dock_.canvas;
		ui_.fill(area, ui_.theme.background);
		const std::vector<ui::Choice> modes = {{"Single pane"}, {"Multiple panes"},   {"Split wipe A / B"},
											   {"Blink A / B"}, {"Difference A / B"}, {"Selection grid"}};
		ui_.combo("view.mode", {area.x + 12, area.y + 10, 180, 32}, modes, mode_);
		ui_.tooltip({area.x + 12, area.y + 10, 180, 32}, "Comparison view\nScroll: zoom · Drag: pan · Space: show A\n0: fit · 1: actual pixels · F: focus");
		auto        candidates    = grid_candidates(s);
		const auto *liveCandidate = find_image(s, candidate_);
		const bool  liveGrid =
				liveCandidate && liveCandidate->decoded && liveCandidate->type == ImageObjectType::LivePreview;
		if (liveGrid) std::erase(candidates, candidate_);
		const std::size_t pageSize = liveGrid ? 2 : 3;
		const auto        pages    = std::max(std::size_t(1), (candidates.size() + pageSize - 1) / pageSize);
		gridPage_                  = std::min(gridPage_, pages - 1);
		if (mode_ == 5) {
			if (ui_.button("grid.previous", "Previous", {area.x + 620, area.y + 10, 100, 32}, gridPage_ > 0)) {
				--gridPage_;
				rebuild_ = true;
			}
			if (ui_.button("grid.next", "Next", {area.x + 728, area.y + 10, 80, 32}, gridPage_ + 1 < pages)) {
				++gridPage_;
				rebuild_ = true;
			}
			ui_.label({area.x + 820, area.y + 10, std::max(0.f, area.w - 834), 32},
					  "Page " + std::to_string(gridPage_ + 1) + " / " + std::to_string(pages) + " · " +
							  std::to_string(candidates.size()) + (liveGrid ? " selected + live B" : " candidates") +
							  " · click a tile to inspect",
					  ui_.theme.muted, 14);
		}
		auto gridBegin = std::min(candidates.size(), gridPage_ * pageSize),
			 gridEnd   = std::min(candidates.size(), gridBegin + pageSize);
		gridCount_     = (find_image(s, reference_) ? 1 : 0) + (liveGrid ? 1 : 0) + gridEnd - gridBegin;
		if (ui_.button("view.fit", "Fit · 0", {area.x + 202, area.y + 10, 86, 32})) fit_ = true;
		if (ui_.button("view.actual", "1:1 · 1", {area.x + 296, area.y + 10, 86, 32})) {
			transform_.scale    = 1 / deviceScale_;
			referenceTransform_ = transform_;
			extraTransforms_.fill(transform_);
			fit_ = false;
		}
		ui_.checkbox("view.link", "Linked", {area.x + 392, area.y + 10, 108, 32}, linked_);

		if (mode_ == 4) {
			double gain = gain_;
			ui_.label({area.x + 620, area.y + 10, 50, 32}, "Gain", ui_.theme.muted);
			if (ui_.number_input("view.gain", {area.x + 674, area.y + 10, 86, 32}, gain, {0, 256, .1, false, 1})
						.committed)
				gain_ = gain;
		}
		const auto *a    = find_image(s, reference_);
		const auto *b    = find_image(s, candidate_);
		float       half = area.w * .5f;
		if (mode_ == 1 || mode_ == 0) {
			for (std::size_t p = 0; p < paneCount_; ++p) {
				Rect r{area.x + 14 + float(p) * 44, area.y + 51, 38, 28};

				if (ui_.button("pane.active." + std::to_string(p), std::string(1, char('A' + p)), r, true,
							   activePane_ == p))
					activePane_ = p;
			}
			if (ui_.button("pane.add", "+ pane", {area.x + 204, area.y + 51, 88, 28}, paneCount_ < 4)) {
				++paneCount_;
				assign_pane(s, paneCount_ - 1, source_);
				mode_ = 1;
				fit_  = true;
			}
			if (ui_.button("pane.remove", "− pane", {area.x + 300, area.y + 51, 88, 28}, paneCount_ > 2)) {
				--paneCount_;
				activePane_ = std::min(activePane_, paneCount_ - 1);
				fit_        = true;
			}
			const auto *active = find_image(s, pane_image(activePane_));
			ui_.label({area.x + 404, area.y + 51, area.w - 418, 28},
					  std::string(1, char('A' + activePane_)) + " · " + (active ? active->displayName : "Empty") +
							  " · Inspecting " + image_context(s, find_image(s, selected_)),
					  ui_.theme.muted, 14);
		} else {
			ui_.label({area.x + 14, area.y + 51, half - 24, 26}, "A  " + image_context(s, a),
					  ui_.theme.muted, 14);
			ui_.label({area.x + half, area.y + 51, half - 14, 26}, "B  " + image_context(s, b),
					  ui_.theme.text, 14);
			ui_.tooltip({area.x + 14, area.y + 51, half - 24, 26}, "Viewing A: " + image_context(s, a));
			ui_.tooltip({area.x + half, area.y + 51, half - 14, 26}, "Viewing B: " + image_context(s, b));
		}
		ui_.line({area.x, area.y + 86}, {area.x + area.w, area.y + 86}, ui_.theme.border);
		if (fit_) fit_view(s);
		ui_.label({area.x + 510, area.y + 10, 100, 32}, number(transform_.scale * deviceScale_ * 100, 1) + "%",
				  ui_.theme.muted);
		auto draw = [&](const ImageObject *image, Rect clip, Rect transformRect, ViewportTransform view,
						std::string caption, bool referenceRole = false, ImageId comparison = {},
						std::size_t paneIndex = 1) {
			ui_.push_clip(clip);
			if (image && image->decoded) {
				Pane pane;
				pane.transform = view;
				if (valid(comparison))
					ui_.image_difference(image_rect_in_pane(pane, *image, transformRect), image->id, comparison,
										 float(gain_));
				else
					ui_.image(image_rect_in_pane(pane, *image, transformRect), image->id);
				displayed_.push_back(
						{image->id, clip, transformRect, view, referenceRole, referenceRole ? 0 : paneIndex});
			} else
				paragraph(ui_, {clip.x + 30, clip.y + 50, clip.w - 60, 120},
						  image && image->encoded ? image->encoded->previewError
												  : "Import a source, choose an encoder, then encode an image.");
			if (!caption.empty()) {
				const std::string context = image_context(s, image);
				if (!valid(comparison)) {
					const std::string role = caption.size() > 1 && caption[0] >= 'A' && caption[0] <= 'D' && caption[1] == ' ' ? caption.substr(0, 1) + " · " : "";
					caption = role + context;
				}
				ui_.fill({clip.x + 12, clip.y + 12, std::min(clip.w - 24, 360.f), 28}, ui_.theme.surface);
				ui_.label({clip.x + 20, clip.y + 12, std::min(clip.w - 40, 344.f), 28}, std::move(caption),
						  ui_.theme.text, 13);
				ui_.tooltip({clip.x + 12, clip.y + 12, std::min(clip.w - 24, 360.f), 28}, context);
			}
			ui_.pop_clip();
		};
		if (holdingA_) {
			draw(a, imageArea_, imageArea_, transform_, "A (Space)", true);
			return;
		}
		if (mode_ == 4 && a && b) {
			if (a->width != b->width || a->height != b->height)
				paragraph(ui_, {imageArea_.x + 30, imageArea_.y + 40, 600, 70},
						  "Difference needs matching image dimensions.");
			else if (a->decoded && b->decoded)
				draw(b, imageArea_, imageArea_, transform_, "Max linear RGB difference × " + number(gain_, 1), false,
					 a->id);
			else
				paragraph(ui_, {imageArea_.x + 30, imageArea_.y + 40, 600, 70},
						  "Difference needs decoded A and B images.");
		} else if (mode_ == 1) {
			for (std::size_t p = 0; p < paneCount_; ++p) {
				Rect        r{imageArea_.x + float(p % 2) * half,
							  imageArea_.y + float(p / 2) * imageArea_.h / (paneCount_ > 2 ? 2 : 1), half,
							  imageArea_.h / (paneCount_ > 2 ? 2 : 1)};
				const auto *image = find_image(s, pane_image(p));
				auto        view  = fit_ && image        ? fit_transform(image->width, image->height, r, 1)
									: p == 0 && !linked_ ? referenceTransform_
									: p >= 2 && !linked_ ? extraTransforms_[p - 2]
														 : transform_;
				draw(image, r, r, view, std::string(1, char('A' + p)) + " · " + (image ? image->displayName : "Empty"),
					 p == 0, {}, p);
				if (activePane_ == p) ui_.border(ui::inset(r, 1), ui_.theme.focus, 1);
				else
					ui_.border(r, ui_.theme.border, 1);
			}
		} else if (mode_ == 2) {
			Rect left = imageArea_, right = imageArea_;
			left.w = imageArea_.w * float(split_);
			right.x += left.w;
			right.w -= left.w;
			draw(a, left, imageArea_, linked_ ? transform_ : referenceTransform_, "A", true);
			draw(b, right, imageArea_, transform_, "B");
			ui_.line({right.x, right.y}, {right.x, right.y + right.h}, ui_.theme.focus, 2);
		} else if (mode_ == 3) {
			bool first = int(seconds * 2) % 2 == 0;
			draw(first ? a : b, imageArea_, imageArea_, first && !linked_ ? referenceTransform_ : transform_,
				 first ? "A" : "B", first);

		} else if (mode_ == 5) {
			std::vector<const ImageObject *> images;
			if (a) images.push_back(a);
			if (liveGrid) images.push_back(liveCandidate);
			for (auto i = gridBegin; i < gridEnd; ++i)
				if (auto *image = find_image(s, candidates[i])) images.push_back(image);
			int cols = std::max(1, int(std::ceil(std::sqrt(double(images.size()))))),
				rows = std::max(1, int((images.size() + cols - 1) / cols));
			for (std::size_t i = 0; i < images.size(); ++i) {
				Rect        r       = {imageArea_.x + float(i % cols) * imageArea_.w / cols,
									   imageArea_.y + float(i / cols) * imageArea_.h / rows, imageArea_.w / cols,
									   imageArea_.h / rows};
				bool        ref     = images[i]->id == reference_;
				auto        view    = fit_              ? fit_transform(images[i]->width, images[i]->height, r, 1)
									  : ref && !linked_ ? referenceTransform_
														: transform_;
				std::string caption = ref ? "A · " : images[i]->id == candidate_ ? "B · " : "";
				if (images[i]->encoded && images[i]->type != ImageObjectType::LivePreview)
					caption += "#" + std::to_string(images[i]->encoded->run.value) + " · ";
				draw(images[i], r, r, view, caption + images[i]->displayName, ref);
				if (images[i]->id == candidate_) ui_.border(ui::inset(r, 2), ui_.theme.focus, 2);
			}

		} else
			draw(find_image(s, pane_image(activePane_)), imageArea_, imageArea_,
				 linked_ || activePane_ == 1 ? transform_
				 : activePane_ == 0          ? referenceTransform_
											 : extraTransforms_[activePane_ - 2],
				 std::string(1, char('A' + activePane_)), activePane_ == 0, {}, activePane_);
	}
	void Workbench::tools(const AppState &s) {
		ui_.fill(dock_.right, ui_.theme.surface);
		float w = (dock_.right.w - 28) / 3;
		for (std::size_t i = 0; i < 3; ++i) {
			Rect r = {dock_.right.x + 14 + float(i) * w, dock_.right.y + 12, w - 4, 34};

			if (ui_.button("task." + std::to_string(i),
						   i == 0   ? "Encode"
						   : i == 1 ? "Decode"
									: "Metrics",
						   r, true, i == task_)) {
				task_ = i;
				emit(i == 1 ? ActionKind::ShowDecoderInspector : ActionKind::ShowEncoderInspector);
			}
		}
		float tabX = dock_.right.x + 14 + float(task_) * w;
		ui_.line({tabX, dock_.right.y + 47}, {tabX + w - 4, dock_.right.y + 47}, ui_.theme.focus, 2);
		Rect body = {dock_.right.x, dock_.right.y + 58, dock_.right.w, dock_.right.h - 58};
		if (task_ == 0) encode_tools(s, body);
		else if (task_ == 1)
			decode_tools(s, body);
		else
			metric_tools(s, body);
	}
	void Workbench::encode_tools(const AppState &s, Rect area) {
		float                   y = area.y;
		const auto* source = find_image(s, source_);
		ui_.label(row(area, y, 28), "New encode · " + (source ? source->displayName : "No source"), ui_.theme.muted, 14);
		y += 32;
		std::vector<ui::Choice> choices;
		std::size_t             selected = 0;
		for (std::size_t i = 0; i < s.backends.size(); ++i) {
			const auto &b = s.backends[i];
			choices.push_back({b.name, b.capabilities.available});
			if (b.id == s.selection.selectedBackend) selected = i;
		}
		ui_.label(row(area, y), "Encoder implementation", ui_.theme.muted, 14);
		y += 30;
		if (ui_.combo("encoder", row(area, y, 36), choices, selected) && selected < s.backends.size()) {
			Action a;
			a.kind    = ActionKind::SelectBackend;
			a.backend = s.backends[selected].id;
			actions_.push_back(a);
		}
		y += 48;
		const auto *backend = find_backend(s, s.selection.selectedBackend);
		if (!backend) {
			paragraph(ui_, row(area, y, 80), "No encoder is available.");
			return;
		}
		ui_.text_input("parameters.search", {area.x + 14, y, area.w - 122, 32}, paramFilter_, true, "Search settings…");
		ui_.checkbox("parameters.help", "Help", {area.x + area.w - 100, y, 88, 32}, showHelp_);
		y += 42;
		const auto *candidate   = find_image(s, selected_);
		const bool  liveCapable = backend->kind == BackendKind::Hardware || backend->codec == "JPEG XS";
		if (liveCapable) {
			bool enabled = s.livePreview.enabled;
			if (ui_.checkbox("live.enabled", "Live feedback while editing", row(area, y), enabled)) {
				emit(ActionKind::SetLivePreviewEnabled);
				actions_.back().value = enabled ? 1 : 0;
			}
			y += 36;
			const auto &live    = s.livePreview;
			std::string quality = enabled ? "Waiting for live feedback" : "Live feedback paused";
			std::string encode = "—", decode = "—", pipeline = "—", size = "—";
			if (live.backend == backend->id && live.source == source_) {
				if (!live.error.empty()) quality = live.error;
				else if (live.displayed) {
					quality = "Shown:";
					for (const auto &p: live.shownParams)
						if (p.name == "qp" || p.name == "qpi" || p.name == "qindex" || p.name == "icq-quality" ||
							p.name == "bpp" || p.name == "target-bpp" || p.name == "bitrate-kbps" ||
							p.name == "bitrate" || p.name == "target-size-kb") {
							quality += p.name == "target-size-kb" ? " target " : " " + p.name + " ";
							if (const auto *v = std::get_if<double>(&p.value))
								quality += number(*v, p.name == "target-size-kb" ? 3 : 2);
							else
								quality += param_value_text(p.value);
							if (p.name == "target-size-kb") quality += " kB";
						}
					if (const auto *shown = find_image(s, live.image); shown && shown->encoded) {
						encode = number(shown->encoded->encodeSeconds * 1000, 2);
						decode = number(shown->encoded->decodeSeconds * 1000, 2);
						size   = number(double(shown->encoded->byteSize) / 1000., 3) + " kB";
						if (shown->width > 0 && shown->height > 0)
							size += " · " +
									number(8. * double(shown->encoded->byteSize) /
												   (double(shown->width) * shown->height),
										   2) +
									" bpp";
						if (const auto target = requested_image_size_bytes(live.shownParams); target && *target)
							size += " · " + number(100. * double(shown->encoded->byteSize) / *target, 0) + "% target";
					}
					pipeline = number(live.seconds * 1000, 2);
				}
			}
			ui_.label(row(area, y, 24), quality, live.error.empty() ? ui_.theme.muted : ui_.theme.danger, 14);
			ui_.label({area.x + area.w - 112, y, 98, 24}, live.busy ? "Updating…" : "", ui_.theme.muted, 13,
					  TextAlign::Right);
			y += 26;
			const float width = (area.w - 28) / 2;
			ui_.label({area.x + 14, y, width, 24}, "Encode " + encode + " ms", ui_.theme.muted, 13, TextAlign::Left,
					  TextOverflow::Clip, FontRole::Monospace);
			ui_.label({area.x + 14 + width, y, width, 24}, "Decode " + decode + " ms", ui_.theme.muted, 13,
					  TextAlign::Left, TextOverflow::Clip, FontRole::Monospace);
			y += 24;
			ui_.label(row(area, y, 24), "Queued pipeline " + pipeline + " ms", ui_.theme.muted, 13, TextAlign::Left,
					  TextOverflow::Clip, FontRole::Monospace);
			y += 24;
			ui_.label(row(area, y, 24), "Size " + size, ui_.theme.text, 13, TextAlign::Left, TextOverflow::Clip,
					  FontRole::Monospace);
			y += 30;
		}
		const float footer = area.y + area.h - 132;
		struct Field {
			const EncoderParamInfo *info;
			bool                    pinned;
		};
		std::vector<Field> fields;
		for (const auto &p: backend->params)
			if (p.relevantForStillImage && matches(p.name + " " + p.label + " " + p.group, paramFilter_))
				fields.push_back({&p, pinned_.contains(std::to_string(backend->id.value) + "/" + p.name)});
		auto essential = [](const EncoderParamInfo &p) {
			static const std::set<std::string> names = {"profile",
														"scc",
														"bit-depth",
														"chroma",
														"chroma-format",
														"chroma-subsampling",
														"subsampling",
														"rate-control",
														"rate-control-mode",
														"qp",
														"qpi",
														"crf",
														"quality",
														"qindex",
														"icq-quality",
														"bitrate-kbps",
														"target-size-kb",
														"bitrate",
														"target-usage",
														"preset",
														"speed",
														"effort",
														"enc-mode",
														"target-bpp",
														"bpp",
														"lossless"};
			return names.contains(p.name);
		};
		auto priority  = [&](const Field &f) { return f.pinned ? 0 : essential(*f.info) ? 1 : 4; };
		auto groupName = [&](const Field &f) {
			return f.pinned                ? std::string("Pinned settings")
				   : essential(*f.info)    ? std::string("Core settings")
				   : f.info->group.empty() ? std::string("Settings")
										   : f.info->group;
		};

		std::map<std::string, int> groupPriority;
		for (const auto &f: fields) {
			auto name           = groupName(f);
			auto [it, inserted] = groupPriority.try_emplace(name, priority(f));
			if (!inserted) it->second = std::min(it->second, priority(f));
		}
		std::stable_sort(fields.begin(), fields.end(), [&](const auto &a, const auto &b) {
			auto ga = groupName(a), gb = groupName(b);
			return std::pair{groupPriority[ga], ga} < std::pair{groupPriority[gb], gb};
		});

		float content = parameterHeight_;
		auto  scroll  = ui_.begin_scroll("parameters." + std::to_string(backend->id.value),
										 {area.x, y, area.w, std::max(0.f, footer - y - 6)}, {area.w, content});
		y             = scroll.content.y;
		std::string lastGroup;
		for (const auto &f: fields) {
			const auto &p       = *f.info;
			std::string group   = groupName(f);
			std::string groupId = std::to_string(backend->id.value) + "/" + group;
			if (group != lastGroup) {
				auto [it, inserted] = groups_.try_emplace(groupId, groupPriority[group] < 4);
				(void) inserted;
				ui_.disclosure("group." + groupId, group, row(area, y), it->second);
				y += 38;
				lastGroup = group;
			}
			if (!groups_[groupId] && paramFilter_.empty()) continue;
			const std::string id      = "param." + std::to_string(backend->id.value) + "." + p.name;
			bool              enabled = parameter_is_enabled(s, *backend, p);
			auto              value   = resolved_param_value(s, backend->id, p);
			bool              changed = false;
			const bool        rate    = p.name == "bitrate-kbps" || p.name == "bitrate" || p.name == "bpp" ||
										p.name == "target-bpp" || p.name == "target-size-kb";
			std::string       label   = p.label;
			if (p.name == "bitrate-kbps") label += " (kbps)";
			if (rate) label += " · log";
			ui_.label({area.x + 30, y, area.w - 90, 28}, label, enabled ? ui_.theme.text : ui_.theme.muted, 14);
			std::string pinKey = std::to_string(backend->id.value) + "/" + p.name;
			if (ui_.button(id + ".pin", f.pinned ? "−" : "+", {area.x + area.w - 56, y, 26, 26})) {
				if (f.pinned) pinned_.erase(pinKey);
				else
					pinned_.insert(pinKey);
			}
			y += 30;
			Rect control = {area.x + 30, y, area.w - 60, 32};
			if (p.kind == ParamKind::Bool) {
				bool v  = std::get_if<bool>(&value) ? std::get<bool>(value) : false;
				changed = ui_.checkbox(id, v ? "On" : "Off", control, v, enabled);
				value   = v;
			} else if (p.kind == ParamKind::Enum) {
				std::vector<ui::Choice> values;
				std::size_t             at = 0;
				const auto scc = std::ranges::find_if(backend->params, [](const auto& setting) { return setting.name == "scc"; });
				const bool keepScc = scc != backend->params.end() && param_value_text(resolved_param_value(s, backend->id, *scc)) == "true";
				for (std::size_t i = 0; i < p.enumValues.size(); ++i) {
					bool allowed = true;
					if (keepScc) for (const auto& condition : scc->enabledWhen)
						if (condition.parameter == p.name && std::ranges::find(condition.acceptedValues, p.enumValues[i].value) == condition.acceptedValues.end()) allowed = false;
					values.push_back({p.enumValues[i].label, allowed});
					if (param_value_text(value) == p.enumValues[i].value) at = i;
				}
				const bool compact  = values.size() >= 2 && values.size() <= 4 &&
									  std::ranges::all_of(values, [](const auto &c) { return c.label.size() <= 7; });
				bool       selected = false;
				if (compact) {
					float w = (control.w - float(values.size() - 1) * 4) / float(values.size());
					for (std::size_t n = 0; n < values.size(); ++n) {
						Rect r{control.x + float(n) * (w + 4), control.y, w, control.h};

						if (ui_.button(id + ".option." + std::to_string(n), values[n].label, r, enabled && values[n].enabled, n == at)) {
							at       = n;
							selected = true;
						}
						if (!values[n].enabled) ui_.tooltip(r, "Unavailable while SCC is enabled. Disable SCC before changing to this configuration.");
					}
				} else
					selected = ui_.combo(id, control, values, at, enabled);
				if (selected && at < p.enumValues.size()) {
					value   = p.enumValues[at].value;
					changed = true;
				}
			} else if (p.kind == ParamKind::Int || p.kind == ParamKind::Float) {
				double v = std::visit(
						[](const auto &item) -> double {
							using T = std::decay_t<decltype(item)>;
							if constexpr (std::is_arithmetic_v<T>) return double(item);
							else
								return 0;
						},
						value);
				ui::NumberOptions options;
				options.integer = p.kind == ParamKind::Int;
				if (p.intRange) {
					options.minimum = double(p.intRange->min);
					options.maximum = double(p.intRange->max);
					options.step    = double(p.intRange->step);
					if (p.automaticIntValue) options.minimum = std::min(options.minimum, double(*p.automaticIntValue));
				}
				if (p.floatRange) {
					options.minimum  = p.floatRange->min;
					options.maximum  = p.floatRange->max;
					options.step     = p.floatRange->step;
					options.decimals = 0;
					while (options.decimals < 8 &&
						   std::abs(options.step * std::pow(10., options.decimals) -
									std::round(options.step * std::pow(10., options.decimals))) > 1e-8)
						++options.decimals;
				}
				const bool quality  = p.name == "qp" || p.name == "qpi" || p.name == "crf" || p.name == "qindex" ||
									  p.name == "icq-quality" || p.name == "quality" || p.name == "bpp" ||
									  p.name == "target-bpp" || p.name == "target-size-kb";
				Rect       editRect = control;
				if (quality || rate) {
					editRect           = {control.x + control.w - 96, control.y, 96, control.h};
					auto sliderOptions = options;
					if (rate) {
						// Zero/automatic remains available through numeric entry; the logarithmic track starts positive.
						sliderOptions.minimum = std::max(options.integer ? 1. : options.step, options.minimum);
						// Coarse backend arrow increments must not collapse the low end of the logarithmic track.
						if (options.integer) sliderOptions.step = 1;
					}
					if (p.name == "target-size-kb") {
						auto focused = [&] {
							return SizeSliderRange{
									std::max(options.minimum, std::floor(v / 4 / options.step) * options.step),
									std::min(options.maximum, std::ceil(v * 4 / options.step) * options.step)};
						};
						auto [it, added] = sizeSliderRanges_.try_emplace(id, focused());
						auto &range      = it->second;
						if (range.full) {
							range.minimum = options.minimum;
							range.maximum = options.maximum;
						} else if (v < range.minimum || v > range.maximum)
							range = focused();
						sliderOptions.minimum = range.minimum;
						sliderOptions.maximum = range.maximum;
					}
					changed = ui_.slider(id + ".slider", {control.x, control.y, control.w - 108, control.h}, v,
										 sliderOptions, enabled,
										 rate ? ui::SliderScale::Logarithmic : ui::SliderScale::Linear);
				}
				auto edited = ui_.number_input(id, editRect, v, options, enabled);
				changed     = (edited.committed && edited.valid) || changed;
				if (changed) value = p.kind == ParamKind::Int ? ParamValue(int64_t(v)) : ParamValue(v);
				if (!edited.valid) {
					paragraph(ui_, row(area, y + 34, 48), edited.error, true);
					y += 50;
				}
			} else {
				std::string v      = param_value_text(value);
				auto        edited = ui_.text_input(id, control, v, enabled);
				changed            = edited.changed || edited.committed;
				value              = v;
			}
			if (changed) {
				Action a;
				a.kind    = ActionKind::SetEncoderParam;
				a.backend = backend->id;
				a.param   = {p.name, value};
				actions_.push_back(std::move(a));
			}
			y += 38;
			if (p.name == "target-size-kb" && sizeSliderRanges_.contains(id)) {
				auto &range = sizeSliderRanges_.at(id);
				ui_.label({control.x, y, control.w - 128, 24},
						  "Slider " + number(range.minimum, 3) + "–" + number(range.maximum, 3) + " kB",
						  ui_.theme.muted, 13);
				if (ui_.button(id + ".range", range.full ? "Focus range" : "Full range",
							   {control.x + control.w - 120, y, 120, 24}, enabled)) {
					range.full = !range.full;
					if (!range.full) {
						range.minimum = 0;
						range.maximum = 0;
					} // Recenter on the current target next frame.
					rebuild_ = true;
				}
				y += 30;
			}
			std::string help = enabled ? p.help : parameter_disabled_reason(s, *backend, p);
			if (p.name == "target-size-kb" && enabled && !showHelp_)
				help = "1 kB = 1000 B. Budget per image; actual size may differ.";
			if (!help.empty() && (!enabled || showHelp_ || p.name == "target-size-kb")) {
				paragraph(ui_, row(area, y, 48), help);
				y += 52;
			}
		}
		parameterHeight_ = std::max(40.f, y - scroll.content.y);
		ui_.end_scroll();
		ui_.line({area.x + 14, footer}, {area.x + area.w - 14, footer}, ui_.theme.border);
		bool scratch = s.scratchResults;
		if (ui_.checkbox("scratch", "Replace previous result for this encoder", row(area, footer + 8), scratch))
			emit(ActionKind::ToggleScratchResults);
		if (ui_.button("config.clone", candidate && candidate->encoded && candidate->type == ImageObjectType::EncodedResult ? "Copy #" + std::to_string(candidate->encoded->run.value) + " settings to draft" : "Copy result settings to draft", row(area, footer + 46),
					   candidate && candidate->encoded && candidate->type == ImageObjectType::EncodedResult)) {
			if (const auto *run =
						candidate && candidate->encoded ? run_by_id(s, candidate->encoded->run.value) : nullptr) {
				Action a;
				a.kind    = ActionKind::SelectBackend;
				a.backend = run->backend;
				actions_.push_back(a);
				if (const auto *b = find_backend(s, run->backend))
					for (const auto &p: b->params) {
						a.kind  = ActionKind::SetEncoderParam;
						a.param = {p.name, std::visit(
												   [](const auto &v) -> ParamValue {
													   using T = std::decay_t<decltype(v)>;
													   if constexpr (std::is_same_v<T, std::monostate>)
														   return std::string{};
													   else
														   return v;
												   },
												   p.defaultValue)};
						actions_.push_back(a);
					}
				for (const auto &p: run->params) {
					a.kind  = ActionKind::SetEncoderParam;
					a.param = p;
					actions_.push_back(a);
				}
			}
		}
		if (ui_.button("encode.run", "Encode", row(area, footer + 86, 36),
					   valid(source_) && backend->capabilities.available &&
							   std::ranges::none_of(backend->params, [&](const auto &p) {
								   return !ui_.edit_error("param." + std::to_string(backend->id.value) + "." + p.name)
												   .empty();
							   }))) {
			Action a;
			a.kind    = ActionKind::StartEncodeRun;
			a.image   = source_;
			a.backend = backend->id;
			// Snapshot all effective settings so later drafts cannot change provenance.
			for (const auto &p: backend->params)
				if (p.relevantForStillImage && parameter_is_enabled(s, *backend, p))
					a.params.push_back({p.name, resolved_param_value(s, backend->id, p)});
			actions_.push_back(std::move(a));
		}
		if (liveCapable && s.livePreview.enabled && valid(source_) && backend->capabilities.available &&
			std::ranges::none_of(backend->params, [&](const auto &p) {
				return !ui_.edit_error("param." + std::to_string(backend->id.value) + "." + p.name).empty();
			})) {
			std::vector<EncoderParam> params;
			for (const auto &p: backend->params)
				if (p.relevantForStillImage && parameter_is_enabled(s, *backend, p))
					params.push_back({p.name, resolved_param_value(s, backend->id, p)});
			const auto &live = s.livePreview;
			bool        same =
					std::equal(params.begin(), params.end(), live.requestedParams.begin(), live.requestedParams.end(),
							   [](const auto &a, const auto &b) { return a.name == b.name && a.value == b.value; });
			const auto *input = find_image(s, source_);
			if (live.source != source_ || live.backend != backend->id || !same ||
				(input && live.requestedSource.lock() != input->decoded)) {
				emit(ActionKind::RequestLivePreview, source_);
				actions_.back().backend = backend->id;
				actions_.back().params  = std::move(params);
			}
		}
	}
	void Workbench::decode_tools(const AppState &s, Rect area) {
		const auto *image = find_image(s, selected_);
		float       y     = area.y + 4;
		if (!image) {
			paragraph(ui_, row(area, y, 90), "Select a source or an encoded image to inspect compatible decoders.");
			return;
		}
		if (image->type == ImageObjectType::LivePreview) {
			paragraph(ui_, row(area, y, 100),
					  "This is the changing live preview. Use Encode to retain a result for independent "
					  "decoder measurements.");
			return;
		}
		auto region = ui_.begin_scroll("decode.tools", area, {area.w, decodeHeight_});
		y           = region.content.y + 4;
		ui_.label(row(area, y, 56), "Inspecting " + image_context(s, image, false), ui_.theme.text, 0, TextAlign::Left, TextOverflow::Wrap);
		ui_.tooltip(row(area, y, 56), image_context(s, image));
		y += 64;
		const auto &d = image->decoder;
		paragraph(ui_, row(area, y, 50),
				  d.querying ? "Testing compatible decoders…"
							 : "Only decoders verified against this image are offered.");
		y += 58;
		std::vector<ui::Choice> choices;
		std::size_t             selected = 0;
		for (std::size_t i = 0; i < d.available.size(); ++i) {
			choices.push_back({d.available[i].name + (d.available[i].hardware ? " · GPU" : "")});
			if (d.available[i].id == d.selected) selected = i;
		}
		if (ui_.combo("decoder." + std::to_string(image->id.value), row(area, y, 36), choices, selected,
					  !d.running && !d.querying && !choices.empty())) {
			Action a;
			a.kind  = ActionKind::SelectDecoder;
			a.image = image->id;
			a.text  = d.available[selected].id;
			actions_.push_back(a);
		}
		y += 48;
		if (ui_.button("decoder.refresh", "Recheck compatibility", row(area, y), !d.running && !d.querying))
			emit(ActionKind::QueryDecoders, image->id);
		y += 44;
		ui_.label(row(area, y), "Number of attempts", ui_.theme.muted, 14);
		y += 30;
		double n = d.repetitions;
		if (ui_.number_input("decoder.repeats." + std::to_string(image->id.value), row(area, y), n, {1, 1000, 1, true},
							 !d.running)
					.committed) {
			Action a;
			a.kind  = ActionKind::SetDecodeRepetitions;
			a.image = image->id;
			a.value = n;
			actions_.push_back(a);
		}
		y += 44;
		bool metricsBusy = image->encoded && (image->encoded->metricState == MetricState::Running ||
											  image->encoded->metricState == MetricState::Queued);
		for (const auto &dependent: s.images)
			if (dependent.encoded && std::ranges::find(dependent.parents, image->id) != dependent.parents.end() &&
				(dependent.encoded->metricState == MetricState::Running ||
				 dependent.encoded->metricState == MetricState::Queued))
				metricsBusy = true;
		if (ui_.button("decoder.measure",
					   d.running ? "Cancel measurement" : "Measure " + std::to_string(d.repetitions) + " decodes",
					   row(area, y, 36), d.running || (!d.selected.empty() && !d.querying && !metricsBusy)))
			emit(d.running ? ActionKind::CancelDecode : ActionKind::StartDecode, image->id);
		y += 46;
		if (ui_.button("decoder.once", "Decode once", row(area, y),
					   !d.running && !d.querying && !d.selected.empty() && !metricsBusy)) {
			emit(ActionKind::StartDecode, image->id);
			actions_.back().value = 1;
		}
		y += 44;
		if (metricsBusy) {
			paragraph(ui_, row(area, y, 52),
					  "A metric job is using this image. Decode becomes available when it finishes.");
			y += 60;
		}
		if (!d.error.empty()) {
			paragraph(ui_, row(area, y, 96), d.error, true);
			y += 104;
		}
		if (!d.history.empty()) {
			const auto         &m = d.history.back();
			std::vector<double> codec, total;
			std::size_t         failures = 0;
			for (const auto &v: m.samples) {
				total.push_back(v.seconds);
				if (v.codecSeconds) codec.push_back(*v.codecSeconds);
				if (!v.error.empty()) ++failures;
			}
			ui_.label(row(area, y),
					  std::to_string(m.samples.size()) + " / " + std::to_string(m.requested) + " attempts · " +
							  std::to_string(failures) + " failed",
					  ui_.theme.text);
			y += 38;
			if (!codec.empty()) {
				auto stats = sample_statistics(codec);
				ui_.label(row(area, y), "Codec mean   " + duration(stats.mean), ui_.theme.text);
				y += 34;
				paragraph(ui_, row(area, y, 52),
						  "Sample SD " + (stats.standardDeviation ? duration(*stats.standardDeviation) : "—") +
								  "   SE " + (stats.standardError ? duration(*stats.standardError) : "—"));
				y += 58;
			}
			if (!total.empty()) {
				ui_.label(row(area, y), "Total mean   " + duration(sample_statistics(total).mean), ui_.theme.text);
				y += 34;
				paragraph(ui_, row(area, y, 70), "Codec time measures decoder calls through completion. Total also includes setup and CPU output preparation.");
				y += 78;
			}
			if (ui_.button("decoder.details", "Inspect all attempts in Measurements", row(area, y))) { detail_ = 1; }
			y += 44;
		}
		y = std::max(y, region.content.y + 580);
		ui_.line({area.x + 14, y}, {area.x + area.w - 14, y}, ui_.theme.border);
		y += 14;
		ui_.label(row(area, y), "ENERGY · THIS SESSION", ui_.theme.muted, 13);
		y += 36;
		std::string energy = s.energyAccessPending   ? "Waiting for authorization…"
							 : s.energyAccessEnabled ? "Enabled · " + s.energyAccessDomain
													 : "Unavailable · timing remains available";
		paragraph(ui_, row(area, y, 50), energy);
		y += 58;
		if (ui_.button("energy.access",
					   s.energyAccessPending   ? "Cancel authorization"
					   : s.energyAccessEnabled ? "Disable energy capture"
											   : "Authorize energy capture…",
					   row(area, y)))
			emit(s.energyAccessPending || s.energyAccessEnabled ? ActionKind::ReleaseEnergyAccess
																: ActionKind::RequestEnergyAccess);
		y += 44;
		if (!s.energyAccessError.empty()) {
			paragraph(ui_, row(area, y, 70), s.energyAccessError, true);
			y += 78;
		}
		paragraph(ui_, row(area, y, 110),
				  "Raw RAPL energy during the measured interval. Includes other system activity; no idle subtraction. "
				  "Domain coverage depends on firmware. This is not wall power or decoder-only energy.");
		decodeHeight_ = std::max(500.f, y + 120 - region.content.y);
		ui_.end_scroll();
	}
	void Workbench::metric_tools(const AppState &s, Rect area) {
        float y = area.y + 4;
        const auto* image = find_image(s, selected_);
        const auto* source = image ? root(s, image) : nullptr;
        ui_.label(row(area, y, 52), "Inspecting " + image_context(s, image, false), ui_.theme.text, 15, TextAlign::Left, TextOverflow::Wrap);
        ui_.tooltip(row(area, y, 52), image_context(s, image));
        y += 58;
        ui_.label(row(area, y, 48), "Scores vs original: " + (source ? source->displayName : "—") + " · end-to-end", ui_.theme.muted, 14, TextAlign::Left, TextOverflow::Wrap);
        y += 54;
        if (image && image->encoded) {
            const auto& m = *image->encoded;
            const auto* input = m.encoderInput.get();
            std::string pipeline = "Source " + (source ? format(source->pixelFormat) : "—") + " → prepared input " +
                (input ? format(input->format) : "unavailable") + " → decoded " + format(image->pixelFormat);
            ui_.label(row(area, y, 64), pipeline, ui_.theme.muted, 14, TextAlign::Left, TextOverflow::Wrap);
            y += 70;
            if (ui_.button("metrics.input", "Compare prepared input / decoded", row(area, y), input && image->decoded && image->type == ImageObjectType::EncodedResult)) {
                emit(ActionKind::InspectEncoderInput, image->id);
                pendingEncoderInput_ = image->id;
            }
            ui_.tooltip(row(area, y), "Assign application-prepared input to A and decoded output to B. Scores still compare the original source. The encoder adapter may perform further format conversion.");
            y += 42;
        }
        const float footer = area.y + area.h - 100;
        auto scroll = ui_.begin_scroll("metrics.tools", {area.x, y, area.w, std::max(0.f, footer - y)}, {area.w, metricHeight_});
        y = scroll.content.y;
        std::vector<const ImageObject*> scored;
        for (auto id : ui_.table_state("experiments").selection)
            if (const auto* run = run_by_id(s, id); run && run->source == source_ && !run->replacedBy && run->producedImage)
                if (const auto* result = find_image(s, *run->producedImage); result && result->encoded) scored.push_back(result);
        if (scored.empty() && image && image->encoded && image->type != ImageObjectType::LivePreview) scored.push_back(image);
        std::vector<ui::Column> columns{{"metric", "Metric", 208, 100}};
        for (const auto* result : scored)
            columns.push_back({std::to_string(result->id.value), "#" + std::to_string(result->encoded->run.value), 150, 100, TextAlign::Right});
        for (auto& c : columns) c.sortable = false;
        ui::TableModel scores;
        scores.count = metric_catalog().size();
        scores.rowId = [](std::size_t i) { return i + 1; };
        scores.indexOf = [&](uint64_t id) -> std::optional<std::size_t> {
            return id && id <= scores.count ? std::optional<std::size_t>(id - 1) : std::nullopt;
        };
        scores.cell = [&](std::size_t i, std::size_t c) {
            const auto& m = metric_catalog()[i];
            if (!c) return std::string(m.label) + (m.unit.empty() ? "" : " (" + std::string(m.unit) + ")") + (m.higherIsBetter ? " ↑" : " ↓");
            return metric_text(*scored[c - 1]->encoded, m.id);
        };
        scores.drawCell = [&](std::size_t i, std::size_t c, Rect rect) {
            const auto& metric = metric_catalog()[i];
            ui_.tooltip(rect, (c ? image_context(s, scored[c - 1]) + "\n" : "") + std::string(metric.accuracyNote));
            return false;
        };
        const float tableHeight = 38 + float(scores.count) * ui_.theme.rowHeight;
        ui_.table("metrics.scores", {area.x + 14, y, area.w - 28, tableHeight}, columns, scores);
        y += tableHeight + 16;
        bool& details = groups_["metrics.methodology"];
        ui_.disclosure("metrics.methodology", "Metric definitions and reference", row(area, y), details);
        y += 38;
        if (details) {
            paragraph(ui_, row(area, y, 114), "End-to-end scores compare the original source with decoded output. Codec losslessness concerns the samples actually submitted to the codec. Prepared input shows the application samples supplied to the encoder adapter. The visual A/B pair is independent of score reference.");
            y += 122;
            for (const auto& m : metric_catalog()) {
                ui_.label(row(area, y), std::string(m.label) + (m.higherIsBetter ? " · higher is better" : " · lower is better"), ui_.theme.text, 14);
                y += 34;
                paragraph(ui_, row(area, y, 78), std::string(m.accuracyNote)); y += 86;
            }
            paragraph(ui_, row(area, y, 108), "Floating point, linear BT.2020. HDR values and gamut excursions are retained for computation and FP16 presentation. Wayland receives a matching relative color description; screen brightness remains the user’s choice."); y += 116;
        }
        for (const auto* result : scored)
            for (const auto& score : result->encoded->metrics)
                if (!score.value && !score.unavailableReason.empty()) {
                    paragraph(ui_, row(area, y, 110), "#" + std::to_string(result->encoded->run.value) + " · " + score.label + ": " + score.unavailableReason, true); y += 118;
                }
        metricHeight_ = std::max(40.f, y - scroll.content.y);
        ui_.end_scroll();

		const auto          &selected = ui_.table_state("experiments").selection;
		std::vector<ImageId> targets;
		std::size_t          selectedRuns = 0, unavailable = 0, busy = 0, blocked = 0, ready = 0;
		for (auto id: selected)
			if (const auto *r = run_by_id(s, id); r && r->source == source_ && !r->replacedBy) {
				++selectedRuns;
				if (r->producedImage) targets.push_back(*r->producedImage);
				else
					++unavailable;
			}
		if (!selectedRuns && image && image->encoded && image->type != ImageObjectType::LivePreview)
			targets.push_back(image->id);
		std::vector<ImageId> eligible;
		for (auto id: targets) {
			const auto *result = find_image(s, id);
			const auto *reference =
					result && !result->parents.empty() ? find_image(s, result->parents.front()) : nullptr;
			if (!result || !result->decoded || !result->encoded || !reference || !reference->decoded) {
				++unavailable;
				continue;
			}
			const auto &metadata = *result->encoded;
			if (metadata.metricState == MetricState::Queued || metadata.metricState == MetricState::Running) {
				++busy;
				continue;
			}
			if (result->decoder.running || reference->decoder.running) {
				++blocked;
				continue;
			}
			bool missing = std::ranges::any_of(chosenMetrics_, [&](const auto &id) {
				const auto *m = metric_by_id(metadata, id);
				return !m || !m->value;
			});
			if (missing) eligible.push_back(id);
			else
				++ready;
		}
		if (ui_.button("metrics.compute", "Compute metrics · " + std::to_string(eligible.size()) + " encodes",
					   row(area, area.y + area.h - 90, 38), !eligible.empty() && !chosenMetrics_.empty()))
			for (auto id: eligible) {
				emit(ActionKind::RequestMetrics, id);
				actions_.back().metricIds = {chosenMetrics_.begin(), chosenMetrics_.end()};
			}
		std::string status;
		if (chosenMetrics_.empty()) status = "Choose at least one metric.";
		else if (!selectedRuns && targets.empty())
			status = image && image->type == ImageObjectType::LivePreview
							 ? "Use Encode to retain the live preview before measuring."
							 : "Select an encoded image.";
		else {
			if (ready) status += std::to_string(ready) + " already computed. ";
			if (busy) status += std::to_string(busy) + " queued or running. ";
			if (blocked) status += std::to_string(blocked) + " waiting for decode. ";
			if (unavailable) status += std::to_string(unavailable) + " without a decoded result. ";
			if (status.empty()) status = "Only missing or failed scores will be computed.";
		}
		paragraph(ui_, row(area, area.y + area.h - 44, 40), status);
	}

	void Workbench::experiments(const AppState &s) {
		Rect area = dock_.bottom;
		ui_.fill(area, ui_.theme.surface);
		float       x      = area.x + 14;
		const char *tabs[] = {"Queue", "Measurements", "Configuration", "Plot"};
		for (std::size_t i = 0; i < 4; ++i) {
			Rect r = {x, area.y + 10, 150, 32};

			if (ui_.button("detail." + std::to_string(i), tabs[i], r, true, detail_ == i)) detail_ = i;
			x += 158;
		}
		ui_.line({area.x + 14 + float(detail_) * 158, area.y + 45}, {area.x + 156 + float(detail_) * 158, area.y + 45},
				 ui_.theme.focus, 2);
		if (detail_ == 3 && ui_.button("plot.expand", plotExpanded_ ? "Compact plot" : "Expand plot", {area.x + 654, area.y + 10, 150, 32})) {
			plotExpanded_ = !plotExpanded_; rebuild_ = true;
		}
		const auto *selected = find_image(s, selected_);
		ui_.label({area.x + area.w - 524, area.y + 10, 134, 32}, "Assign selected", ui_.theme.muted, 14);
		assignment_buttons(s, "selection", selected_, {area.x + area.w - 390, area.y + 10, 150, 32});
		const auto *activeRun = run_by_id(s, s.selection.selectedRun.value);
		if (ui_.button("run.cancel", "Cancel run", {area.x + area.w - 160, area.y + 10, 140, 32},
					   activeRun && (activeRun->state == EncodeRunState::Running ||
									 activeRun->state == EncodeRunState::Queued))) {
			Action a;
			a.kind = ActionKind::CancelEncodeRun;
			a.run  = activeRun->id;
			actions_.push_back(a);
		}
		bool removable = selected && selected->encoded && !selected->decoder.running &&
						 selected->encoded->metricState != MetricState::Running &&
						 selected->encoded->metricState != MetricState::Queued;
		if (ui_.button("result.remove", "Remove result", {area.x + area.w - 670, area.y + 10, 132, 32}, removable))
			emit(ActionKind::RemoveImage, selected_);

		Rect content = {area.x + 14, area.y + 56, area.w - 28, area.h - 66};
		if (detail_ == 1) {
			measurements(s, content);
			return;
		}
		if (detail_ == 2) {
			settings(s, content);
			return;
		}
		if (detail_ == 3) {
			plot(s, content);
			return;
		}
		ui_.text_input("experiments.filter", {content.x, content.y, 320, 32}, filter_, true,
					   "Filter encoder, settings, or state…");
		const std::vector<ui::Choice> profiles{{"Quality columns"}, {"Speed columns"}, {"All columns"}};
		ui_.combo("experiments.columns", {content.x + 338, content.y, 190, 32}, profiles, resultColumns_);

		std::vector<const EncodeRun *> runs;
		for (const auto &r: s.encodeRuns)
			if (r.source == source_ && !r.replacedBy) {
				const auto *b = find_backend(s, r.backend);
				if (matches((b ? b->name : "") + " " + summary(r) + " " + state_text(r.state), filter_))
					runs.push_back(&r);
			}
		std::map<uint64_t, DecodeSummary> decodeSummaries;
		for (const auto *run: runs)
			if (run->producedImage)
				decodeSummaries.emplace(run->id.value, decode_summary(find_image(s, *run->producedImage)));
		auto &table     = ui_.table_state("experiments");
		auto  metricFor = [&](const EncodeRun &r, std::string_view id) -> std::optional<double> {
			const auto *image = r.producedImage ? find_image(s, *r.producedImage) : nullptr;
			if (image && image->encoded)
				if (auto *m = metric_by_id(*image->encoded, id)) return m->value;
			return {};
		};
		auto numeric = [&](const EncodeRun &r, std::string_view column) -> std::optional<double> {
			const auto *image = r.producedImage ? find_image(s, *r.producedImage) : nullptr;
			if (column == "run") return double(r.id.value);
			if (column == "decode" || column == "decode-sd" || column == "decode-total" || column == "attempts") {
				const auto &decoded = decodeSummaries[r.id.value];
				if (column == "decode-total")
					return decoded.total.count ? std::optional(decoded.total.mean) : std::nullopt;
				if (column == "attempts")
					return decoded.session ? std::optional(double(decoded.session->samples.size())) : std::nullopt;
				if (!decoded.codec.count) return {};
				return column == "decode" ? std::optional(decoded.codec.mean) : decoded.codec.standardDeviation;
			}
			if (column == "size")
				return image && image->encoded ? std::optional(double(image->encoded->byteSize)) : std::nullopt;
			if (column == "time")
				return image && image->encoded ? std::optional(image->encoded->encodeSeconds) : std::nullopt;
			return metricFor(r, column);
		};
		std::stable_sort(runs.begin(), runs.end(), [&](auto a, auto b) {
			const auto &key = table.sortColumn;
			if (key.empty() || key == "run")
				return table.ascending ? a->id.value < b->id.value : a->id.value > b->id.value;
			if (key == "encoder" || key == "settings" || key == "state" || key == "decoder") {
				auto text = [&](auto *r) {
					const auto *backend = find_backend(s, r->backend);
					return key == "encoder"    ? (backend ? backend->name : "")
						   : key == "settings" ? summary(*r)
						   : key == "decoder"  ? (decodeSummaries[r->id.value].session
														  ? decodeSummaries[r->id.value].session->decoderName
														  : "")
											   : state_text(r->state);
				};
				auto av = text(a), bv = text(b);
				if (av == bv) return a->id.value < b->id.value;
				return table.ascending ? av < bv : av > bv;
			}
			auto av = numeric(*a, key), bv = numeric(*b, key);
			if (bool(av) != bool(bv)) return bool(av);
			if (!av || *av == *bv) return a->id.value < b->id.value;
			return table.ascending ? *av < *bv : *av > *bv;
		});
		for (auto &c: columns_) {
			const bool metric = std::ranges::any_of(metric_catalog(), [&](const auto &m) { return c.id == m.id; });
			if (metric) c.visible = resultColumns_ != 1 && chosenMetrics_.contains(c.id);
			else if (c.id == "decode-sd") c.visible = resultColumns_ == 2;
			else if (c.id == "attempts") c.visible = resultColumns_ != 0;
			else if (c.id == "decoder" || c.id == "decode" || c.id == "decode-total") c.visible = resultColumns_ != 0;
		}
		ui::TableModel model;
		model.count   = runs.size();
		model.rowId   = [&](std::size_t i) { return runs[i]->id.value; };
		model.indexOf = [&](uint64_t id) -> std::optional<std::size_t> {
			for (std::size_t i = 0; i < runs.size(); ++i)
				if (runs[i]->id.value == id) return i;
			return {};
		};
		model.cell = [&](std::size_t rowIndex, std::size_t col) {
			const auto &r     = *runs[rowIndex];
			const auto *b     = find_backend(s, r.backend);
			const auto *image = r.producedImage ? find_image(s, *r.producedImage) : nullptr;
			const auto &key   = columns_[col].id;
			if (key == "run") return "#" + std::to_string(r.id.value);
			if (key == "encoder") return b ? b->name : "Unknown encoder";
			if (key == "settings") return decisive_settings(r);
			if (key == "state") return state_text(r.state);
			const auto &decoded = decodeSummaries[r.id.value];
			if (key == "decoder")
				return decoded.session ? (decoded.session->decoderName.empty() ? decoded.session->decoderId
																			   : decoded.session->decoderName)
									   : "Not measured";
			if (key == "decode")
				return decoded.codec.count ? duration(decoded.codec.mean)
					   : decoded.session   ? std::string("Unavailable")
										   : std::string("Not measured");
			if (key == "decode-sd")
				return decoded.codec.standardDeviation ? duration(*decoded.codec.standardDeviation) : "—";
			if (key == "decode-total")
				return decoded.total.count ? duration(decoded.total.mean) : "—";
			if (key == "attempts")
				return decoded.session ? std::to_string(decoded.session->samples.size()) + " / " +
												 std::to_string(decoded.session->requested) + " · " +
												 std::to_string(decoded.failures) + " failed"
									   : "—";
			if (key == "size") return image && image->encoded ? bytes(image->encoded->byteSize) : "—";
			if (key == "time") return image && image->encoded ? duration(image->encoded->encodeSeconds) : "—";
			return image && image->encoded ? metric_text(*image->encoded, key) : "—";
		};
		model.drawCell = [&](std::size_t r, std::size_t col, Rect rect) {
			if (columns_[col].id == "settings" || columns_[col].id == "encoder") {
				ui_.label(ui::inset(rect, ui_.theme.padding), model.cell(r, col), ui_.theme.text);
				ui_.tooltip(rect, columns_[col].id == "settings" ? summary(*runs[r]) : model.cell(r, col));
				return true;
			}
			if (columns_[col].id != "panes") return false;
			assignment_buttons(s, "run." + std::to_string(runs[r]->id.value),
							   runs[r]->producedImage.value_or(ImageId{}), {rect.x + 4, rect.y, rect.w - 8, rect.h});
			return true;
		};
		std::size_t selectedCount = 0, hidden = 0;
		for (auto id: table.selection)
			if (const auto *r = run_by_id(s, id); r && r->source == source_ && !r->replacedBy) {
				++selectedCount;
				if (std::ranges::none_of(runs, [&](const auto *visible) { return visible->id.value == id; })) ++hidden;
			}
		std::string selection = std::to_string(runs.size()) + " visible · " + std::to_string(selectedCount) +
								" selected" + (hidden ? " (" + std::to_string(hidden) + " hidden by filter)" : "");
		ui_.label({content.x + 546, content.y, std::max(0.f, content.w - 700), 32}, selection, ui_.theme.muted, 14);
		ui_.tooltip({content.x + 546, content.y, std::max(0.f, content.w - 700), 32}, "Ctrl / Shift: select multiple runs. Click column headers to sort. Selection inspects results; A/B buttons assign viewed images.");
		if (ui_.button("experiments.clear-selection", "Clear selection",
					   {content.x + content.w - 140, content.y, 140, 32}, selectedCount > 0)) {
			table.selection.clear();
			table.anchor.reset();
			rebuild_ = true;
		}

		auto result = ui_.table("experiments", {content.x, content.y + 42, content.w, content.h - 42}, columns_, model);
		if (result.sortChanged && valid(selected_)) emit(ActionKind::SelectImage, selected_);
		if (result.selectionChanged && table.active)
			if (const auto *run = run_by_id(s, *table.active)) {
				Action a;
				a.kind = ActionKind::SelectEncodeRun;
				a.run  = run->id;
				actions_.push_back(a);
				if (run->producedImage) choose(s, *run->producedImage);
			}
		if (runs.empty())
			paragraph(ui_, {content.x + 24, content.y + 94, 800, 80},
					  s.encodeRuns.empty() ? "The queue is empty. Choose settings and press Encode."
										   : "No encodes match this source and filter.");
	}
	void Workbench::measurements(const AppState &s, Rect area) {
		const auto *image = find_image(s, selected_);
		if (!image) {
			paragraph(ui_, area, "Select an image to see decoder measurements.");
			return;
		}
		const std::string   tableId = "measurement.attempts." + std::to_string(image->id.value);
		const auto         &history = image->decoder.history;
		std::vector<double> codec, total, energy;
		std::size_t         failures = 0;
		struct Attempt {
			const DecodeMeasurement *measurement;
			const DecodeSample      *sample;
			std::size_t              session, index;
		};
		std::vector<Attempt> attempts;
		for (std::size_t m = 0; m < history.size(); ++m)
			for (std::size_t n = 0; n < history[m].samples.size(); ++n) {
				const auto &sample = history[m].samples[n];
				attempts.push_back({&history[m], &sample, m, n});
			}
		// Statistics describe one measurement session; different decoder runs are never pooled.
		const DecodeMeasurement *session = history.empty() ? nullptr : &history.back();
		if (auto active = ui_.table_state(tableId).active; active && *active && *active <= attempts.size())
			session = attempts[*active - 1].measurement;
		if (session)
			for (const auto &sample: session->samples) {
				total.push_back(sample.seconds);
				if (sample.codecSeconds) codec.push_back(*sample.codecSeconds);
				if (sample.joules) energy.push_back(*sample.joules);
				if (!sample.error.empty()) ++failures;
			}
		std::string text = "All attempts retained. Codec time excludes setup and readback; total includes them.";
		if (!codec.empty()) {
			auto stats = sample_statistics(codec);
			text = "Session · " + std::to_string(session->samples.size()) + " attempts · " + std::to_string(failures) +
				   " failures · codec n=" + std::to_string(stats.count) + " mean " + duration(stats.mean) +
				   " · sample SD " + (stats.standardDeviation ? duration(*stats.standardDeviation) : "—") + " · SE " +
				   (stats.standardError ? duration(*stats.standardError) : "—");
		}
		ui_.label({area.x, area.y, area.w, 30}, text, ui_.theme.text, 14);
		auto statsText = [&](const std::vector<double> &values, bool joules) {
			if (values.empty()) return std::string("Unavailable");
			auto stats = sample_statistics(values);
			auto value = [&](double x) { return joules ? number(x, 5) + " J" : duration(x); };
			return "mean " + value(stats.mean) + " · SD " +
				   (stats.standardDeviation ? value(*stats.standardDeviation) : "—") + " · SE " +
				   (stats.standardError ? value(*stats.standardError) : "—");
		};
		ui_.label({area.x, area.y + 32, area.w, 30},
				  "Total: " + statsText(total, false) + "    Raw energy: " + statsText(energy, true) +
						  "    Select an attempt to inspect its session.",
				  ui_.theme.muted, 14);

		std::vector<std::size_t> order(attempts.size());
		std::iota(order.begin(), order.end(), 0);
		const auto &sort = ui_.table_state(tableId);
		std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
			const auto &av  = attempts[a];
			const auto &bv  = attempts[b];
			const auto &key = sort.sortColumn;
			if (key.empty() || key == "attempt") return sort.ascending ? a < b : a > b;
			if (key == "decoder" || key == "outcome") {
				auto at = key == "decoder" ? av.measurement->decoderName : av.sample->error,
					 bt = key == "decoder" ? bv.measurement->decoderName : bv.sample->error;
				if (at == bt) return a < b;
				return sort.ascending ? at < bt : at > bt;
			}
			auto value = [&](const auto &r) -> std::optional<double> {
				if (key == "codec") return r.sample->codecSeconds;
				if (key == "total") return r.sample->seconds;
				if (key == "energy") return r.sample->joules;
				if (key == "window" && r.sample->joules) return r.sample->energySeconds;
				return {};
			};
			auto an = value(av), bn = value(bv);
			if (bool(an) != bool(bn)) return bool(an);
			if (!an || *an == *bn) return a < b;
			return sort.ascending ? *an < *bn : *an > *bn;
		});

		ui::TableModel model;
		model.count   = attempts.size();
		model.rowId   = [&](std::size_t i) { return order[i] + 1; };
		model.indexOf = [&](uint64_t id) -> std::optional<std::size_t> {
			for (std::size_t i = 0; i < order.size(); ++i)
				if (order[i] + 1 == id) return i;
			return {};
		};
		model.cell = [&](std::size_t i, std::size_t c) {
			const auto &r = attempts[order[i]];
			const auto &v = *r.sample;
			switch (c) {
				case 0:
					return std::to_string(r.session + 1) + "." + std::to_string(r.index + 1);
				case 1:
					return r.measurement->decoderName.empty() ? r.measurement->decoderId : r.measurement->decoderName;
				case 2:
					return v.codecSeconds ? duration(*v.codecSeconds) : "Unavailable";
				case 3:
					return duration(v.seconds);
				case 4:
					return v.joules ? number(*v.joules, 5) + " J" : "Unavailable";
				case 5:
					return v.joules ? duration(v.energySeconds) : "—";
				default:
					return v.error.empty() ? std::string("Complete") : v.error;
			}
		};
		auto result = ui_.table(tableId, {area.x, area.y + 70, area.w, std::max(0.f, area.h - 110)},
								measurementColumns_, model);
		if (result.selectionChanged || result.sortChanged) emit(ActionKind::SelectImage, selected_);
		if (attempts.empty())
			paragraph(ui_, {area.x + 24, area.y + 120, 800, 70},
					  "No measurements yet. Open Decode to choose a compatible decoder and measure repeated attempts.");
		std::string note = history.empty() ? "Energy: raw counter change; no idle subtraction."
										   : session->energyDomain + " · " + session->energyNote;
		ui_.label({area.x, area.y + area.h - 32, area.w, 30}, note, ui_.theme.muted, 13);
	}
	void Workbench::settings(const AppState &s, Rect area) {
		std::vector<const EncodeRun *> runs;
		for (auto id: ui_.table_state("experiments").selection)
			if (const auto *r = run_by_id(s, id); r && r->source == source_ && !r->replacedBy) runs.push_back(r);
		if (runs.empty()) {
			if (const auto *i = find_image(s, selected_); i && i->encoded)
				if (const auto *r = run_by_id(s, i->encoded->run.value)) runs.push_back(r);
		}
		const auto *inspected = find_image(s, selected_);
		if (runs.empty() || (inspected && inspected->type == ImageObjectType::Source)) {
			if (inspected) {
				std::string text = inspected->displayName + "\n" + std::to_string(inspected->width) + " × " +
								   std::to_string(inspected->height) + "  " + format(inspected->pixelFormat) +
								   "\nColor: " + inspected->color.primaries + " / " + inspected->color.transfer +
								   " / " + inspected->color.matrix +
								   (inspected->color.fullRange ? " / full range" : " / limited range");
				if (inspected->sourceByteSize)
					text += "\nFile: " + bytes(*inspected->sourceByteSize) + " (" + std::to_string(*inspected->sourceByteSize) + " B)";
				text += "\nUncompressed: " + uncompressed_size(inspected->decoded.get(), true);
				if (inspected->decoded && !inspected->decoded->importNote.empty())
					text += "\nInterpretation: " + inspected->decoded->importNote;
				auto region = ui_.begin_scroll("source.details", area, {area.w, 300});
				ui_.label({area.x + 10, region.content.y + 8, area.w - 20, 290}, text, ui_.theme.text, 0,
						  TextAlign::Left, TextOverflow::Wrap);
				ui_.end_scroll();
			} else
				paragraph(ui_, area, "Select a source or an encode for its recorded settings.");
			return;
		}
		std::set<std::string> names;
		for (auto *r: runs)
			for (const auto &p: r->params) names.insert(p.name);
		std::vector<std::string> rows = {
				"Implementation",        "Source → prepared → decoded", "Color interpretation",
				"Encoded bytes",         "Encode time / preview decode",   "Source interpretation",
				"Preview interpretation", "Prepared input samples",
				"Uncompressed source", "Uncompressed prepared input", "Uncompressed decoded output"};
		rows.insert(rows.end(), names.begin(), names.end());
		std::vector<ui::Column> cols = {{"setting", "Recorded setting", 280}};
		for (auto *r: runs) cols.push_back({std::to_string(r->id.value), "Run #" + std::to_string(r->id.value), 420});
		for (auto &c: cols) {
			c.sortable = false;
			for (const auto &previous: configurationColumns_)
				if (previous.id == c.id) c.width = previous.width;
		}
		configurationColumns_ = std::move(cols);

		ui::TableModel model;
		model.indexOf = [&](uint64_t id) -> std::optional<std::size_t> {
			if (id && id <= rows.size()) return id - 1;
			return {};
		};
		model.count = rows.size();
		model.rowId = [](std::size_t i) { return i + 1; };
		model.cell  = [&](std::size_t i, std::size_t c) -> std::string {
			if (c == 0) return rows[i];
			const auto *r       = runs[c - 1];
			const auto *backend = find_backend(s, r->backend);
			const auto *src     = find_image(s, r->source);
			const auto *image   = r->producedImage ? find_image(s, *r->producedImage) : nullptr;
			if (i == 0) return backend ? backend->name : "—";
			if (i == 1)
				return (src ? format(src->pixelFormat) : "—") + " → " +
					(image && image->encoded && image->encoded->encoderInput ? format(image->encoded->encoderInput->format) : "unavailable") +
					" → " + (image ? format(image->pixelFormat) : "—");
			if (i == 2)
				return image ? image->color.primaries + " / " + image->color.transfer + " / " + image->color.matrix +
									   (image->color.fullRange ? " / full" : " / limited")
							 : "—";
			if (i == 3) return image && image->encoded ? std::to_string(image->encoded->byteSize) + " B" : "—";
			if (i == 4)
				return image && image->encoded ? duration(image->encoded->encodeSeconds) + " / " +
														 duration(image->encoded->decodeSeconds)
											   : "—";
			if (i == 5) return src && src->decoded ? src->decoded->importNote : "—";
			if (i == 6) return image && image->decoded ? image->decoded->importNote : "—";
			if (i == 7) return image && image->encoded && image->encoded->encoderInput ? sample_description(*image->encoded->encoderInput) : "Unavailable";
			if (i == 8) return uncompressed_size(src ? src->decoded.get() : nullptr, true);
			if (i == 9) return uncompressed_size(image && image->encoded ? image->encoded->encoderInput.get() : nullptr, true);
			if (i == 10) return uncompressed_size(image ? image->decoded.get() : nullptr, true);

			for (const auto &p: r->params)
				if (p.name == rows[i]) return parameter_text(p.value, 3);
			return "Not applicable";
		};
		ui_.label({area.x, area.y, area.w - 200, 28},
				  "Requested settings are captured at submission. Formats below describe the source and actual decoded "
				  "output; effective encoder internals are not verified.",
				  ui_.theme.muted, 14);
		const auto *selected = find_image(s, selected_);
		if (ui_.button("result.export", "Export bitstream…", {area.x + area.w - 190, area.y, 186, 30},
					   selected && selected->encoded))
			emit(ActionKind::SaveEncodedResult, selected_);
		ui_.table("configuration", {area.x, area.y + 36, area.w, area.h - 36}, configurationColumns_, model);
	}
	void Workbench::plot(const AppState &s, Rect area) {
		std::vector<ui::Choice> choices;
		for (const auto &m: metric_catalog()) choices.push_back({std::string(m.label)});
		ui_.combo("plot.metric", {area.x, area.y, 240, 32}, choices, plotMetric_);
		const std::vector<ui::Choice> xs = {{"Encoded size (bytes)"}, {"Encode time (seconds)"}};
		ui_.combo("plot.x", {area.x + 252, area.y, 240, 32}, xs, plotX_);
		const auto* inspected = find_image(s, selected_);
		if (ui_.button("plot.view", inspected && inspected->encoded ? "View #" + std::to_string(inspected->encoded->run.value) + " in B" : "View selected in B",
			{area.x + 504, area.y, 230, 32}, inspected && inspected->decoded))
			assign_pane(s, 1, selected_);
		ui_.label({area.x, area.y + 40, area.w, 28}, "Inspecting " + image_context(s, inspected), ui_.theme.text, 14);
		ui_.tooltip({area.x, area.y + 40, area.w, 28}, "Inspecting " + image_context(s, inspected));
		if (metric_catalog().empty()) return;
		const auto                &metric = metric_catalog()[std::min(plotMetric_, metric_catalog().size() - 1)];
		std::vector<ui::PlotPoint> points;
		ui::PlotAxes               axes;
		axes.xMin = axes.yMin = INFINITY;
		axes.xMax = axes.yMax = -INFINITY;
		axes.xLabel           = plotX_ ? "Encode time (s)" : "Encoded bytes";
		axes.yLabel           = std::string(metric.label) + (metric.higherIsBetter ? " ↑" : " ↓");
		for (const auto &r: s.encodeRuns)
			if (r.source == source_ && r.producedImage)
				if (const auto *i = find_image(s, *r.producedImage); i && i->encoded)
					if (const auto *value = metric_by_id(*i->encoded, metric.id);
						value && value->value && std::isfinite(*value->value)) {
						double x = plotX_ ? i->encoded->encodeSeconds : double(i->encoded->byteSize);
						points.push_back({i->id.value, x, *value->value, image_context(s, i, false) + "\n" +
							std::to_string(i->encoded->byteSize) + " bytes · encode " + duration(i->encoded->encodeSeconds) +
							"\n" + std::string(metric.label) + ": " + number(*value->value, 6) + " " + std::string(metric.unit) +
							"\n" + decisive_settings(r)});
						axes.xMin = std::min(axes.xMin, x);
						axes.xMax = std::max(axes.xMax, x);
						axes.yMin = std::min(axes.yMin, *value->value);
						axes.yMax = std::max(axes.yMax, *value->value);
					}
		if (points.empty()) {
			paragraph(ui_, {area.x + 20, area.y + 70, 900, 70},
					  "No finite scores for this metric yet. Compute metrics in Metrics; the plot never starts "
					  "computation.");
			return;
		}
		double dx = axes.xMax == axes.xMin ? std::max(1., std::abs(axes.xMin) * .08)
										   : std::max(1e-6, (axes.xMax - axes.xMin) * .08),
			   dy = axes.yMax == axes.yMin ? std::max(.01, std::abs(axes.yMin) * .05)
										   : std::max(1e-6, (axes.yMax - axes.yMin) * .08);
		axes.xMin -= dx;
		axes.xMax += dx;
		axes.yMin -= dy;
		axes.yMax += dy;
		if (auto id = ui_.plot("quality.plot", {area.x, area.y + 80, area.w, area.h - 80}, points, axes,
							   selected_.value)) {
			if (const auto *image = find_image(s, ImageId{*id}); image && image->encoded) {
				auto &selected     = ui_.table_state("experiments");
				selected.active    = image->encoded->run.value;
				selected.anchor    = selected.active;
				selected.selection = {*selected.active};
			}
			choose(s, ImageId{*id});
		}
	}
} // namespace codec_gui::gui
