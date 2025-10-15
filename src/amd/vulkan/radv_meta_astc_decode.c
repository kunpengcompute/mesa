/* Copyright (c) 2017-2023 Hans-Kristian Arntzen
 *
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdbool.h>

#include "radv_meta.h"
#include "sid.h"
#include "vk_common_entrypoints.h"
#include "vk_format.h"

struct radv_dispatch_info {
   /**
    * Determine the layout of the grid (in block units) to be used.
    */
   uint32_t blocks[3];

   /**
    * A starting offset for the grid. If unaligned is set, the offset
    * must still be aligned.
    */
   uint32_t offsets[3];

   /**
    * Whether it's an unaligned compute dispatch.
    */
   bool unaligned;

   /**
    * Whether waves must be launched in order.
    */
   bool ordered;

   /**
    * Indirect compute parameters resource.
    */
   struct radeon_winsys_bo *indirect;
   uint64_t va;
};

VkResult
radv_device_init_meta_astc_decode_state(struct radv_device *device, bool on_demand)
{
   const struct radv_physical_device *pdev = device->physical_device;
   struct radv_meta_state *state = &device->meta_state;

   if (!pdev->emulate_astc)
      return VK_SUCCESS;

   return vk_texcompress_astc_init(&device->vk, &state->alloc, radv_pipeline_cache_to_handle(&state->cache), &state->astc_decode);
}

void
radv_device_finish_meta_astc_decode_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;
   struct vk_texcompress_astc_state *astc = state->astc_decode;

   if (astc)
      vk_texcompress_astc_finish(&device->vk, &state->alloc, astc);
}

extern void
radv_compute_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info);

static void
decode_astc(struct radv_cmd_buffer *cmd_buffer, struct radv_image_view *src_iview, struct radv_image_view *dst_iview,
            VkImageLayout layout, const VkOffset3D *offset, const VkExtent3D *extent)
{
   struct radv_device *device = cmd_buffer->device;
   struct radv_meta_state *state = &device->meta_state;
   struct vk_texcompress_astc_write_descriptor_set write_desc_set;
   VkFormat format = src_iview->image->vk_format;
   int blk_w = vk_format_get_blockwidth(format);
   int blk_h = vk_format_get_blockheight(format);

   vk_texcompress_astc_fill_write_descriptor_sets(state->astc_decode, &write_desc_set,
                                                  radv_image_view_to_handle(src_iview), layout,
                                                  radv_image_view_to_handle(dst_iview), format);
   radv_meta_push_descriptor_set(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->astc_decode->p_layout,
                                 0, /* set number */
                                 VK_TEXCOMPRESS_ASTC_WRITE_DESC_SET_COUNT, write_desc_set.descriptor_set);

   VkPipeline pipeline =
      vk_texcompress_astc_get_decode_pipeline(&device->vk, &state->alloc, state->astc_decode, radv_pipeline_cache_to_handle(&state->cache), format);
   if (pipeline == VK_NULL_HANDLE)
      return;

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   bool is_3Dimage = (src_iview->image->type == VK_IMAGE_TYPE_3D) ? true : false;
   int push_constants[5] = {offset->x / blk_w, offset->y / blk_h, extent->width + offset->x, extent->height + offset->y,
                            is_3Dimage};
   radv_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), device->meta_state.etc_decode.p_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, push_constants);

   struct radv_dispatch_info info = {
      .blocks[0] = DIV_ROUND_UP(extent->width, blk_w * 2),
      .blocks[1] = DIV_ROUND_UP(extent->height, blk_h * 2),
      .blocks[2] = extent->depth,
      .offsets[0] = 0,
      .offsets[1] = 0,
      .offsets[2] = offset->z,
      .unaligned = 0,
   };
   radv_compute_dispatch(cmd_buffer, &info);
}

static VkImageViewType
get_view_type(const struct radv_image *image)
{
   switch (image->type) {
   case VK_IMAGE_TYPE_2D:
      return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
   case VK_IMAGE_TYPE_3D:
      return VK_IMAGE_VIEW_TYPE_3D;
   default:
      unreachable("bad VkImageViewType");
   }
}

static void
image_view_init(struct radv_device *device, struct radv_image *image, VkFormat format, VkImageAspectFlags aspectMask,
                uint32_t baseMipLevel, uint32_t baseArrayLayer, uint32_t layerCount, struct radv_image_view *iview)
{
   VkImageViewCreateInfo iview_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = radv_image_to_handle(image),
      .viewType = get_view_type(image),
      .format = format,
      .subresourceRange =
         {
            .aspectMask = aspectMask,
            .baseMipLevel = baseMipLevel,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = baseArrayLayer + layerCount,
         },
   };

   radv_image_view_init(iview, device, &iview_create_info, NULL);
}

void
radv_meta_decode_astc(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout layout,
                      const VkImageSubresourceLayers *subresource, VkOffset3D offset, VkExtent3D extent)
{
   struct radv_device *device = cmd_buffer->device;
   struct radv_meta_saved_state saved_state;
   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS | RADV_META_SAVE_DESCRIPTORS);

   VkImage _image = radv_image_to_handle(image);
   VK_FROM_HANDLE(vk_image, vkImage, _image);

   uint32_t base_slice = radv_meta_get_iview_layer(image, subresource, &offset);
   uint32_t slice_count = image->type == VK_IMAGE_TYPE_3D
                           ? extent.depth 
                           : vk_image_subresource_layer_count(vkImage, subresource);

   extent = radv_sanitize_image_extent(image->type, extent);
   offset = radv_sanitize_image_offset(image->type, offset);

   struct radv_image_view src_iview, dst_iview;
   image_view_init(device, image, VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, subresource->mipLevel,
                   subresource->baseArrayLayer, vk_image_subresource_layer_count(vkImage, subresource), &src_iview);
   image_view_init(device, image, VK_FORMAT_R8G8B8A8_UINT, VK_IMAGE_ASPECT_PLANE_1_BIT, subresource->mipLevel,
                   subresource->baseArrayLayer, vk_image_subresource_layer_count(vkImage, subresource), &dst_iview);

   VkExtent3D extent_copy = {
      .width = extent.width,
      .height = extent.height,
      .depth = slice_count,
   };
   decode_astc(cmd_buffer, &src_iview, &dst_iview, layout, &(VkOffset3D){offset.x, offset.y, base_slice}, &extent_copy);

   radv_image_view_finish(&src_iview);
   radv_image_view_finish(&dst_iview);

   radv_meta_restore(&saved_state, cmd_buffer);
}
