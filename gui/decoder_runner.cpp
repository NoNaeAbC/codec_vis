#include "decoder_runner.hpp"
#include "decode_energy.hpp"
#include <algorithm>
namespace codec_gui::gui {
DecoderRunner::DecoderRunner() : worker_([this] { work(); }) {}
DecoderRunner::~DecoderRunner() {
	{
		std::lock_guard lock(mutex_);
		stopping_ = true;
		for (auto &[id, cancel] : cancellations_)
			*cancel = true;
	}
	cv_.notify_all();
	worker_.join();
}
void DecoderRunner::cancel(ImageId id) {
	std::lock_guard lock(mutex_);
	for (auto &[image, flag] : cancellations_)
		if (image == id)
			*flag = true;
}
void DecoderRunner::submit(const Command &command, const AppState &state) {
	const auto *image = find_image(state, command.image);
	if (!image)
		return;
	auto flag = std::make_shared<std::atomic_bool>(false);
	std::lock_guard lock(mutex_);
	jobs_.push({command, image_decode_input(*image), flag});
	cancellations_.push_back({command.image, flag});
	cv_.notify_one();
}
void DecoderRunner::publish(Action a) {
	std::lock_guard lock(mutex_);
	actions_.push_back(std::move(a));
}
std::vector<Action> DecoderRunner::take_actions() {
	std::lock_guard lock(mutex_);
	std::vector<Action> out;
	out.swap(actions_);
	return out;
}
void DecoderRunner::work() {
	for (;;) {
		Job job;
		{
			std::unique_lock lock(mutex_);
			cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
			if (stopping_)
				return;
			job = std::move(jobs_.front());
			jobs_.pop();
		}
		Action action;
		action.image = job.command.image;
		action.decoder.generation = job.command.revision;
		try {
			auto candidates = image_decoder_candidates(job.input);
			if (job.command.kind == CommandKind::QueryImageDecoders) {
				action.kind = ActionKind::DecodersReady;
				for (const auto &candidate : candidates) {
					if (*job.cancel)
						break;
					auto decoded = candidate.decode(*job.input.bytes);
					if (decoder_output_matches(job.input, decoded))
						action.decoder.available.push_back(candidate.info);
				}
				if (action.decoder.available.empty())
					action.decoder.error = "No compatible decoder found for this image.";
				if (!job.input.bytes)
					action.decoder.error = "This image has no retained compressed bitstream.";
			} else {
				action.kind = ActionKind::DecodeFinished;
				auto &result = action.decoder.measurement;
				result.decoderId = job.command.text;
				result.requested = std::clamp(int(job.command.value), 1, 1000);
				const auto chosen = std::find_if(candidates.begin(), candidates.end(),
												 [&](const auto &d) { return d.info.id == job.command.text; });
				if (chosen == candidates.end())
					throw std::runtime_error("The selected decoder is no longer available.");
				result.decoderName = chosen->info.name;
				DecodeEnergy energy;
				result.energyDomain = energy.domain;
				result.energyNote = energy.note;
				for (int i = 0; i < result.requested && !*job.cancel; ++i) {
					DecodeSample sample;
					const auto before = energy.read();
					const auto start = std::chrono::steady_clock::now();
					DecodeResult decoded;
					try {
						decoded = chosen->decode(*job.input.bytes);
					} catch (const std::exception &e) {
						decoded.error = e.what();
					}
					sample.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
					const auto after = energy.read();
					sample.codecSeconds = decoded.codecSeconds;
					if (before && after) {
						sample.joules = energy.joules(*before, *after);
						sample.energySeconds = std::chrono::duration<double>(after->time - before->time).count();
					}
					if (!decoder_output_matches(job.input, decoded))
						sample.error = decoded.error.empty() ? "Decoder output differs from the reference or changes "
															   "dimensions, precision, or chroma."
															 : decoded.error;
					result.samples.push_back(sample);
					Action progress = action;
					progress.kind = ActionKind::DecodeProgress;
					publish(std::move(progress));
					if (!sample.error.empty())
						break;
				}
				result.canceled = *job.cancel;
				result.finished = true;
			}
		} catch (const std::exception &e) {
			action.kind = job.command.kind == CommandKind::QueryImageDecoders ? ActionKind::DecodersReady
																			  : ActionKind::DecodeFinished;
			action.decoder.error = e.what();
			action.decoder.measurement.finished = true;
		}
		publish(std::move(action));
		{
			std::lock_guard lock(mutex_);
			std::erase_if(cancellations_, [&](const auto &entry) { return entry.second == job.cancel; });
		}
	}
}
} // namespace codec_gui::gui
