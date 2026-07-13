#define VK_USE_PLATFORM_WAYLAND_KHR
#include "render_vulkan.hpp"
#include "raw_image_conversion.hpp"
#include "text_atlas.hpp"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace codec_gui::gui {
namespace {

void vk_checked(VkResult result, const char* operation) {
	if (result != VK_SUCCESS) {
		throw std::runtime_error(std::string(operation) + " failed");
	}
}

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
	for (const VkExtensionProperties& extension : extensions) {
		if (std::strcmp(extension.extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

std::vector<VkExtensionProperties> instance_extensions() {
	uint32_t count = 0;
	vk_checked(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "vkEnumerateInstanceExtensionProperties");
	std::vector<VkExtensionProperties> out(count);
	vk_checked(vkEnumerateInstanceExtensionProperties(nullptr, &count, out.data()), "vkEnumerateInstanceExtensionProperties");
	return out;
}

std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device) {
	uint32_t count = 0;
	vk_checked(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
	std::vector<VkExtensionProperties> out(count);
	vk_checked(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, out.data()), "vkEnumerateDeviceExtensionProperties");
	return out;
}

VkSurfaceKHR create_wayland_surface(VkInstance instance, const WaylandWindow& window) {
	if (!window.valid()) {
		throw std::runtime_error("cannot create Vulkan surface without a valid Wayland window");
	}
	VkWaylandSurfaceCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	info.display = window.display();
	info.surface = window.surface();
	VkSurfaceKHR out = VK_NULL_HANDLE;
	vk_checked(vkCreateWaylandSurfaceKHR(instance, &info, nullptr, &out), "vkCreateWaylandSurfaceKHR");
	return out;
}

std::vector<uint32_t> read_spirv(const char* path) {
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in) {
		throw std::runtime_error(std::string("failed to open shader: ") + path);
	}
	const std::streamsize size = in.tellg();
	if (size <= 0 || (size % 4) != 0) {
		throw std::runtime_error(std::string("invalid shader size: ") + path);
	}
	in.seekg(0, std::ios::beg);
	std::vector<uint32_t> words(static_cast<std::size_t>(size) / 4);
	in.read(reinterpret_cast<char*>(words.data()), size);
	if (!in) {
		throw std::runtime_error(std::string("failed to read shader: ") + path);
	}
	return words;
}

VkShaderModule create_shader_module(VkDevice device, const char* path) {
	const std::vector<uint32_t> code = read_spirv(path);
	VkShaderModuleCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = code.size() * sizeof(uint32_t);
	info.pCode = code.data();
	VkShaderModule module = VK_NULL_HANDLE;
	vk_checked(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
	return module;
}

VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
	for (const VkSurfaceFormatKHR& format : formats) {
		if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return format;
		}
	}
	for (const VkSurfaceFormatKHR& format : formats) {
		if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return format;
		}
	}
	if (formats.empty()) {
		throw std::runtime_error("surface has no supported formats");
	}
	return formats.front();
}

VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes) {
	for (VkPresentModeKHR mode : modes) {
		if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
			return mode;
		}
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& caps, const WaylandWindow& window) {
	if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
		return caps.currentExtent;
	}
	VkExtent2D extent{
		static_cast<uint32_t>(std::max(1, window.framebuffer_width())),
		static_cast<uint32_t>(std::max(1, window.framebuffer_height())),
	};
	extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
	extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
	return extent;
}

struct RectPushConstants {
	float rect[4];
	float color[4];
	float viewport[2];
};

struct TextPushConstants {
	float rect[4];
	float uv[4];
	float color[4];
	float viewport[2];
};

