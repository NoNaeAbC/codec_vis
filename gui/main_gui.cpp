#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "app_commands.hpp"
#include "draw_list.hpp"
#include "encode_runner.hpp"
#include "decoder_runner.hpp"
#include "energy_access.hpp"
#include "encoder_backends.hpp"
#include "layout.hpp"
#include "render_vulkan.hpp"
#include "text_atlas.hpp"
#include "text_shaper.hpp"
#include "workbench.hpp"
#include <map>
#include "wayland_window.hpp"
#include "viewer_model.hpp"

#include "../codec_gui_image_io.hpp"
#include "../build/embedded_shaders.hpp"

#include <cstddef>
#include <hb.h>

#include <algorithm>
#include <exception>
#include <chrono>
#include <cmath>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

DecoderRunner* imageDecoderRunner=nullptr;
EnergyAccess* energyAccessRunner=nullptr;

using ActionFuture = std::future<std::vector<Action>>;

void print_runtime_probe() {
	std::cout << "Vulkan: " << vulkan_runtime_version_string() << '\n';
	std::cout << "HarfBuzz: "
	          << hb_version_string() << '\n';
	try {
		TextShaper const shaper(default_font_path(), 16.0f);
		std::cout << "Text shaping glyphs: " << shaper.shape("codec_vis").glyphs.size() << '\n';
	} catch (const std::exception& e) {
		std::cout << "Text shaping: unavailable (" << e.what() << ")\n";
	}
}

bool should_run_async(CommandKind kind) {
	return kind == CommandKind::ShowOpenFilePortal ||
	       kind == CommandKind::ShowSaveFilePortal ||
	       kind == CommandKind::LoadSourceImage ||
           kind == CommandKind::ComputeDerivedImage ||
	       kind == CommandKind::SaveBytesToFile;
}

void apply_action_and_commands(
	AppState& state,
	const Action& action,
	std::span<const EncoderBackend> backends,
	EncodeRunner* runner = nullptr,
	std::vector<ActionFuture>* asyncCommands = nullptr
) {
	std::vector<Action> pending{action};
	while (!pending.empty()) {
		Action const next = std::move(pending.back());
		pending.pop_back();
		UpdateResult result = update(std::move(state), next);
		state = std::move(result.state);
		for (const Command& command : result.commands) {
            if(energyAccessRunner && command.kind==CommandKind::AuthorizeEnergyAccess){energyAccessRunner->request();continue;}
            if(energyAccessRunner && command.kind==CommandKind::CloseEnergyAccess){energyAccessRunner->cancel();set_session_energy_counter({});continue;}
            if(imageDecoderRunner && (command.kind==CommandKind::QueryImageDecoders || command.kind==CommandKind::RunImageDecode)) {imageDecoderRunner->submit(command,state);continue;}
            if(imageDecoderRunner && command.kind==CommandKind::CancelImageDecode){imageDecoderRunner->cancel(command.image);continue;}
			if (runner != nullptr && command.kind == CommandKind::ComputeMetric) {
				runner->submit_metrics(command, state);
				continue;
			}
			if (runner != nullptr && (command.kind == CommandKind::RunEncode || command.kind==CommandKind::RunLivePreview || command.kind==CommandKind::CancelLivePreview)) {
				runner->submit(command, state);
				continue;
			}
			if (runner != nullptr && command.kind == CommandKind::RequestEncodeCancel) {
				runner->request_cancel(command.run);
				continue;
			}
			if (asyncCommands != nullptr && should_run_async(command.kind)) {
				AppState snapshot = state;
				std::vector<EncoderBackend> backendSnapshot(backends.begin(), backends.end());
				asyncCommands->push_back(std::async(std::launch::async, [command, snapshot = std::move(snapshot), backendSnapshot = std::move(backendSnapshot)]() mutable {
					return execute_command(command, snapshot, CommandContext{backendSnapshot});
				}));
				continue;
			}
			std::vector<Action> produced = execute_command(command, state, CommandContext{backends});
			// pending is a stack: preserve command notification order (e.g.
			// metrics started -> individual scores -> metrics completed).
			for (auto it=produced.rbegin();it!=produced.rend();++it) {
				pending.push_back(std::move(*it));
			}
		}
	}
}

