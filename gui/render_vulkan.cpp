#include "../codec_gui_x265.hpp"
#include "app_state.hpp"
#include "draw_list.hpp"
#include "wayland_window.hpp"
#include <cstdint>
#include <ios>
#include <span>
#include <vulkan/vulkan_core.h>
#define VK_USE_PLATFORM_WAYLAND_KHR
#include "render_vulkan.hpp"
#include "raw_image_conversion.hpp"
#include "raw_image_utils.hpp"
#include "text_atlas.hpp"
#include "../build/embedded_shaders.hpp"
#include "ui_render_frame.hpp"

#include <vulkan/vulkan_wayland.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>

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

std::vector<uint32_t> read_spirv(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in) {
		throw std::runtime_error("failed to open shader: " + path.string());
	}
	const std::streamsize size = in.tellg();
	if (size <= 0 || (size % 4) != 0) {
		throw std::runtime_error("invalid shader size: " + path.string());
	}
	in.seekg(0, std::ios::beg);
	std::vector<uint32_t> words(static_cast<std::size_t>(size) / 4);
	in.read(reinterpret_cast<char*>(words.data()), size);
	if (!in) {
		throw std::runtime_error("failed to read shader: " + path.string());
	}
	return words;
}

VkShaderModule create_shader_module(VkDevice device, const char* filename) {
    std::vector<uint32_t> overrideCode;
    std::span<const uint32_t> code;
    if (const char* assetDir = std::getenv("CODEC_VIS_ASSET_DIR"); assetDir && *assetDir) {
        overrideCode = read_spirv(std::filesystem::path(assetDir) / filename);
        code = overrideCode;
    } else if (std::string_view(filename) == "linearize.comp.spv") code = embedded::linearize;
    else if (std::string_view(filename) == "compose.comp.spv") code = embedded::compose;
    else throw std::invalid_argument("Unknown embedded shader");
	VkShaderModuleCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = code.size() * sizeof(uint32_t);
	info.pCode = code.data();
	VkShaderModule module = VK_NULL_HANDLE;
	vk_checked(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
	return module;
}

VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    // Wayland color management is owned by WaylandWindow. Pass-through prevents
    // WSI from replacing that description or applying a different interpretation.
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_R16G16B16A16_SFLOAT && f.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT)
            return f;
    throw std::runtime_error("Linear BT.2020 presentation requires an FP16 pass-through Wayland swapchain");
}

VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes) {
	for (VkPresentModeKHR const mode : modes) {
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
		.width=static_cast<uint32_t>(std::max(1, window.framebuffer_width())),
		.height=static_cast<uint32_t>(std::max(1, window.framebuffer_height())),
	};
	extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
	extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
	return extent;
}



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
	VkDeviceMemory& memory,
	uint32_t mipLevels = 1
) {
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent = {.width=width, .height=height, .depth=1};
	imageInfo.mipLevels = mipLevels;
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
	VkPipelineStageFlags dstStage,
	uint32_t baseMipLevel = 0,
	uint32_t levelCount = 1
) {
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = baseMipLevel;
	barrier.subresourceRange.levelCount = levelCount;
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
	const auto x0 = static_cast<int32_t>(std::floor(x0f));
	const auto y0 = static_cast<int32_t>(std::floor(y0f));
	const auto x1 = static_cast<int32_t>(std::ceil(x1f));
	const auto y1 = static_cast<int32_t>(std::ceil(y1f));
	return VkRect2D{.offset={.x=x0, .y=y0}, .extent={.width=static_cast<uint32_t>(std::max(0, x1 - x0)), .height=static_cast<uint32_t>(std::max(0, y1 - y0))}};
}

Rect scaled_rect(Rect rect, float scale) {
	return Rect{.x=rect.x * scale, .y=rect.y * scale, .w=rect.w * scale, .h=rect.h * scale};
}

