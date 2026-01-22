/*
 * Copyright © 2016 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_formats.h"
#include "radv_meta.h"
#include "radv_sdma.h"
#include "vk_format.h"

#include "main/formats.h"
#include "main/texcompress_etc.h"
#include "util/format/texcompress_bptc_tmp.h"
#include "vk_common_entrypoints.h"

static VkFormat
vk_format_for_size(int bs)
{
   switch (bs) {
   case 1:
      return VK_FORMAT_R8_UINT;
   case 2:
      return VK_FORMAT_R8G8_UINT;
   case 4:
      return VK_FORMAT_R8G8B8A8_UINT;
   case 8:
      return VK_FORMAT_R16G16B16A16_UINT;
   case 12:
      return VK_FORMAT_R32G32B32_UINT;
   case 16:
      return VK_FORMAT_R32G32B32A32_UINT;
   default:
      unreachable("Invalid format block size");
   }
}

static struct radv_meta_blit2d_surf
blit_surf_for_image_level_layer(struct radv_image *image, VkImageLayout layout, const VkImageSubresourceLayers *subres,
                                VkImageAspectFlags aspect_mask)
{
   VkFormat format = radv_get_aspect_format(image, aspect_mask);

   if (!radv_dcc_enabled(image, subres->mipLevel) && !(radv_image_is_tc_compat_htile(image)))
      format = vk_format_for_size(vk_format_get_blocksize(format));

   format = vk_format_no_srgb(format);

   return (struct radv_meta_blit2d_surf){
      .format = format,
      .bs = vk_format_get_blocksize(format),
      .level = subres->mipLevel,
      .layer = subres->baseArrayLayer,
      .image = image,
      .aspect_mask = aspect_mask,
      .current_layout = layout,
   };
}

static bool
alloc_transfer_temp_bo(struct radv_cmd_buffer *cmd_buffer)
{
   if (cmd_buffer->transfer.copy_temp)
      return true;

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const VkResult r =
      radv_bo_create(device, &cmd_buffer->vk.base, RADV_SDMA_TRANSFER_TEMP_BYTES, 4096, RADEON_DOMAIN_VRAM,
                     RADEON_FLAG_NO_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING, RADV_BO_PRIORITY_SCRATCH, 0, true,
                     &cmd_buffer->transfer.copy_temp);

   if (r != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, r);
      return false;
   }

   radv_cs_add_buffer(device->ws, cmd_buffer->cs, cmd_buffer->transfer.copy_temp);
   return true;
}

static void
transfer_copy_buffer_image(struct radv_cmd_buffer *cmd_buffer, struct radv_buffer *buffer, struct radv_image *image,
                           const VkBufferImageCopy2 *region, bool to_image)
{
   const struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   const VkImageAspectFlags aspect_mask = region->imageSubresource.aspectMask;
   const unsigned binding_idx = image->disjoint ? radv_plane_from_aspect(aspect_mask) : 0;

   radv_cs_add_buffer(device->ws, cs, image->bindings[binding_idx].bo);
   radv_cs_add_buffer(device->ws, cs, buffer->bo);

   struct radv_sdma_surf buf = radv_sdma_get_buf_surf(buffer, image, region, aspect_mask);
   const struct radv_sdma_surf img =
      radv_sdma_get_surf(device, image, region->imageSubresource, region->imageOffset, aspect_mask);
   const VkExtent3D extent = radv_sdma_get_copy_extent(image, region->imageSubresource, region->imageExtent);

   if (radv_sdma_use_unaligned_buffer_image_copy(device, &buf, &img, extent)) {
      if (!alloc_transfer_temp_bo(cmd_buffer))
         return;

      radv_sdma_copy_buffer_image_unaligned(device, cs, &buf, &img, extent, cmd_buffer->transfer.copy_temp, to_image);
      return;
   }

   radv_sdma_copy_buffer_image(device, cs, &buf, &img, extent, to_image);
}

// 函数返回 image 所占的字节数
static size_t
radv_get_texel_count(const VkBufferImageCopy2* pRegions, struct radv_image* image)
{
   if (image == NULL) {
      return 0;
   }
   const VkExtent3D bufferExtent = {
      .width  = pRegions->bufferRowLength ? pRegions->bufferRowLength : pRegions->imageExtent.width,
      .height = pRegions->bufferImageHeight ? pRegions->bufferImageHeight : pRegions->imageExtent.height,
   };
   const VkExtent3D buf_extent_el = vk_image_extent_to_elements(&image->vk, bufferExtent);
   size_t w = buf_extent_el.width;
   size_t h = buf_extent_el.height; 
   int bs = vk_format_get_blocksize(image->vk.format);   
   return w * h * bs;
}

static mesa_format
radv_translate_etc_to_mesa_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
      return MESA_FORMAT_ETC2_RGB8;
   case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
      return MESA_FORMAT_ETC2_SRGB8;
   case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
      return MESA_FORMAT_ETC2_RGB8_PUNCHTHROUGH_ALPHA1;
   case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
      return MESA_FORMAT_ETC2_SRGB8_PUNCHTHROUGH_ALPHA1;
   case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
      return MESA_FORMAT_ETC2_RGBA8_EAC;
   case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
      return MESA_FORMAT_ETC2_SRGB8_ALPHA8_EAC;
   default:
      return MESA_FORMAT_ETC2_RGB8;
   }
}

static mesa_format
radv_translate_vkformat_to_mesa_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8_UNORM:
      return MESA_FORMAT_RGB_UNORM8;
   case VK_FORMAT_R8G8B8A8_UNORM:
      return MESA_FORMAT_R8G8B8A8_UNORM;
   case VK_FORMAT_R8G8B8_SRGB:
      return MESA_FORMAT_BGR_SRGB8;
   case VK_FORMAT_R8G8B8A8_SRGB:
      return MESA_FORMAT_R8G8B8A8_SRGB;
   case VK_FORMAT_BC7_UNORM_BLOCK:
      return MESA_FORMAT_BPTC_RGBA_UNORM;
   case VK_FORMAT_BC7_SRGB_BLOCK:
      return MESA_FORMAT_BPTC_SRGB_ALPHA_UNORM;
   default:
      return MESA_FORMAT_RGB_UNORM8;
   }
}

static void
prepare_bufferMemory(struct radv_device* device, struct radv_buffer *buffer,
                     struct radv_image *image, const VkBufferImageCopy2 *region,
                     void** pDstData, VkBuffer* dstBuffer, VkDeviceMemory* memory_h, bool isDecodeBuffer) {
      int dstBufferSize = radv_get_texel_count(region, image);

      if (isDecodeBuffer && image->isNeedSoftEncode) {
         VkFormat tempFormat = image->vk.format;
         image->vk.format = image->unpackETCFormat;
         dstBufferSize = radv_get_texel_count(region, image);
         image->vk.format = tempFormat;
      }
      if(VK_SUCCESS != radv_CreateBuffer(radv_device_to_handle(device), &(VkBufferCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.flags = buffer->vk.create_flags,
				.size = dstBufferSize,
				.usage = buffer->vk.usage,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE
            }, NULL, dstBuffer)) {
			return;
		}

      VK_FROM_HANDLE(radv_buffer, _dstbuffer, *dstBuffer);
      if (isDecodeBuffer) {
         buffer->unpack_buffer_for_ct = _dstbuffer;
      } else {
         buffer->compressed_buffer_for_BC = _dstbuffer;
      }

		VkMemoryRequirements2 memoryRequirements2 = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
         };
      vk_common_GetBufferMemoryRequirements2(radv_device_to_handle(device),
         &(VkBufferMemoryRequirementsInfo2) {
            .buffer = *dstBuffer,
         }, &memoryRequirements2);
		int memory_type_index = -1;
      const struct radv_physical_device *pdev = radv_device_physical(device);
		for (int i = 0; i < pdev->memory_properties.memoryTypeCount; ++i) {
			bool is_local = !!(pdev->memory_properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
			if (is_local) {
				memory_type_index = i;
				break;
			}
		}

      // 分配设备内存，返回设备内存地址 memory_h
		if(VK_SUCCESS != radv_AllocateMemory(radv_device_to_handle(device),
			&(VkMemoryAllocateInfo) {
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.pNext = NULL,
				.allocationSize = memoryRequirements2.memoryRequirements.size,
				.memoryTypeIndex = memory_type_index,
		  }, NULL, memory_h)) {
			return;
		}

      VK_FROM_HANDLE(radv_device_memory, _mem, *memory_h);
      if (isDecodeBuffer) {
         buffer->unpack_mem_for_ct = _mem;
      } else {
         buffer->compressed_mem_for_BC = _mem;
      }

      // 将 buffer 绑定到设备内存
		if(VK_SUCCESS != radv_BindBufferMemory2(radv_device_to_handle(device), 1,
         &(VkBindBufferMemoryInfo) {
            .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
            .buffer = *dstBuffer,
            .memory = *memory_h,
            .memoryOffset = 0,
         })) {
			return;
		}
      VkMemoryMapInfoKHR pMemoryMapInfo = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR,
         .pNext = NULL,
         .flags = 0,
         .memory = *memory_h,
         .offset = 0,
         .size = dstBufferSize,
      };
		// 将设备内存 memory_h 映射到 pDstData
		radv_MapMemory2KHR(radv_device_to_handle(device), &pMemoryMapInfo, pDstData);
}

static void
copy_buffer_to_image(struct radv_cmd_buffer *cmd_buffer, struct radv_buffer *buffer, struct radv_image *image,
                     VkImageLayout layout, const VkBufferImageCopy2 *region)
{
   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER) {
      transfer_copy_buffer_image(cmd_buffer, buffer, image, region, true);
      return;
   }

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   VkBuffer dstBuffer;
   struct radv_buffer* radvDstBuffer = buffer;
   VkDeviceMemory memory_h;
   void* pDstData = NULL;
   void* pSrcData = NULL;

   VkBuffer dstBCnBuffer;
   VkDeviceMemory memory_h_BCn;
   void* pDstBCnData = NULL;

   struct radv_meta_saved_state saved_state;
   bool cs;

   /* The Vulkan 1.0 spec says "dstImage must have a sample count equal to
    * VK_SAMPLE_COUNT_1_BIT."
    */
   assert(image->vk.samples == 1);

   cs = cmd_buffer->qf == RADV_QUEUE_COMPUTE || !radv_image_is_renderable(device, image);

   /* VK_EXT_conditional_rendering says that copy commands should not be
    * affected by conditional rendering.
    */
   radv_meta_save(&saved_state, cmd_buffer,
                  (cs ? RADV_META_SAVE_COMPUTE_PIPELINE : RADV_META_SAVE_GRAPHICS_PIPELINE) | RADV_META_SAVE_CONSTANTS |
                     RADV_META_SAVE_DESCRIPTORS | RADV_META_SUSPEND_PREDICATING);

   pSrcData = radv_buffer_map(device->ws, buffer->bo) + buffer->offset;
   // 申请软解 buffer 内存
   if (image->isNeedSoftDecode) {
      prepare_bufferMemory(device, buffer, image, region, &pDstData, &dstBuffer, &memory_h, true);
   }
   // 申请软编 buffer 内存
   if (image->isNeedSoftEncode) {
      prepare_bufferMemory(device, buffer, image, region, &pDstBCnData, &dstBCnBuffer, &memory_h_BCn, false);
   }

   void* realDstData = pDstData;
   void* realDstBcData = pDstBCnData;

   /**
    * From the Vulkan 1.0.6 spec: 18.3 Copying Data Between Images
    *    extent is the size in texels of the source image to copy in width,
    *    height and depth. 1D images use only x and width. 2D images use x, y,
    *    width and height. 3D images use x, y, z, width, height and depth.
    *
    *
    * Also, convert the offsets and extent from units of texels to units of
    * blocks - which is the highest resolution accessible in this command.
    */
   const VkOffset3D img_offset_el = vk_image_offset_to_elements(&image->vk, region->imageOffset);

   /* Start creating blit rect */
   const VkExtent3D img_extent_el = vk_image_extent_to_elements(&image->vk, region->imageExtent);
   struct radv_meta_blit2d_rect rect = {
      .width = img_extent_el.width,
      .height = img_extent_el.height,
   };

   /* Create blit surfaces */
   struct radv_meta_blit2d_surf img_bsurf =
      blit_surf_for_image_level_layer(image, layout, &region->imageSubresource, region->imageSubresource.aspectMask);

   if (!radv_is_buffer_format_supported(img_bsurf.format, NULL)) {
      uint32_t queue_mask = radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf);
      bool compressed =
         radv_layout_dcc_compressed(device, image, region->imageSubresource.mipLevel, layout, queue_mask);
      if (compressed) {
         radv_describe_barrier_start(cmd_buffer, RGP_BARRIER_UNKNOWN_REASON);

         radv_decompress_dcc(cmd_buffer, image,
                             &(VkImageSubresourceRange){
                                .aspectMask = region->imageSubresource.aspectMask,
                                .baseMipLevel = region->imageSubresource.mipLevel,
                                .levelCount = 1,
                                .baseArrayLayer = region->imageSubresource.baseArrayLayer,
                                .layerCount = vk_image_subresource_layer_count(&image->vk, &region->imageSubresource),
                             });
         img_bsurf.disable_compression = true;

         radv_describe_barrier_end(cmd_buffer);
      }
      img_bsurf.format = vk_format_for_size(vk_format_get_blocksize(img_bsurf.format));
   }

   const VkExtent3D bufferExtent = {
      .width  = region->bufferRowLength ? region->bufferRowLength : region->imageExtent.width,
      .height = region->bufferImageHeight ? region->bufferImageHeight : region->imageExtent.height,
   };
   const VkExtent3D buf_extent_el = vk_image_extent_to_elements(&image->vk, bufferExtent);

   uint32_t offset = region->bufferOffset;
   mesa_format unpackETCFormat;
   if (image->isNeedSoftDecode) {
      // translate vkFormat to MESA format
      unpackETCFormat = radv_translate_vkformat_to_mesa_format(image->unpackETCFormat);
      mesa_format srcFormat = radv_translate_etc_to_mesa_format(image->srcFormat);
      bool bgra = false;
      void * realSrcData = pSrcData;
      realSrcData += region->bufferOffset;
      unsigned dst_stride = _mesa_format_row_stride(unpackETCFormat, region->imageExtent.width);
      unsigned src_stride =_mesa_format_row_stride(srcFormat, bufferExtent.width);
      _mesa_unpack_etc2_format(realDstData, dst_stride, realSrcData, src_stride, bufferExtent.width, bufferExtent.height, srcFormat, bgra);
      if (image->isNeedSoftEncode) {
         VK_FROM_HANDLE(radv_buffer, _tmp, dstBuffer);
         radvDstBuffer = _tmp;
         offset = realDstData - pDstData;
      }
   }
   if (image->isNeedSoftEncode) {
      mesa_format srcFormat;
      void * realSrcData;
      if (image->isNeedSoftDecode) {
         srcFormat = unpackETCFormat;
         realSrcData = realDstData;
      } else {
         srcFormat = radv_translate_vkformat_to_mesa_format(image->srcFormat);
         realSrcData = pSrcData;
         realSrcData += region->bufferOffset;
      }
      mesa_format dstFormat = radv_translate_vkformat_to_mesa_format(image->vk.format);
      unsigned dst_stride = _mesa_format_row_stride(dstFormat, region->imageExtent.width);
      unsigned src_stride = _mesa_format_row_stride(srcFormat, bufferExtent.width);

      compress_rgba_unorm(bufferExtent.width, bufferExtent.height, realSrcData, src_stride, realDstBcData, dst_stride);

      VK_FROM_HANDLE(radv_buffer, _tmp, dstBCnBuffer);
      radvDstBuffer = _tmp;
      offset = realDstBcData - pDstBCnData;
   }

   const struct vk_image_buffer_layout buf_layout = vk_image_buffer_copy_layout(&image->vk, region);
   struct radv_meta_blit2d_buffer buf_bsurf = {
      .bs = img_bsurf.bs,
      .format = img_bsurf.format,
      .buffer = radvDstBuffer,
      .offset = offset,
      .pitch = buf_layout.row_stride_B / buf_layout.element_size_B,
   };

   if (image->vk.image_type == VK_IMAGE_TYPE_3D)
      img_bsurf.layer = img_offset_el.z;
   /* Loop through each 3D or array slice */
   unsigned num_slices_3d = img_extent_el.depth;
   unsigned num_slices_array = vk_image_subresource_layer_count(&image->vk, &region->imageSubresource);
   unsigned slice_3d = 0;
   unsigned slice_array = 0;
   while (slice_3d < num_slices_3d && slice_array < num_slices_array) {

      rect.dst_x = img_offset_el.x;
      rect.dst_y = img_offset_el.y;

      /* Perform Blit */
      if (cs) {
         radv_meta_buffer_to_image_cs(cmd_buffer, &buf_bsurf, &img_bsurf, &rect);
      } else {
         radv_meta_blit2d(cmd_buffer, NULL, &buf_bsurf, &img_bsurf, &rect);
      }

      /* Once we've done the blit, all of the actual information about
       * the image is embedded in the command buffer so we can just
       * increment the offset directly in the image effectively
       * re-binding it to different backing memory.
       */
      buf_bsurf.offset += buf_layout.image_stride_B;
      if (image->isNeedSoftDecode) {
         realDstData += buf_extent_el.width * buf_extent_el.height * buf_bsurf.bs;
      }
      if (image->isNeedSoftEncode) {
         realDstBcData += buf_extent_el.width * buf_extent_el.height * buf_bsurf.bs;
      }
      img_bsurf.layer++;
      if (image->vk.image_type == VK_IMAGE_TYPE_3D)
         slice_3d++;
      else
         slice_array++;
   }
   pSrcData = NULL;
   VkMemoryUnmapInfoKHR pMemoryUnmapInfo = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
      .pNext = NULL,
      .flags = 0,
   };
   if (image->isNeedSoftDecode) {
      pMemoryUnmapInfo.memory = memory_h;
      radv_UnmapMemory2KHR(radv_device_to_handle(device), &pMemoryUnmapInfo);
   }
   if (image->isNeedSoftEncode) {
      pMemoryUnmapInfo.memory = memory_h_BCn;
      radv_UnmapMemory2KHR(radv_device_to_handle(device), &pMemoryUnmapInfo);
      // 压缩纹理的原始纹理由ETC2纹理解压而来因此需要释放掉内存
      if (buffer->unpack_mem_for_ct) {
         radv_free_memory(device, NULL, buffer->unpack_mem_for_ct);
         buffer->unpack_mem_for_ct = NULL;
      }
   }

   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyBufferToImage2(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2 *pCopyBufferToImageInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_buffer, src_buffer, pCopyBufferToImageInfo->srcBuffer);
   VK_FROM_HANDLE(radv_image, dst_image, pCopyBufferToImageInfo->dstImage);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   for (unsigned r = 0; r < pCopyBufferToImageInfo->regionCount; r++) {
      copy_buffer_to_image(cmd_buffer, src_buffer, dst_image, pCopyBufferToImageInfo->dstImageLayout,
                           &pCopyBufferToImageInfo->pRegions[r]);
   }

   if (radv_is_format_emulated(pdev, dst_image->vk.format)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH |
                                      radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                            VK_ACCESS_TRANSFER_WRITE_BIT, dst_image) |
                                      radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                            VK_ACCESS_TRANSFER_READ_BIT, dst_image);

      const enum util_format_layout format_layout = vk_format_description(dst_image->vk.format)->layout;
      for (unsigned r = 0; r < pCopyBufferToImageInfo->regionCount; r++) {
         if (format_layout == UTIL_FORMAT_LAYOUT_ASTC) {
            radv_meta_decode_astc(cmd_buffer, dst_image, pCopyBufferToImageInfo->dstImageLayout,
                                  &pCopyBufferToImageInfo->pRegions[r].imageSubresource,
                                  pCopyBufferToImageInfo->pRegions[r].imageOffset,
                                  pCopyBufferToImageInfo->pRegions[r].imageExtent);
         } else {
            radv_meta_decode_etc(cmd_buffer, dst_image, pCopyBufferToImageInfo->dstImageLayout,
                                 &pCopyBufferToImageInfo->pRegions[r].imageSubresource,
                                 pCopyBufferToImageInfo->pRegions[r].imageOffset,
                                 pCopyBufferToImageInfo->pRegions[r].imageExtent);
         }
      }
   }
}

