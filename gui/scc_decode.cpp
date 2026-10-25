#include "scc_decode.hpp"
#include "raw_image_utils.hpp"
#include "h265/vulkan_video_decode.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace codec_gui::gui {
namespace {
namespace native=native_video::h265;
namespace decoder=native_video::h265::decoder;
void check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+" failed ("+std::to_string(result)+")");
}
uint32_t memory_type(VkPhysicalDevice physical,uint32_t bits,VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties;vkGetPhysicalDeviceMemoryProperties(physical,&properties);
    for(uint32_t i=0;i<properties.memoryTypeCount;++i)
        if((bits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags&required)==required) return i;
    throw std::runtime_error("SCC decoder could not allocate the required GPU/host memory");
}
struct Session {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};
    VkQueue video{},transfer{};uint32_t videoFamily=0,transferFamily=0;
    VkImage image{};VkDeviceMemory imageMemory{};
    VkBuffer staging{};VkDeviceMemory stagingMemory{};
    VkCommandPool pool{};VkSemaphore complete{};VkFence fence{};
    std::unique_ptr<decoder::Vulkan_Video_Decode_Context> context;
    ~Session() {
        if(device) vkDeviceWaitIdle(device);
        context.reset();
        if(fence) vkDestroyFence(device,fence,nullptr);
        if(complete) vkDestroySemaphore(device,complete,nullptr);
        if(pool) vkDestroyCommandPool(device,pool,nullptr);
        if(staging) vkDestroyBuffer(device,staging,nullptr);
        if(stagingMemory) vkFreeMemory(device,stagingMemory,nullptr);
        if(image) vkDestroyImage(device,image,nullptr);
        if(imageMemory) vkFreeMemory(device,imageMemory,nullptr);
        if(device) vkDestroyDevice(device,nullptr);
        if(instance) vkDestroyInstance(instance,nullptr);
    }
};
std::shared_ptr<const RawImage> decode(const EncodedImage& encoded,const std::string& index) {
    const auto bytes=std::span(reinterpret_cast<const uint8_t*>(encoded.hevcAnnexB.data()),encoded.hevcAnnexB.size());
    auto parsed=decoder::Annex_B_Parser{}.parse(bytes);
    if(!parsed) throw std::runtime_error("HEVC SCC parsing: "+parsed.error().message_);
    if(parsed->requires_unsupported_) throw std::runtime_error("HEVC SCC parsing: "+parsed->unsupported_message_);
    if(!parsed->sequence_.screen_content_ || parsed->decode_frames_.empty())
        throw std::runtime_error("The input is not a decodable HEVC SCC picture");
    const auto& sequence=parsed->sequence_;
    const auto& metadata=parsed->frame_metadata_.front();
    if(!metadata.irap_ || metadata.active_reference_mask_)
        throw std::runtime_error("SCC import requires a first independently decodable picture");
    const bool is420=sequence.chroma_format_==native::H265_Chroma_Format::YUV420;
    if((!is420 && sequence.chroma_format_!=native::H265_Chroma_Format::YUV444) ||
       (sequence.bit_depth_!=8 && sequence.bit_depth_!=10) || sequence.chroma_bit_depth_!=sequence.bit_depth_)
        throw std::runtime_error("HEVC SCC decoding supports 8/10-bit 4:2:0 and 4:4:4");
    const uint32_t width=sequence.max_width_,height=sequence.max_height_;
    const bool high=sequence.bit_depth_==10;
    VkVideoDecodeH265ProfileInfoKHR h265{.sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR,
        .stdProfileIdc=STD_VIDEO_H265_PROFILE_IDC_SCC_EXTENSIONS};
    VkVideoProfileInfoKHR profile{.sType=VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,.pNext=&h265,
        .videoCodecOperation=VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
        .chromaSubsampling=is420?VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR:VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR,
        .lumaBitDepth=high?VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR:VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .chromaBitDepth=high?VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR:VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR};
    Session s;
    VkApplicationInfo app{.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="codec_vis native SCC",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo instance{.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    check(vkCreateInstance(&instance,nullptr,&s.instance),"Creating SCC Vulkan instance");
    uint32_t count=0;check(vkEnumeratePhysicalDevices(s.instance,&count,nullptr),"Enumerating SCC Vulkan devices");
    std::vector<VkPhysicalDevice> devices(count);check(vkEnumeratePhysicalDevices(s.instance,&count,devices.data()),"Enumerating SCC Vulkan devices");
    auto capabilities=reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(vkGetInstanceProcAddr(s.instance,"vkGetPhysicalDeviceVideoCapabilitiesKHR"));
    if(!capabilities) throw std::runtime_error("The Vulkan driver does not expose video decoding");
    for(uint32_t i=0;i<count && !s.physical;++i) {
        if(!index.empty() && std::to_string(i)!=index) continue;
        VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(devices[i],&properties);
        if(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties2(devices[i],&families,nullptr);
        std::vector<VkQueueFamilyProperties2> queues(families);
        std::vector<VkQueueFamilyVideoPropertiesKHR> video(families);
        for(uint32_t q=0;q<families;++q) {
            video[q].sType=VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
            queues[q].sType=VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;queues[q].pNext=&video[q];
        }
        vkGetPhysicalDeviceQueueFamilyProperties2(devices[i],&families,queues.data());
        uint32_t v=UINT32_MAX,t=UINT32_MAX;
        for(uint32_t q=0;q<families;++q) {
            const auto flags=queues[q].queueFamilyProperties.queueFlags;
            if(queues[q].queueFamilyProperties.queueCount && (flags&VK_QUEUE_VIDEO_DECODE_BIT_KHR) &&
               (video[q].videoCodecOperations&VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR)) v=q;
            if(queues[q].queueFamilyProperties.queueCount && (flags&VK_QUEUE_TRANSFER_BIT)) t=q;
        }
        if(v==UINT32_MAX || t==UINT32_MAX) continue;
        VkVideoDecodeH265CapabilitiesKHR hc{.sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR};
        VkVideoDecodeCapabilitiesKHR dc{.sType=VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR,.pNext=&hc};
        VkVideoCapabilitiesKHR cap{.sType=VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,.pNext=&dc};
        if(capabilities(devices[i],&profile,&cap)!=VK_SUCCESS || width>cap.maxCodedExtent.width || height>cap.maxCodedExtent.height) continue;
        s.physical=devices[i];s.videoFamily=v;s.transferFamily=t;
    }
    if(!s.physical) throw std::runtime_error("No Vulkan device supports this HEVC SCC depth, chroma layout and image size");
    const float priority=1;
    std::vector<VkDeviceQueueCreateInfo> queues{{.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=s.videoFamily,.queueCount=1,.pQueuePriorities=&priority}};
    if(s.transferFamily!=s.videoFamily) queues.push_back({.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=s.transferFamily,.queueCount=1,.pQueuePriorities=&priority});
    const char* extensions[]={VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME};
    VkPhysicalDeviceVulkan13Features f13{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.synchronization2=VK_TRUE};
    VkPhysicalDeviceVulkan12Features f12{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&f13,.timelineSemaphore=VK_TRUE};
    VkPhysicalDeviceVulkan11Features f11{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=&f12,.samplerYcbcrConversion=VK_TRUE};
    VkDeviceCreateInfo device{.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f11,.queueCreateInfoCount=uint32_t(queues.size()),.pQueueCreateInfos=queues.data(),.enabledExtensionCount=3,.ppEnabledExtensionNames=extensions};
    check(vkCreateDevice(s.physical,&device,nullptr,&s.device),"Creating native SCC Vulkan device");
    vkGetDeviceQueue(s.device,s.videoFamily,0,&s.video);vkGetDeviceQueue(s.device,s.transferFamily,0,&s.transfer);
    VkVideoProfileListInfoKHR profiles{.sType=VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,.profileCount=1,.pProfiles=&profile};
    const VkFormat format=is420 ? (high?VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
                                : (high?VK_FORMAT_A2R10G10B10_UNORM_PACK32:VK_FORMAT_A8B8G8R8_UNORM_PACK32);
    const uint32_t familyIndices[]={s.videoFamily,s.transferFamily};
    VkImageCreateInfo image{.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.pNext=&profiles,.imageType=VK_IMAGE_TYPE_2D,
        .format=format,.extent={width,height,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR|VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode=s.videoFamily==s.transferFamily?VK_SHARING_MODE_EXCLUSIVE:VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount=s.videoFamily==s.transferFamily?0u:2u,.pQueueFamilyIndices=familyIndices};
    check(vkCreateImage(s.device,&image,nullptr,&s.image),"Allocating SCC decode image");
    VkMemoryRequirements req;vkGetImageMemoryRequirements(s.device,s.image,&req);
    VkMemoryDedicatedAllocateInfo dedicated{.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=s.image};
    VkMemoryAllocateInfo allocation{.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&dedicated,.allocationSize=req.size,.memoryTypeIndex=memory_type(s.physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(s.device,&allocation,nullptr,&s.imageMemory),"Allocating SCC image memory");
    check(vkBindImageMemory(s.device,s.image,s.imageMemory,0),"Binding SCC image memory");
    VkSemaphoreTypeCreateInfo timeline{.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo semaphore{.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&timeline};
    check(vkCreateSemaphore(s.device,&semaphore,nullptr,&s.complete),"Creating SCC completion semaphore");
    native::Vulkan_Device_Context borrowed{.instance_=s.instance,.physical_device_=s.physical,.device_=s.device,
        .queue_family_=s.videoFamily,.queue_=s.video,.video_codec_operations_=VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,.queue_mutex_=std::make_shared<std::mutex>()};
    auto context=decoder::Vulkan_Video_Decode_Context::create(sequence,borrowed);
    if(!context) throw std::runtime_error(context.error().message_);s.context=std::move(*context);
    native::Decode_Frame_Request request{.source_={1},.frame_={0},.independent_random_access_=true};
    request.output_.image_.width_=width;request.output_.image_.height_=height;request.output_.image_.format_=format;
    request.output_.image_.image_planes_.push_back({.image_=s.image,.format_=format,.aspect_=VK_IMAGE_ASPECT_COLOR_BIT,.width_=width,.height_=height});
    request.output_.vulkan_safe_to_write_={s.complete,0};
    request.output_.vulkan_decode_complete_={s.complete,1};
    auto decoded=s.context->decode(parsed->decode_frames_.front(),parsed->frame_states_.front(),metadata,bytes,0,std::move(request));
    if(!decoded) throw std::runtime_error(decoded.error().message_);

    const uint32_t bps=high?2:1;
    const VkDeviceSize chromaOffset=(VkDeviceSize(width)*height*bps+255)&~VkDeviceSize(255);
    const VkDeviceSize size=is420?chromaOffset+VkDeviceSize((width+1)/2)*((height+1)/2)*2*bps:VkDeviceSize(width)*height*4;
    VkBufferCreateInfo buffer{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateBuffer(s.device,&buffer,nullptr,&s.staging),"Creating SCC readback buffer");
    vkGetBufferMemoryRequirements(s.device,s.staging,&req);
    allocation.pNext=nullptr;allocation.allocationSize=req.size;allocation.memoryTypeIndex=memory_type(s.physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    check(vkAllocateMemory(s.device,&allocation,nullptr,&s.stagingMemory),"Allocating SCC readback memory");
    check(vkBindBufferMemory(s.device,s.staging,s.stagingMemory,0),"Binding SCC readback memory");
    VkCommandPoolCreateInfo pool{.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=s.transferFamily};
    check(vkCreateCommandPool(s.device,&pool,nullptr,&s.pool),"Creating SCC readback command pool");
    VkCommandBufferAllocateInfo commands{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=s.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;check(vkAllocateCommandBuffers(s.device,&commands,&command),"Allocating SCC readback command");
    VkCommandBufferBeginInfo begin{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    check(vkBeginCommandBuffer(command,&begin),"Beginning SCC readback");
    VkImageMemoryBarrier2 barrier{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=s.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkDependencyInfo dependency{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier};
    vkCmdPipelineBarrier2(command,&dependency);
    VkBufferImageCopy copy{.imageSubresource={is420?VK_IMAGE_ASPECT_PLANE_0_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={width,height,1}};
    vkCmdCopyImageToBuffer(command,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.staging,1,&copy);
    if(is420) {copy.bufferOffset=chromaOffset;copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_PLANE_1_BIT;copy.imageExtent={(width+1)/2,(height+1)/2,1};vkCmdCopyImageToBuffer(command,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.staging,1,&copy);}
    check(vkEndCommandBuffer(command),"Ending SCC readback");
    const uint64_t value=1;VkTimelineSemaphoreSubmitInfo wait{.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,.waitSemaphoreValueCount=1,.pWaitSemaphoreValues=&value};
    const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=&wait,.waitSemaphoreCount=1,.pWaitSemaphores=&s.complete,.pWaitDstStageMask=&stage,.commandBufferCount=1,.pCommandBuffers=&command};
    VkFenceCreateInfo fence{.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(s.device,&fence,nullptr,&s.fence),"Creating SCC readback fence");
    check(vkQueueSubmit(s.transfer,1,&submit,s.fence),"Submitting SCC readback");
    check(vkWaitForFences(s.device,1,&s.fence,VK_TRUE,30'000'000'000ull),"Waiting for SCC readback");
    void* mapping=nullptr;check(vkMapMemory(s.device,s.stagingMemory,0,VK_WHOLE_SIZE,0,&mapping),"Mapping SCC readback");
    VkMappedMemoryRange range{.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=s.stagingMemory,.size=VK_WHOLE_SIZE};
    const auto invalidate=vkInvalidateMappedMemoryRanges(s.device,1,&range);
    if(invalidate!=VK_SUCCESS) {vkUnmapMemory(s.device,s.stagingMemory);check(invalidate,"Invalidating SCC readback");}
    // The decoder's staging bytes are copied before unmapping, including on
    // allocation failure. No mapped pointer escapes this native boundary.
    std::vector<uint8_t> pixels;
    try {pixels.assign(static_cast<uint8_t*>(mapping),static_cast<uint8_t*>(mapping)+size);}
    catch(...) {vkUnmapMemory(s.device,s.stagingMemory);throw;}
    vkUnmapMemory(s.device,s.stagingMemory);
    auto raw=std::make_shared<RawImage>();
    const uint32_t x0=metadata.visible_x_,y0=metadata.visible_y_;
    raw->width=metadata.visible_width_;raw->height=metadata.visible_height_;
    if(raw->width<=0 || raw->height<=0 || uint64_t(x0)+raw->width>width || uint64_t(y0)+raw->height>height)
        throw std::runtime_error("SCC decoded crop lies outside the coded image");
    raw->format=is420?(high?PixelFormat::YUV420P10LE:PixelFormat::YUV420P8):(high?PixelFormat::YUV444P10LE:PixelFormat::YUV444P8);
    const auto& sps=parsed->decode_frames_.front().parameters_->sps_;
    if(sps.vui_parameters_present_flag && sps.vui_params.parsed) {
        const auto& vui=sps.vui_params;
        if(vui.aspect_ratio_info_present_flag && vui.par_n && vui.par_d) raw->pixelAspectRatio=PixelAspectRatio(vui.par_n,vui.par_d);
        if(vui.colour_description_present_flag) {raw->color.primaries=ColorPrimaries(vui.colour_primaries);raw->color.transfer=TransferCharacteristics(vui.transfer_characteristics);raw->color.matrix=MatrixCoefficients(vui.matrix_coefficients);}
        raw->color.range=vui.video_full_range_flag?ColorRange::Full:ColorRange::Limited;
        if(is420 && vui.chroma_loc_info_present_flag) raw->color.chroma420Location=Chroma420SampleLocation(vui.chroma_sample_loc_type_top_field);
    }
    for(int p=0;p<3;++p) {
        auto& plane=raw->planes[p];plane.strideBytes=plane_width(*raw,p)*bps;plane.bytes.resize(size_t(plane.strideBytes)*plane_height(*raw,p));
        for(int y=0;y<plane_height(*raw,p);++y) for(int x=0;x<plane_width(*raw,p);++x) {
            uint32_t sample=0;
            if(is420) {
                const size_t at=p?chromaOffset+(size_t(y+y0/2)*((width+1)/2)*2+(x+x0/2)*2+p-1)*bps:(size_t(y+y0)*width+x+x0)*bps;
                sample=pixels[at];if(high) sample=(sample|(uint32_t(pixels[at+1])<<8))>>6;
            } else {
                const size_t at=(size_t(y+y0)*width+x+x0)*4;
                if(high) {uint32_t word;std::memcpy(&word,pixels.data()+at,4);sample=(word>>(p==0?10:p==1?0:20))&1023;}
                else sample=pixels[at+(p==0?2:p==1?1:0)];
            }
            put_sample(plane,x,y,bps,sample,high?1023:255);
        }
    }
    return raw;
}
} // namespace
bool hevc_is_scc(const EncodedImage& encoded) {
    const auto* b=reinterpret_cast<const uint8_t*>(encoded.hevcAnnexB.data());
    for(size_t i=0;i+6<encoded.hevcAnnexB.size();++i)
        if(!b[i] && !b[i+1] && b[i+2]==1 && ((b[i+3]>>1)&63)==33) return (b[i+6]&31)==9;
    return false;
}
DecodeResult decode_scc_vulkan(const EncodedImage& encoded,const std::string& deviceIndex) {
    try {return {.image=decode(encoded,deviceIndex),.error={}};}
    catch(const std::exception& e) {return {.image=nullptr,.error="HEVC SCC Vulkan decode: "+std::string(e.what())};}
}
} // namespace codec_gui::gui
