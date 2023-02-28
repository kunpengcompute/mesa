/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "radv_meta.h"
#include "vk_format.h"
#include "main/formats.h"
#include "main/texcompress_etc.h"

static VkExtent3D
meta_image_block_size(const struct radv_image *image)
{
	const struct vk_format_description *desc = vk_format_description(image->vk_format);
	return (VkExtent3D) { desc->block.width, desc->block.height, 1 };
}

/* Returns the user-provided VkBufferImageCopy::imageExtent in units of
 * elements rather than texels. One element equals one texel or one block
 * if Image is uncompressed or compressed, respectively.
 */
static struct VkExtent3D
meta_region_extent_el(const struct radv_image *image,
                      const VkImageType imageType,
                      const struct VkExtent3D *extent)
{
	const VkExtent3D block = meta_image_block_size(image);
	return radv_sanitize_image_extent(imageType, (VkExtent3D) {
			.width  = DIV_ROUND_UP(extent->width , block.width),
				.height = DIV_ROUND_UP(extent->height, block.height),
				.depth  = DIV_ROUND_UP(extent->depth , block.depth),
				});
}

/* Returns the user-provided VkBufferImageCopy::imageOffset in units of
 * elements rather than texels. One element equals one texel or one block
 * if Image is uncompressed or compressed, respectively.
 */
static struct VkOffset3D
meta_region_offset_el(const struct radv_image *image,
                      const struct VkOffset3D *offset)
{
	const VkExtent3D block = meta_image_block_size(image);
	return radv_sanitize_image_offset(image->type, (VkOffset3D) {
			.x = offset->x / block.width,
				.y = offset->y / block.height,
				.z = offset->z / block.depth,
				});
}

static VkFormat
vk_format_for_size(int bs)
{
	switch (bs) {
	case 1: return VK_FORMAT_R8_UINT;
	case 2: return VK_FORMAT_R8G8_UINT;
	case 4: return VK_FORMAT_R8G8B8A8_UINT;
	case 8: return VK_FORMAT_R16G16B16A16_UINT;
	case 12: return VK_FORMAT_R32G32B32_UINT;
	case 16: return VK_FORMAT_R32G32B32A32_UINT;
	default:
		unreachable("Invalid format block size");
	}
}

static struct radv_meta_blit2d_surf
blit_surf_for_image_level_layer(struct radv_image *image,
				VkImageLayout layout,
				const VkImageSubresourceLayers *subres,
				VkImageAspectFlags aspect_mask)
{
	VkFormat format = radv_get_aspect_format(image, aspect_mask);

	if (!radv_dcc_enabled(image, subres->mipLevel) &&
	    !(radv_image_is_tc_compat_htile(image)))
		format = vk_format_for_size(vk_format_get_blocksize(format));

	format = vk_format_no_srgb(format);

	return (struct radv_meta_blit2d_surf) {
		.format = format,
		.bs = vk_format_get_blocksize(format),
		.level = subres->mipLevel,
		.layer = subres->baseArrayLayer,
		.image = image,
		.aspect_mask = aspect_mask,
		.current_layout = layout,
	};
}

static size_t
radv_get_texel_count(const VkBufferImageCopy* pRegions, uint32_t regionCount, struct radv_image* image)
{
	if (image == NULL) {
		return 0;
	}
	size_t w = 0;
	size_t h = 0;

	for (int i = 0; i < regionCount; i++) {
		const VkExtent3D bufferExtent = {
			.width  = pRegions[i].bufferRowLength ?
			pRegions[i].bufferRowLength : pRegions[i].imageExtent.width,
			.height = pRegions[i].bufferImageHeight ?
			pRegions[i].bufferImageHeight : pRegions[i].imageExtent.height,
		};
		const VkExtent3D buf_extent_el =
			meta_region_extent_el(image, image->type, &bufferExtent);
		w += buf_extent_el.width;
		h += buf_extent_el.height;	
	}
	int bs = vk_format_get_blocksize(image->vk_format);
	return w * h * bs;
}