uint32_t find_memory_type(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties memoryProperties{};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
	for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
		if (((typeBits & (1u << i)) != 0) &&
		    (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}
	throw std::runtime_error("no compatible Vulkan memory type");
}

void create_buffer(
	VkDevice device,
	VkPhysicalDevice physicalDevice,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkMemoryPropertyFlags properties,
	VkBuffer& buffer,
	VkDeviceMemory& memory
) {
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	vk_checked(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer");

	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(device, buffer, &requirements);
	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = find_memory_type(physicalDevice, requirements.memoryTypeBits, properties);
	vk_checked(vkAllocateMemory(device, &allocInfo, nullptr, &memory), "vkAllocateMemory");
	vk_checked(vkBindBufferMemory(device, buffer, memory, 0), "vkBindBufferMemory");
}

void create_image(
	VkDevice device,
	VkPhysicalDevice physicalDevice,
	uint32_t width,
	uint32_t height,
	VkFormat format,
	VkImageUsageFlags usage,
	VkImage& image,
	VkDeviceMemory& memory
) {
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent = {width, height, 1};
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = format;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = usage;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	vk_checked(vkCreateImage(device, &imageInfo, nullptr, &image), "vkCreateImage");

	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(device, image, &requirements);
	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = find_memory_type(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	vk_checked(vkAllocateMemory(device, &allocInfo, nullptr, &memory), "vkAllocateMemory");
	vk_checked(vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory");
}

void image_barrier(
	VkCommandBuffer commandBuffer,
	VkImage image,
	VkImageLayout oldLayout,
	VkImageLayout newLayout,
	VkAccessFlags srcAccess,
	VkAccessFlags dstAccess,
	VkPipelineStageFlags srcStage,
	VkPipelineStageFlags dstStage
) {
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkRect2D clamp_scissor(Rect rect, VkExtent2D extent, float scale = 1.0f) {
	const float x0f = std::clamp(rect.x * scale, 0.0f, static_cast<float>(extent.width));
	const float y0f = std::clamp(rect.y * scale, 0.0f, static_cast<float>(extent.height));
	const float x1f = std::clamp((rect.x + std::max(0.0f, rect.w)) * scale, 0.0f, static_cast<float>(extent.width));
	const float y1f = std::clamp((rect.y + std::max(0.0f, rect.h)) * scale, 0.0f, static_cast<float>(extent.height));
	const int32_t x0 = static_cast<int32_t>(std::floor(x0f));
	const int32_t y0 = static_cast<int32_t>(std::floor(y0f));
	const int32_t x1 = static_cast<int32_t>(std::ceil(x1f));
	const int32_t y1 = static_cast<int32_t>(std::ceil(y1f));
	return VkRect2D{{x0, y0}, {static_cast<uint32_t>(std::max(0, x1 - x0)), static_cast<uint32_t>(std::max(0, y1 - y0))}};
}

Rect scaled_rect(Rect rect, float scale) {
	return Rect{rect.x * scale, rect.y * scale, rect.w * scale, rect.h * scale};
}

std::size_t swapchain_bytes_per_pixel(VkFormat format) {
	switch (format) {
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_R8G8B8A8_UNORM:
			return 4;
		default:
			throw std::runtime_error("pixel probe supports only 8-bit RGBA/BGRA swapchain formats");
	}
}

bool readback_has_non_background_pixels(
	const std::vector<uint8_t>& bytes,
	VkExtent2D extent,
	VkFormat format,
	Rect probeRect
) {
	const std::size_t bpp = swapchain_bytes_per_pixel(format);
	if (bytes.size() < static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height) * bpp) {
		return false;
	}
	const VkRect2D probe = clamp_scissor(probeRect, extent);
	const uint32_t xEnd = probe.offset.x < 0 ? 0u : static_cast<uint32_t>(probe.offset.x) + probe.extent.width;
	const uint32_t yEnd = probe.offset.y < 0 ? 0u : static_cast<uint32_t>(probe.offset.y) + probe.extent.height;
	for (uint32_t y = static_cast<uint32_t>(std::max(0, probe.offset.y)); y < yEnd; ++y) {
		for (uint32_t x = static_cast<uint32_t>(std::max(0, probe.offset.x)); x < xEnd; ++x) {
			const std::size_t offset = (static_cast<std::size_t>(y) * extent.width + x) * bpp;
			const uint8_t c0 = bytes[offset + 0];
			const uint8_t c1 = bytes[offset + 1];
			const uint8_t c2 = bytes[offset + 2];
			if (std::max({c0, c1, c2}) > 48 && (std::max({c0, c1, c2}) - std::min({c0, c1, c2}) > 8 || std::max({c0, c1, c2}) > 96)) {
				return true;
			}
		}
	}
	return false;
}

} // namespace

struct TransientTextResources {
	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory imageMemory = VK_NULL_HANDLE;
	VkImageView imageView = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

struct GpuImageResource {
	ImageId id;
	int width = 0;
	int height = 0;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

struct VulkanRenderer::Impl {
	VkInstance instance = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	std::string deviceName;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchainExtent{};
	VkExtent2D logicalExtent{};
	float outputScale = 1.0f;
	bool swapchainTransferSrc = false;
	std::vector<VkImage> swapchainImages;
	std::vector<VkImageView> swapchainImageViews;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> framebuffers;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkSemaphore imageAvailable = VK_NULL_HANDLE;
	std::vector<VkSemaphore> renderFinishedByImage;
	VkFence inFlight = VK_NULL_HANDLE;
	VkPipelineLayout rectPipelineLayout = VK_NULL_HANDLE;
	VkPipeline rectPipeline = VK_NULL_HANDLE;
	VkDescriptorSetLayout textDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	VkPipelineLayout textPipelineLayout = VK_NULL_HANDLE;
	VkPipeline textPipeline = VK_NULL_HANDLE;
	VkPipelineLayout imagePipelineLayout = VK_NULL_HANDLE;
	VkPipeline imagePipeline = VK_NULL_HANDLE;
	TransientTextResources text;
	std::vector<uint8_t> textAlpha;
	std::vector<GpuImageResource> images;
	std::optional<Rect> pendingProbeRect;
	bool lastProbeNonblank = false;
};

namespace {

void destroy_render_finished_semaphores(VkDevice device, std::vector<VkSemaphore>& semaphores) {
	for (VkSemaphore semaphore : semaphores) {
		if (semaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(device, semaphore, nullptr);
		}
	}
	semaphores.clear();
}

void create_render_finished_semaphores(VkDevice device, std::size_t count, std::vector<VkSemaphore>& semaphores) {
	destroy_render_finished_semaphores(device, semaphores);
	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphores.resize(count, VK_NULL_HANDLE);
	for (VkSemaphore& semaphore : semaphores) {
		vk_checked(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore");
	}
}

void destroy_transient_text(VkDevice device, VkDescriptorPool descriptorPool, TransientTextResources& text) {
	if (text.descriptorSet != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(device, descriptorPool, 1, &text.descriptorSet);
	}
	if (text.sampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, text.sampler, nullptr);
	}
	if (text.imageView != VK_NULL_HANDLE) {
		vkDestroyImageView(device, text.imageView, nullptr);
	}
	if (text.image != VK_NULL_HANDLE) {
		vkDestroyImage(device, text.image, nullptr);
	}
	if (text.imageMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, text.imageMemory, nullptr);
	}
	if (text.stagingBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, text.stagingBuffer, nullptr);
	}
	if (text.stagingMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, text.stagingMemory, nullptr);
	}
	text = {};
}

void destroy_gpu_image(VkDevice device, VkDescriptorPool descriptorPool, GpuImageResource& resource) {
	if (resource.descriptorSet != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(device, descriptorPool, 1, &resource.descriptorSet);
	}
	if (resource.sampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, resource.sampler, nullptr);
	}
	if (resource.view != VK_NULL_HANDLE) {
		vkDestroyImageView(device, resource.view, nullptr);
	}
	if (resource.image != VK_NULL_HANDLE) {
		vkDestroyImage(device, resource.image, nullptr);
	}
	if (resource.memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, resource.memory, nullptr);
	}
	resource = {};
}

GpuImageResource* find_gpu_image(std::vector<GpuImageResource>& resources, ImageId id) {
	const auto it = std::find_if(resources.begin(), resources.end(), [id](const GpuImageResource& resource) {
		return resource.id == id;
	});
	return it == resources.end() ? nullptr : &*it;
}

TransientTextResources create_transient_text(
	VkDevice device,
	VkPhysicalDevice physicalDevice,
	VkDescriptorPool descriptorPool,
	VkDescriptorSetLayout descriptorSetLayout,
	const TextAtlas& atlas
) {
	if (atlas.width <= 0 || atlas.height <= 0 || atlas.alpha.empty() || atlas.quads.empty()) {
		return {};
	}

	TransientTextResources text;
	const VkDeviceSize size = static_cast<VkDeviceSize>(atlas.alpha.size());
	create_buffer(
		device,
		physicalDevice,
		size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		text.stagingBuffer,
		text.stagingMemory
	);
	void* mapped = nullptr;
	vk_checked(vkMapMemory(device, text.stagingMemory, 0, size, 0, &mapped), "vkMapMemory");
	std::memcpy(mapped, atlas.alpha.data(), atlas.alpha.size());
	vkUnmapMemory(device, text.stagingMemory);

	create_image(
		device,
		physicalDevice,
		static_cast<uint32_t>(atlas.width),
		static_cast<uint32_t>(atlas.height),
		VK_FORMAT_R8_UNORM,
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		text.image,
		text.imageMemory
	);

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = text.image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = 1;
	vk_checked(vkCreateImageView(device, &viewInfo, nullptr, &text.imageView), "vkCreateImageView");

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxLod = 0.0f;
	vk_checked(vkCreateSampler(device, &samplerInfo, nullptr, &text.sampler), "vkCreateSampler");

	VkDescriptorSetAllocateInfo descriptorInfo{};
	descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorInfo.descriptorPool = descriptorPool;
	descriptorInfo.descriptorSetCount = 1;
	descriptorInfo.pSetLayouts = &descriptorSetLayout;
	vk_checked(vkAllocateDescriptorSets(device, &descriptorInfo, &text.descriptorSet), "vkAllocateDescriptorSets");

	VkDescriptorImageInfo imageInfo{};
	imageInfo.sampler = text.sampler;
	imageInfo.imageView = text.imageView;
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = text.descriptorSet;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageInfo;
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

	return text;
}

} // namespace

