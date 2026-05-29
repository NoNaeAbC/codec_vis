#include "encode_runner.hpp"

#include "metrics.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace codec_gui::gui {
namespace {

const ImageObject* image_by_id(const AppState& state, ImageId id) {
	return find_image(state, id);
}

const EncodeRun* run_by_id(const AppState& state, EncodeRunId id) {
	for (const EncodeRun& run : state.encodeRuns) {
		if (run.id == id) {
			return &run;
		}
	}
	return nullptr;
}

Action failed_action(EncodeRunId run, BackendId backend, std::string message) {
	Action action;
	action.kind = ActionKind::EncodeRunFailed;
	action.run = run;
	action.backend = backend;
	action.text = std::move(message);
	return action;
}

} // namespace

EncodeRunner::EncodeRunner(std::span<const EncoderBackend> backends) : backends_(backends.begin(), backends.end()), clockStart_(std::chrono::steady_clock::now()) {
	encodeWorker_ = std::thread([this] {
		run_encode_worker();
	});
	decodeWorker_ = std::thread([this] {
		run_decode_worker();
	});
}

EncodeRunner::~EncodeRunner() {
	{
		std::lock_guard lock(mutex_);
		stopping_ = true;
	}
	encodeCv_.notify_all();
	decodeCv_.notify_all();
	if (encodeWorker_.joinable()) {
		encodeWorker_.join();
	}
	if (decodeWorker_.joinable()) {
		decodeWorker_.join();
	}
}

void EncodeRunner::submit(const Command& command, const AppState& state) {
	if (command.kind != CommandKind::RunEncode) {
		return;
	}
	const EncoderBackend* backend = find_backend(backends_, command.backend);
	const ImageObject* image = image_by_id(state, command.image);
	const EncodeRun* run = run_by_id(state, command.run);
	if (backend == nullptr || image == nullptr || image->decoded == nullptr || run == nullptr) {
		push_action(failed_action(command.run, command.backend, "invalid encode command"));
		return;
	}

	Job job;
	job.command = command;
	job.backend = *backend;
	job.source = image->decoded;
	{
		std::lock_guard lock(mutex_);
		encodeQueue_.push(std::move(job));
	}
	encodeCv_.notify_one();
}

void EncodeRunner::request_cancel(EncodeRunId run) {
	std::lock_guard lock(mutex_);
	cancelRequested_.push_back(run);
}

std::vector<Action> EncodeRunner::take_actions() {
	std::lock_guard lock(mutex_);
	std::vector<Action> out;
	out.swap(actions_);
	return out;
}

void EncodeRunner::run_encode_worker() {
	for (;;) {
		Job job;
		{
			std::unique_lock lock(mutex_);
			encodeCv_.wait(lock, [&] {
				return stopping_ || !encodeQueue_.empty();
			});
			if (stopping_ && encodeQueue_.empty()) {
				return;
			}
			job = std::move(encodeQueue_.front());
			encodeQueue_.pop();
		}

		if (consume_cancel_requested(job.command.run)) {
			Action canceled;
			canceled.kind = ActionKind::EncodeRunCanceled;
			canceled.run = job.command.run;
			canceled.value = now_seconds();
			push_action(std::move(canceled));
			continue;
		}

		Action started;
		started.kind = ActionKind::EncodeRunStarted;
		started.run = job.command.run;
		started.value = now_seconds();
		push_action(started);

		try {
			EncodeResult encode = run_backend_encode(job.backend, *job.source, job.command.params);
			encode.metadata.run = job.command.run;
			DecodeJob decode;
			decode.command = job.command;
			decode.backend = job.backend;
			decode.source = job.source;
			decode.encode = std::move(encode);
			push_decode_job(std::move(decode));
		} catch (const std::exception& e) {
			Action failed = failed_action(job.command.run, job.command.backend, e.what());
			failed.value = now_seconds();
			push_action(std::move(failed));
		}
	}
}

void EncodeRunner::run_decode_worker() {
	for (;;) {
		DecodeJob job;
		{
			std::unique_lock lock(mutex_);
			decodeCv_.wait(lock, [&] {
				return stopping_ || !decodeQueue_.empty();
			});
			if (stopping_ && decodeQueue_.empty()) {
				return;
			}
			job = std::move(decodeQueue_.front());
			decodeQueue_.pop();
		}

		if (consume_cancel_requested(job.command.run)) {
			Action canceled;
			canceled.kind = ActionKind::EncodeRunCanceled;
			canceled.run = job.command.run;
			canceled.value = now_seconds();
			push_action(std::move(canceled));
			continue;
		}

		try {
			DecodeResult preview = job.backend.decodePreview(job.encode.encoded);
			if (preview.image) {
				MetricResult metric = compute_quality_metrics(*job.source, *preview.image);
				job.encode.metadata.psnrY = metric.psnrY;
				job.encode.metadata.psnrRgb = metric.psnrAll;
				job.encode.metadata.metrics = std::move(metric.metrics);
				if (!metric.psnrY && !metric.unavailableReason.empty()) {
					job.encode.metadata.metricError = metric.unavailableReason;
				}
			} else {
				job.encode.metadata.previewError = preview.error.empty() ? "preview decoder did not return an image" : preview.error;
			job.encode.metadata.metricError = "metrics unavailable because decoded preview is unavailable";
				job.encode.metadata.metrics = {
					QualityMetricRecord{"psnr-y", "PSNR-Y", std::nullopt, "dB", true, job.encode.metadata.metricError},
				};
			}

			if (consume_cancel_requested(job.command.run)) {
				Action canceled;
				canceled.kind = ActionKind::EncodeRunCanceled;
				canceled.run = job.command.run;
				canceled.value = now_seconds();
				push_action(std::move(canceled));
				continue;
			}

			Action completed;
			completed.kind = ActionKind::EncodeRunCompleted;
			completed.encodeCompleted.run = job.command.run;
			completed.encodeCompleted.metadata = std::move(job.encode.metadata);
			completed.encodeCompleted.preview = std::move(preview.image);
			completed.value = now_seconds();
			push_action(std::move(completed));
		} catch (const std::exception& e) {
			Action failed = failed_action(job.command.run, job.command.backend, e.what());
			failed.value = now_seconds();
			push_action(std::move(failed));
		}
	}
}

void EncodeRunner::push_action(Action action) {
	std::lock_guard lock(mutex_);
	actions_.push_back(std::move(action));
}

void EncodeRunner::push_decode_job(DecodeJob job) {
	{
		std::lock_guard lock(mutex_);
		decodeQueue_.push(std::move(job));
	}
	decodeCv_.notify_one();
}

double EncodeRunner::now_seconds() const {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - clockStart_).count();
}

bool EncodeRunner::consume_cancel_requested(EncodeRunId run) {
	std::lock_guard lock(mutex_);
	const auto it = std::find(cancelRequested_.begin(), cancelRequested_.end(), run);
	if (it == cancelRequested_.end()) {
		return false;
	}
	cancelRequested_.erase(it);
	return true;
}

} // namespace codec_gui::gui