static bool
image_is_renderable(struct radv_device *device, struct radv_image *image)
{
	if (image->vk_format == VK_FORMAT_R32G32B32_UINT ||
	    image->vk_format == VK_FORMAT_R32G32B32_SINT ||
	    image->vk_format == VK_FORMAT_R32G32B32_SFLOAT)
		return false;

	if (device->physical_device->rad_info.chip_class >= GFX9 &&
	    image->type == VK_IMAGE_TYPE_3D &&
	    vk_format_get_blocksizebits(image->vk_format) == 128 &&
	    vk_format_is_compressed(image->vk_format))
		return false;
	return true;
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
	case VK_FORMAT_EAC_R11_UNORM_BLOCK:
		return MESA_FORMAT_ETC2_R11_EAC;
	case VK_FORMAT_EAC_R11_SNORM_BLOCK:
		return MESA_FORMAT_ETC2_SIGNED_R11_EAC;
	case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
		return MESA_FORMAT_ETC2_RG11_EAC;
	case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
		return MESA_FORMAT_ETC2_SIGNED_RG11_EAC;
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
	case VK_FORMAT_R8_UNORM:
		return MESA_FORMAT_R_UNORM8;
	case VK_FORMAT_R8_SRGB:
		return MESA_FORMAT_R_SRGB8;
	case VK_FORMAT_R8G8_UNORM:
		return MESA_FORMAT_RG_UNORM8;
	case VK_FORMAT_R8G8_SRGB:
		return MESA_FORMAT_RG_SNORM8;
	default:
		return MESA_FORMAT_RGB_UNORM8;
	}
}
static void
meta_copy_buffer_to_image(struct radv_cmd_buffer *cmd_buffer,
                          struct radv_buffer* buffer,
                          struct radv_image* image,
			  VkImageLayout layout,
                          uint32_t regionCount,
                          const VkBufferImageCopy* pRegions)
{
	struct radv_device* device = cmd_buffer->device;
	VkBuffer dstBuffer;
	struct radv_buffer* radvDstBuffer = buffer;
	VkDeviceMemory memory_h;
	void* pDstData = NULL;
	void* pSrcData = NULL;
	bool cs = cmd_buffer->queue_family_index == RADV_QUEUE_COMPUTE;
	struct radv_meta_saved_state saved_state;
	bool old_predicating;


	/* The Vulkan 1.0 spec says "dstImage must have a sample count equal to
	 * VK_SAMPLE_COUNT_1_BIT."
	 */
	assert(image->info.samples == 1);

	radv_meta_save(&saved_state, cmd_buffer,
		       (cs ? RADV_META_SAVE_COMPUTE_PIPELINE :
			RADV_META_SAVE_GRAPHICS_PIPELINE) |
		       RADV_META_SAVE_CONSTANTS |
		       RADV_META_SAVE_DESCRIPTORS);

	/* VK_EXT_conditional_rendering says that copy commands should not be
	 * affected by conditional rendering.
	 */
	old_predicating = cmd_buffer->state.predicating;
	cmd_buffer->state.predicating = false;
	if (image->istranslate) {
		//Create translate dstBuffer
		pSrcData = device->ws->buffer_map(buffer->bo) + buffer->offset;
		int dstBufferSize = radv_get_texel_count(pRegions, regionCount, image);
		if(VK_SUCCESS != radv_CreateBuffer(radv_device_to_handle(device),
			&(VkBufferCreateInfo) {
			  .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.flags = buffer->flags,
				.size = dstBufferSize,
				.usage = buffer->usage,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE 
      }, 
      NULL, 
      &dstBuffer)) {
			return;
		}
   
    RADV_FROM_HANDLE(radv_buffer, _dstbuffer, dstBuffer);
		buffer->unpack_buffer_for_ct = _dstbuffer;
	
  	//get tranlated dstBuffer memory size
		VkMemoryRequirements memoryRequirements;
		radv_GetBufferMemoryRequirements(radv_device_to_handle(device), dstBuffer, &memoryRequirements);
		int memory_type_index = -1;
		for (int i = 0; i < device->physical_device->memory_properties.memoryTypeCount; ++i) {
			bool is_local = !!(device->physical_device->memory_properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));
			if (is_local) {
				memory_type_index = i;
				break;
			}
		}
		if(VK_SUCCESS != radv_AllocateMemory(radv_device_to_handle(device),
			&(VkMemoryAllocateInfo) {
			  .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.pNext = NULL,
				.allocationSize = memoryRequirements.size,
				.memoryTypeIndex = memory_type_index,
		  },
			NULL,
			& memory_h)) {
			return;
		}
   
    RADV_FROM_HANDLE(radv_device_memory, _mem, memory_h);
		buffer->unpack_mem_for_ct = _mem;
   
		if(VK_SUCCESS != radv_BindBufferMemory(
			radv_device_to_handle(device),
			dstBuffer,
			memory_h,
			0)) {
			return;
		}
		radv_MapMemory(radv_device_to_handle(device), memory_h, 0, dstBufferSize, 0, &pDstData);	
	}
	void* realDstData = pDstData;
	for (unsigned r = 0; r < regionCount; r++) {

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
		const VkOffset3D img_offset_el =
			meta_region_offset_el(image, &pRegions[r].imageOffset);
		const VkExtent3D bufferExtent = {
			.width  = pRegions[r].bufferRowLength ?
			pRegions[r].bufferRowLength : pRegions[r].imageExtent.width,
			.height = pRegions[r].bufferImageHeight ?
			pRegions[r].bufferImageHeight : pRegions[r].imageExtent.height,
		};
		const VkExtent3D buf_extent_el =
			meta_region_extent_el(image, image->type, &bufferExtent);

		/* Start creating blit rect */
		const VkExtent3D img_extent_el =
			meta_region_extent_el(image, image->type, &pRegions[r].imageExtent);
		struct radv_meta_blit2d_rect rect = {
			.width = img_extent_el.width,
			.height =  img_extent_el.height,
		};

		/* Create blit surfaces */
		struct radv_meta_blit2d_surf img_bsurf =
			blit_surf_for_image_level_layer(image,
							layout,
							&pRegions[r].imageSubresource,
							pRegions[r].imageSubresource.aspectMask);

		if (!radv_is_buffer_format_supported(img_bsurf.format, NULL)) {
			uint32_t queue_mask = radv_image_queue_family_mask(image,
			                                                   cmd_buffer->queue_family_index,
			                                                   cmd_buffer->queue_family_index);
			bool compressed = radv_layout_dcc_compressed(cmd_buffer->device, image, layout, false, queue_mask);
			if (compressed) {
				radv_decompress_dcc(cmd_buffer, image, &(VkImageSubresourceRange) {
								.aspectMask = pRegions[r].imageSubresource.aspectMask,
								.baseMipLevel = pRegions[r].imageSubresource.mipLevel,
								.levelCount = 1,
								.baseArrayLayer = pRegions[r].imageSubresource.baseArrayLayer,
								.layerCount = pRegions[r].imageSubresource.layerCount,
			                                });
			}
			img_bsurf.format = vk_format_for_size(vk_format_get_blocksize(img_bsurf.format));
			img_bsurf.current_layout = VK_IMAGE_LAYOUT_GENERAL;
		}

		uint32_t offset = pRegions[r].bufferOffset;
		if (image->istranslate) {
			// translate vkFormat to MESA format
			mesa_format dstFormat = radv_translate_vkformat_to_mesa_format(image->vk_format);
			mesa_format srcFormat = radv_translate_etc_to_mesa_format(image->vk_srcFormat);
			bool bgra = false;
			void * realSrcData = pSrcData;
			realSrcData += pRegions[r].bufferOffset;
			unsigned dst_stride = _mesa_format_row_stride(dstFormat, pRegions[r].imageExtent.width);
			unsigned src_stride =_mesa_format_row_stride(srcFormat, bufferExtent.width);
      _mesa_unpack_etc2_format(realDstData, dst_stride, realSrcData , src_stride, bufferExtent.width, bufferExtent.height, srcFormat, bgra);
			RADV_FROM_HANDLE(radv_buffer, tmp, dstBuffer);
      radvDstBuffer = tmp;
			offset = realDstData - pDstData;
		}
		struct radv_meta_blit2d_buffer buf_bsurf = {
			.bs = img_bsurf.bs,
			.format = img_bsurf.format,
			.buffer = radvDstBuffer,
			.offset = offset,
			.pitch = buf_extent_el.width,
		};

		if (image->type == VK_IMAGE_TYPE_3D)
			img_bsurf.layer = img_offset_el.z;
		/* Loop through each 3D or array slice */
		unsigned num_slices_3d = img_extent_el.depth;
		unsigned num_slices_array = pRegions[r].imageSubresource.layerCount;
		unsigned slice_3d = 0;
		unsigned slice_array = 0;
		while (slice_3d < num_slices_3d && slice_array < num_slices_array) {

			rect.dst_x = img_offset_el.x;
			rect.dst_y = img_offset_el.y;


			/* Perform Blit */
			if (cs ||
			    !image_is_renderable(cmd_buffer->device, img_bsurf.image)) {
				radv_meta_buffer_to_image_cs(cmd_buffer, &buf_bsurf, &img_bsurf, 1, &rect);
			} else {
				radv_meta_blit2d(cmd_buffer, NULL, &buf_bsurf, &img_bsurf, 1, &rect);
			}

			/* Once we've done the blit, all of the actual information about
			 * the image is embedded in the command buffer so we can just
			 * increment the offset directly in the image effectively
			 * re-binding it to different backing memory.
			 */
			buf_bsurf.offset += buf_extent_el.width *
			                    buf_extent_el.height * buf_bsurf.bs;
			realDstData += buf_extent_el.width *
			                    buf_extent_el.height * buf_bsurf.bs;
			img_bsurf.layer++;
			if (image->type == VK_IMAGE_TYPE_3D)
				slice_3d++;
			else
				slice_array++;
		}
	}
	
	if (image->istranslate){
		device->ws->buffer_unmap(buffer->bo);
    pSrcData = NULL;
		radv_UnmapMemory(radv_device_to_handle(device), memory_h);
	}

	/* Restore conditional rendering. */
	cmd_buffer->state.predicating = old_predicating;

	radv_meta_restore(&saved_state, cmd_buffer);
}