std::string vulkan_runtime_version_string() {
	uint32_t instanceVersion = 0;
	const VkResult versionResult = vkEnumerateInstanceVersion(&instanceVersion);
	if (versionResult != VK_SUCCESS) {
		return "unavailable (" + std::to_string(static_cast<int>(versionResult)) + ")";
	}
	std::ostringstream oss;
	oss << VK_VERSION_MAJOR(instanceVersion) << '.'
	    << VK_VERSION_MINOR(instanceVersion) << '.'
	    << VK_VERSION_PATCH(instanceVersion);
	return oss.str();
}

VulkanRenderer::VulkanRenderer(Impl* impl) : impl_(impl) {}

VulkanRenderer::VulkanRenderer(VulkanRenderer&& other) noexcept : impl_(std::exchange(other.impl_, nullptr)) {}

VulkanRenderer& VulkanRenderer::operator=(VulkanRenderer&& other) noexcept {
	if (this != &other) {
		reset();
		impl_ = std::exchange(other.impl_, nullptr);
	}
	return *this;
}

VulkanRenderer::~VulkanRenderer() {
	reset();
}

VulkanRenderer VulkanRenderer::create(const WaylandWindow& window) {
	Impl* impl = new Impl;
	try {
		const std::vector<VkExtensionProperties> extensions = instance_extensions();
		if (!has_extension(extensions, VK_KHR_SURFACE_EXTENSION_NAME) ||
		    !has_extension(extensions, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME)) {
			throw std::runtime_error("required Vulkan Wayland surface extensions are unavailable");
		}

		const char* instanceExts[] = {
			VK_KHR_SURFACE_EXTENSION_NAME,
			VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
		};
		VkApplicationInfo app{};
		app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app.pApplicationName = "codec_vis";
		app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
		app.pEngineName = "codec_vis";
		app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
		app.apiVersion = VK_API_VERSION_1_2;

		VkInstanceCreateInfo instanceInfo{};
		instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instanceInfo.pApplicationInfo = &app;
		instanceInfo.enabledExtensionCount = 2;
		instanceInfo.ppEnabledExtensionNames = instanceExts;
		vk_checked(vkCreateInstance(&instanceInfo, nullptr, &impl->instance), "vkCreateInstance");

		impl->surface = create_wayland_surface(impl->instance, window);

		uint32_t physicalCount = 0;
		vk_checked(vkEnumeratePhysicalDevices(impl->instance, &physicalCount, nullptr), "vkEnumeratePhysicalDevices");
		if (physicalCount == 0) {
			throw std::runtime_error("no Vulkan physical devices available");
		}
		std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
		vk_checked(vkEnumeratePhysicalDevices(impl->instance, &physicalCount, physicalDevices.data()), "vkEnumeratePhysicalDevices");

		for (VkPhysicalDevice candidate : physicalDevices) {
			if (!has_extension(device_extensions(candidate), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
				continue;
			}
			uint32_t familyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
			for (uint32_t i = 0; i < familyCount; ++i) {
				VkBool32 present = VK_FALSE;
				vk_checked(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, impl->surface, &present), "vkGetPhysicalDeviceSurfaceSupportKHR");
				if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present == VK_TRUE) {
					impl->physicalDevice = candidate;
					impl->queueFamily = i;
					break;
				}
			}
			if (impl->physicalDevice != VK_NULL_HANDLE) {
				break;
			}
		}
		if (impl->physicalDevice == VK_NULL_HANDLE) {
			throw std::runtime_error("no Vulkan queue family supports graphics and Wayland presentation");
		}

		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(impl->physicalDevice, &properties);
		impl->deviceName = properties.deviceName;

		const float priority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo{};
		queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfo.queueFamilyIndex = impl->queueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;

		const char* deviceExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
		VkDeviceCreateInfo deviceInfo{};
		deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = 1;
		deviceInfo.ppEnabledExtensionNames = deviceExts;
		vk_checked(vkCreateDevice(impl->physicalDevice, &deviceInfo, nullptr, &impl->device), "vkCreateDevice");
		vkGetDeviceQueue(impl->device, impl->queueFamily, 0, &impl->queue);

		VkSurfaceCapabilitiesKHR surfaceCaps{};
		vk_checked(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(impl->physicalDevice, impl->surface, &surfaceCaps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

		uint32_t formatCount = 0;
		vk_checked(vkGetPhysicalDeviceSurfaceFormatsKHR(impl->physicalDevice, impl->surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
		std::vector<VkSurfaceFormatKHR> formats(formatCount);
		vk_checked(vkGetPhysicalDeviceSurfaceFormatsKHR(impl->physicalDevice, impl->surface, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
		const VkSurfaceFormatKHR surfaceFormat = choose_surface_format(formats);

		uint32_t presentModeCount = 0;
		vk_checked(vkGetPhysicalDeviceSurfacePresentModesKHR(impl->physicalDevice, impl->surface, &presentModeCount, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR");
		std::vector<VkPresentModeKHR> presentModes(presentModeCount);
		vk_checked(vkGetPhysicalDeviceSurfacePresentModesKHR(impl->physicalDevice, impl->surface, &presentModeCount, presentModes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");

		uint32_t imageCount = surfaceCaps.minImageCount + 1;
		if (surfaceCaps.maxImageCount > 0) {
			imageCount = std::min(imageCount, surfaceCaps.maxImageCount);
		}
		impl->swapchainFormat = surfaceFormat.format;
		impl->swapchainExtent = choose_extent(surfaceCaps, window);
		impl->logicalExtent = {static_cast<uint32_t>(std::max(1, window.width())), static_cast<uint32_t>(std::max(1, window.height()))};
		impl->outputScale = window.output_scale();
		impl->swapchainTransferSrc = (surfaceCaps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

		VkSwapchainCreateInfoKHR swapchainInfo{};
		swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		swapchainInfo.surface = impl->surface;
		swapchainInfo.minImageCount = imageCount;
		swapchainInfo.imageFormat = surfaceFormat.format;
		swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
		swapchainInfo.imageExtent = impl->swapchainExtent;
		swapchainInfo.imageArrayLayers = 1;
		swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (impl->swapchainTransferSrc) {
			swapchainInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		swapchainInfo.preTransform = surfaceCaps.currentTransform;
		swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		swapchainInfo.presentMode = choose_present_mode(presentModes);
		swapchainInfo.clipped = VK_TRUE;
		vk_checked(vkCreateSwapchainKHR(impl->device, &swapchainInfo, nullptr, &impl->swapchain), "vkCreateSwapchainKHR");

		uint32_t actualImageCount = 0;
		vk_checked(vkGetSwapchainImagesKHR(impl->device, impl->swapchain, &actualImageCount, nullptr), "vkGetSwapchainImagesKHR");
		impl->swapchainImages.resize(actualImageCount);
		vk_checked(vkGetSwapchainImagesKHR(impl->device, impl->swapchain, &actualImageCount, impl->swapchainImages.data()), "vkGetSwapchainImagesKHR");

		impl->swapchainImageViews.reserve(impl->swapchainImages.size());
		for (VkImage image : impl->swapchainImages) {
			VkImageViewCreateInfo viewInfo{};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = image;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = impl->swapchainFormat;
			viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
			viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
			viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
			viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
			viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			viewInfo.subresourceRange.baseMipLevel = 0;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.subresourceRange.baseArrayLayer = 0;
			viewInfo.subresourceRange.layerCount = 1;
			VkImageView view = VK_NULL_HANDLE;
			vk_checked(vkCreateImageView(impl->device, &viewInfo, nullptr, &view), "vkCreateImageView");
			impl->swapchainImageViews.push_back(view);
		}

		VkAttachmentDescription colorAttachment{};
		colorAttachment.format = impl->swapchainFormat;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorRef{};
		colorRef.attachment = 0;
		colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = 1;
		renderPassInfo.pAttachments = &colorAttachment;
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;
		vk_checked(vkCreateRenderPass(impl->device, &renderPassInfo, nullptr, &impl->renderPass), "vkCreateRenderPass");

		impl->framebuffers.reserve(impl->swapchainImageViews.size());
		for (VkImageView view : impl->swapchainImageViews) {
			VkFramebufferCreateInfo framebufferInfo{};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.renderPass = impl->renderPass;
			framebufferInfo.attachmentCount = 1;
			framebufferInfo.pAttachments = &view;
			framebufferInfo.width = impl->swapchainExtent.width;
			framebufferInfo.height = impl->swapchainExtent.height;
			framebufferInfo.layers = 1;
			VkFramebuffer framebuffer = VK_NULL_HANDLE;
			vk_checked(vkCreateFramebuffer(impl->device, &framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer");
			impl->framebuffers.push_back(framebuffer);
		}

		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = impl->queueFamily;
		vk_checked(vkCreateCommandPool(impl->device, &poolInfo, nullptr, &impl->commandPool), "vkCreateCommandPool");

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = impl->commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		vk_checked(vkAllocateCommandBuffers(impl->device, &allocInfo, &impl->commandBuffer), "vkAllocateCommandBuffers");

		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		vk_checked(vkCreateSemaphore(impl->device, &semaphoreInfo, nullptr, &impl->imageAvailable), "vkCreateSemaphore");
		create_render_finished_semaphores(impl->device, impl->swapchainImages.size(), impl->renderFinishedByImage);

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		vk_checked(vkCreateFence(impl->device, &fenceInfo, nullptr, &impl->inFlight), "vkCreateFence");

		VkPushConstantRange pushRange{};
		pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		pushRange.offset = 0;
		pushRange.size = sizeof(RectPushConstants);
		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.pushConstantRangeCount = 1;
		pipelineLayoutInfo.pPushConstantRanges = &pushRange;
		vk_checked(vkCreatePipelineLayout(impl->device, &pipelineLayoutInfo, nullptr, &impl->rectPipelineLayout), "vkCreatePipelineLayout");

		VkShaderModule vert = create_shader_module(impl->device, "build/rect.vert.spv");
		VkShaderModule frag = create_shader_module(impl->device, "build/rect.frag.spv");
		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vert;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = frag;
		stages[1].pName = "main";

		VkPipelineVertexInputStateCreateInfo vertexInput{};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo rasterizer{};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.cullMode = VK_CULL_MODE_NONE;
		rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		rasterizer.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState colorBlendAttachment{};
		colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachment.blendEnable = VK_TRUE;
		colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
		VkPipelineColorBlendStateCreateInfo colorBlend{};
		colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlend.attachmentCount = 1;
		colorBlend.pAttachments = &colorBlendAttachment;
		VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = 2;
		dynamicState.pDynamicStates = dynamicStates;

		VkGraphicsPipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = stages;
		pipelineInfo.pVertexInputState = &vertexInput;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisample;
		pipelineInfo.pColorBlendState = &colorBlend;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.layout = impl->rectPipelineLayout;
		pipelineInfo.renderPass = impl->renderPass;
		pipelineInfo.subpass = 0;
		vk_checked(vkCreateGraphicsPipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &impl->rectPipeline), "vkCreateGraphicsPipelines");
		vkDestroyShaderModule(impl->device, frag, nullptr);
		vkDestroyShaderModule(impl->device, vert, nullptr);

		VkDescriptorSetLayoutBinding atlasBinding{};
		atlasBinding.binding = 0;
		atlasBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		atlasBinding.descriptorCount = 1;
		atlasBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo textSetLayoutInfo{};
		textSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		textSetLayoutInfo.bindingCount = 1;
		textSetLayoutInfo.pBindings = &atlasBinding;
		vk_checked(vkCreateDescriptorSetLayout(impl->device, &textSetLayoutInfo, nullptr, &impl->textDescriptorSetLayout), "vkCreateDescriptorSetLayout");

		VkDescriptorPoolSize poolSize{};
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSize.descriptorCount = 64;
		VkDescriptorPoolCreateInfo descriptorPoolInfo{};
		descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		descriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		descriptorPoolInfo.maxSets = 64;
		descriptorPoolInfo.poolSizeCount = 1;
		descriptorPoolInfo.pPoolSizes = &poolSize;
		vk_checked(vkCreateDescriptorPool(impl->device, &descriptorPoolInfo, nullptr, &impl->descriptorPool), "vkCreateDescriptorPool");

		VkPushConstantRange textPushRange{};
		textPushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		textPushRange.offset = 0;
		textPushRange.size = sizeof(TextPushConstants);
		VkPipelineLayoutCreateInfo textPipelineLayoutInfo{};
		textPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		textPipelineLayoutInfo.setLayoutCount = 1;
		textPipelineLayoutInfo.pSetLayouts = &impl->textDescriptorSetLayout;
		textPipelineLayoutInfo.pushConstantRangeCount = 1;
		textPipelineLayoutInfo.pPushConstantRanges = &textPushRange;
		vk_checked(vkCreatePipelineLayout(impl->device, &textPipelineLayoutInfo, nullptr, &impl->textPipelineLayout), "vkCreatePipelineLayout");

		VkShaderModule textVert = create_shader_module(impl->device, "build/text.vert.spv");
		VkShaderModule textFrag = create_shader_module(impl->device, "build/text.frag.spv");
		VkPipelineShaderStageCreateInfo textStages[2]{};
		textStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		textStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		textStages[0].module = textVert;
		textStages[0].pName = "main";
		textStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		textStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		textStages[1].module = textFrag;
		textStages[1].pName = "main";
		pipelineInfo.pStages = textStages;
		pipelineInfo.layout = impl->textPipelineLayout;
		vk_checked(vkCreateGraphicsPipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &impl->textPipeline), "vkCreateGraphicsPipelines");
		vkDestroyShaderModule(impl->device, textFrag, nullptr);
		vkDestroyShaderModule(impl->device, textVert, nullptr);

		VkPipelineLayoutCreateInfo imagePipelineLayoutInfo{};
		imagePipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		imagePipelineLayoutInfo.setLayoutCount = 1;
		imagePipelineLayoutInfo.pSetLayouts = &impl->textDescriptorSetLayout;
		imagePipelineLayoutInfo.pushConstantRangeCount = 1;
		imagePipelineLayoutInfo.pPushConstantRanges = &textPushRange;
		vk_checked(vkCreatePipelineLayout(impl->device, &imagePipelineLayoutInfo, nullptr, &impl->imagePipelineLayout), "vkCreatePipelineLayout");

		VkShaderModule imageVert = create_shader_module(impl->device, "build/text.vert.spv");
		VkShaderModule imageFrag = create_shader_module(impl->device, "build/image.frag.spv");
		VkPipelineShaderStageCreateInfo imageStages[2]{};
		imageStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		imageStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		imageStages[0].module = imageVert;
		imageStages[0].pName = "main";
		imageStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		imageStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		imageStages[1].module = imageFrag;
		imageStages[1].pName = "main";
		pipelineInfo.pStages = imageStages;
		pipelineInfo.layout = impl->imagePipelineLayout;
		vk_checked(vkCreateGraphicsPipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &impl->imagePipeline), "vkCreateGraphicsPipelines");
		vkDestroyShaderModule(impl->device, imageFrag, nullptr);
		vkDestroyShaderModule(impl->device, imageVert, nullptr);
	} catch (...) {
		VulkanRenderer cleanup(impl);
		throw;
	}
	return VulkanRenderer(impl);
}

bool VulkanRenderer::valid() const {
	return impl_ != nullptr && impl_->instance != VK_NULL_HANDLE && impl_->device != VK_NULL_HANDLE;
}

std::string VulkanRenderer::device_name() const {
	return impl_ == nullptr ? "" : impl_->deviceName;
}

uint32_t VulkanRenderer::queue_family() const {
	return impl_ == nullptr ? 0 : impl_->queueFamily;
}

uint32_t VulkanRenderer::width() const {
	return impl_ == nullptr ? 0 : impl_->swapchainExtent.width;
}

uint32_t VulkanRenderer::height() const {
	return impl_ == nullptr ? 0 : impl_->swapchainExtent.height;
}

void VulkanRenderer::recreate_swapchain(const WaylandWindow& window) {
	if (!valid()) {
		throw std::runtime_error("renderer is not initialized");
	}
	vk_checked(vkWaitForFences(impl_->device, 1, &impl_->inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
	vk_checked(vkDeviceWaitIdle(impl_->device), "vkDeviceWaitIdle");

	for (VkFramebuffer framebuffer : impl_->framebuffers) {
		vkDestroyFramebuffer(impl_->device, framebuffer, nullptr);
	}
	impl_->framebuffers.clear();
	destroy_render_finished_semaphores(impl_->device, impl_->renderFinishedByImage);
	for (VkImageView view : impl_->swapchainImageViews) {
		vkDestroyImageView(impl_->device, view, nullptr);
	}
	impl_->swapchainImageViews.clear();
	impl_->swapchainImages.clear();

	VkSurfaceCapabilitiesKHR surfaceCaps{};
	vk_checked(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(impl_->physicalDevice, impl_->surface, &surfaceCaps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
	uint32_t formatCount = 0;
	vk_checked(vkGetPhysicalDeviceSurfaceFormatsKHR(impl_->physicalDevice, impl_->surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
	std::vector<VkSurfaceFormatKHR> formats(formatCount);
	vk_checked(vkGetPhysicalDeviceSurfaceFormatsKHR(impl_->physicalDevice, impl_->surface, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
	const VkSurfaceFormatKHR surfaceFormat = choose_surface_format(formats);
	if (surfaceFormat.format != impl_->swapchainFormat) {
		throw std::runtime_error("swapchain format changed; full renderer rebuild is not implemented yet");
	}

	uint32_t presentModeCount = 0;
	vk_checked(vkGetPhysicalDeviceSurfacePresentModesKHR(impl_->physicalDevice, impl_->surface, &presentModeCount, nullptr), "vkGetPhysicalDeviceSurfacePresentModesKHR");
	std::vector<VkPresentModeKHR> presentModes(presentModeCount);
	vk_checked(vkGetPhysicalDeviceSurfacePresentModesKHR(impl_->physicalDevice, impl_->surface, &presentModeCount, presentModes.data()), "vkGetPhysicalDeviceSurfacePresentModesKHR");

	uint32_t imageCount = surfaceCaps.minImageCount + 1;
	if (surfaceCaps.maxImageCount > 0) {
		imageCount = std::min(imageCount, surfaceCaps.maxImageCount);
	}
	impl_->swapchainExtent = choose_extent(surfaceCaps, window);
	impl_->logicalExtent = {static_cast<uint32_t>(std::max(1, window.width())), static_cast<uint32_t>(std::max(1, window.height()))};
	impl_->outputScale = window.output_scale();
	impl_->swapchainTransferSrc = (surfaceCaps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
	VkSwapchainKHR oldSwapchain = impl_->swapchain;
	VkSwapchainCreateInfoKHR swapchainInfo{};
	swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchainInfo.surface = impl_->surface;
	swapchainInfo.minImageCount = imageCount;
	swapchainInfo.imageFormat = surfaceFormat.format;
	swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
	swapchainInfo.imageExtent = impl_->swapchainExtent;
	swapchainInfo.imageArrayLayers = 1;
	swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (impl_->swapchainTransferSrc) {
		swapchainInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}
	swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchainInfo.preTransform = surfaceCaps.currentTransform;
	swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchainInfo.presentMode = choose_present_mode(presentModes);
	swapchainInfo.clipped = VK_TRUE;
	swapchainInfo.oldSwapchain = oldSwapchain;
	vk_checked(vkCreateSwapchainKHR(impl_->device, &swapchainInfo, nullptr, &impl_->swapchain), "vkCreateSwapchainKHR");
	if (oldSwapchain != VK_NULL_HANDLE) {
		vkDestroySwapchainKHR(impl_->device, oldSwapchain, nullptr);
	}

	uint32_t actualImageCount = 0;
	vk_checked(vkGetSwapchainImagesKHR(impl_->device, impl_->swapchain, &actualImageCount, nullptr), "vkGetSwapchainImagesKHR");
	impl_->swapchainImages.resize(actualImageCount);
	vk_checked(vkGetSwapchainImagesKHR(impl_->device, impl_->swapchain, &actualImageCount, impl_->swapchainImages.data()), "vkGetSwapchainImagesKHR");

	impl_->swapchainImageViews.reserve(impl_->swapchainImages.size());
	for (VkImage image : impl_->swapchainImages) {
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = impl_->swapchainFormat;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;
		VkImageView view = VK_NULL_HANDLE;
		vk_checked(vkCreateImageView(impl_->device, &viewInfo, nullptr, &view), "vkCreateImageView");
		impl_->swapchainImageViews.push_back(view);
	}

	impl_->framebuffers.reserve(impl_->swapchainImageViews.size());
	for (VkImageView view : impl_->swapchainImageViews) {
		VkFramebufferCreateInfo framebufferInfo{};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = impl_->renderPass;
		framebufferInfo.attachmentCount = 1;
		framebufferInfo.pAttachments = &view;
		framebufferInfo.width = impl_->swapchainExtent.width;
		framebufferInfo.height = impl_->swapchainExtent.height;
		framebufferInfo.layers = 1;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		vk_checked(vkCreateFramebuffer(impl_->device, &framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer");
		impl_->framebuffers.push_back(framebuffer);
	}
	create_render_finished_semaphores(impl_->device, impl_->swapchainImages.size(), impl_->renderFinishedByImage);
}

void VulkanRenderer::sync_images(std::span<const ImageObject> images) {
	if (!valid()) {
		throw std::runtime_error("renderer is not initialized");
	}

	impl_->images.erase(
		std::remove_if(impl_->images.begin(), impl_->images.end(), [&](GpuImageResource& resource) {
			const bool keep = std::any_of(images.begin(), images.end(), [&](const ImageObject& image) {
				return image.id == resource.id && image.decoded != nullptr;
			});
			if (!keep) {
				destroy_gpu_image(impl_->device, impl_->descriptorPool, resource);
			}
			return !keep;
		}),
		impl_->images.end()
	);

	for (const ImageObject& imageObject : images) {
		if (imageObject.decoded == nullptr || find_gpu_image(impl_->images, imageObject.id) != nullptr) {
			continue;
		}
		const RawImage& raw = *imageObject.decoded;
		const std::vector<uint8_t> rgba = raw_image_to_rgba8(raw);
		if (rgba.empty()) {
			continue;
		}

		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
		const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(rgba.size());
		create_buffer(
			impl_->device,
			impl_->physicalDevice,
			uploadSize,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			stagingBuffer,
			stagingMemory
		);
		void* mapped = nullptr;
		vk_checked(vkMapMemory(impl_->device, stagingMemory, 0, uploadSize, 0, &mapped), "vkMapMemory");
		std::memcpy(mapped, rgba.data(), rgba.size());
		vkUnmapMemory(impl_->device, stagingMemory);

		GpuImageResource resource;
		resource.id = imageObject.id;
		resource.width = raw.width;
		resource.height = raw.height;
		create_image(
			impl_->device,
			impl_->physicalDevice,
			static_cast<uint32_t>(raw.width),
			static_cast<uint32_t>(raw.height),
			VK_FORMAT_R8G8B8A8_SRGB,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			resource.image,
			resource.memory
		);

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = resource.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;
		vk_checked(vkCreateImageView(impl_->device, &viewInfo, nullptr, &resource.view), "vkCreateImageView");

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_NEAREST;
		samplerInfo.minFilter = VK_FILTER_NEAREST;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.maxLod = 0.0f;
		vk_checked(vkCreateSampler(impl_->device, &samplerInfo, nullptr, &resource.sampler), "vkCreateSampler");

		VkDescriptorSetAllocateInfo descriptorInfo{};
		descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptorInfo.descriptorPool = impl_->descriptorPool;
		descriptorInfo.descriptorSetCount = 1;
		descriptorInfo.pSetLayouts = &impl_->textDescriptorSetLayout;
		vk_checked(vkAllocateDescriptorSets(impl_->device, &descriptorInfo, &resource.descriptorSet), "vkAllocateDescriptorSets");

		VkDescriptorImageInfo imageInfo{};
		imageInfo.sampler = resource.sampler;
		imageInfo.imageView = resource.view;
		imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = resource.descriptorSet;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imageInfo;
		vkUpdateDescriptorSets(impl_->device, 1, &write, 0, nullptr);

		vk_checked(vkWaitForFences(impl_->device, 1, &impl_->inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
		vk_checked(vkResetCommandBuffer(impl_->commandBuffer, 0), "vkResetCommandBuffer");
		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vk_checked(vkBeginCommandBuffer(impl_->commandBuffer, &beginInfo), "vkBeginCommandBuffer");
		image_barrier(
			impl_->commandBuffer,
			resource.image,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT
		);
		VkBufferImageCopy copy{};
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = 0;
		copy.imageSubresource.baseArrayLayer = 0;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent = {static_cast<uint32_t>(raw.width), static_cast<uint32_t>(raw.height), 1};
		vkCmdCopyBufferToImage(impl_->commandBuffer, stagingBuffer, resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		image_barrier(
			impl_->commandBuffer,
			resource.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
		);
		vk_checked(vkEndCommandBuffer(impl_->commandBuffer), "vkEndCommandBuffer");
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &impl_->commandBuffer;
		vk_checked(vkResetFences(impl_->device, 1, &impl_->inFlight), "vkResetFences");
		vk_checked(vkQueueSubmit(impl_->queue, 1, &submitInfo, impl_->inFlight), "vkQueueSubmit");
		vk_checked(vkWaitForFences(impl_->device, 1, &impl_->inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");

		vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
		vkFreeMemory(impl_->device, stagingMemory, nullptr);
		impl_->images.push_back(resource);
	}
}

void VulkanRenderer::render_clear_frame(float r, float g, float b, float a) {
	if (!valid()) {
		throw std::runtime_error("renderer is not initialized");
	}
	vk_checked(vkWaitForFences(impl_->device, 1, &impl_->inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
	vk_checked(vkResetFences(impl_->device, 1, &impl_->inFlight), "vkResetFences");

	uint32_t imageIndex = 0;
	const VkResult acquire = vkAcquireNextImageKHR(impl_->device, impl_->swapchain, UINT64_MAX, impl_->imageAvailable, VK_NULL_HANDLE, &imageIndex);
	if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("vkAcquireNextImageKHR failed");
	}
	VkSemaphore renderFinished = impl_->renderFinishedByImage.at(imageIndex);

	vk_checked(vkResetCommandBuffer(impl_->commandBuffer, 0), "vkResetCommandBuffer");
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vk_checked(vkBeginCommandBuffer(impl_->commandBuffer, &beginInfo), "vkBeginCommandBuffer");

	VkClearValue clear{};
	clear.color = VkClearColorValue{{r, g, b, a}};
	VkRenderPassBeginInfo passInfo{};
	passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	passInfo.renderPass = impl_->renderPass;
	passInfo.framebuffer = impl_->framebuffers.at(imageIndex);
	passInfo.renderArea.offset = {0, 0};
	passInfo.renderArea.extent = impl_->swapchainExtent;
	passInfo.clearValueCount = 1;
	passInfo.pClearValues = &clear;
	vkCmdBeginRenderPass(impl_->commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(impl_->commandBuffer);
	vk_checked(vkEndCommandBuffer(impl_->commandBuffer), "vkEndCommandBuffer");

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &impl_->imageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &impl_->commandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &renderFinished;
	vk_checked(vkQueueSubmit(impl_->queue, 1, &submitInfo, impl_->inFlight), "vkQueueSubmit");

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &renderFinished;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &impl_->swapchain;
	presentInfo.pImageIndices = &imageIndex;
	const VkResult present = vkQueuePresentKHR(impl_->queue, &presentInfo);
	if (present != VK_SUCCESS && present != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("vkQueuePresentKHR failed");
	}
}

void VulkanRenderer::render_draw_list(const std::vector<DrawCommand>& commands) {
	render_draw_list(commands, nullptr);
}

void VulkanRenderer::render_draw_list(const std::vector<DrawCommand>& commands, const TextAtlas* textAtlas) {
	if (!valid()) {
		throw std::runtime_error("renderer is not initialized");
	}
	vk_checked(vkWaitForFences(impl_->device, 1, &impl_->inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
	vk_checked(vkResetFences(impl_->device, 1, &impl_->inFlight), "vkResetFences");

	uint32_t imageIndex = 0;
	const VkResult acquire = vkAcquireNextImageKHR(impl_->device, impl_->swapchain, UINT64_MAX, impl_->imageAvailable, VK_NULL_HANDLE, &imageIndex);
	if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("vkAcquireNextImageKHR failed");
	}
	VkSemaphore renderFinished = impl_->renderFinishedByImage.at(imageIndex);

	bool uploadText = false;
	if (textAtlas != nullptr && impl_->textPipeline != VK_NULL_HANDLE) {
		if (impl_->text.image == VK_NULL_HANDLE || impl_->textAlpha != textAtlas->alpha) {
			destroy_transient_text(impl_->device, impl_->descriptorPool, impl_->text);
			impl_->text = create_transient_text(
				impl_->device,
				impl_->physicalDevice,
				impl_->descriptorPool,
				impl_->textDescriptorSetLayout,
				*textAtlas
			);
			impl_->textAlpha = textAtlas->alpha;
			uploadText = impl_->text.image != VK_NULL_HANDLE;
		}
	}
	TransientTextResources& text = impl_->text;
	const std::optional<Rect> probeRect = impl_->pendingProbeRect;
	impl_->pendingProbeRect.reset();
	impl_->lastProbeNonblank = false;
	VkBuffer readbackBuffer = VK_NULL_HANDLE;
	VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
	VkDeviceSize readbackSize = 0;
	if (probeRect) {
		if (!impl_->swapchainTransferSrc) {
			throw std::runtime_error("swapchain image transfer source usage is unavailable for pixel probe");
		}
		readbackSize = static_cast<VkDeviceSize>(impl_->swapchainExtent.width) *
		               static_cast<VkDeviceSize>(impl_->swapchainExtent.height) *
		               static_cast<VkDeviceSize>(swapchain_bytes_per_pixel(impl_->swapchainFormat));
		create_buffer(
			impl_->device,
			impl_->physicalDevice,
			readbackSize,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			readbackBuffer,
			readbackMemory
		);
	}

	vk_checked(vkResetCommandBuffer(impl_->commandBuffer, 0), "vkResetCommandBuffer");
	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vk_checked(vkBeginCommandBuffer(impl_->commandBuffer, &beginInfo), "vkBeginCommandBuffer");

	if (text.image != VK_NULL_HANDLE && uploadText) {
		image_barrier(
			impl_->commandBuffer,
			text.image,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT
		);
		VkBufferImageCopy copy{};
		copy.bufferOffset = 0;
		copy.bufferRowLength = 0;
		copy.bufferImageHeight = 0;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = 0;
		copy.imageSubresource.baseArrayLayer = 0;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent = {
			static_cast<uint32_t>(textAtlas->width),
			static_cast<uint32_t>(textAtlas->height),
			1
		};
		vkCmdCopyBufferToImage(impl_->commandBuffer, text.stagingBuffer, text.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		image_barrier(
			impl_->commandBuffer,
			text.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
		);
	}

	VkClearValue clear{};
	clear.color = VkClearColorValue{{0.015f, 0.017f, 0.02f, 1.0f}};
	VkRenderPassBeginInfo passInfo{};
	passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	passInfo.renderPass = impl_->renderPass;
	passInfo.framebuffer = impl_->framebuffers.at(imageIndex);
	passInfo.renderArea.offset = {0, 0};
	passInfo.renderArea.extent = impl_->swapchainExtent;
	passInfo.clearValueCount = 1;
	passInfo.pClearValues = &clear;
	vkCmdBeginRenderPass(impl_->commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport{};
	viewport.x = 0;
	viewport.y = 0;
	viewport.width = static_cast<float>(impl_->swapchainExtent.width);
	viewport.height = static_cast<float>(impl_->swapchainExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(impl_->commandBuffer, 0, 1, &viewport);
	VkRect2D fullScissor{{0, 0}, impl_->swapchainExtent};
	vkCmdSetScissor(impl_->commandBuffer, 0, 1, &fullScissor);
	vkCmdBindPipeline(impl_->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->rectPipeline);

	std::vector<VkRect2D> scissorStack{fullScissor};
	auto draw_rect = [&](Rect rect, Color color) {
		if (rect.w <= 0.0f || rect.h <= 0.0f || color.a <= 0.0f) {
			return;
		}
		RectPushConstants push{
			{rect.x, rect.y, rect.w, rect.h},
			{color.r, color.g, color.b, color.a},
			{static_cast<float>(impl_->logicalExtent.width), static_cast<float>(impl_->logicalExtent.height)}
		};
		vkCmdPushConstants(impl_->commandBuffer, impl_->rectPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(impl_->commandBuffer, 6, 1, 0, 0);
	};
	auto draw_image = [&](const DrawCommand& command) {
		GpuImageResource* image = find_gpu_image(impl_->images, command.image);
		if (image == nullptr || image->descriptorSet == VK_NULL_HANDLE) {
			draw_rect(command.rect, Color{0.035f, 0.04f, 0.046f, 1.0f});
			return;
		}
		vkCmdBindPipeline(impl_->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->imagePipeline);
		vkCmdBindDescriptorSets(impl_->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->imagePipelineLayout, 0, 1, &image->descriptorSet, 0, nullptr);
		TextPushConstants push{
			{command.rect.x, command.rect.y, command.rect.w, command.rect.h},
			{command.uvRect.x, command.uvRect.y, command.uvRect.w, command.uvRect.h},
			{command.color.r, command.color.g, command.color.b, command.opacity},
			{static_cast<float>(impl_->logicalExtent.width), static_cast<float>(impl_->logicalExtent.height)}
		};
		vkCmdPushConstants(impl_->commandBuffer, impl_->imagePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(impl_->commandBuffer, 6, 1, 0, 0);
		vkCmdBindPipeline(impl_->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->rectPipeline);
	};
	auto draw_geometry_layer = [&](int layer) {
		scissorStack.assign(1, fullScissor);
		vkCmdSetScissor(impl_->commandBuffer, 0, 1, &fullScissor);
		vkCmdBindPipeline(impl_->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->rectPipeline);
		for (const DrawCommand& command : commands) {
			if (command.layer != layer) continue;
			switch (command.kind) {
			case DrawCommandKind::Rect:
				draw_rect(command.rect, command.color);
				break;
			case DrawCommandKind::Border: {
				const float t = 1.0f;
				draw_rect(Rect{command.rect.x, command.rect.y, command.rect.w, t}, command.color);
				draw_rect(Rect{command.rect.x, command.rect.y + command.rect.h - t, command.rect.w, t}, command.color);
				draw_rect(Rect{command.rect.x, command.rect.y, t, command.rect.h}, command.color);
				draw_rect(Rect{command.rect.x + command.rect.w - t, command.rect.y, t, command.rect.h}, command.color);
				break;
			}
			case DrawCommandKind::Image:
				draw_image(command);
				break;
			case DrawCommandKind::ScissorBegin: {
				VkRect2D scissor = clamp_scissor(command.rect, impl_->swapchainExtent, impl_->outputScale);
				scissorStack.push_back(scissor);
				vkCmdSetScissor(impl_->commandBuffer, 0, 1, &scissor);
				break;
			}
			case DrawCommandKind::ScissorEnd:
				if (scissorStack.size() > 1) {
					scissorStack.pop_back();
				}
				vkCmdSetScissor(impl_->commandBuffer, 0, 1, &scissorStack.back());
				break;
			case DrawCommandKind::Text:
				break;
			}
		}
	};
	auto draw_text_layer = [&](int layer) {
		if (text.image == VK_NULL_HANDLE || textAtlas == nullptr) return;
		vkCmdBindPipeline(impl_->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->textPipeline);
		vkCmdBindDescriptorSets(impl_->commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->textPipelineLayout, 0, 1, &text.descriptorSet, 0, nullptr);
		for (const GlyphQuad& quad : textAtlas->quads) {
			if (quad.layer != layer || quad.rect.w <= 0.0f || quad.rect.h <= 0.0f || quad.color.a <= 0.0f) {
				continue;
			}
			const VkRect2D clip = clamp_scissor(quad.clip, impl_->swapchainExtent, impl_->outputScale);
			vkCmdSetScissor(impl_->commandBuffer, 0, 1, &clip);
			TextPushConstants push{
				{quad.rect.x, quad.rect.y, quad.rect.w, quad.rect.h},
				{quad.uv.x, quad.uv.y, quad.uv.w, quad.uv.h},
				{quad.color.r, quad.color.g, quad.color.b, quad.color.a},
				{static_cast<float>(impl_->logicalExtent.width), static_cast<float>(impl_->logicalExtent.height)}
			};
			vkCmdPushConstants(impl_->commandBuffer, impl_->textPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			vkCmdDraw(impl_->commandBuffer, 6, 1, 0, 0);
		}
	};
	draw_geometry_layer(0);
	draw_text_layer(0);
	draw_geometry_layer(1);
	draw_text_layer(1);

	vkCmdEndRenderPass(impl_->commandBuffer);
	if (probeRect) {
		image_barrier(
			impl_->commandBuffer,
			impl_->swapchainImages.at(imageIndex),
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			VK_ACCESS_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT
		);
		VkBufferImageCopy copy{};
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = 0;
		copy.imageSubresource.baseArrayLayer = 0;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent = {impl_->swapchainExtent.width, impl_->swapchainExtent.height, 1};
		vkCmdCopyImageToBuffer(
			impl_->commandBuffer,
			impl_->swapchainImages.at(imageIndex),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			readbackBuffer,
			1,
			&copy
		);
		image_barrier(
			impl_->commandBuffer,
			impl_->swapchainImages.at(imageIndex),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_ACCESS_TRANSFER_READ_BIT,
			0,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
		);
	}
	vk_checked(vkEndCommandBuffer(impl_->commandBuffer), "vkEndCommandBuffer");
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &impl_->imageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &impl_->commandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &renderFinished;
	vk_checked(vkQueueSubmit(impl_->queue, 1, &submitInfo, impl_->inFlight), "vkQueueSubmit");

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &renderFinished;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &impl_->swapchain;
	presentInfo.pImageIndices = &imageIndex;
	const VkResult present = vkQueuePresentKHR(impl_->queue, &presentInfo);
	if (present != VK_SUCCESS && present != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("vkQueuePresentKHR failed");
	}
	vk_checked(vkWaitForFences(impl_->device, 1, &impl_->inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
	if (probeRect) {
		std::vector<uint8_t> pixels(static_cast<std::size_t>(readbackSize));
		void* mapped = nullptr;
		vk_checked(vkMapMemory(impl_->device, readbackMemory, 0, readbackSize, 0, &mapped), "vkMapMemory");
		std::memcpy(pixels.data(), mapped, pixels.size());
		vkUnmapMemory(impl_->device, readbackMemory);
		impl_->lastProbeNonblank = readback_has_non_background_pixels(pixels, impl_->swapchainExtent, impl_->swapchainFormat, scaled_rect(*probeRect, impl_->outputScale));
		vkDestroyBuffer(impl_->device, readbackBuffer, nullptr);
		vkFreeMemory(impl_->device, readbackMemory, nullptr);
	}
}

bool VulkanRenderer::render_draw_list_pixel_probe(
	const std::vector<DrawCommand>& commands,
	const TextAtlas* textAtlas,
	Rect probeRect
) {
	if (!valid()) {
		throw std::runtime_error("renderer is not initialized");
	}
	impl_->pendingProbeRect = probeRect;
	render_draw_list(commands, textAtlas);
	return impl_->lastProbeNonblank;
}

void VulkanRenderer::reset() {
	if (impl_ == nullptr) {
		return;
	}
	if (impl_->device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(impl_->device);
		destroy_transient_text(impl_->device, impl_->descriptorPool, impl_->text);
		impl_->textAlpha.clear();
		for (GpuImageResource& image : impl_->images) {
			destroy_gpu_image(impl_->device, impl_->descriptorPool, image);
		}
		if (impl_->imagePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(impl_->device, impl_->imagePipeline, nullptr);
		}
		if (impl_->imagePipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(impl_->device, impl_->imagePipelineLayout, nullptr);
		}
		if (impl_->textPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(impl_->device, impl_->textPipeline, nullptr);
		}
		if (impl_->textPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(impl_->device, impl_->textPipelineLayout, nullptr);
		}
		if (impl_->descriptorPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(impl_->device, impl_->descriptorPool, nullptr);
		}
		if (impl_->textDescriptorSetLayout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(impl_->device, impl_->textDescriptorSetLayout, nullptr);
		}
		if (impl_->rectPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(impl_->device, impl_->rectPipeline, nullptr);
		}
		if (impl_->rectPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(impl_->device, impl_->rectPipelineLayout, nullptr);
		}
		if (impl_->inFlight != VK_NULL_HANDLE) {
			vkDestroyFence(impl_->device, impl_->inFlight, nullptr);
		}
		destroy_render_finished_semaphores(impl_->device, impl_->renderFinishedByImage);
		if (impl_->imageAvailable != VK_NULL_HANDLE) {
			vkDestroySemaphore(impl_->device, impl_->imageAvailable, nullptr);
		}
		if (impl_->commandPool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(impl_->device, impl_->commandPool, nullptr);
		}
		for (VkFramebuffer framebuffer : impl_->framebuffers) {
			vkDestroyFramebuffer(impl_->device, framebuffer, nullptr);
		}
		if (impl_->renderPass != VK_NULL_HANDLE) {
			vkDestroyRenderPass(impl_->device, impl_->renderPass, nullptr);
		}
		for (VkImageView view : impl_->swapchainImageViews) {
			vkDestroyImageView(impl_->device, view, nullptr);
		}
		if (impl_->swapchain != VK_NULL_HANDLE) {
			vkDestroySwapchainKHR(impl_->device, impl_->swapchain, nullptr);
		}
		vkDestroyDevice(impl_->device, nullptr);
	}
	if (impl_->surface != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
	}
	if (impl_->instance != VK_NULL_HANDLE) {
		vkDestroyInstance(impl_->instance, nullptr);
	}
	delete impl_;
	impl_ = nullptr;
}

} // namespace codec_gui::gui