static void
copy_image_to_buffer(struct radv_cmd_buffer *cmd_buffer, struct radv_buffer *buffer, struct radv_image *image,
                     VkImageLayout layout, const VkBufferImageCopy2 *region)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER) {
      transfer_copy_buffer_image(cmd_buffer, buffer, image, region, false);
      return;
   }

   struct radv_meta_saved_state saved_state;

   /* VK_EXT_conditional_rendering says that copy commands should not be
    * affected by conditional rendering.
    */
   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS | RADV_META_SAVE_DESCRIPTORS |
                     RADV_META_SUSPEND_PREDICATING);

   /**
    * From the Vulkan 1.0.6 spec: 18.3 Copying Data Between Images
    *    extent is the size in texels of the source image to copy in width,
    *    height and depth. 1D images use only x and width. 2D images use x, y,
    *    width and height. 3D images use x, y, z, width, height and depth.
    *
    *
    * Also, convert the offsets and extent from units of texels to units of
    * blocks - which is the highest resolution accessible in this command.
    */
   const VkOffset3D img_offset_el = vk_image_offset_to_elements(&image->vk, region->imageOffset);
   const VkExtent3D bufferExtent = {
      .width = region->bufferRowLength ? region->bufferRowLength : region->imageExtent.width,
      .height = region->bufferImageHeight ? region->bufferImageHeight : region->imageExtent.height,
   };
   const VkExtent3D buf_extent_el = vk_image_extent_to_elements(&image->vk, bufferExtent);

   /* Start creating blit rect */
   const VkExtent3D img_extent_el = vk_image_extent_to_elements(&image->vk, region->imageExtent);
   struct radv_meta_blit2d_rect rect = {
      .width = img_extent_el.width,
      .height = img_extent_el.height,
   };

   /* Create blit surfaces */
   struct radv_meta_blit2d_surf img_info =
      blit_surf_for_image_level_layer(image, layout, &region->imageSubresource, region->imageSubresource.aspectMask);

   if (!radv_is_buffer_format_supported(img_info.format, NULL)) {
      uint32_t queue_mask = radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf);
      bool compressed =
         radv_layout_dcc_compressed(device, image, region->imageSubresource.mipLevel, layout, queue_mask);
      if (compressed) {
         radv_describe_barrier_start(cmd_buffer, RGP_BARRIER_UNKNOWN_REASON);

         radv_decompress_dcc(cmd_buffer, image,
                             &(VkImageSubresourceRange){
                                .aspectMask = region->imageSubresource.aspectMask,
                                .baseMipLevel = region->imageSubresource.mipLevel,
                                .levelCount = 1,
                                .baseArrayLayer = region->imageSubresource.baseArrayLayer,
                                .layerCount = vk_image_subresource_layer_count(&image->vk, &region->imageSubresource),
                             });
         img_info.disable_compression = true;

         radv_describe_barrier_end(cmd_buffer);
      }
      img_info.format = vk_format_for_size(vk_format_get_blocksize(img_info.format));
   }

   struct radv_meta_blit2d_buffer buf_info = {
      .bs = img_info.bs,
      .format = img_info.format,
      .buffer = buffer,
      .offset = region->bufferOffset,
      .pitch = buf_extent_el.width,
   };

   if (image->vk.image_type == VK_IMAGE_TYPE_3D)
      img_info.layer = img_offset_el.z;
   /* Loop through each 3D or array slice */
   unsigned num_slices_3d = img_extent_el.depth;
   unsigned num_slices_array = vk_image_subresource_layer_count(&image->vk, &region->imageSubresource);
   unsigned slice_3d = 0;
   unsigned slice_array = 0;
   while (slice_3d < num_slices_3d && slice_array < num_slices_array) {

      rect.src_x = img_offset_el.x;
      rect.src_y = img_offset_el.y;

      /* Perform Blit */
      radv_meta_image_to_buffer(cmd_buffer, &img_info, &buf_info, &rect);

      buf_info.offset += buf_extent_el.width * buf_extent_el.height * buf_info.bs;
      img_info.layer++;
      if (image->vk.image_type == VK_IMAGE_TYPE_3D)
         slice_3d++;
      else
         slice_array++;
   }

   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyImageToBuffer2(VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2 *pCopyImageToBufferInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_image, src_image, pCopyImageToBufferInfo->srcImage);
   VK_FROM_HANDLE(radv_buffer, dst_buffer, pCopyImageToBufferInfo->dstBuffer);

   for (unsigned r = 0; r < pCopyImageToBufferInfo->regionCount; r++) {
      copy_image_to_buffer(cmd_buffer, dst_buffer, src_image, pCopyImageToBufferInfo->srcImageLayout,
                           &pCopyImageToBufferInfo->pRegions[r]);
   }
}

