#include "encode_runner.hpp"

#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "encoder_backends.hpp"
#include "metrics.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace codec_gui::gui {
	namespace {

		const ImageObject *image_by_id(const AppState &state, ImageId id) { return find_image(state, id); }

		const EncodeRun *run_by_id(const AppState &state, EncodeRunId id) {
			for (const EncodeRun &run: state.encodeRuns) {
				if (run.id == id) { return &run; }
			}
			return nullptr;
		}

		Action failed_action(EncodeRunId run, BackendId backend, std::string message) {
			Action action;
			action.kind    = ActionKind::EncodeRunFailed;
			action.run     = run;
			action.backend = backend;
			action.text    = std::move(message);
			return action;
		}

	} // namespace

	EncodeRunner::EncodeRunner(std::span<const EncoderBackend> backends, MetricEvaluator evaluator)
		: backends_(backends.begin(), backends.end()), metricEvaluator_(std::move(evaluator)),
		  clockStart_(std::chrono::steady_clock::now()) {
		encodeWorker_     = std::thread([this] { run_encode_worker(); });
		decodeWorker_     = std::thread([this] { run_decode_worker(); });
		liveEncodeWorker_ = std::thread([this] { run_encode_worker(true); });
		liveDecodeWorker_ = std::thread([this] { run_decode_worker(true); });
		metricWorker_     = std::thread([this] { run_metric_worker(); });
	}

	EncodeRunner::~EncodeRunner() {
		{
			std::scoped_lock const lock(mutex_);
			stopping_ = true;
		}
		encodeCv_.notify_all();
		decodeCv_.notify_all();
		liveEncodeCv_.notify_all();
		liveDecodeCv_.notify_all();
		metricCv_.notify_all();
		if (encodeWorker_.joinable()) { encodeWorker_.join(); }
		if (decodeWorker_.joinable()) { decodeWorker_.join(); }
		if (liveEncodeWorker_.joinable()) liveEncodeWorker_.join();
		if (liveDecodeWorker_.joinable()) liveDecodeWorker_.join();
		if (metricWorker_.joinable()) metricWorker_.join();
	}

	void EncodeRunner::submit(const Command &command, const AppState &state) {
		if (command.kind == CommandKind::CancelLivePreview) {
			std::scoped_lock lock(mutex_);
			liveJob_.reset();
			liveDecodeJob_.reset();
			return;
		}
		const bool live = command.kind == CommandKind::RunLivePreview;
		if (command.kind != CommandKind::RunEncode && !live) { return; }
		const EncoderBackend *backend = find_backend(backends_, command.backend);
		const ImageObject    *image   = image_by_id(state, command.image);
		const EncodeRun      *run     = run_by_id(state, command.run);
		if (backend == nullptr || image == nullptr || image->decoded == nullptr || (!live && run == nullptr)) {
			auto failed = failed_action(command.run, command.backend, "invalid encode command");
			if (live) {
				failed.kind     = ActionKind::LivePreviewFailed;
				failed.image    = command.image;
				failed.revision = command.revision;
			}
			push_action(std::move(failed));
			return;
		}

		Job job;
		job.command = command;
		job.backend = *backend;
		job.source  = image->decoded;
		{
			std::scoped_lock const lock(mutex_);
			if (live) liveJob_ = std::move(job);
			else
				encodeQueue_.push(std::move(job));
		}
		(live ? liveEncodeCv_ : encodeCv_).notify_one();
	}

	void EncodeRunner::request_cancel(EncodeRunId run) {
		std::scoped_lock const lock(mutex_);
		cancelRequested_.push_back(run);
	}

	std::vector<Action> EncodeRunner::take_actions() {
		std::scoped_lock const lock(mutex_);
		std::vector<Action>    out;
		out.swap(actions_);
		return out;
	}

	void EncodeRunner::run_encode_worker(bool liveLane) {
		EncodePreparationCache preparation;
		for (;;) {
			Job job;
			{
				std::unique_lock lock(mutex_);
				(liveLane ? liveEncodeCv_ : encodeCv_).wait(lock, [&] {
					return stopping_ || (liveLane ? liveJob_.has_value() : !encodeQueue_.empty());
				});
				if (stopping_) { return; }
				if (!liveLane) {
					job = std::move(encodeQueue_.front());
					encodeQueue_.pop();
				} else {
					job = std::move(*liveJob_);
					liveJob_.reset();
				}
			}
			const bool live = job.command.kind == CommandKind::RunLivePreview;

			if (consume_cancel_requested(job.command.run)) {
				Action canceled;
				canceled.kind  = ActionKind::EncodeRunCanceled;
				canceled.run   = job.command.run;
				canceled.value = now_seconds();
				push_action(std::move(canceled));
				continue;
			}

			Action started;
			started.kind  = ActionKind::EncodeRunStarted;
			started.run   = job.command.run;
			started.value = now_seconds();
			if (!live) push_action(started);

			try {
				EncodeResult encode = run_backend_encode(job.backend, job.source, job.command.params, preparation);
				encode.metadata.run = job.command.run;
				DecodeJob decode;
				decode.command = job.command;
				decode.backend = job.backend;
				decode.source  = job.source;
				decode.encode  = std::move(encode);
				decode.queued  = job.queued;
				push_decode_job(std::move(decode));
			} catch (const std::exception &e) {
				Action failed = failed_action(job.command.run, job.command.backend, e.what());
				if (live) {
					failed.kind     = ActionKind::LivePreviewFailed;
					failed.image    = job.command.image;
					failed.revision = job.command.revision;
				}
				failed.value = now_seconds();
				push_action(std::move(failed));
			}
		}
	}

	void EncodeRunner::run_decode_worker(bool liveLane) {
		for (;;) {
			DecodeJob job;
			{
				std::unique_lock lock(mutex_);
				(liveLane ? liveDecodeCv_ : decodeCv_).wait(lock, [&] {
					return stopping_ || (liveLane ? liveDecodeJob_.has_value() : !decodeQueue_.empty());
				});
				if (stopping_) { return; }
				if (!liveLane) {
					job = std::move(decodeQueue_.front());
					decodeQueue_.pop();
				} else {
					job = std::move(*liveDecodeJob_);
					liveDecodeJob_.reset();
				}
			}
			const bool live = job.command.kind == CommandKind::RunLivePreview;

			if (consume_cancel_requested(job.command.run)) {
				Action canceled;
				canceled.kind  = ActionKind::EncodeRunCanceled;
				canceled.run   = job.command.run;
				canceled.value = now_seconds();
				push_action(std::move(canceled));
				continue;
			}

			try {
				const auto   decodeStart = std::chrono::steady_clock::now();
				DecodeResult preview     = decode_backend_preview(job.backend, job.encode.encoded);
				job.encode.metadata.decodeSeconds =
						std::chrono::duration<double>(std::chrono::steady_clock::now() - decodeStart).count();
				if (live && !preview.image)
					throw std::runtime_error(preview.error.empty() ? "live preview decoder produced no image"
																   : preview.error);
				if (preview.image) {
					job.encode.metadata.codedPixelFormat = preview.image->format;
					job.encode.metadata.codedColor       = preview.image->color;
				} else {
					job.encode.metadata.previewError =
							preview.error.empty() ? "preview decoder did not return an image" : preview.error;
					job.encode.metadata.metricError = "metrics unavailable because decoded preview is unavailable";
					job.encode.metadata.metrics     = {
							QualityMetricRecord{.id                = "psnr-y",
												.label             = "PSNR-Y",
												.value             = std::nullopt,
												.unit              = "dB",
												.higherIsBetter    = true,
												.unavailableReason = job.encode.metadata.metricError},
					};
				}

				if (consume_cancel_requested(job.command.run)) {
					Action canceled;
					canceled.kind  = ActionKind::EncodeRunCanceled;
					canceled.run   = job.command.run;
					canceled.value = now_seconds();
					push_action(std::move(canceled));
					continue;
				}

				Action completed;
				completed.kind = live ? ActionKind::LivePreviewCompleted : ActionKind::EncodeRunCompleted;
				if (live) {
					completed.image    = job.command.image;
					completed.backend  = job.command.backend;
					completed.revision = job.command.revision;
					completed.params   = job.command.params;
				}
				completed.encodeCompleted.run      = job.command.run;
				completed.encodeCompleted.metadata = std::move(job.encode.metadata);
				completed.encodeCompleted.preview  = std::move(preview.image);
				completed.value =
						live ? std::chrono::duration<double>(std::chrono::steady_clock::now() - job.queued).count()
							 : now_seconds();
				push_action(std::move(completed));
			} catch (const std::exception &e) {
				Action failed = failed_action(job.command.run, job.command.backend, e.what());
				if (live) {
					failed.kind     = ActionKind::LivePreviewFailed;
					failed.image    = job.command.image;
					failed.backend  = job.command.backend;
					failed.revision = job.command.revision;
				}
				failed.value = now_seconds();
				push_action(std::move(failed));
			}
		}
	}

	void EncodeRunner::submit_metrics(const Command &command, const AppState &state) {
		const auto *image  = find_image(state, command.image);
		const auto *source = find_image(state, command.otherImage);
		if (!image || !image->decoded || !source || !source->decoded) {
			Action failed;
			failed.kind  = ActionKind::MetricsFailed;
			failed.image = command.image;
			failed.text  = "decoded result or source is unavailable";
			push_action(std::move(failed));
			return;
		}
		{
			std::scoped_lock lock(mutex_);
			if (stopping_) return;
			metricQueue_.push({command.image, source->decoded, image->decoded, command.metricIds});
		}
		metricCv_.notify_one();
	}

	void EncodeRunner::run_metric_worker() {
		for (;;) {
			MetricJob job;
			{
				std::unique_lock lock(mutex_);
				metricCv_.wait(lock, [&] { return stopping_ || !metricQueue_.empty(); });
				if (stopping_) return;
				job = std::move(metricQueue_.front());
				metricQueue_.pop();
			}
			Action started;
			started.kind  = ActionKind::MetricsStarted;
			started.image = job.image;
			push_action(std::move(started));
			const auto start = std::chrono::steady_clock::now();
			try {
				for (const auto &id: job.metricIds)
					if (std::ranges::none_of(metric_catalog(), [&](const auto &d) { return d.id == id; }))
						throw std::invalid_argument("unknown metric: " + id);
				// One FP32 working-space conversion per image pair, shared by all
				// registered adapters. Neither the decoder nor UI thread waits here.
				const auto reference = raw_image_to_linear_bt2020(*job.reference);
				const auto candidate = raw_image_to_linear_bt2020(*job.candidate);
				for (const auto &descriptor: metric_catalog()) {
					if (!job.metricIds.empty() &&
						std::ranges::find(job.metricIds, descriptor.id) == job.metricIds.end())
						continue;
					{
						std::scoped_lock lock(mutex_);
						if (stopping_) return;
					}
					Action computed;
					computed.kind   = ActionKind::MetricComputed;
					computed.image  = job.image;
					computed.metric = metricEvaluator_(descriptor.id, reference, candidate);
					push_action(std::move(computed));
				}
				Action done;
				done.kind  = ActionKind::MetricsCompleted;
				done.image = job.image;
				done.value = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				push_action(std::move(done));
			} catch (const std::exception &e) {
				Action failed;
				failed.kind  = ActionKind::MetricsFailed;
				failed.image = job.image;
				failed.text  = e.what();
				push_action(std::move(failed));
			}
		}
	}

	void EncodeRunner::push_action(Action action) {
		std::scoped_lock const lock(mutex_);
		if (action.kind == ActionKind::LivePreviewCompleted)
			std::erase_if(actions_, [](const auto &a) { return a.kind == ActionKind::LivePreviewCompleted; });
		actions_.push_back(std::move(action));
	}

	void EncodeRunner::push_decode_job(DecodeJob job) {
		const bool live = job.command.kind == CommandKind::RunLivePreview;
		{
			std::scoped_lock const lock(mutex_);
			if (stopping_) return;
			if (job.command.kind == CommandKind::RunLivePreview) liveDecodeJob_ = std::move(job);
			else
				decodeQueue_.push(std::move(job));
		}
		(live ? liveDecodeCv_ : decodeCv_).notify_one();
	}

	double EncodeRunner::now_seconds() const {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - clockStart_).count();
	}

	bool EncodeRunner::consume_cancel_requested(EncodeRunId run) {
		std::scoped_lock const lock(mutex_);
		const auto             it = std::ranges::find(cancelRequested_, run);
		if (it == cancelRequested_.end()) { return false; }
		cancelRequested_.erase(it);
		return true;
	}

} // namespace codec_gui::gui
