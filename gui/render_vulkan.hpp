#pragma once

#include "draw_list.hpp"
#include "wayland_window.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace codec_gui::gui {

	struct TextAtlas;

	[[nodiscard]] std::string vulkan_runtime_version_string();

	class VulkanRenderer {
	public:
		VulkanRenderer() = default;
		VulkanRenderer(const VulkanRenderer&) = delete;
		VulkanRenderer& operator=(const VulkanRenderer&) = delete;
		VulkanRenderer(VulkanRenderer&& other) noexcept;
		VulkanRenderer& operator=(VulkanRenderer&& other) noexcept;
		~VulkanRenderer();

		static VulkanRenderer create(const WaylandWindow& window);

		[[nodiscard]] bool valid() const;
		[[nodiscard]] std::string device_name() const;
		[[nodiscard]] uint32_t queue_family() const;
		[[nodiscard]] uint32_t width() const;
		[[nodiscard]] uint32_t height() const;

		void sync_images(std::span<const ImageObject> images);
		void recreate_swapchain(const WaylandWindow& window);
		void render_clear_frame(float r, float g, float b, float a);
		void render_draw_list(const std::vector<DrawCommand>& commands);
		void render_draw_list(const std::vector<DrawCommand>& commands, const TextAtlas* textAtlas);
		[[nodiscard]] bool render_draw_list_pixel_probe(
			const std::vector<DrawCommand>& commands,
			const TextAtlas* textAtlas,
			Rect probeRect
		);

	private:
		struct Impl;
		Impl* impl_ = nullptr;

		explicit VulkanRenderer(Impl* impl);
		void reset();
	};

} // namespace codec_gui::gui
