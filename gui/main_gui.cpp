#include "app_state.hpp"
#include "app_commands.hpp"
#include "draw_list.hpp"
#include "encode_runner.hpp"
#include "encoder_backends.hpp"
#include "layout.hpp"
#include "render_vulkan.hpp"
#include "text_atlas.hpp"
#include "text_shaper.hpp"
#include "ui_interaction.hpp"
#include "wayland_window.hpp"
#include "viewer_model.hpp"

#include "../codec_gui_image_io.hpp"

#include <hb.h>

#include <exception>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace codec_gui;
using namespace codec_gui::gui;

namespace {

using ActionFuture = std::future<std::vector<Action>>;

void print_runtime_probe() {
	std::cout << "Vulkan: " << vulkan_runtime_version_string() << '\n';
	std::cout << "HarfBuzz: "
	          << hb_version_string() << '\n';
	try {
		TextShaper shaper(default_font_path(), 16.0f);
		std::cout << "Text shaping glyphs: " << shaper.shape("codec_vis").glyphs.size() << '\n';
	} catch (const std::exception& e) {
		std::cout << "Text shaping: unavailable (" << e.what() << ")\n";
	}
}

bool should_run_async(CommandKind kind) {
	return kind == CommandKind::ShowOpenFilePortal ||
	       kind == CommandKind::ShowSaveFilePortal ||
	       kind == CommandKind::LoadSourceImage ||
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
		Action next = std::move(pending.back());
		pending.pop_back();
		UpdateResult result = update(std::move(state), next);
		state = std::move(result.state);
		for (const Command& command : result.commands) {
			if (runner != nullptr && command.kind == CommandKind::RunEncode) {
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
			for (Action& producedAction : produced) {
				pending.push_back(std::move(producedAction));
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

int main(int argc, char** argv) {
	print_runtime_probe();

	bool smokeOnce = false;
	int smokeFrames = 0;
	bool probeImagePixels = false;
	std::string_view sourcePath;
	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		if (arg == "--smoke-once") {
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
		} else if (sourcePath.empty()) {
			sourcePath = arg;
		}
	}

	AppState state;
	const std::vector<EncoderBackend> backends = initial_encoder_backends();
	EncodeRunner encodeRunner(backends);
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
	state.interaction.framebufferWidth = 1280;
	state.interaction.framebufferHeight = 720;
	if (!sourcePath.empty()) {
		Action chosen;
		chosen.kind = ActionKind::SourcePathChosen;
		chosen.path = sourcePath;
		apply_action_and_commands(state, chosen, backends, &encodeRunner);
	} else {
		Action loaded;
		loaded.kind = ActionKind::SourceLoaded;
		loaded.sourceLoaded.path = "test-pattern";
		loaded.sourceLoaded.image = std::make_shared<RawImage>(make_test_pattern());
		apply_action_and_commands(state, loaded, backends, &encodeRunner);
	}

	LayoutResult layout = compute_layout(
		state.layout,
		state.interaction.framebufferWidth,
		state.interaction.framebufferHeight,
		state.interaction.outputScale
	);
	std::vector<PaneRect> panes = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
	std::vector<DrawCommand> drawCommands = build_draw_list(state, layout, {}, 0.0);
	TextAtlas atlas;
	std::size_t textQuadCount = 0;
	const auto startTime = std::chrono::steady_clock::now();
	auto elapsed_seconds = [&]() {
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
	};
	auto rebuild_frame_model = [&]() {
		layout = compute_layout(
			state.layout,
			state.interaction.framebufferWidth,
			state.interaction.framebufferHeight,
			state.interaction.outputScale
		);
		panes = compute_pane_rects(state.viewMode, state.panes, layout.viewer);
		drawCommands = build_draw_list(state, layout, {}, elapsed_seconds());
		try {
			TextShaper shaper(default_font_path(), 16.0f);
			atlas = build_text_atlas(drawCommands, shaper);
			textQuadCount = atlas.quads.size();
		} catch (const std::exception&) {
			atlas = {};
			textQuadCount = 0;
		}
	};
	rebuild_frame_model();

	try {
		WaylandWindow window = WaylandWindow::create(state.interaction.framebufferWidth, state.interaction.framebufferHeight, "codec_vis");
		std::cout << "Wayland: xdg-shell window " << window.width() << "x" << window.height() << '\n';
		VulkanRenderer renderer = VulkanRenderer::create(window);
		renderer.sync_images(state.images);
		int presentedFrames = 0;
		do {
			window.dispatch_pending();
			bool frameDirty = false;
			for (const Action& action : window.take_actions()) {
				bool consumed = false;
				if (action.kind == ActionKind::PointerPressed) {
					std::vector<Action> uiActions = actions_for_pointer_press(state, layout, action.point);
					for (const Action& uiAction : uiActions) {
						apply_action_and_commands(state, uiAction, backends, &encodeRunner, &asyncCommands);
					}
					consumed = !uiActions.empty();
				}
				if (!consumed) {
					apply_action_and_commands(state, action, backends, &encodeRunner, &asyncCommands);
				}
				frameDirty = true;
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
			if (window.width() != static_cast<int>(renderer.width()) || window.height() != static_cast<int>(renderer.height())) {
				renderer.recreate_swapchain(window);
				state.interaction.framebufferWidth = window.width();
				state.interaction.framebufferHeight = window.height();
				frameDirty = true;
			}
			if (frameDirty) {
				renderer.sync_images(state.images);
				rebuild_frame_model();
			}
			if (state.viewMode.kind == ViewModeKind::Blink) {
				rebuild_frame_model();
			}
			if (probeImagePixels) {
				Rect probeRect = layout.viewer;
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
		std::cout << "Wayland/Vulkan render: unavailable (" << e.what() << ")\n";
	}

	std::cout << "Images: " << state.images.size() << '\n';
	std::cout << "Panes: " << state.panes.size() << '\n';
	std::cout << "Backends: " << state.backends.size() << '\n';
	std::cout << "Visible pane rects: " << panes.size() << '\n';
	std::cout << "Draw commands: " << drawCommands.size() << '\n';
	std::cout << "Text quads: " << textQuadCount << '\n';
	std::cout << "Viewer: " << layout.viewer.w << "x" << layout.viewer.h << '\n';
	for (const AppError& error : state.errors) {
		std::cerr << error.operation << ": " << error.message << '\n';
	}

	return state.errors.empty() ? 0 : 1;
}
