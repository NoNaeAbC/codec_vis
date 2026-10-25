#include "app_commands.hpp"

#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "encoder_backends.hpp"
#include "image_ops.hpp"
#include "metrics.hpp"
#include "platform_portal.hpp"

#include "../codec_gui_image_io.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace codec_gui::gui {
namespace {

const ImageObject* command_image(const AppState& state, ImageId id) {
	return find_image(state, id);
}

const EncodeRun* command_run(const AppState& state, EncodeRunId id) {
	for (const EncodeRun& run : state.encodeRuns) {
		if (run.id == id) {
			return &run;
		}
	}
	return nullptr;
}

Action failed_action(ActionKind kind, std::string message, BackendId backend = {}, EncodeRunId run = {}) {
	Action action;
	action.kind = kind;
	action.text = std::move(message);
	action.backend = backend;
	action.run = run;
	return action;
}

} // namespace

std::vector<Action> execute_command(const Command& command, const AppState& state, CommandContext context) {
	std::vector<Action> actions;

	try {
		switch (command.kind) {
			case CommandKind::LoadSourceImage: {
				Action action;
				action.kind = ActionKind::SourceLoaded;
				action.sourceLoaded.path = command.path;
				auto loaded = std::make_shared<RawImage>(load_input_image(command.path));
				if (command.importPixelAspectRatio) {
					loaded->intrinsicPixelAspectRatio = loaded->pixelAspectRatio;
					loaded->pixelAspectRatio = *command.importPixelAspectRatio;
					loaded->importNote += " External pixel aspect ratio " + std::to_string(loaded->pixelAspectRatio.width) + ":" + std::to_string(loaded->pixelAspectRatio.height) + "; original samples retained.";
				}
				action.sourceLoaded.image = std::move(loaded);
				std::error_code sizeError;
				const uintmax_t fileSize = std::filesystem::file_size(command.path, sizeError);
				if (!sizeError && fileSize <= std::numeric_limits<uint64_t>::max()) {
					action.sourceLoaded.fileByteSize = static_cast<uint64_t>(fileSize);
				}
				actions.push_back(std::move(action));
				break;
			}
			case CommandKind::QueryBackendCapabilities: {
				const EncoderBackend* backend = find_backend(context.backends, command.backend);
				if (backend == nullptr) {
					actions.push_back(failed_action(ActionKind::BackendCapabilitiesFailed, "unknown backend", command.backend));
					break;
				}
				CapabilityResult caps = backend->queryCapabilities();
				Action action;
				action.kind = caps.snapshot.available ? ActionKind::BackendCapabilitiesReady : ActionKind::BackendCapabilitiesFailed;
				action.backend = command.backend;
				action.backendCapabilities.backend = command.backend;
				action.backendCapabilities.snapshot = std::move(caps.snapshot);
				action.backendCapabilities.params = std::move(caps.params);
				if (!action.backendCapabilities.snapshot.available) {
					action.text = action.backendCapabilities.snapshot.error;
				}
				actions.push_back(std::move(action));
				break;
			}
			case CommandKind::RunEncode: {
				const EncoderBackend* backend = find_backend(context.backends, command.backend);
				const ImageObject* image = command_image(state, command.image);
				const EncodeRun* run = command_run(state, command.run);
				if (backend == nullptr || image == nullptr || image->decoded == nullptr || run == nullptr) {
					actions.push_back(failed_action(ActionKind::EncodeRunFailed, "invalid encode command", command.backend, command.run));
					break;
				}

				Action started;
				started.kind = ActionKind::EncodeRunStarted;
				started.run = command.run;
				started.value = 0.0;
				actions.push_back(started);

				EncodeResult encode = run_backend_encode(*backend, *image->decoded, command.params);
				encode.metadata.run = command.run;
				const auto decodeStart = std::chrono::steady_clock::now();
				DecodeResult preview = backend->decodePreview(encode.encoded);
				encode.metadata.decodeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - decodeStart).count();
				if (preview.image) {
					encode.metadata.codedPixelFormat = preview.image->format;
					encode.metadata.codedColor = preview.image->color;
				} else {
					encode.metadata.previewError = preview.error.empty() ? "preview decoder did not return an image" : preview.error;
				encode.metadata.metricError = "metrics unavailable because decoded preview is unavailable";
					encode.metadata.metrics = {
						QualityMetricRecord{.id="psnr-y", .label="PSNR-Y", .value=std::nullopt, .unit="dB", .higherIsBetter=true, .unavailableReason=encode.metadata.metricError},
					};
				}

				Action completed;
				completed.kind = ActionKind::EncodeRunCompleted;
				completed.encodeCompleted.run = command.run;
				completed.encodeCompleted.metadata = std::move(encode.metadata);
				completed.encodeCompleted.preview = std::move(preview.image);
				completed.value = completed.encodeCompleted.metadata.encodeSeconds;
				actions.push_back(std::move(completed));
				break;
			}
			case CommandKind::SaveBytesToFile: {
				if (std::filesystem::exists(command.path) && command.value < 1.0) {
					actions.push_back(failed_action(ActionKind::SaveFailed, "output file exists; overwrite was not confirmed"));
					break;
				}
				dump_to_file(command.path, command.bytes);
				Action action;
				action.kind = ActionKind::SaveCompleted;
				action.image = command.image;
				action.path = command.path;
				actions.push_back(std::move(action));
				break;
			}
			case CommandKind::ComputeDerivedImage: {
				const ImageObject* first = command_image(state, command.image);
				const ImageObject* second = command_image(state, command.otherImage);
				if (first == nullptr || second == nullptr || !first->decoded || !second->decoded) {
					actions.push_back(failed_action(ActionKind::DerivedImageFailed, "difference inputs are not decoded"));
					break;
				}
				DerivedImageResult diff = command.text=="linear-difference"?compute_linear_difference(*first->decoded,*second->decoded,command.value):compute_absolute_difference(*first->decoded, *second->decoded, command.value);
				if (!diff.image) {
					actions.push_back(failed_action(ActionKind::DerivedImageFailed, diff.error.empty() ? "difference image could not be computed" : diff.error));
					break;
				}
				Action action;
				action.kind = ActionKind::DerivedImageComputed;
				action.derivedImage.first = first->id;
				action.derivedImage.second = second->id;
                action.derivedImage.firstRevision=first->previewRevision;action.derivedImage.secondRevision=second->previewRevision;
				action.derivedImage.gain = command.value;
				action.derivedImage.displayName = "Difference: " + first->displayName + " vs " + second->displayName;
				action.derivedImage.image = std::move(diff.image);
				actions.push_back(std::move(action));
				break;
			}
			case CommandKind::ShowOpenFilePortal:
				actions.push_back(show_open_file_portal());
				break;
			case CommandKind::ShowSaveFilePortal:
				actions.push_back(show_save_file_portal(command.image, command.path));
				break;
			case CommandKind::ComputeMetric: {
				const auto* image=command_image(state,command.image);
				const auto* reference=command_image(state,command.otherImage);
				if(!image || !image->decoded || !reference || !reference->decoded)
					throw std::invalid_argument("decoded result or source is unavailable");
				Action started;started.kind=ActionKind::MetricsStarted;started.image=command.image;actions.push_back(started);
				const auto start=std::chrono::steady_clock::now();
				const auto result=compute_quality_metrics(*reference->decoded,*image->decoded);
				if(!result.unavailableReason.empty()) throw std::runtime_error(result.unavailableReason);
				for(const auto& metric:result.metrics) {
					Action computed;computed.kind=ActionKind::MetricComputed;computed.image=command.image;computed.metric=metric;actions.push_back(std::move(computed));
				}
				Action done;done.kind=ActionKind::MetricsCompleted;done.image=command.image;
				done.value=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();actions.push_back(done);
				break;
			}
			case CommandKind::UploadImageTexture:
			case CommandKind::RequestRedraw:
			case CommandKind::RequestEncodeCancel:
			case CommandKind::RunLivePreview:
			case CommandKind::CancelLivePreview:
			// Image decoder work is routed to DecoderRunner by the GUI.
			case CommandKind::QueryImageDecoders:
			case CommandKind::RunImageDecode:
			case CommandKind::CancelImageDecode:
			// Authorization is routed to EnergyAccess by the GUI.
			case CommandKind::AuthorizeEnergyAccess:
			case CommandKind::CloseEnergyAccess:
			case CommandKind::DecodeEncodedBytes:
				break;
		}
	} catch (const std::exception& e) {
		switch (command.kind) {
			case CommandKind::ComputeMetric: {
				Action failed;failed.kind=ActionKind::MetricsFailed;failed.image=command.image;failed.text=e.what();actions.push_back(std::move(failed));break;
			}
			case CommandKind::LoadSourceImage:
				actions.push_back(failed_action(ActionKind::SourceLoadFailed, e.what()));
				break;
			case CommandKind::QueryBackendCapabilities:
				actions.push_back(failed_action(ActionKind::BackendCapabilitiesFailed, e.what(), command.backend));
				break;
			case CommandKind::RunEncode:
				actions.push_back(failed_action(ActionKind::EncodeRunFailed, e.what(), command.backend, command.run));
				break;
			case CommandKind::SaveBytesToFile:
				actions.push_back(failed_action(ActionKind::SaveFailed, e.what()));
				break;
			case CommandKind::ShowSaveFilePortal:
				actions.push_back(failed_action(ActionKind::SaveFailed, e.what()));
				break;
			case CommandKind::ComputeDerivedImage:
				actions.push_back(failed_action(ActionKind::DerivedImageFailed, e.what()));
				break;
			default:
				break;
		}
	}

	return actions;
}

} // namespace codec_gui::gui
