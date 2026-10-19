#pragma once

#include "draw_list.hpp"
#include "wayland_window.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <array>

namespace codec_gui::gui {

	struct TextAtlas;
	struct RendererStatistics {
		std::size_t instances=0, dispatches=0, instanceUploadBytes=0, textUploadBytes=0;
		std::size_t residentImages=0, residentImageBytes=0;
	};
	struct RenderPixel { uint8_t r=0,g=0,b=0,a=0; };

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
		void sync_images(std::span<const ImageObject> images,std::span<const DrawCommand> visibleCommands,std::span<const ImageId> keepResident = {});
		void recreate_swapchain(const WaylandWindow& window);
		void render_clear_frame(float r, float g, float b, float a);
		void render_draw_list(const std::vector<DrawCommand>& commands);
		void render_draw_list(const std::vector<DrawCommand>& commands, const TextAtlas* textAtlas);
		[[nodiscard]] RendererStatistics statistics() const;
		// Diagnostic samples are read back into memory only; no image files.
		// Native surface samples: relative linear BT.2020, no clipping.
        [[nodiscard]] std::vector<std::array<float,4>> render_draw_list_linear_samples(
            const std::vector<DrawCommand>& commands,const TextAtlas* atlas,std::span<const Point> logicalPoints);
        [[nodiscard]] std::vector<RenderPixel> render_draw_list_samples(
			const std::vector<DrawCommand>& commands, const TextAtlas* atlas, std::span<const Point> logicalPoints);
		[[nodiscard]] std::vector<std::array<float,4>> read_image_samples(ImageId image,std::span<const Point> pixelPositions);
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