void radv_CmdCopyBufferToImage(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    srcBuffer,
	VkImage                                     destImage,
	VkImageLayout                               destImageLayout,
	uint32_t                                    regionCount,
	const VkBufferImageCopy*                    pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_image, dest_image, destImage);
	RADV_FROM_HANDLE(radv_buffer, src_buffer, srcBuffer);

	meta_copy_buffer_to_image(cmd_buffer, src_buffer, dest_image, destImageLayout,
				  regionCount, pRegions);
}

static void
meta_copy_image_to_buffer(struct radv_cmd_buffer *cmd_buffer,
                          struct radv_buffer* buffer,
                          struct radv_image* image,
			  VkImageLayout layout,
                          uint32_t regionCount,
                          const VkBufferImageCopy* pRegions)
{
	struct radv_meta_saved_state saved_state;
	bool old_predicating;

	radv_meta_save(&saved_state, cmd_buffer,
		       RADV_META_SAVE_COMPUTE_PIPELINE |
		       RADV_META_SAVE_CONSTANTS |
		       RADV_META_SAVE_DESCRIPTORS);

	/* VK_EXT_conditional_rendering says that copy commands should not be
	 * affected by conditional rendering.
	 */
	old_predicating = cmd_buffer->state.predicating;
	cmd_buffer->state.predicating = false;

	for (unsigned r = 0; r < regionCount; r++) {

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
		const VkOffset3D img_offset_el =
			meta_region_offset_el(image, &pRegions[r].imageOffset);
		const VkExtent3D bufferExtent = {
			.width  = pRegions[r].bufferRowLength ?
			pRegions[r].bufferRowLength : pRegions[r].imageExtent.width,
			.height = pRegions[r].bufferImageHeight ?
			pRegions[r].bufferImageHeight : pRegions[r].imageExtent.height,
		};
		const VkExtent3D buf_extent_el =
			meta_region_extent_el(image, image->type, &bufferExtent);

		/* Start creating blit rect */
		const VkExtent3D img_extent_el =
			meta_region_extent_el(image, image->type, &pRegions[r].imageExtent);
		struct radv_meta_blit2d_rect rect = {
			.width = img_extent_el.width,
			.height =  img_extent_el.height,
		};

		/* Create blit surfaces */
		struct radv_meta_blit2d_surf img_info =
			blit_surf_for_image_level_layer(image,
							layout,
							&pRegions[r].imageSubresource,
							pRegions[r].imageSubresource.aspectMask);

		if (!radv_is_buffer_format_supported(img_info.format, NULL)) {
			uint32_t queue_mask = radv_image_queue_family_mask(image,
			                                                   cmd_buffer->queue_family_index,
			                                                   cmd_buffer->queue_family_index);
			bool compressed = radv_layout_dcc_compressed(cmd_buffer->device, image, layout, false, queue_mask);
			if (compressed) {
				radv_decompress_dcc(cmd_buffer, image, &(VkImageSubresourceRange) {
								.aspectMask = pRegions[r].imageSubresource.aspectMask,
								.baseMipLevel = pRegions[r].imageSubresource.mipLevel,
								.levelCount = 1,
								.baseArrayLayer = pRegions[r].imageSubresource.baseArrayLayer,
								.layerCount = pRegions[r].imageSubresource.layerCount,
			                                });
			}
			img_info.format = vk_format_for_size(vk_format_get_blocksize(img_info.format));
			img_info.current_layout = VK_IMAGE_LAYOUT_GENERAL;
		}

		struct radv_meta_blit2d_buffer buf_info = {
			.bs = img_info.bs,
			.format = img_info.format,
			.buffer = buffer,
			.offset = pRegions[r].bufferOffset,
			.pitch = buf_extent_el.width,
		};

		if (image->type == VK_IMAGE_TYPE_3D)
			img_info.layer = img_offset_el.z;
		/* Loop through each 3D or array slice */
		unsigned num_slices_3d = img_extent_el.depth;
		unsigned num_slices_array = pRegions[r].imageSubresource.layerCount;
		unsigned slice_3d = 0;
		unsigned slice_array = 0;
		while (slice_3d < num_slices_3d && slice_array < num_slices_array) {

			rect.src_x = img_offset_el.x;
			rect.src_y = img_offset_el.y;


			/* Perform Blit */
			radv_meta_image_to_buffer(cmd_buffer, &img_info, &buf_info, 1, &rect);

			buf_info.offset += buf_extent_el.width *
			                    buf_extent_el.height * buf_info.bs;
			img_info.layer++;
			if (image->type == VK_IMAGE_TYPE_3D)
				slice_3d++;
			else
				slice_array++;
		}
	}

	/* Restore conditional rendering. */
	cmd_buffer->state.predicating = old_predicating;

	radv_meta_restore(&saved_state, cmd_buffer);
}

