/*
 * AHardwareBuffer backed swapchain images for the Vulkan wrapper
 * (Termux / Winlator X server). The X11 backend hands the AHB to the X
 * server through a unix socket instead of using DRI3.
 *
 * Based on the wrapper-25 (wsi_common_android.c) and bionic wrapper
 * (wsi_common_ahardware_buffer.c) implementations.
 */
#include "wsi_common.h"
#include "wsi_common_private.h"
#include "vk_log.h"
#include "util/log.h"

#ifdef __TERMUX__

#include <android/hardware_buffer.h>

#define AHB_FORMAT_B8G8R8A8_UNORM 5 /* AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM (hidden in NDK) */

static uint32_t
to_ahardware_buffer_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB:
      return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
      return AHB_FORMAT_B8G8R8A8_UNORM;
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
      return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
      return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
   case VK_FORMAT_R16G16B16A16_SFLOAT:
      return AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
   default:
      /* Unknown formats go through the blit path (RGBA8 AHB). */
      return 0;
   }
}

/* Can the driver import a plain BGRA8 AHB as a render target? If not,
 * render into a normal image and copy into an RGBA8 AHB. */
enum wsi_swapchain_blit_type
wsi_get_android_blit_type(const struct wsi_device *wsi,
                          const struct wsi_base_image_params *params,
                          VkDevice device)
{
   if (wsi->needs_blit)
      return WSI_SWAPCHAIN_IMAGE_BLIT;

   AHardwareBuffer *ahb = NULL;
   if (AHardwareBuffer_allocate(&(AHardwareBuffer_Desc) {
         .width = 64,
         .height = 64,
         .layers = 1,
         .format = wsi->force_rgba8_unorm_first ? AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
                                                : AHB_FORMAT_B8G8R8A8_UNORM,
         .usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                  AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN },
         &ahb) != 0) {
      mesa_logw("wsi: AHardwareBuffer probe allocation failed, blitting");
      return WSI_SWAPCHAIN_IMAGE_BLIT;
   }

   VkAndroidHardwareBufferFormatPropertiesANDROID format_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
   };
   VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
      .pNext = &format_props,
   };
   VkResult result = wsi->GetAndroidHardwareBufferPropertiesANDROID(device, ahb, &ahb_props);
   AHardwareBuffer_release(ahb);
   if (result != VK_SUCCESS || format_props.format == VK_FORMAT_UNDEFINED) {
      mesa_logw("wsi: AHB properties unusable (%d, format %d), blitting",
                result, format_props.format);
      return WSI_SWAPCHAIN_IMAGE_BLIT;
   }

   VkPhysicalDeviceExternalImageFormatInfo external_format_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
   };
   VkPhysicalDeviceImageFormatInfo2 format_info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &external_format_info,
      .format = format_props.format,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
   };
   VkExternalImageFormatProperties external_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
   };
   VkImageFormatProperties2 image_props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      .pNext = &external_props,
   };
   result = wsi->GetPhysicalDeviceImageFormatProperties2(wsi->pdevice, &format_info, &image_props);
   if (result != VK_SUCCESS ||
       !(external_props.externalMemoryProperties.externalMemoryFeatures &
         VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
      mesa_logw("wsi: AHB render targets not importable, blitting");
      return WSI_SWAPCHAIN_IMAGE_BLIT;
   }

   return WSI_SWAPCHAIN_NO_BLIT;
}

static VkResult
import_ahb_memory(const struct wsi_swapchain *chain,
                  AHardwareBuffer *ahb, VkImage image,
                  VkDeviceMemory *out_memory)
{
   const struct wsi_device *wsi = chain->wsi;

   VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
   };
   VkResult result = wsi->GetAndroidHardwareBufferPropertiesANDROID(chain->device, ahb, &ahb_props);
   if (result != VK_SUCCESS)
      return result;

   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image,
   };
   const VkImportAndroidHardwareBufferInfoANDROID import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
      .pNext = &dedicated,
      .buffer = ahb,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_info,
      .allocationSize = ahb_props.allocationSize,
      .memoryTypeIndex = wsi_select_device_memory_type(wsi, ahb_props.memoryTypeBits),
   };
   return wsi->AllocateMemory(chain->device, &memory_info, &chain->alloc, out_memory);
}

/* No blit: the swapchain image itself is backed by the AHB allocated in
 * wsi_create_image(). */
static VkResult
wsi_create_ahardware_buffer_image_mem(const struct wsi_swapchain *chain,
                                      const struct wsi_image_info *info,
                                      struct wsi_image *image)
{
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   VkAndroidHardwareBufferFormatPropertiesANDROID format_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
   };
   VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
      .pNext = &format_props,
   };
   result = wsi->GetAndroidHardwareBufferPropertiesANDROID(
      chain->device, image->ahardware_buffer, &ahb_props);
   if (result != VK_SUCCESS)
      return vk_errorf(NULL, result, "wsi: GetAndroidHardwareBufferProperties failed");

   /* The image was created before the AHB format was known: recreate it
    * with the driver's format for this buffer. */
   VkImageCreateInfo create = info->create;
   if (format_props.format != VK_FORMAT_UNDEFINED)
      create.format = format_props.format;
   if (format_props.externalFormat)
      create.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

   VkImage new_image;
   result = wsi->CreateImage(chain->device, &create, &chain->alloc, &new_image);
   if (result != VK_SUCCESS)
      return vk_errorf(NULL, result, "wsi: CreateImage for AHB failed");
   wsi->DestroyImage(chain->device, image->image, &chain->alloc);
   image->image = new_image;

   result = import_ahb_memory(chain, image->ahardware_buffer, image->image, &image->memory);
   if (result != VK_SUCCESS)
      return vk_errorf(NULL, result, "wsi: AHB import failed");

   image->num_planes = 1;
   image->drm_modifier = 1255; /* arbitrary, not DRM: marks AHB images */
   return VK_SUCCESS;
}