std::size_t swapchain_bytes_per_pixel(VkFormat format) {
    if(format!=VK_FORMAT_R16G16B16A16_SFLOAT)throw std::runtime_error("Expected FP16 presentation");
    return 8;
}
float decode_half(uint16_t value){
    const int exponent=(value>>10)&31;
    const float magnitude=exponent==0?std::ldexp(float(value&1023),-24):
        exponent==31?std::numeric_limits<float>::infinity():std::ldexp(float(1024+(value&1023)),exponent-25);
    return value&0x8000?-magnitude:magnitude;
}
std::array<float,4> read_linear_pixel(const std::vector<uint8_t>& bytes,std::size_t pixel){
    std::array<uint16_t,4> half{};std::memcpy(half.data(),bytes.data()+pixel*8,8);
    return {decode_half(half[0]),decode_half(half[1]),decode_half(half[2]),decode_half(half[3])};
}
bool readback_has_non_background_pixels(const std::vector<uint8_t>& bytes,VkExtent2D extent,VkFormat format,Rect rect){
    if(bytes.size()<std::size_t(extent.width)*extent.height*swapchain_bytes_per_pixel(format))return false;
    const auto clip=clamp_scissor(rect,extent);
    for(uint32_t y=clip.offset.y;y<uint32_t(clip.offset.y)+clip.extent.height;++y)
        for(uint32_t x=clip.offset.x;x<uint32_t(clip.offset.x)+clip.extent.width;++x){
            auto p=read_linear_pixel(bytes,std::size_t(y)*extent.width+x);
            if(std::max({p[0],p[1],p[2]})>.15f)return true;
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

struct LinearizeParameters {
    std::array<uint32_t,4> geometry{},planes{},strides{};
    std::array<float,4> chroma{},luma{},row0{},row1{},row2{};
};
static_assert(sizeof(LinearizeParameters)==128);
LinearizeParameters linearize_parameters(const RawImage& raw) {
    if(raw.width<=0 || raw.height<=0)throw std::invalid_argument("invalid preview dimensions");
    for(int i=0;i<plane_count(raw.format);++i)if(!plane_available(raw,i))throw std::invalid_argument("incomplete preview plane");
    switch(raw.color.transfer) {
        case TransferCharacteristics::BT709:case TransferCharacteristics::BT601:
        case TransferCharacteristics::BT2020_10:case TransferCharacteristics::BT2020_12:
        case TransferCharacteristics::Linear:case TransferCharacteristics::SRGB:
        case TransferCharacteristics::Gamma22:case TransferCharacteristics::Gamma28:
        case TransferCharacteristics::PQ:case TransferCharacteristics::HLG:break;
        default:throw std::invalid_argument("unsupported preview transfer");
    }
    LinearizeParameters p;
    p.geometry={uint32_t(raw.width),uint32_t(raw.height),uint32_t(bytes_per_sample(raw.format)),uint32_t(bit_depth(raw.format))};
    p.planes[3]=(is_rgb(raw.format)?1u:is_gray(raw.format)?2u:0u)|(raw.color.range==ColorRange::Full?4u:0u);
    uint64_t offset=0;
    for(int i=0;i<plane_count(raw.format);++i){
        if(offset>UINT32_MAX)throw std::invalid_argument("preview exceeds Vulkan source addressing limit");
        p.planes[i]=uint32_t(offset);p.strides[i]=raw.planes[i].strideBytes;
        offset+=(raw.planes[i].bytes.size()+3)&~std::size_t(3);
    }
    p.strides[3]=uint32_t(raw.color.transfer);
    p.chroma={is_420(raw.format)||is_422(raw.format)?2.f:1.f,is_420(raw.format)?2.f:1.f,0,is_420(raw.format)?.5f:0.f};
    if(is_420(raw.format)){
        const int loc=int(raw.color.chroma420Location.value_or(Chroma420SampleLocation::LeftCenter));
        p.chroma[2]=loc%2?.5f:0.f;p.chroma[3]=loc>=4?1.f:loc>=2?0.f:.5f;
    }
    if(!is_rgb(raw.format)&&!is_gray(raw.format)){
        if(raw.color.matrix==MatrixCoefficients::BT601){p.luma[0]=.299f;p.luma[1]=.114f;}
        else if(raw.color.matrix==MatrixCoefficients::BT709){p.luma[0]=.2126f;p.luma[1]=.0722f;}
        else if(raw.color.matrix==MatrixCoefficients::BT2020NonConstant){p.luma[0]=.2627f;p.luma[1]=.0593f;}
        else throw std::invalid_argument("unsupported preview YUV matrix");
    }
    const auto matrix=linear_bt2020_matrix(raw.color.primaries);
    for(int i=0;i<3;++i){p.row0[i]=float(matrix[i]);p.row1[i]=float(matrix[3+i]);p.row2[i]=float(matrix[6+i]);}
    p.row0[3]=raw.color.primaries==ColorPrimaries::BT2020?.2627f:raw.color.primaries==ColorPrimaries::DisplayP3?.2289746f:.2126f;
    p.row2[3]=raw.color.primaries==ColorPrimaries::BT2020?.0593f:raw.color.primaries==ColorPrimaries::DisplayP3?.0792869f:.0722f;
    p.row1[3]=1-p.row0[3]-p.row2[3];
    double peak=raw.nominalPeakNits.value_or(1000.);
    p.luma[2]=float(peak/203.);p.luma[3]=float(std::clamp(1.2+.42*std::log10(std::max(peak,1.)/1000.),1.,1.5)-1.);
    return p;
}

struct GpuImageResource {
	uint64_t previewRevision=0;
    std::weak_ptr<const RawImage> source;
    uint64_t lastUsed=0;
    std::size_t bytes=0;
	ImageId id;
	int width = 0;
	int height = 0;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkImageView storageView = VK_NULL_HANDLE;
	uint32_t mipLevels = 1;
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
	VkDescriptorSetLayout composeSetLayout=VK_NULL_HANDLE;
    VkPipelineLayout composeLayout=VK_NULL_HANDLE;
    VkPipeline composePipeline=VK_NULL_HANDLE;
    VkDescriptorSet composeSet=VK_NULL_HANDLE;
    VkBuffer tileBuffer=VK_NULL_HANDLE;
    VkDeviceMemory tileMemory=VK_NULL_HANDLE;
    void* tileMapped=nullptr;
    VkDeviceSize tileCapacity=0;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkSemaphore imageAvailable = VK_NULL_HANDLE;
	std::vector<VkSemaphore> renderFinishedByImage;
	VkFence inFlight = VK_NULL_HANDLE;
	VkDescriptorSetLayout textDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	VkDescriptorSetLayout linearizeSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout linearizeLayout = VK_NULL_HANDLE;
	VkPipeline linearizePipeline = VK_NULL_HANDLE;
	VkBuffer linearSource=VK_NULL_HANDLE;
	VkDeviceMemory linearMemory=VK_NULL_HANDLE;
	void* linearMapped=nullptr;
	VkDeviceSize linearCapacity=0;
	VkBuffer linearStatus=VK_NULL_HANDLE;
	VkDeviceMemory linearStatusMemory=VK_NULL_HANDLE;
	uint32_t* linearInvalid=nullptr;
	TransientTextResources text;
	std::vector<uint8_t> textAlpha;
    int textWidth=0,textHeight=0;
    VkBuffer uiBuffer=VK_NULL_HANDLE;
    VkDeviceMemory uiMemory=VK_NULL_HANDLE;
    void* uiMapped=nullptr;
    VkDeviceSize uiCapacity=0;
    RendererStatistics statistics;
    std::vector<Point> probePoints;
    std::vector<RenderPixel> probeSamples;
    std::vector<std::array<float,4>> linearProbeSamples;
	std::vector<GpuImageResource> images;
    std::optional<std::unordered_set<uint64_t>> requestedImages;
    uint64_t imageTick=0;
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
	if (resource.storageView != VK_NULL_HANDLE) vkDestroyImageView(device, resource.storageView, nullptr);
	if (resource.image != VK_NULL_HANDLE) {
		vkDestroyImage(device, resource.image, nullptr);
	}
	if (resource.memory != VK_NULL_HANDLE) {
		vkFreeMemory(device, resource.memory, nullptr);
	}
	resource = {};
}

GpuImageResource* find_gpu_image(std::vector<GpuImageResource>& resources, ImageId id) {
	const auto it = std::ranges::find_if(resources, [id](const GpuImageResource& resource) {
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
	if (atlas.width <= 0 || atlas.height <= 0 || atlas.alpha.empty()) {
		return {};
	}

	TransientTextResources text;
	const auto size = static_cast<VkDeviceSize>(atlas.alpha.size());
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

		if (!has_extension(extensions, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
            throw std::runtime_error("Vulkan swapchain color-space extension unavailable");
        const char* const instanceExts[] = {
			VK_KHR_SURFACE_EXTENSION_NAME,
			VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
            VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME,
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
		instanceInfo.enabledExtensionCount = 3;
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
				if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 && present == VK_TRUE) {
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
			throw std::runtime_error("no Vulkan queue family supports compute and Wayland presentation");
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

		const char* const deviceExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
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
		impl->logicalExtent = {.width=static_cast<uint32_t>(std::max(1, window.width())), .height=static_cast<uint32_t>(std::max(1, window.height()))};
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
		const VkImageUsageFlags requiredUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((surfaceCaps.supportedUsageFlags & requiredUsage) != requiredUsage)
            throw std::runtime_error("Wayland swapchain does not support compute storage and clear operations");
        swapchainInfo.imageUsage = requiredUsage;
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

		VkDescriptorSetLayoutBinding atlasBinding{};
		atlasBinding.binding = 0;
		atlasBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		atlasBinding.descriptorCount = 1;
		atlasBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		VkDescriptorSetLayoutCreateInfo textSetLayoutInfo{};
		textSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		textSetLayoutInfo.bindingCount = 1;
		textSetLayoutInfo.pBindings = &atlasBinding;
		vk_checked(vkCreateDescriptorSetLayout(impl->device, &textSetLayoutInfo, nullptr, &impl->textDescriptorSetLayout), "vkCreateDescriptorSetLayout");

		VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1024},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,8},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,8}};
		VkDescriptorPoolCreateInfo descriptorPoolInfo{};
		descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		descriptorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		descriptorPoolInfo.maxSets = 1024;
		descriptorPoolInfo.poolSizeCount = 3;
		descriptorPoolInfo.pPoolSizes = poolSizes;
		vk_checked(vkCreateDescriptorPool(impl->device, &descriptorPoolInfo, nullptr, &impl->descriptorPool), "vkCreateDescriptorPool");

        VkDescriptorSetLayoutBinding linearBindings[]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo linearSet{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        linearSet.bindingCount=3;linearSet.pBindings=linearBindings;
        vk_checked(vkCreateDescriptorSetLayout(impl->device,&linearSet,nullptr,&impl->linearizeSetLayout),"create linear conversion descriptor layout");
        VkPushConstantRange linearPush{VK_SHADER_STAGE_COMPUTE_BIT,0,128};
        VkPipelineLayoutCreateInfo linearLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        linearLayout.setLayoutCount=1;linearLayout.pSetLayouts=&impl->linearizeSetLayout;
        linearLayout.pushConstantRangeCount=1;linearLayout.pPushConstantRanges=&linearPush;
        vk_checked(vkCreatePipelineLayout(impl->device,&linearLayout,nullptr,&impl->linearizeLayout),"create linear conversion layout");
        auto linearShader=create_shader_module(impl->device,"linearize.comp.spv");
        VkComputePipelineCreateInfo linearPipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        linearPipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,linearShader,"main",nullptr};
        linearPipeline.layout=impl->linearizeLayout;
        auto linearResult=vkCreateComputePipelines(impl->device,VK_NULL_HANDLE,1,&linearPipeline,nullptr,&impl->linearizePipeline);
        vkDestroyShaderModule(impl->device,linearShader,nullptr);
        vk_checked(linearResult,"create linear conversion pipeline");
        create_buffer(impl->device,impl->physicalDevice,4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,impl->linearStatus,impl->linearStatusMemory);
        void* invalid=nullptr;
        vk_checked(vkMapMemory(impl->device,impl->linearStatusMemory,0,4,0,&invalid),"map conversion status");
        impl->linearInvalid=static_cast<uint32_t*>(invalid);

        VkDescriptorSetLayoutBinding bindings[] = {
            {0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount=3;setInfo.pBindings=bindings;
        vk_checked(vkCreateDescriptorSetLayout(impl->device,&setInfo,nullptr,&impl->composeSetLayout),"create compositor descriptor layout");
        VkDescriptorSetLayout layouts[]={impl->composeSetLayout,impl->textDescriptorSetLayout,impl->textDescriptorSetLayout};
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,96};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout.setLayoutCount=3;layout.pSetLayouts=layouts;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
        vk_checked(vkCreatePipelineLayout(impl->device,&layout,nullptr,&impl->composeLayout),"create compute compositor layout");
        auto shader=create_shader_module(impl->device,"compose.comp.spv");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,shader,"main",nullptr};
        pipeline.layout=impl->composeLayout;
        auto result=vkCreateComputePipelines(impl->device,VK_NULL_HANDLE,1,&pipeline,nullptr,&impl->composePipeline);
        vkDestroyShaderModule(impl->device,shader,nullptr);vk_checked(result,"create compute compositor pipeline");
        VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sets.descriptorPool=impl->descriptorPool;sets.descriptorSetCount=1;sets.pSetLayouts=&impl->composeSetLayout;
        vk_checked(vkAllocateDescriptorSets(impl->device,&sets,&impl->composeSet),"allocate compute compositor descriptors");

	} catch (...) {
		VulkanRenderer const cleanup(impl);
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
	impl_->logicalExtent = {.width=static_cast<uint32_t>(std::max(1, window.width())), .height=static_cast<uint32_t>(std::max(1, window.height()))};
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
	const VkImageUsageFlags requiredUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((surfaceCaps.supportedUsageFlags & requiredUsage) != requiredUsage)
            throw std::runtime_error("Wayland swapchain does not support compute storage and clear operations");
        swapchainInfo.imageUsage = requiredUsage;
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

	create_render_finished_semaphores(impl_->device, impl_->swapchainImages.size(), impl_->renderFinishedByImage);
}

void VulkanRenderer::sync_images(std::span<const ImageObject> images,std::span<const DrawCommand> commands,std::span<const ImageId> keepResident) {
    if(!valid())throw std::runtime_error("Renderer is not initialized");
    std::unordered_set<uint64_t> requested;
    for(const auto& item:compile_paint_scene(commands)) {
        const auto& command=commands[item.commandIndex];
        const Rect viewport{0,0,float(impl_->logicalExtent.width),float(impl_->logicalExtent.height)};
        if(command.kind==DrawCommandKind::Image && ui::valid_rect(ui::intersect(command.rect,ui::intersect(item.clip,viewport)))){
            requested.insert(command.image.value);if(codec_gui::gui::valid(command.comparisonImage))requested.insert(command.comparisonImage.value);
        }
    }
    // Both members of a comparison must survive single-image blink/Space frames.
    // Otherwise large A/B images evict and re-upload one another every flip.
    for(auto id:keepResident)if(codec_gui::gui::valid(id))requested.insert(id.value);
    impl_->requestedImages=std::move(requested);
    try{sync_images(images);}catch(...){impl_->requestedImages.reset();throw;}
    impl_->requestedImages.reset();
}

static uint32_t image_mip_levels(int width, int height) {
    uint32_t levels = 1;
    for (int size = std::max(width, height); size > 1; size /= 2) ++levels;
    return levels;
}
static std::size_t image_mip_bytes(int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    std::size_t pixels = 0;
    do {
        pixels += std::size_t(width) * height;
        if (width == 1 && height == 1) break;
        width = std::max(1, width / 2); height = std::max(1, height / 2);
    } while (true);
    return pixels * sizeof(float) * 4;
}

void VulkanRenderer::sync_images(std::span<const ImageObject> images) {
	if (!valid()) {
		throw std::runtime_error("renderer is not initialized");
	}

    const bool replacing=std::ranges::any_of(impl_->images,[&](const auto& resource){
        return std::ranges::none_of(images,[&](const auto& image){return image.id==resource.id && image.decoded && image.decoded->width==resource.width && image.decoded->height==resource.height;});
    });
    if(replacing)vk_checked(vkDeviceWaitIdle(impl_->device),"wait before replacing decoded preview");
	std::erase_if(impl_->images, [&](GpuImageResource& resource) {
			const bool keep = std::ranges::any_of(images, [&](const ImageObject& image) {
				return image.id == resource.id && image.decoded != nullptr && image.decoded->width==resource.width && image.decoded->height==resource.height;
			});
			if (!keep) {
				destroy_gpu_image(impl_->device, impl_->descriptorPool, resource);
			}
			return !keep;
		});

    ++impl_->imageTick;
    auto wanted=[&](ImageId id){return !impl_->requestedImages || impl_->requestedImages->contains(id.value);};
    std::size_t requiredBytes=0,requiredCount=0,residentBytes=0;
    for(auto& resource:impl_->images){residentBytes+=resource.bytes;if(wanted(resource.id))resource.lastUsed=impl_->imageTick;}
    for(const auto& image:images)if(wanted(image.id) && image.decoded && !find_gpu_image(impl_->images,image.id)) {
        requiredBytes+=image_mip_bytes(image.decoded->width,image.decoded->height);++requiredCount;
    }
    constexpr std::size_t cacheBudget=512ull*1024*1024;
    while((residentBytes+requiredBytes>cacheBudget || impl_->images.size()+requiredCount>32) && !impl_->images.empty()) {
        auto oldest=impl_->images.end();
        for(auto it=impl_->images.begin();it!=impl_->images.end();++it)if(!wanted(it->id) && (oldest==impl_->images.end() || it->lastUsed<oldest->lastUsed))oldest=it;
        if(oldest==impl_->images.end())break; // Active images are allowed to exceed the idle-cache budget.
        vk_checked(vkWaitForFences(impl_->device,1,&impl_->inFlight,VK_TRUE,UINT64_MAX),"wait before evicting image");
        residentBytes-=oldest->bytes;destroy_gpu_image(impl_->device,impl_->descriptorPool,*oldest);impl_->images.erase(oldest);
    }
	for (const ImageObject& imageObject : images) {
		auto* existing=find_gpu_image(impl_->images,imageObject.id);
		if (!wanted(imageObject.id) || imageObject.decoded == nullptr || (existing && existing->source.lock()==imageObject.decoded && existing->previewRevision==imageObject.previewRevision)) {
			continue;
		}
		const RawImage& raw = *imageObject.decoded;
		const auto parameters=linearize_parameters(raw);
        const std::size_t pixels=std::size_t(raw.width)*raw.height;

		VkDeviceSize uploadSize=0;
        for(int i=0;i<plane_count(raw.format);++i)uploadSize+=(raw.planes[i].bytes.size()+3)&~std::size_t(3);
        VkPhysicalDeviceProperties limits{};vkGetPhysicalDeviceProperties(impl_->physicalDevice,&limits);
        const bool gpuConversion=uploadSize<=limits.limits.maxStorageBufferRange;
        std::vector<float> fallback;
        if(!gpuConversion){
            const auto linear=raw_image_to_linear_bt2020(raw);fallback.resize(pixels*4);
            for(std::size_t i=0;i<pixels;++i){for(int p=0;p<3;++p)fallback[i*4+p]=linear.planes[p][i];fallback[i*4+3]=1;}
            uploadSize=fallback.size()*sizeof(float);
        }
        if(impl_->linearCapacity<uploadSize){
        if(impl_->linearMapped)vkUnmapMemory(impl_->device,impl_->linearMemory);
        if(impl_->linearSource)vkDestroyBuffer(impl_->device,impl_->linearSource,nullptr);
        if(impl_->linearMemory)vkFreeMemory(impl_->device,impl_->linearMemory,nullptr);
        impl_->linearSource=VK_NULL_HANDLE;impl_->linearMemory=VK_NULL_HANDLE;impl_->linearMapped=nullptr;impl_->linearCapacity=0;
		create_buffer(
			impl_->device,
			impl_->physicalDevice,
			uploadSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			impl_->linearSource,
			impl_->linearMemory
		);
        vk_checked(vkMapMemory(impl_->device,impl_->linearMemory,0,uploadSize,0,&impl_->linearMapped),"map conversion source");
        impl_->linearCapacity=uploadSize;
        }
        void* mapped=impl_->linearMapped;
		if(gpuConversion)for(int i=0;i<plane_count(raw.format);++i)std::memcpy(static_cast<uint8_t*>(mapped)+parameters.planes[i],raw.planes[i].bytes.data(),raw.planes[i].bytes.size());
        else std::memcpy(mapped,fallback.data(),uploadSize);

		GpuImageResource resource;
        if(existing)resource=*existing;
		resource.id = imageObject.id;
		resource.previewRevision=imageObject.previewRevision;resource.source=imageObject.decoded;resource.lastUsed=impl_->imageTick;resource.bytes=image_mip_bytes(raw.width,raw.height);
        resource.mipLevels=image_mip_levels(raw.width,raw.height);
		resource.width = raw.width;
		resource.height = raw.height;
        if(!existing){
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(impl_->physicalDevice,VK_FORMAT_R32G32B32A32_SFLOAT,&formatProperties);
        const auto mipFeatures=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((formatProperties.optimalTilingFeatures&mipFeatures)!=mipFeatures)
            throw std::runtime_error("FP32 linear image mipmapping is not supported by this Vulkan device");
		create_image(
			impl_->device,
			impl_->physicalDevice,
			static_cast<uint32_t>(raw.width),
			static_cast<uint32_t>(raw.height),
			VK_FORMAT_R32G32B32A32_SFLOAT,
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			resource.image,
			resource.memory,
            resource.mipLevels
		);

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = resource.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = resource.mipLevels;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;
		vk_checked(vkCreateImageView(impl_->device, &viewInfo, nullptr, &resource.view), "vkCreateImageView");
        viewInfo.subresourceRange.levelCount = 1;
        vk_checked(vkCreateImageView(impl_->device, &viewInfo, nullptr, &resource.storageView), "create base-level storage view");

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_NEAREST;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.maxLod = float(resource.mipLevels - 1);
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
        }

        VkDescriptorSet convertSet=VK_NULL_HANDLE;
        *impl_->linearInvalid=0;
        if(gpuConversion){
        VkDescriptorSetAllocateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        descriptorInfo.descriptorPool=impl_->descriptorPool;descriptorInfo.descriptorSetCount=1;
        descriptorInfo.pSetLayouts=&impl_->linearizeSetLayout;
        vk_checked(vkAllocateDescriptorSets(impl_->device,&descriptorInfo,&convertSet),"allocate conversion descriptors");
        VkDescriptorBufferInfo sourceBuffer{impl_->linearSource,0,uploadSize};
        VkDescriptorImageInfo destination{VK_NULL_HANDLE,resource.storageView,VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo statusBuffer{impl_->linearStatus,0,4};
        *impl_->linearInvalid=0;
        VkWriteDescriptorSet conversionWrites[3]{};
        for(auto& w:conversionWrites){w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=convertSet;w.descriptorCount=1;}
        conversionWrites[0].dstBinding=0;conversionWrites[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;conversionWrites[0].pBufferInfo=&sourceBuffer;
        conversionWrites[1].dstBinding=1;conversionWrites[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;conversionWrites[1].pImageInfo=&destination;
        conversionWrites[2].dstBinding=2;conversionWrites[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;conversionWrites[2].pBufferInfo=&statusBuffer;
        vkUpdateDescriptorSets(impl_->device,3,conversionWrites,0,nullptr);
        }

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
			gpuConversion?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0,
			gpuConversion?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			gpuConversion?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT
		);
        if(gpuConversion){
        vkCmdBindPipeline(impl_->commandBuffer,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->linearizePipeline);
        vkCmdBindDescriptorSets(impl_->commandBuffer,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->linearizeLayout,0,1,&convertSet,0,nullptr);
        vkCmdPushConstants(impl_->commandBuffer,impl_->linearizeLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(parameters),&parameters);
        vkCmdDispatch(impl_->commandBuffer,(raw.width+15)/16,(raw.height+15)/16,1);
        VkMemoryBarrier statusBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        statusBarrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;statusBarrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(impl_->commandBuffer,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&statusBarrier,0,nullptr,0,nullptr);
        }else {
            VkBufferImageCopy copy{};copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.imageSubresource.layerCount=1;
            copy.imageExtent={uint32_t(raw.width),uint32_t(raw.height),1};
            vkCmdCopyBufferToImage(impl_->commandBuffer,impl_->linearSource,resource.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        }
        // Build the pyramid from linear BT.2020 samples. Only lower levels are
        // filtered; level zero remains the original full-precision conversion.
        image_barrier(impl_->commandBuffer,resource.image,
            gpuConversion?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            gpuConversion?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
            gpuConversion?VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        int mipWidth=raw.width,mipHeight=raw.height;
        for(uint32_t level=1;level<resource.mipLevels;++level){
            const int nextWidth=std::max(1,mipWidth/2),nextHeight=std::max(1,mipHeight/2);
            image_barrier(impl_->commandBuffer,resource.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,level);
            VkImageBlit blit{};
            blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level-1,0,1};
            blit.srcOffsets[1]={mipWidth,mipHeight,1};
            blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,1};
            blit.dstOffsets[1]={nextWidth,nextHeight,1};
            vkCmdBlitImage(impl_->commandBuffer,resource.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                resource.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
            image_barrier(impl_->commandBuffer,resource.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,level);
            mipWidth=nextWidth;mipHeight=nextHeight;
        }
        image_barrier(impl_->commandBuffer,resource.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,resource.mipLevels);
		vk_checked(vkEndCommandBuffer(impl_->commandBuffer), "vkEndCommandBuffer");
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &impl_->commandBuffer;
		vk_checked(vkResetFences(impl_->device, 1, &impl_->inFlight), "vkResetFences");
	vk_checked(vkQueueSubmit(impl_->queue, 1, &submitInfo, impl_->inFlight), "vkQueueSubmit");
		vk_checked(vkWaitForFences(impl_->device, 1, &impl_->inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");

        if(convertSet)vkFreeDescriptorSets(impl_->device,impl_->descriptorPool,1,&convertSet);
		if(*impl_->linearInvalid){if(!existing)destroy_gpu_image(impl_->device,impl_->descriptorPool,resource);throw std::invalid_argument("non-finite linear preview conversion (invalid EOTF domain)");}
		if(existing)*existing=resource;else impl_->images.push_back(resource);
	}
}

void VulkanRenderer::render_clear_frame(float r, float g, float b, float a) {
	if (!valid()) {
		throw std::runtime_error("renderer is not initialized");
	}
	vk_checked(vkWaitForFences(impl_->device, 1, &impl_->inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
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

    auto target=impl_->swapchainImages.at(imageIndex);
    image_barrier(impl_->commandBuffer,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue clear{{r,g,b,a}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(impl_->commandBuffer,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    image_barrier(impl_->commandBuffer,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	vk_checked(vkEndCommandBuffer(impl_->commandBuffer), "vkEndCommandBuffer");

	VkPipelineStageFlags const waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &impl_->imageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &impl_->commandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &renderFinished;
	vk_checked(vkResetFences(impl_->device, 1, &impl_->inFlight), "vkResetFences");
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
	uint32_t imageIndex = 0;
	const VkResult acquire = vkAcquireNextImageKHR(impl_->device, impl_->swapchain, UINT64_MAX, impl_->imageAvailable, VK_NULL_HANDLE, &imageIndex);
	if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
		throw std::runtime_error("vkAcquireNextImageKHR failed");
	}
	VkSemaphore renderFinished = impl_->renderFinishedByImage.at(imageIndex);

    // Keep a one-pixel atlas bound even for a frame containing only geometry.
    TextAtlas fallbackAtlas;
    if(!textAtlas || textAtlas->alpha.empty()) {
        fallbackAtlas.width=fallbackAtlas.height=1;fallbackAtlas.alpha={255};textAtlas=&fallbackAtlas;
    }
    const auto frame=compile_ui_frame(commands,textAtlas);
    impl_->statistics={};
    impl_->statistics.instances=frame.instances.size();
    impl_->statistics.instanceUploadBytes=frame.instances.size()*sizeof(UiInstance);
    const VkDeviceSize instanceBytes=std::max<VkDeviceSize>(sizeof(UiInstance),impl_->statistics.instanceUploadBytes);
    if(instanceBytes>impl_->uiCapacity) {
        if(impl_->uiMapped)vkUnmapMemory(impl_->device,impl_->uiMemory);
        impl_->uiMapped=nullptr;
        if(impl_->uiBuffer)vkDestroyBuffer(impl_->device,impl_->uiBuffer,nullptr);
        if(impl_->uiMemory)vkFreeMemory(impl_->device,impl_->uiMemory,nullptr);
        impl_->uiBuffer=VK_NULL_HANDLE;impl_->uiMemory=VK_NULL_HANDLE;
        impl_->uiCapacity=std::max<VkDeviceSize>(65536,instanceBytes*2);
        create_buffer(impl_->device,impl_->physicalDevice,impl_->uiCapacity,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,impl_->uiBuffer,impl_->uiMemory);
        vk_checked(vkMapMemory(impl_->device,impl_->uiMemory,0,impl_->uiCapacity,0,&impl_->uiMapped),"vkMapMemory(UI instances)");
    }
    if(!frame.instances.empty())std::memcpy(impl_->uiMapped,frame.instances.data(),impl_->statistics.instanceUploadBytes);
    const uint32_t gridW=(impl_->swapchainExtent.width+15)/16;
    const uint32_t gridH=(impl_->swapchainExtent.height+15)/16;
    std::vector<uint32_t> tiles;
    std::vector<uint32_t> tileBases(frame.batches.size());
    std::vector<VkRect2D> batchBounds(frame.batches.size());
    for(std::size_t bi=0;bi<frame.batches.size();++bi){
        const auto& batch=frame.batches[bi];
        Rect bounds{};bool have=false;
        if(batch.image())bounds=commands[batch.imageCommand].rect;
        else for(uint32_t i=batch.first;i<batch.first+batch.count;++i){
            const auto& v=frame.instances[i];
            const float c=std::abs(std::cos(v.shape[3])),sn=std::abs(std::sin(v.shape[3]));
            const float w=c*v.rect[2]+sn*v.rect[3],h=sn*v.rect[2]+c*v.rect[3];
            Rect r{v.rect[0]+(v.rect[2]-w)*.5f,v.rect[1]+(v.rect[3]-h)*.5f,w,h};
            if(!have){bounds=r;have=true;}else{float x=std::min(bounds.x,r.x),y=std::min(bounds.y,r.y);
                bounds={x,y,std::max((bounds.x+bounds.w),(r.x+r.w))-x,std::max((bounds.y+bounds.h),(r.y+r.h))-y};}
        }
        bounds=ui::intersect(bounds,batch.clip);
        batchBounds[bi]=clamp_scissor(bounds,impl_->swapchainExtent,impl_->outputScale);
        if(batch.image())continue;
        tileBases[bi]=uint32_t(tiles.size());
        std::vector<std::vector<uint32_t>> bins(std::size_t(gridW)*gridH);
        for(uint32_t i=batch.first;i<batch.first+batch.count;++i){
            const auto& v=frame.instances[i];
            const float c=std::abs(std::cos(v.shape[3])),sn=std::abs(std::sin(v.shape[3]));
            const float w=c*v.rect[2]+sn*v.rect[3],h=sn*v.rect[2]+c*v.rect[3];
            Rect r{v.rect[0]+(v.rect[2]-w)*.5f,v.rect[1]+(v.rect[3]-h)*.5f,w,h};
            auto clip=clamp_scissor(ui::intersect(r,batch.clip),impl_->swapchainExtent,impl_->outputScale);
            if(!clip.extent.width||!clip.extent.height)continue;
            for(uint32_t y=uint32_t(clip.offset.y)/16;y<=(uint32_t(clip.offset.y)+clip.extent.height-1)/16;++y)
                for(uint32_t x=uint32_t(clip.offset.x)/16;x<=(uint32_t(clip.offset.x)+clip.extent.width-1)/16;++x)
                    bins[y*gridW+x].push_back(i);
        }
        tiles.resize(tiles.size()+std::size_t(gridW)*gridH*2);
        for(std::size_t i=0;i<bins.size();++i){
            tiles[tileBases[bi]+i*2]=uint32_t(tiles.size());
            tiles[tileBases[bi]+i*2+1]=uint32_t(bins[i].size());
            tiles.insert(tiles.end(),bins[i].begin(),bins[i].end());
        }
    }
    const VkDeviceSize tileBytes=std::max<VkDeviceSize>(4,tiles.size()*sizeof(uint32_t));
    if(tileBytes>impl_->tileCapacity){
        if(impl_->tileMapped)vkUnmapMemory(impl_->device,impl_->tileMemory);
        if(impl_->tileBuffer)vkDestroyBuffer(impl_->device,impl_->tileBuffer,nullptr);
        if(impl_->tileMemory)vkFreeMemory(impl_->device,impl_->tileMemory,nullptr);
        impl_->tileCapacity=tileBytes*2;
        create_buffer(impl_->device,impl_->physicalDevice,impl_->tileCapacity,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,impl_->tileBuffer,impl_->tileMemory);
        vk_checked(vkMapMemory(impl_->device,impl_->tileMemory,0,impl_->tileCapacity,0,&impl_->tileMapped),"map tile lists");
    }
    if(!tiles.empty())std::memcpy(impl_->tileMapped,tiles.data(),tiles.size()*sizeof(uint32_t));
    VkDescriptorImageInfo output{VK_NULL_HANDLE,impl_->swapchainImageViews.at(imageIndex),VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo instances{impl_->uiBuffer,0,instanceBytes},tileInfo{impl_->tileBuffer,0,tileBytes};
    VkWriteDescriptorSet writes[3]{};
    for(uint32_t i=0;i<3;++i){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=impl_->composeSet;
        writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;}
    writes[0].pImageInfo=&output;writes[1].pBufferInfo=&instances;writes[2].pBufferInfo=&tileInfo;
    vkUpdateDescriptorSets(impl_->device,3,writes,0,nullptr);
    bool newAtlas=impl_->text.image==VK_NULL_HANDLE || impl_->textWidth!=textAtlas->width || impl_->textHeight!=textAtlas->height;
    bool uploadText=newAtlas || impl_->textAlpha!=textAtlas->alpha;
    if(newAtlas) {
        destroy_transient_text(impl_->device,impl_->descriptorPool,impl_->text);
        impl_->text=create_transient_text(impl_->device,impl_->physicalDevice,impl_->descriptorPool,impl_->textDescriptorSetLayout,*textAtlas);
        impl_->textWidth=textAtlas->width;impl_->textHeight=textAtlas->height;
    } else if(uploadText) {
        void* mapped=nullptr;
        vk_checked(vkMapMemory(impl_->device,impl_->text.stagingMemory,0,textAtlas->alpha.size(),0,&mapped),"vkMapMemory(text update)");
        std::memcpy(mapped,textAtlas->alpha.data(),textAtlas->alpha.size());vkUnmapMemory(impl_->device,impl_->text.stagingMemory);
    }
    if(uploadText){impl_->textAlpha=textAtlas->alpha;impl_->statistics.textUploadBytes=textAtlas->alpha.size();}
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
			newAtlas ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			newAtlas ? VkAccessFlags(0) : VkAccessFlags(VK_ACCESS_SHADER_READ_BIT),
			VK_ACCESS_TRANSFER_WRITE_BIT,
			newAtlas ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
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
			.width=static_cast<uint32_t>(textAtlas->width),
			.height=static_cast<uint32_t>(textAtlas->height),
			.depth=1
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
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
		);
	}

    auto target=impl_->swapchainImages.at(imageIndex);
    image_barrier(impl_->commandBuffer,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue clear{{.015f,.017f,.02f,1.f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(impl_->commandBuffer,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    image_barrier(impl_->commandBuffer,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    vkCmdBindPipeline(impl_->commandBuffer,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->composePipeline);
    struct ComposePush {int32_t bounds[4];float rect[4],uv[4],color[4],params[4];uint32_t meta[4];};
    static_assert(sizeof(ComposePush)==96);
    for(std::size_t bi=0;bi<frame.batches.size();++bi){
        const auto& batch=frame.batches[bi];const auto& clip=batchBounds[bi];
        if(!clip.extent.width||!clip.extent.height)continue;
        ComposePush push{};
        push.bounds[0]=clip.offset.x;push.bounds[1]=clip.offset.y;
        push.bounds[2]=clip.offset.x+clip.extent.width;push.bounds[3]=clip.offset.y+clip.extent.height;
        push.params[0]=impl_->outputScale;
        push.meta[0]=tileBases[bi];push.meta[1]=gridW;
        VkDescriptorSet sampled=text.descriptorSet,reference=sampled;
        if(batch.image()){
            const auto& command=commands[batch.imageCommand];
            const auto* image=find_gpu_image(impl_->images,command.image);
            const auto* ref=codec_gui::gui::valid(command.comparisonImage)?find_gpu_image(impl_->images,command.comparisonImage):image;
            if(!image||!ref)continue;
            sampled=image->descriptorSet;reference=ref->descriptorSet;
            push.rect[0]=command.rect.x;push.rect[1]=command.rect.y;push.rect[2]=command.rect.w;push.rect[3]=command.rect.h;
            push.uv[0]=command.uvRect.x;push.uv[1]=command.uvRect.y;push.uv[2]=command.uvRect.w;push.uv[3]=command.uvRect.h;
            push.color[0]=command.color.r;push.color[1]=command.color.g;push.color[2]=command.color.b;push.color[3]=command.opacity*command.color.a;
            push.params[1]=command.differenceGain;push.meta[2]=codec_gui::gui::valid(command.comparisonImage)?2:1;
        }
        VkDescriptorSet sets[]={impl_->composeSet,sampled,reference};
        vkCmdBindDescriptorSets(impl_->commandBuffer,VK_PIPELINE_BIND_POINT_COMPUTE,impl_->composeLayout,0,3,sets,0,nullptr);
        vkCmdPushConstants(impl_->commandBuffer,impl_->composeLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        vkCmdDispatch(impl_->commandBuffer,(clip.extent.width+15)/16,(clip.extent.height+15)/16,1);
        image_barrier(impl_->commandBuffer,target,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ++impl_->statistics.dispatches;
    }
    image_barrier(impl_->commandBuffer,target,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_ACCESS_SHADER_WRITE_BIT,0,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	if (probeRect) {
		image_barrier(
			impl_->commandBuffer,
			impl_->swapchainImages.at(imageIndex),
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			VK_ACCESS_SHADER_WRITE_BIT,
			VK_ACCESS_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT
		);
		VkBufferImageCopy copy{};
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = 0;
		copy.imageSubresource.baseArrayLayer = 0;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent = {.width=impl_->swapchainExtent.width, .height=impl_->swapchainExtent.height, .depth=1};
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
	VkPipelineStageFlags const waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &impl_->imageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &impl_->commandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &renderFinished;
	vk_checked(vkResetFences(impl_->device, 1, &impl_->inFlight), "vkResetFences");
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
        impl_->probeSamples.clear();
        impl_->linearProbeSamples.clear();
        for(const auto& point:impl_->probePoints) {
            if(!std::isfinite(point.x) || !std::isfinite(point.y) || point.x<0 || point.y<0 ||
               point.x>=impl_->logicalExtent.width || point.y>=impl_->logicalExtent.height)
                throw std::invalid_argument("Pixel probe is outside the window");
            const auto x=std::min(impl_->swapchainExtent.width-1,uint32_t(point.x*impl_->outputScale));
            const auto y=std::min(impl_->swapchainExtent.height-1,uint32_t(point.y*impl_->outputScale));
            const auto value=read_linear_pixel(pixels,std::size_t(y)*impl_->swapchainExtent.width+x);
            impl_->linearProbeSamples.push_back(value);
            auto byte=[](float x){return uint8_t(std::lround(std::clamp(x,0.f,1.f)*255));};
            impl_->probeSamples.push_back({byte(value[0]),byte(value[1]),byte(value[2]),byte(value[3])});
        }
        impl_->probePoints.clear();
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

std::vector<std::array<float,4>> VulkanRenderer::read_image_samples(ImageId id,std::span<const Point> positions){
    if(!valid())throw std::runtime_error("Renderer is not initialized");
    const auto* image=find_gpu_image(impl_->images,id);
    if(!image)throw std::invalid_argument("Image is not resident");
    if(positions.empty())return {};
    std::vector<VkBufferImageCopy> copies;
    for(std::size_t i=0;i<positions.size();++i){
        const auto p=positions[i];
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.y<0||p.x>=image->width||p.y>=image->height)throw std::invalid_argument("Image sample is outside the image");
        VkBufferImageCopy copy{};copy.bufferOffset=i*sizeof(float)*4;
        copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageOffset={int(p.x),int(p.y),0};copy.imageExtent={1,1,1};copies.push_back(copy);
    }
    vk_checked(vkWaitForFences(impl_->device,1,&impl_->inFlight,VK_TRUE,UINT64_MAX),"wait for image samples");
    VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
    const VkDeviceSize bytes=positions.size()*sizeof(float)*4;
    try {
        create_buffer(impl_->device,impl_->physicalDevice,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,buffer,memory);
        vk_checked(vkResetCommandBuffer(impl_->commandBuffer,0),"reset for image samples");
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vk_checked(vkBeginCommandBuffer(impl_->commandBuffer,&begin),"begin image samples");
        image_barrier(impl_->commandBuffer,image->image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(impl_->commandBuffer,image->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,uint32_t(copies.size()),copies.data());
        image_barrier(impl_->commandBuffer,image->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        vk_checked(vkEndCommandBuffer(impl_->commandBuffer),"end image samples");
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&impl_->commandBuffer;
        vk_checked(vkResetFences(impl_->device,1,&impl_->inFlight),"reset image sample fence");
        vk_checked(vkQueueSubmit(impl_->queue,1,&submit,impl_->inFlight),"submit image samples");
        vk_checked(vkWaitForFences(impl_->device,1,&impl_->inFlight,VK_TRUE,UINT64_MAX),"wait for image sample transfer");
        std::vector<std::array<float,4>> result(positions.size());void* mapped=nullptr;
        vk_checked(vkMapMemory(impl_->device,memory,0,bytes,0,&mapped),"map image samples");
        std::memcpy(result.data(),mapped,bytes);vkUnmapMemory(impl_->device,memory);
        vkDestroyBuffer(impl_->device,buffer,nullptr);vkFreeMemory(impl_->device,memory,nullptr);return result;
    }catch(...){if(buffer)vkDestroyBuffer(impl_->device,buffer,nullptr);if(memory)vkFreeMemory(impl_->device,memory,nullptr);throw;}
}

RendererStatistics VulkanRenderer::statistics()const{
    if(!impl_)return {};
    auto result=impl_->statistics;result.residentImages=impl_->images.size();
    for(const auto& image:impl_->images)result.residentImageBytes+=image.bytes;
    return result;
}
std::vector<RenderPixel> VulkanRenderer::render_draw_list_samples(const std::vector<DrawCommand>& commands,const TextAtlas* atlas,std::span<const Point> points) {
    if(!valid())throw std::runtime_error("Renderer is not initialized");
    for(auto p:points)if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.y<0||p.x>=impl_->logicalExtent.width||p.y>=impl_->logicalExtent.height)
        throw std::invalid_argument("Pixel probe is outside the window");
    impl_->probePoints.assign(points.begin(),points.end());
    impl_->pendingProbeRect=Rect{0,0,float(impl_->logicalExtent.width),float(impl_->logicalExtent.height)};
    render_draw_list(commands,atlas);
    return impl_->probeSamples;
}

std::vector<std::array<float,4>> VulkanRenderer::render_draw_list_linear_samples(
    const std::vector<DrawCommand>& commands,const TextAtlas* atlas,std::span<const Point> points){
    (void)render_draw_list_samples(commands,atlas,points);
    return impl_->linearProbeSamples;
}

void VulkanRenderer::reset() {
	if (impl_ == nullptr) {
		return;
	}
	if (impl_->device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(impl_->device);
		destroy_transient_text(impl_->device, impl_->descriptorPool, impl_->text);
        if(impl_->composePipeline)vkDestroyPipeline(impl_->device,impl_->composePipeline,nullptr);
        if(impl_->composeLayout)vkDestroyPipelineLayout(impl_->device,impl_->composeLayout,nullptr);
        if(impl_->composeSetLayout)vkDestroyDescriptorSetLayout(impl_->device,impl_->composeSetLayout,nullptr);
        if(impl_->tileMapped)vkUnmapMemory(impl_->device,impl_->tileMemory);
        if(impl_->tileBuffer)vkDestroyBuffer(impl_->device,impl_->tileBuffer,nullptr);
        if(impl_->tileMemory)vkFreeMemory(impl_->device,impl_->tileMemory,nullptr);
        if(impl_->uiMapped)vkUnmapMemory(impl_->device,impl_->uiMemory);
        if(impl_->uiBuffer)vkDestroyBuffer(impl_->device,impl_->uiBuffer,nullptr);
        if(impl_->uiMemory)vkFreeMemory(impl_->device,impl_->uiMemory,nullptr);
		impl_->textAlpha.clear();
		for (GpuImageResource& image : impl_->images) {
			destroy_gpu_image(impl_->device, impl_->descriptorPool, image);
		}
        if(impl_->linearizePipeline)vkDestroyPipeline(impl_->device,impl_->linearizePipeline,nullptr);
        if(impl_->linearizeLayout)vkDestroyPipelineLayout(impl_->device,impl_->linearizeLayout,nullptr);
        if(impl_->linearizeSetLayout)vkDestroyDescriptorSetLayout(impl_->device,impl_->linearizeSetLayout,nullptr);
        if(impl_->linearMapped)vkUnmapMemory(impl_->device,impl_->linearMemory);
        if(impl_->linearSource)vkDestroyBuffer(impl_->device,impl_->linearSource,nullptr);
        if(impl_->linearMemory)vkFreeMemory(impl_->device,impl_->linearMemory,nullptr);
        if(impl_->linearInvalid)vkUnmapMemory(impl_->device,impl_->linearStatusMemory);
        if(impl_->linearStatus)vkDestroyBuffer(impl_->device,impl_->linearStatus,nullptr);
        if(impl_->linearStatusMemory)vkFreeMemory(impl_->device,impl_->linearStatusMemory,nullptr);
		if (impl_->descriptorPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(impl_->device, impl_->descriptorPool, nullptr);
		}
		if (impl_->textDescriptorSetLayout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(impl_->device, impl_->textDescriptorSetLayout, nullptr);
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
