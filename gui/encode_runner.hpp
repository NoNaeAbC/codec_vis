#pragma once

#include "app_state.hpp"
#include "encoder_backends.hpp"

#include <condition_variable>
#include <chrono>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <thread>
#include <vector>

namespace codec_gui::gui {

	class EncodeRunner {
	public:
		explicit EncodeRunner(std::span<const EncoderBackend> backends);
		EncodeRunner(const EncodeRunner&) = delete;
		EncodeRunner& operator=(const EncodeRunner&) = delete;
		~EncodeRunner();

		void submit(const Command& command, const AppState& state);
		void request_cancel(EncodeRunId run);
		[[nodiscard]] std::vector<Action> take_actions();

	private:
		struct Job {
			Command command;
			EncoderBackend backend;
			std::shared_ptr<const RawImage> source;
		};

		struct DecodeJob {
			Command command;
			EncoderBackend backend;
			std::shared_ptr<const RawImage> source;
			EncodeResult encode;
		};

		std::vector<EncoderBackend> backends_;
		std::mutex mutex_;
		std::condition_variable encodeCv_;
		std::condition_variable decodeCv_;
		std::queue<Job> encodeQueue_;
		std::queue<DecodeJob> decodeQueue_;
		std::vector<Action> actions_;
		std::vector<EncodeRunId> cancelRequested_;
		std::thread encodeWorker_;
		std::thread decodeWorker_;
		std::chrono::steady_clock::time_point clockStart_;
		bool stopping_ = false;

		void run_encode_worker();
		void run_decode_worker();
		void push_action(Action action);
		void push_decode_job(DecodeJob job);
		[[nodiscard]] double now_seconds() const;
		[[nodiscard]] bool consume_cancel_requested(EncodeRunId run);
	};

} // namespace codec_gui::gui
