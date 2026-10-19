#pragma once
#include "decoder_registry.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
namespace codec_gui::gui {
class DecoderRunner {
	struct Job {
		Command command;
		ImageDecodeInput input;
		std::shared_ptr<std::atomic_bool> cancel;
	};
	std::mutex mutex_;
	std::condition_variable cv_;
	std::queue<Job> jobs_;
	std::vector<Action> actions_;
	std::vector<std::pair<ImageId, std::shared_ptr<std::atomic_bool>>> cancellations_;
	bool stopping_ = false;
	std::thread worker_;
	void work();
	void publish(Action);

  public:
	DecoderRunner();
	~DecoderRunner();
	void submit(const Command &, const AppState &);
	void cancel(ImageId);
	std::vector<Action> take_actions();
};
} // namespace codec_gui::gui