/* Blit: render into a regular image, copy into an RGBA8 AHB (linear). */
static VkResult
wsi_create_ahardware_buffer_blit_context(const struct wsi_swapchain *chain,
                                         const struct wsi_image_info *info,
                                         struct wsi_image *image)
{
   assert(chain->blit.type == WSI_SWAPCHAIN_IMAGE_BLIT);
   const struct wsi_device *wsi = chain->wsi;
   VkResult result;

   VkAndroidHardwareBufferFormatPropertiesANDROID format_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
   };
   VkAndroidHardwareBufferPropertiesANDROID ahb_props = {
      .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
      .pNext = &format_props,
   };
   result = wsi->GetAndroidHardwareBufferPropertiesANDROID(
      chain->device, image->ahardware_buffer, &ahb_props);
   if (result != VK_SUCCESS)
      return vk_errorf(NULL, result, "wsi: GetAndroidHardwareBufferProperties failed");

   const VkExternalMemoryImageCreateInfo external_memory_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
   };
   const VkImageCreateInfo blit_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external_memory_info,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format_props.format != VK_FORMAT_UNDEFINED ?
                format_props.format : VK_FORMAT_R8G8B8A8_UNORM,
      .extent = info->create.extent,
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .queueFamilyIndexCount = info->create.queueFamilyIndexCount,
      .pQueueFamilyIndices = info->create.pQueueFamilyIndices,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   result = wsi->CreateImage(chain->device, &blit_info, &chain->alloc, &image->blit.image);
   if (result != VK_SUCCESS)
      return vk_errorf(NULL, result, "wsi: blit image creation failed");

   result = import_ahb_memory(chain, image->ahardware_buffer, image->blit.image, &image->blit.memory);
   if (result != VK_SUCCESS)
      return vk_errorf(NULL, result, "wsi: blit AHB import failed");

   result = wsi->BindImageMemory(chain->device, image->blit.image, image->blit.memory, 0);
   if (result != VK_SUCCESS)
      return result;

   VkMemoryRequirements reqs;
   wsi->GetImageMemoryRequirements(chain->device, image->image, &reqs);
   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->image,
   };
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated,
      .allocationSize = reqs.size,
      .memoryTypeIndex = wsi_select_device_memory_type(wsi, reqs.memoryTypeBits),
   };
   result = wsi->AllocateMemory(chain->device, &memory_info, &chain->alloc, &image->memory);
   if (result != VK_SUCCESS)
      return result;

   image->num_planes = 1;
   image->drm_modifier = 1255;
   return VK_SUCCESS;
}

VkResult
wsi_configure_android_image(const struct wsi_swapchain *chain,
                            const VkSwapchainCreateInfoKHR *pCreateInfo,
                            const struct wsi_base_image_params *params,
                            struct wsi_image_info *info)
{
   assert(params->image_type == WSI_IMAGE_TYPE_ANDROID);
   assert(chain->blit.type == WSI_SWAPCHAIN_NO_BLIT ||
          chain->blit.type == WSI_SWAPCHAIN_IMAGE_BLIT);

   bool blit = chain->blit.type == WSI_SWAPCHAIN_IMAGE_BLIT;
   const VkExternalMemoryHandleTypeFlags handle_type =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

   uint32_t ahb_format = to_ahardware_buffer_format(pCreateInfo->imageFormat);
   if (!blit && ahb_format == 0) {
      mesa_loge("wsi: swapchain format %d has no AHB equivalent", pCreateInfo->imageFormat);
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }

   VkResult result = wsi_configure_image(chain, pCreateInfo,
                                         blit ? 0 : handle_type, info);
   if (result != VK_SUCCESS)
      return result;

   /* Let the driver add the AHB usage bits it needs for this image. */
   uint64_t driver_usage = 0;
   if (!blit) {
      VkPhysicalDeviceExternalImageFormatInfo external_format_info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
         .handleType = handle_type,
      };
      VkPhysicalDeviceImageFormatInfo2 format_info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
         .pNext = &external_format_info,
         .format = info->create.format,
         .type = VK_IMAGE_TYPE_2D,
         .tiling = info->create.tiling,
         .usage = info->create.usage,
         .flags = info->create.flags,
      };
      VkAndroidHardwareBufferUsageANDROID ahb_usage = {
         .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID,
      };
      VkImageFormatProperties2 format_props = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
         .pNext = &ahb_usage,
      };
      if (chain->wsi->GetPhysicalDeviceImageFormatProperties2(
             chain->wsi->pdevice, &format_info, &format_props) == VK_SUCCESS)
         driver_usage = ahb_usage.androidHardwareBufferUsage;
   }

   info->ahardware_buffer_desc = vk_zalloc(&chain->alloc, sizeof(AHardwareBuffer_Desc), 8,
                                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!info->ahardware_buffer_desc) {
      wsi_destroy_image_info(chain, info);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *info->ahardware_buffer_desc = (AHardwareBuffer_Desc) {
      .width = pCreateInfo->imageExtent.width,
      .height = pCreateInfo->imageExtent.height,
      .layers = pCreateInfo->imageArrayLayers,
      .format = blit ? AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM : ahb_format,
      .usage = (driver_usage & ~(uint64_t) AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER) |
               AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
               AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
               AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
   };

   if (blit) {
      wsi_configure_image_blit_image(chain, info);
      info->create_mem = wsi_create_ahardware_buffer_blit_context;
   } else {
      info->create_mem = wsi_create_ahardware_buffer_image_mem;
   }

   return VK_SUCCESS;
}

#endif /* __TERMUX__ */