void collect_async_actions(std::vector<ActionFuture>& futures, std::vector<Action>& out) {
	for (auto it = futures.begin(); it != futures.end();) {
		if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			std::vector<Action> actions = it->get();
			for (Action& action : actions) {
				out.push_back(std::move(action));
			}
			it = futures.erase(it);
		} else {
			++it;
		}
	}
}

} // namespace

int codec_vis_energy_helper_main(int argc, char** argv);
int main(int argc, char** argv) {
    // Dispatch before font initialization, Vulkan probing, or GUI setup.
    if (argc > 1 && std::string_view(argv[1]) == "--energy-helper")
        return codec_vis_energy_helper_main(argc - 1, argv + 1);
    if (argc > 1 && std::string_view(argv[1]) == "--check-assets") {
        for (const auto shader : {embedded::compose, embedded::linearize})
            if (shader.size() < 5 || shader.front() != 0x07230203) return 1;
        for (const auto font : {default_font_path(), default_monospace_font_path()}) {
            const TextShaper shaper(font, 16);
            if (shaper.shape("codec_vis AV2 VVC 0123456789").glyphs.empty()) return 1;
        }
        std::cout << "Embedded shaders and fonts verified\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--help") {
        std::cout << "codec_vis [--window-size W H] [--smoke-frames N] [--probe-image-pixels] [--check-assets] [IMAGE ...]\n"
                     "Single executable: software codecs, fonts, shaders and energy helper are built in.\n";
        return 0;
    }
	print_runtime_probe();

	bool smokeOnce = false;
	int smokeFrames = 0;
	bool probeImagePixels = false;
	std::vector<std::string> sourcePaths;
	int windowWidth = 1920;
	int windowHeight = 1080;
	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == "--window-size") {
			if (i + 2 >= argc) throw std::runtime_error("--window-size requires width and height");
			windowWidth = std::stoi(argv[++i]);
			windowHeight = std::stoi(argv[++i]);
			if (windowWidth <= 0 || windowHeight <= 0) throw std::runtime_error("window dimensions must be positive");
		} else if (arg == "--smoke-once") {
			smokeOnce = true;
		} else if (arg == "--smoke-frames") {
			if (i + 1 >= argc) {
				throw std::runtime_error("--smoke-frames requires a frame count");
			}
			smokeFrames = std::stoi(argv[++i]);
			if (smokeFrames <= 0) {
				throw std::runtime_error("--smoke-frames must be positive");
			}
		} else if (arg == "--probe-image-pixels") {
			probeImagePixels = true;
		} else {
			sourcePaths.emplace_back(arg);
		}
	}

	AppState state;
	const std::vector<EncoderBackend> backends = initial_encoder_backends();
	EncodeRunner encodeRunner(backends);
	DecoderRunner decoderRunner;imageDecoderRunner=&decoderRunner;
    EnergyAccess energyAccess;energyAccessRunner=&energyAccess;
    if(auto counter=open_energy_counter()) {
        state.energyAccessEnabled=true;state.energyAccessDomain=counter->domain;
        set_session_energy_counter(std::move(counter));
    }
	try {
		state.backends = query_backend_infos(backends);
	} catch (const std::exception& e) {
		std::cerr << "query backends: " << e.what() << '\n';
	}
	if (state.backends.empty()) {
		state.backends.reserve(backends.size());
		for (const EncoderBackend& backend : backends) {
			BackendInfo info;
			info.id = backend.id;
			info.name = backend.name;
			info.codec = backend.codec;
			info.kind = backend.kind;
			info.capabilities.implementation = backend.name;
			state.backends.push_back(std::move(info));
		}
	}
	if (!state.backends.empty()) {
		state.selection.selectedBackend = state.backends.front().id;
	}
	std::vector<ActionFuture> asyncCommands;
	state.interaction.framebufferWidth = windowWidth;
	state.interaction.framebufferHeight = windowHeight;
	if (!sourcePaths.empty()) {
        for(const auto& sourcePath:sourcePaths){
		Action chosen;
		chosen.kind = ActionKind::SourcePathChosen;
		chosen.path = sourcePath;
		apply_action_and_commands(state, chosen, backends, &encodeRunner);
        }
	} else if (smokeOnce || smokeFrames > 0 || probeImagePixels) {
		Action loaded;
		loaded.kind = ActionKind::SourceLoaded;
		loaded.sourceLoaded.path = "test-pattern";
		loaded.sourceLoaded.image = std::make_shared<RawImage>(make_test_pattern());
		apply_action_and_commands(state, loaded, backends, &encodeRunner);
	}

    std::vector<DrawCommand> drawCommands;
    std::map<std::pair<FontRole,int>,std::unique_ptr<TextShaper>> measureFonts;
    float renderScale = 1.0f;
    Workbench workbench([&](std::string_view text,float size,FontRole role){
        int pixels=int(std::round(size*renderScale));auto key=std::pair{role,pixels};
        auto& font=measureFonts[key];if(!font)font=std::make_unique<TextShaper>(role==FontRole::Monospace?default_monospace_font_path():default_font_path(),float(pixels));
        return font->shape(text).width/renderScale;
    });
	TextAtlasCache textCache;
	TextAtlas& atlas=textCache.atlas();
	std::size_t textQuadCount = 0;
	const auto startTime = std::chrono::steady_clock::now();
	std::unique_ptr<TextShaper> textShaper;
	auto elapsed_seconds = [&]() {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
	};
	auto rebuild_frame_model = [&]() {
        for(int pass=0;pass<3;++pass){
            auto actions=workbench.build(state,{0,0,float(state.interaction.framebufferWidth),float(state.interaction.framebufferHeight)},elapsed_seconds(),renderScale);
            if(actions.empty()&&!workbench.needs_rebuild())break;
            for(const auto& action:actions)apply_action_and_commands(state,action,backends,&encodeRunner,&asyncCommands);
        }
        drawCommands=workbench.commands();
		try {
			if (!textShaper) textShaper = std::make_unique<TextShaper>(default_font_path(), 16.0f * renderScale);
			textCache.build(drawCommands, *textShaper, 1024, renderScale);
			textQuadCount = atlas.quads.size();
		} catch (const std::exception& e) {
			textCache.clear();
			textQuadCount = 0;
			const bool alreadyReported = std::ranges::any_of(state.errors, [](const AppError& error) {
				return error.subsystem == "text renderer" && error.operation == "shape UI text";
			});
			if (!alreadyReported) {
				state.errors.push_back(AppError{
					.id = state.nextId++,
					.severity = ErrorSeverity::Error,
					.operation = "shape UI text",
					.subsystem = "text renderer",
					.message = e.what(),
					.detail = {},
					.image = std::nullopt,
					.run = std::nullopt,
				});
			}
		}
	};
	rebuild_frame_model();

	try {
		WaylandWindow window = WaylandWindow::create(state.interaction.framebufferWidth, state.interaction.framebufferHeight, "codec_vis");
		renderScale = window.output_scale();
		state.interaction.framebufferWidth = window.width();
		state.interaction.framebufferHeight = window.height();
		state.interaction.outputScale = 1.0f;
		textShaper.reset();
		rebuild_frame_model();
		std::cout << "Wayland: xdg-shell window " << window.width() << "x" << window.height()
		          << " framebuffer " << window.framebuffer_width() << "x" << window.framebuffer_height()
		          << " scale " << renderScale << '\n';
		VulkanRenderer renderer = VulkanRenderer::create(window);
		renderer.sync_images(state.images,drawCommands,workbench.comparison_images());
		int renderedLogicalWidth = window.width();
		int renderedLogicalHeight = window.height();
		float renderedScale = renderScale;
		int presentedFrames = 0;
		do {
			window.dispatch_pending();
			bool frameDirty = false;
            static std::string pasteTarget;
            for(const Action& action:window.take_actions()) {
                if(action.kind==ActionKind::ClipboardText)workbench.context().paste(pasteTarget,action.text);
                else {
                    for(const auto& generated:workbench.handle(action,state))apply_action_and_commands(state,generated,backends,&encodeRunner,&asyncCommands);
                    if(action.kind==ActionKind::PointerPressed && action.button==272 && !workbench.input_consumed())
                        window.begin_move(action.pointerSerial);
                    if(!ui::input_from_action(action))apply_action_and_commands(state,action,backends,&encodeRunner,&asyncCommands);
                }
                // Consume each input against the frame produced by the preceding input.
                rebuild_frame_model();
                for(auto& request:workbench.context().take_clipboard_requests()) {
                    if(request.kind==ui::ClipboardRequest::Copy)window.set_clipboard_text(request.text);
                    else {pasteTarget=request.widget;window.request_clipboard_text();}
                }
                frameDirty=true;
            }
            if(auto result=energyAccess.take_result()) {
                Action ready;ready.kind=ActionKind::EnergyAccessReady;ready.value=result->counter?1:0;
                ready.text=result->counter?result->counter->domain:result->error;
                set_session_energy_counter(std::move(result->counter));
                apply_action_and_commands(state,ready,backends,&encodeRunner,&asyncCommands);frameDirty=true;
            }
			for (const Action& notification : decoderRunner.take_actions()) {
                apply_action_and_commands(state,notification,backends,&encodeRunner,&asyncCommands);frameDirty=true;
            }
            for (const Action& action : encodeRunner.take_actions()) {
				apply_action_and_commands(state, action, backends, &encodeRunner, &asyncCommands);
				frameDirty = true;
			}
			std::vector<Action> asyncActions;
			collect_async_actions(asyncCommands, asyncActions);
			for (const Action& action : asyncActions) {
				apply_action_and_commands(state, action, backends, &encodeRunner, &asyncCommands);
				frameDirty = true;
			}
			if (window.framebuffer_width() != static_cast<int>(renderer.width()) ||
			    window.framebuffer_height() != static_cast<int>(renderer.height()) ||
			    window.width() != renderedLogicalWidth || window.height() != renderedLogicalHeight ||
			    std::abs(window.output_scale() - renderedScale) > 0.001f) {
				renderer.recreate_swapchain(window);
				state.interaction.framebufferWidth = window.width();
				state.interaction.framebufferHeight = window.height();
				state.interaction.outputScale = 1.0f;
				if (std::abs(window.output_scale() - renderedScale) > 0.001f) {
					renderScale = window.output_scale();
					textShaper.reset();
                    measureFonts.clear();
				}
				renderedLogicalWidth = window.width();
				renderedLogicalHeight = window.height();
				renderedScale = window.output_scale();
				frameDirty = true;
			}
			if (frameDirty) {
				rebuild_frame_model();
				renderer.sync_images(state.images,drawCommands,workbench.comparison_images());
			}
			if (workbench.animated()) {
				rebuild_frame_model();
                renderer.sync_images(state.images,drawCommands,workbench.comparison_images());
			}
			if (probeImagePixels) {
				Rect probeRect = workbench.canvas();
				for (const DrawCommand& command : drawCommands) {
					if (command.kind == DrawCommandKind::Image && valid(command.image)) {
						probeRect = command.rect;
						break;
					}
				}
				const bool nonblank = renderer.render_draw_list_pixel_probe(drawCommands, &atlas, probeRect);
				std::cout << "Image pixel probe: " << (nonblank ? "nonblank" : "blank") << '\n';
				probeImagePixels = false;
			} else {
				renderer.render_draw_list(drawCommands, &atlas);
			}
			++presentedFrames;
			if (smokeFrames > 0 && presentedFrames >= smokeFrames) {
				break;
			}
			if (!smokeOnce && smokeFrames == 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(16));
			}
		} while (!smokeOnce && !window.close_requested());
		std::cout << "Frames presented: " << presentedFrames << '\n';
		std::cout << "Vulkan renderer: " << renderer.device_name() << " queue " << renderer.queue_family()
		          << " swapchain " << renderer.width() << "x" << renderer.height() << '\n';
	} catch (const std::exception& e) {
		std::cerr << "Wayland/Vulkan render failed: " << e.what() << '\n';
        return 1;
	}

	std::cout << "Images: " << state.images.size() << '\n';
	std::cout << "Panes: " << state.panes.size() << '\n';
	std::cout << "Backends: " << state.backends.size() << '\n';

	std::cout << "Draw commands: " << drawCommands.size() << '\n';
	std::cout << "Text quads: " << textQuadCount << '\n';
	std::cout << "Viewer: " << workbench.canvas().w << "x" << workbench.canvas().h << '\n';
	for (const AppError& error : state.errors) {
		std::cerr << error.operation << ": " << error.message << '\n';
	}

	return state.errors.empty() ? 0 : 1;
}
