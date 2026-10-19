#pragma once
// Native Vulkan physical-device and per-codec video-queue admission.
// Keep the video queue's codec operations: a graphics/compute queue is not
// evidence that a device can decode video.
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
namespace codec_gui::gui {
struct VulkanDecodeDevice {
	std::string index, name;
	VkVideoCodecOperationFlagsKHR operations = 0;
};
inline std::vector<VulkanDecodeDevice> vulkan_decode_devices() {
	std::vector<VulkanDecodeDevice> result;
	VkApplicationInfo application{};
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo create{};
	create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	create.pApplicationInfo = &application;
	VkInstance instance{};
	if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS)
		return result;
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	for (uint32_t i = 0; i < count; ++i) {
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(devices[i], &props);
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
			continue;
		uint32_t queueCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties2(devices[i], &queueCount, nullptr);
		std::vector<VkQueueFamilyProperties2> queues(queueCount);
		std::vector<VkQueueFamilyVideoPropertiesKHR> video(queueCount);
		for (uint32_t q = 0; q < queueCount; ++q) {
			queues[q].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
			video[q].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
			queues[q].pNext = &video[q];
		}
		vkGetPhysicalDeviceQueueFamilyProperties2(devices[i], &queueCount, queues.data());
		VkVideoCodecOperationFlagsKHR ops = 0;
		for (uint32_t q = 0; q < queueCount; ++q)
			if (queues[q].queueFamilyProperties.queueCount &&
				(queues[q].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR))
				ops |= video[q].videoCodecOperations;
		if (ops)
			result.push_back({std::to_string(i), props.deviceName, ops});
	}
	vkDestroyInstance(instance, nullptr);
	return result;
}
} // namespace codec_gui::gui