void radv_CmdCopyImageToBuffer(
	VkCommandBuffer                             commandBuffer,
	VkImage                                     srcImage,
	VkImageLayout                               srcImageLayout,
	VkBuffer                                    destBuffer,
	uint32_t                                    regionCount,
	const VkBufferImageCopy*                    pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_image, src_image, srcImage);
	RADV_FROM_HANDLE(radv_buffer, dst_buffer, destBuffer);

	meta_copy_image_to_buffer(cmd_buffer, dst_buffer, src_image,
				  srcImageLayout,
				  regionCount, pRegions);
}

static void
meta_copy_image(struct radv_cmd_buffer *cmd_buffer,
		struct radv_image *src_image,
		VkImageLayout src_image_layout,
		struct radv_image *dest_image,
		VkImageLayout dest_image_layout,
		uint32_t regionCount,
		const VkImageCopy *pRegions)
{
	bool cs = cmd_buffer->queue_family_index == RADV_QUEUE_COMPUTE;
	struct radv_meta_saved_state saved_state;
	bool old_predicating;

	/* From the Vulkan 1.0 spec:
	 *
	 *    vkCmdCopyImage can be used to copy image data between multisample
	 *    images, but both images must have the same number of samples.
	 */
	assert(src_image->info.samples == dest_image->info.samples);

	radv_meta_save(&saved_state, cmd_buffer,
		       (cs ? RADV_META_SAVE_COMPUTE_PIPELINE :
			RADV_META_SAVE_GRAPHICS_PIPELINE) |
		       RADV_META_SAVE_CONSTANTS |
		       RADV_META_SAVE_DESCRIPTORS);

	/* VK_EXT_conditional_rendering says that copy commands should not be
	 * affected by conditional rendering.
	 */
	old_predicating = cmd_buffer->state.predicating;
	cmd_buffer->state.predicating = false;

	for (unsigned r = 0; r < regionCount; r++) {
		VkImageAspectFlags src_aspects[3] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_ASPECT_PLANE_2_BIT};
		VkImageAspectFlags dst_aspects[3] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_ASPECT_PLANE_2_BIT};
		unsigned aspect_count = pRegions[r].srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT ? src_image->plane_count : 1;
		if (pRegions[r].srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
			src_aspects[0] = pRegions[r].srcSubresource.aspectMask;
		if (pRegions[r].dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
			dst_aspects[0] = pRegions[r].dstSubresource.aspectMask;

		for (unsigned a = 0; a < aspect_count; ++a) {
			/* Create blit surfaces */
			struct radv_meta_blit2d_surf b_src =
				blit_surf_for_image_level_layer(src_image,
								src_image_layout,
								&pRegions[r].srcSubresource,
								src_aspects[a]);

			struct radv_meta_blit2d_surf b_dst =
				blit_surf_for_image_level_layer(dest_image,
								dest_image_layout,
								&pRegions[r].dstSubresource,
								dst_aspects[a]);

			uint32_t dst_queue_mask = radv_image_queue_family_mask(dest_image,
			                                                       cmd_buffer->queue_family_index,
			                                                       cmd_buffer->queue_family_index);
			bool dst_compressed = radv_layout_dcc_compressed(cmd_buffer->device, dest_image, dest_image_layout, false, dst_queue_mask);
			uint32_t src_queue_mask = radv_image_queue_family_mask(src_image,
			                                                       cmd_buffer->queue_family_index,
			                                                       cmd_buffer->queue_family_index);
			bool src_compressed = radv_layout_dcc_compressed(cmd_buffer->device, src_image, src_image_layout, false, src_queue_mask);

			if (!src_compressed || radv_dcc_formats_compatible(b_src.format, b_dst.format)) {
				b_src.format = b_dst.format;
			} else if (!dst_compressed) {
				b_dst.format = b_src.format;
			} else {
				radv_decompress_dcc(cmd_buffer, dest_image, &(VkImageSubresourceRange) {
				                        .aspectMask = dst_aspects[a],
				                        .baseMipLevel = pRegions[r].dstSubresource.mipLevel,
				                        .levelCount = 1,
				                        .baseArrayLayer = pRegions[r].dstSubresource.baseArrayLayer,
				                        .layerCount = pRegions[r].dstSubresource.layerCount,
					});
				b_dst.format = b_src.format;
				b_dst.current_layout = VK_IMAGE_LAYOUT_GENERAL;
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
			const VkOffset3D dst_offset_el =
				meta_region_offset_el(dest_image, &pRegions[r].dstOffset);
			const VkOffset3D src_offset_el =
				meta_region_offset_el(src_image, &pRegions[r].srcOffset);

			/*
			 * From Vulkan 1.0.68, "Copying Data Between Images":
			 *    "When copying between compressed and uncompressed formats
			 *     the extent members represent the texel dimensions of the
			 *     source image and not the destination."
			 * However, we must use the destination image type to avoid
			 * clamping depth when copying multiple layers of a 2D image to
			 * a 3D image.
			 */
			const VkExtent3D img_extent_el =
				meta_region_extent_el(src_image, dest_image->type, &pRegions[r].extent);

			/* Start creating blit rect */
			struct radv_meta_blit2d_rect rect = {
				.width = img_extent_el.width,
				.height = img_extent_el.height,
			};

			if (src_image->type == VK_IMAGE_TYPE_3D)
				b_src.layer = src_offset_el.z;

			if (dest_image->type == VK_IMAGE_TYPE_3D)
				b_dst.layer = dst_offset_el.z;

			/* Loop through each 3D or array slice */
			unsigned num_slices_3d = img_extent_el.depth;
			unsigned num_slices_array = pRegions[r].dstSubresource.layerCount;
			unsigned slice_3d = 0;
			unsigned slice_array = 0;
			while (slice_3d < num_slices_3d && slice_array < num_slices_array) {

				/* Finish creating blit rect */
				rect.dst_x = dst_offset_el.x;
				rect.dst_y = dst_offset_el.y;
				rect.src_x = src_offset_el.x;
				rect.src_y = src_offset_el.y;

				/* Perform Blit */
				if (cs ||
				    !image_is_renderable(cmd_buffer->device, b_dst.image)) {
					radv_meta_image_to_image_cs(cmd_buffer, &b_src, &b_dst, 1, &rect);
				} else {
					radv_meta_blit2d(cmd_buffer, &b_src, NULL, &b_dst, 1, &rect);
				}

				b_src.layer++;
				b_dst.layer++;
				if (dest_image->type == VK_IMAGE_TYPE_3D)
					slice_3d++;
				else
					slice_array++;
			}
		}
	}

	/* Restore conditional rendering. */
	cmd_buffer->state.predicating = old_predicating;

	radv_meta_restore(&saved_state, cmd_buffer);
}

void radv_CmdCopyImage(
	VkCommandBuffer                             commandBuffer,
	VkImage                                     srcImage,
	VkImageLayout                               srcImageLayout,
	VkImage                                     destImage,
	VkImageLayout                               destImageLayout,
	uint32_t                                    regionCount,
	const VkImageCopy*                          pRegions)
{
	RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
	RADV_FROM_HANDLE(radv_image, src_image, srcImage);
	RADV_FROM_HANDLE(radv_image, dest_image, destImage);

	meta_copy_image(cmd_buffer,
			src_image, srcImageLayout,
			dest_image, destImageLayout,
			regionCount, pRegions);
}
