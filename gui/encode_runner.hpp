#pragma once

#include "app_state.hpp"
#include "encoder_backends.hpp"
#include "metrics.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <thread>
#include <vector>

namespace codec_gui::gui {

	class EncodeRunner {
	public:
		using MetricEvaluator = std::function<QualityMetricRecord(std::string_view, const LinearBt2020Image &,
																  const LinearBt2020Image &)>;
		explicit EncodeRunner(std::span<const EncoderBackend> backends, MetricEvaluator evaluator = compute_metric);
		EncodeRunner(const EncodeRunner &)            = delete;
		EncodeRunner &operator=(const EncodeRunner &) = delete;
		~EncodeRunner();

		void                              submit(const Command &command, const AppState &state);
		void                              request_cancel(EncodeRunId run);
		void                              submit_metrics(const Command &command, const AppState &state);
		[[nodiscard]] std::vector<Action> take_actions();

	private:
		struct Job {
			Command                               command;
			EncoderBackend                        backend;
			std::shared_ptr<const RawImage>       source;
			std::chrono::steady_clock::time_point queued = std::chrono::steady_clock::now();
		};

		struct DecodeJob {
			Command                               command;
			EncoderBackend                        backend;
			std::shared_ptr<const RawImage>       source;
			EncodeResult                          encode;
			std::chrono::steady_clock::time_point queued;
		};
		struct MetricJob {
			ImageId                         image;
			std::shared_ptr<const RawImage> reference, candidate;
			std::vector<std::string>        metricIds;
		};

		std::vector<EncoderBackend>           backends_;
		MetricEvaluator                       metricEvaluator_;
		std::mutex                            mutex_;
		std::condition_variable               encodeCv_;
		std::condition_variable               decodeCv_;
		std::condition_variable               liveEncodeCv_, liveDecodeCv_;
		std::condition_variable               metricCv_;
		std::queue<Job>                       encodeQueue_;
		std::optional<Job>                    liveJob_;
		std::queue<DecodeJob>                 decodeQueue_;
		std::optional<DecodeJob>              liveDecodeJob_;
		std::queue<MetricJob>                 metricQueue_;
		std::vector<Action>                   actions_;
		std::vector<EncodeRunId>              cancelRequested_;
		std::thread                           encodeWorker_;
		std::thread                           decodeWorker_;
		std::thread                           liveEncodeWorker_, liveDecodeWorker_;
		std::thread                           metricWorker_;
		std::chrono::steady_clock::time_point clockStart_;
		bool                                  stopping_ = false;

		void                 run_encode_worker(bool liveLane = false);
		void                 run_decode_worker(bool liveLane = false);
		void                 run_metric_worker();
		void                 push_action(Action action);
		void                 push_decode_job(DecodeJob job);
		[[nodiscard]] double now_seconds() const;
		[[nodiscard]] bool   consume_cancel_requested(EncodeRunId run);
	};

} // namespace codec_gui::gui
