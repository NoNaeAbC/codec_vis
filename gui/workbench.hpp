#pragma once
#include "ui_framework.hpp"
#include "viewer_model.hpp"
#include <array>
#include <map>

namespace codec_gui::gui {
	// Presentation state is separate from immutable encode runs and encoder drafts.
	class Workbench {
	public:
		explicit Workbench(ui::Context::Measure measure);
		std::vector<Action>             build(const AppState &, Rect bounds, double seconds, float deviceScale = 1);
		std::vector<Action>             handle(const Action &, const AppState &);
		ui::Context                    &context() { return ui_; }
		Rect                            canvas() const { return imageArea_; }
		bool                            animated() const { return mode_ == 3; }
		bool                            needs_rebuild() const { return rebuild_; }
        bool                            input_consumed() const { return inputConsumed_; }
		const std::vector<DrawCommand> &commands() const { return ui_.commands(); }
		ImageId                         reference() const { return reference_; }
		ImageId                         candidate() const { return candidate_; }
		std::array<ImageId, 2>          comparison_images() const { return {reference_, candidate_}; }
		ViewportTransform               viewport() const { return transform_; }

	private:
		ui::Context    ui_;
		ui::DockSizes  sizes_{260, 460, 360, 52, 30};
		ui::DockLayout dock_;
		Rect           imageArea_;
		struct DisplayedPane {
			ImageId           image;
			Rect              clip, transformRect;
			ViewportTransform transform;
			bool              reference = false;
			std::size_t       paneIndex = 1;
		};
		std::optional<DisplayedPane>     dragPane_;
		Point                            pressPoint_;
		bool                             moved_   = false;
		bool                             rebuild_ = false;
        bool                             inputConsumed_ = false;
		std::vector<DisplayedPane>       displayed_;
		ImageId                          source_, reference_, candidate_, observedSelection_, selected_;
		std::array<ImageId, 2>           extraPanes_{};
		std::array<EncodeRunId, 4>       paneRuns_{};
		std::size_t                      paneCount_ = 2, activePane_ = 1;
		ViewportTransform                transform_, referenceTransform_;
		std::array<ViewportTransform, 2> extraTransforms_{};
		std::set<uint64_t>               observedResults_;
		bool                             showHelp_ = false;
		bool sourcesVisible_ = true, toolsVisible_ = true, resultsVisible_ = true, plotExpanded_ = false;
		std::size_t resultColumns_ = 0;
		std::optional<ImageId> pendingEncoderInput_;
		bool        fit_ = true, linked_ = true, holdingA_ = false, dragging_ = false, splitDragging_ = false;
		Point       pointer_, dragPoint_;
		float       deviceScale_     = 1;
		float       parameterHeight_ = 1000, decodeHeight_ = 1000, metricHeight_ = 1600;
		std::size_t mode_ = 1, task_ = 0, detail_ = 0, plotMetric_ = 0, plotX_ = 0;
		std::size_t gridPage_ = 0, gridCount_ = 1;
		double      split_ = .5, gain_ = 1;
		std::string filter_, paramFilter_;
		std::map<std::string, bool> groups_;
		struct SizeSliderRange {
			double minimum, maximum;
			bool   full = false;
		};
		std::map<std::string, SizeSliderRange> sizeSliderRanges_;
		std::set<std::string>                  pinned_, chosenMetrics_;
		std::vector<ui::Column>                columns_;
		std::vector<ui::Column>                measurementColumns_;
		std::vector<ui::Column>                configurationColumns_;
		struct Bookmark {
			uint64_t          id = 0;
			std::string       name;
			ImageId           source;
			ViewportTransform transform, referenceTransform;
			bool              linked = true;
		};
		uint64_t              nextBookmark_ = 1;
		std::vector<Bookmark> bookmarks_;
		std::vector<Action>   actions_;
		void                  emit(ActionKind, ImageId image = {});
		void                  choose(const AppState &, ImageId);
		void                  reconcile(const AppState &);
		ImageId               pane_image(std::size_t) const;
		void                  assign_pane(const AppState &, std::size_t, ImageId);
		void                  assignment_buttons(const AppState &, std::string, ImageId, Rect);
		void                  sources(const AppState &);
		void                  canvas_view(const AppState &, double);
		void                  tools(const AppState &);
		void                  encode_tools(const AppState &, Rect);
		void                  decode_tools(const AppState &, Rect);
		void                  metric_tools(const AppState &, Rect);
		void                  experiments(const AppState &);
		void                  measurements(const AppState &, Rect);
		void                  settings(const AppState &, Rect);
		void                  plot(const AppState &, Rect);
		void                  fit_view(const AppState &);
		const DisplayedPane  *pane_at(Point) const;
		void                  navigate(const DisplayedPane &, ViewportTransform);
		std::vector<ImageId>  grid_candidates(const AppState &);
	};
} // namespace codec_gui::gui