static void
transfer_copy_image(struct radv_cmd_buffer *cmd_buffer, struct radv_image *src_image, VkImageLayout src_image_layout,
                    struct radv_image *dst_image, VkImageLayout dst_image_layout, const VkImageCopy2 *region)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radeon_cmdbuf *cs = cmd_buffer->cs;
   unsigned int dst_aspect_mask_remaining = region->dstSubresource.aspectMask;

   u_foreach_bit (b, region->srcSubresource.aspectMask) {
      const VkImageAspectFlags src_aspect_mask = BITFIELD_BIT(b);
      const VkImageAspectFlags dst_aspect_mask = BITFIELD_BIT(u_bit_scan(&dst_aspect_mask_remaining));
      const unsigned src_binding_idx = src_image->disjoint ? radv_plane_from_aspect(src_aspect_mask) : 0;
      const unsigned dst_binding_idx = dst_image->disjoint ? radv_plane_from_aspect(dst_aspect_mask) : 0;

      radv_cs_add_buffer(device->ws, cs, src_image->bindings[src_binding_idx].bo);
      radv_cs_add_buffer(device->ws, cs, dst_image->bindings[dst_binding_idx].bo);

      const struct radv_sdma_surf src =
         radv_sdma_get_surf(device, src_image, region->srcSubresource, region->srcOffset, src_aspect_mask);
      const struct radv_sdma_surf dst =
         radv_sdma_get_surf(device, dst_image, region->dstSubresource, region->dstOffset, dst_aspect_mask);
      const VkExtent3D extent = radv_sdma_get_copy_extent(src_image, region->srcSubresource, region->extent);

      if (radv_sdma_use_t2t_scanline_copy(device, &src, &dst, extent)) {
         if (!alloc_transfer_temp_bo(cmd_buffer))
            return;

         radv_sdma_copy_image_t2t_scanline(device, cs, &src, &dst, extent, cmd_buffer->transfer.copy_temp);
      } else {
         radv_sdma_copy_image(device, cs, &src, &dst, extent);
      }
   }
}

static void
copy_image(struct radv_cmd_buffer *cmd_buffer, struct radv_image *src_image, VkImageLayout src_image_layout,
           struct radv_image *dst_image, VkImageLayout dst_image_layout, const VkImageCopy2 *region)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (cmd_buffer->qf == RADV_QUEUE_TRANSFER) {
      transfer_copy_image(cmd_buffer, src_image, src_image_layout, dst_image, dst_image_layout, region);
      return;
   }

   struct radv_meta_saved_state saved_state;
   bool cs;

   /* From the Vulkan 1.0 spec:
    *
    *    vkCmdCopyImage can be used to copy image data between multisample images, but both images must have the same
    *    number of samples.
    */
   assert(src_image->vk.samples == dst_image->vk.samples);
   /* From the Vulkan 1.3 spec:
    *
    *    Multi-planar images can only be copied on a per-plane basis, and the subresources used in each region when
    *    copying to or from such images must specify only one plane, though different regions can specify different
    *    planes.
    */
   assert(src_image->plane_count == 1 || util_is_power_of_two_nonzero(region->srcSubresource.aspectMask));
   assert(dst_image->plane_count == 1 || util_is_power_of_two_nonzero(region->dstSubresource.aspectMask));

   cs = cmd_buffer->qf == RADV_QUEUE_COMPUTE || !radv_image_is_renderable(device, dst_image);

   /* VK_EXT_conditional_rendering says that copy commands should not be
    * affected by conditional rendering.
    */
   radv_meta_save(&saved_state, cmd_buffer,
                  (cs ? RADV_META_SAVE_COMPUTE_PIPELINE : RADV_META_SAVE_GRAPHICS_PIPELINE) | RADV_META_SAVE_CONSTANTS |
                     RADV_META_SAVE_DESCRIPTORS | RADV_META_SUSPEND_PREDICATING);

   if (cs) {
      /* For partial copies, HTILE should be decompressed before copying because the metadata is
       * re-initialized to the uncompressed state after.
       */
      uint32_t queue_mask = radv_image_queue_family_mask(dst_image, cmd_buffer->qf, cmd_buffer->qf);

      if (radv_layout_is_htile_compressed(device, dst_image, dst_image_layout, queue_mask) &&
          (region->dstOffset.x || region->dstOffset.y || region->dstOffset.z ||
           region->extent.width != dst_image->vk.extent.width || region->extent.height != dst_image->vk.extent.height ||
           region->extent.depth != dst_image->vk.extent.depth)) {
         radv_describe_barrier_start(cmd_buffer, RGP_BARRIER_UNKNOWN_REASON);

         u_foreach_bit (i, region->dstSubresource.aspectMask) {
            unsigned aspect_mask = 1u << i;
            radv_expand_depth_stencil(
               cmd_buffer, dst_image,
               &(VkImageSubresourceRange){
                  .aspectMask = aspect_mask,
                  .baseMipLevel = region->dstSubresource.mipLevel,
                  .levelCount = 1,
                  .baseArrayLayer = region->dstSubresource.baseArrayLayer,
                  .layerCount = vk_image_subresource_layer_count(&dst_image->vk, &region->dstSubresource),
               },
               NULL);
         }

         radv_describe_barrier_end(cmd_buffer);
      }
   }

   /* Create blit surfaces */
   struct radv_meta_blit2d_surf b_src = blit_surf_for_image_level_layer(
      src_image, src_image_layout, &region->srcSubresource, region->srcSubresource.aspectMask);

   struct radv_meta_blit2d_surf b_dst = blit_surf_for_image_level_layer(
      dst_image, dst_image_layout, &region->dstSubresource, region->dstSubresource.aspectMask);

   uint32_t dst_queue_mask = radv_image_queue_family_mask(dst_image, cmd_buffer->qf, cmd_buffer->qf);
   bool dst_compressed =
      radv_layout_dcc_compressed(device, dst_image, region->dstSubresource.mipLevel, dst_image_layout, dst_queue_mask);
   uint32_t src_queue_mask = radv_image_queue_family_mask(src_image, cmd_buffer->qf, cmd_buffer->qf);
   bool src_compressed =
      radv_layout_dcc_compressed(device, src_image, region->srcSubresource.mipLevel, src_image_layout, src_queue_mask);
   bool need_dcc_sign_reinterpret = false;

   if (!src_compressed ||
       (radv_dcc_formats_compatible(pdev->info.gfx_level, b_src.format, b_dst.format, &need_dcc_sign_reinterpret) &&
        !need_dcc_sign_reinterpret)) {
      b_src.format = b_dst.format;
   } else if (!dst_compressed) {
      b_dst.format = b_src.format;
   } else {
      radv_describe_barrier_start(cmd_buffer, RGP_BARRIER_UNKNOWN_REASON);

      radv_decompress_dcc(cmd_buffer, dst_image,
                          &(VkImageSubresourceRange){
                             .aspectMask = region->dstSubresource.aspectMask,
                             .baseMipLevel = region->dstSubresource.mipLevel,
                             .levelCount = 1,
                             .baseArrayLayer = region->dstSubresource.baseArrayLayer,
                             .layerCount = vk_image_subresource_layer_count(&dst_image->vk, &region->dstSubresource),
                          });
      b_dst.format = b_src.format;
      b_dst.disable_compression = true;

      radv_describe_barrier_end(cmd_buffer);
   }

   /**
    * From the Vulkan 1.0.6 spec: 18.4 Copying Data Between Buffers and Images
    *    imageExtent is the size in texels of the image to copy in width, height
    *    and depth. 1D images use only x and width. 2D images use x, y, width
    *    and height. 3D images use x, y, z, width, height and depth.
    *
    * Also, convert the offsets and extent from units of texels to units of
    * blocks - which is the highest resolution accessible in this command.
    */
   const VkOffset3D dst_offset_el = vk_image_offset_to_elements(&dst_image->vk, region->dstOffset);
   const VkOffset3D src_offset_el = vk_image_offset_to_elements(&src_image->vk, region->srcOffset);

   /*
    * From Vulkan 1.0.68, "Copying Data Between Images":
    *    "When copying between compressed and uncompressed formats
    *     the extent members represent the texel dimensions of the
    *     source image and not the destination."
    * However, we must use the destination image type to avoid
    * clamping depth when copying multiple layers of a 2D image to
    * a 3D image.
    */
   const VkExtent3D img_extent_el = vk_image_extent_to_elements(&src_image->vk, region->extent);

   /* Start creating blit rect */
   struct radv_meta_blit2d_rect rect = {
      .width = img_extent_el.width,
      .height = img_extent_el.height,
   };

   unsigned num_slices = vk_image_subresource_layer_count(&src_image->vk, &region->srcSubresource);

   if (src_image->vk.image_type == VK_IMAGE_TYPE_3D) {
      b_src.layer = src_offset_el.z;
      num_slices = img_extent_el.depth;
   }

   if (dst_image->vk.image_type == VK_IMAGE_TYPE_3D)
      b_dst.layer = dst_offset_el.z;

   for (unsigned slice = 0; slice < num_slices; slice++) {
      /* Finish creating blit rect */
      rect.dst_x = dst_offset_el.x;
      rect.dst_y = dst_offset_el.y;
      rect.src_x = src_offset_el.x;
      rect.src_y = src_offset_el.y;

      /* Perform Blit */
      if (cs) {
         radv_meta_image_to_image_cs(cmd_buffer, &b_src, &b_dst, &rect);
      } else {
         if (radv_can_use_fmask_copy(cmd_buffer, b_src.image, b_dst.image, &rect)) {
            radv_fmask_copy(cmd_buffer, &b_src, &b_dst);
         } else {
            radv_meta_blit2d(cmd_buffer, &b_src, NULL, &b_dst, &rect);
         }
      }

      b_src.layer++;
      b_dst.layer++;
   }

   if (cs) {
      /* Fixup HTILE after a copy on compute. */
      uint32_t queue_mask = radv_image_queue_family_mask(dst_image, cmd_buffer->qf, cmd_buffer->qf);

      if (radv_layout_is_htile_compressed(device, dst_image, dst_image_layout, queue_mask)) {
         cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_INV_VCACHE;

         VkImageSubresourceRange range = {
            .aspectMask = region->dstSubresource.aspectMask,
            .baseMipLevel = region->dstSubresource.mipLevel,
            .levelCount = 1,
            .baseArrayLayer = region->dstSubresource.baseArrayLayer,
            .layerCount = vk_image_subresource_layer_count(&dst_image->vk, &region->dstSubresource),
         };

         uint32_t htile_value = radv_get_htile_initial_value(device, dst_image);

         cmd_buffer->state.flush_bits |= radv_clear_htile(cmd_buffer, dst_image, &range, htile_value, false);
      }
   }

   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2 *pCopyImageInfo)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_image, src_image, pCopyImageInfo->srcImage);
   VK_FROM_HANDLE(radv_image, dst_image, pCopyImageInfo->dstImage);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);

   for (unsigned r = 0; r < pCopyImageInfo->regionCount; r++) {
      copy_image(cmd_buffer, src_image, pCopyImageInfo->srcImageLayout, dst_image, pCopyImageInfo->dstImageLayout,
                 &pCopyImageInfo->pRegions[r]);
   }

   if (radv_is_format_emulated(pdev, dst_image->vk.format)) {
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH |
                                      radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                            VK_ACCESS_TRANSFER_WRITE_BIT, dst_image) |
                                      radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                                            VK_ACCESS_TRANSFER_READ_BIT, dst_image);

      const enum util_format_layout format_layout = vk_format_description(dst_image->vk.format)->layout;
      for (unsigned r = 0; r < pCopyImageInfo->regionCount; r++) {
         VkExtent3D dst_extent = pCopyImageInfo->pRegions[r].extent;
         if (src_image->vk.format != dst_image->vk.format) {
            dst_extent.width = dst_extent.width / vk_format_get_blockwidth(src_image->vk.format) *
                               vk_format_get_blockwidth(dst_image->vk.format);
            dst_extent.height = dst_extent.height / vk_format_get_blockheight(src_image->vk.format) *
                                vk_format_get_blockheight(dst_image->vk.format);
         }
         if (format_layout == UTIL_FORMAT_LAYOUT_ASTC) {
            radv_meta_decode_astc(cmd_buffer, dst_image, pCopyImageInfo->dstImageLayout,
                                  &pCopyImageInfo->pRegions[r].dstSubresource, pCopyImageInfo->pRegions[r].dstOffset,
                                  dst_extent);
         } else {
            radv_meta_decode_etc(cmd_buffer, dst_image, pCopyImageInfo->dstImageLayout,
                                 &pCopyImageInfo->pRegions[r].dstSubresource, pCopyImageInfo->pRegions[r].dstOffset,
                                 dst_extent);
         }
      }
   }
}
