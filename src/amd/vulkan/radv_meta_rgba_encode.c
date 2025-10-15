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
#include "vk_texcompress_bcn.h"
#include "radv_debug.h"

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
radv_device_init_meta_rgba_encode_state(struct radv_device *device)
{
   const struct radv_physical_device *pdev = device->physical_device;
   struct radv_meta_state *state = &device->meta_state;

   return vk_texcompress_bcn_init(&device->vk, &state->alloc, radv_pipeline_cache_to_handle(&state->cache), state->bcn_encoded);
}

void
radv_device_finish_meta_rgba_encode_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;
   struct vk_texcompress_bcn_state **bcns = state->bcn_encoded;

   if (bcns)
      vk_texcompress_bcn_finish(&device->vk, &state->alloc, state->bcn_encoded);
}

extern void
radv_compute_dispatch(struct radv_cmd_buffer *cmd_buffer, const struct radv_dispatch_info *info);

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
                uint32_t baseMipLevel, uint32_t baseArrayLayer, uint32_t layerCount, VkComponentMapping *component,
                struct radv_image_view *iview)
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
   if (component) {
      iview_create_info.components = *component;
   }

   radv_image_view_init(iview, device, &iview_create_info, NULL);
}

static void
encode_bc1(struct radv_cmd_buffer *cmd_buffer, VkImageLayout layout,
           const VkExtent3D *extent, const VkImageSubresourceLayers *subresource,
           struct radv_image *src_image, struct radv_image *dst_image)
{
   struct radv_device *device = cmd_buffer->device;
   struct radv_meta_state *state = &device->meta_state;

   struct radv_image_view src_iview, dst_iview;
   VkComponentMapping component = {
      .r = VK_COMPONENT_SWIZZLE_R,
      .g = VK_COMPONENT_SWIZZLE_G,
      .b = VK_COMPONENT_SWIZZLE_B,
      .a = VK_COMPONENT_SWIZZLE_A
   };
   image_view_init(device, src_image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, subresource->mipLevel,
                   subresource->baseArrayLayer, 1, &component, &src_iview);
   image_view_init(device, dst_image, VK_FORMAT_R16G16B16A16_UINT, VK_IMAGE_ASPECT_COLOR_BIT, subresource->mipLevel,
                   subresource->baseArrayLayer, 1, NULL, &dst_iview);

   cmd_buffer->state.flush_bits |=
      RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH |
      radv_src_access_flush(cmd_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, src_iview.image) |
      radv_dst_access_flush(cmd_buffer, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, dst_iview.image);

   struct vk_texcompress_bcn_write_descriptor_set write_desc_set;
   vk_texcompress_bc1_fill_write_descriptor_sets(state->bcn_encoded[BC1_ENCODE], &write_desc_set,
                                                  radv_image_view_to_handle(&src_iview), layout,
                                                  radv_image_view_to_handle(&dst_iview));

   radv_meta_push_descriptor_set(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->bcn_encoded[BC1_ENCODE]->p_layout,
                                 0, /* set number */
                                 3, write_desc_set.descriptor_set);

   VkPipeline pipeline =
      vk_texcompress_rgba_get_encode_pipeline(&device->vk, &state->alloc,
                                              state->bcn_encoded[BC1_ENCODE],
                                              radv_pipeline_cache_to_handle(&state->cache), BC1_ENCODE);
   if (pipeline == VK_NULL_HANDLE)
      return;

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   const unsigned num_refinements = 1;
   const unsigned push_constants[2] = {num_refinements};
   radv_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), state->bcn_encoded[BC1_ENCODE]->p_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

   struct radv_dispatch_info info = {
      .blocks[0] = DIV_ROUND_UP(extent->width, 32),
      .blocks[1] = DIV_ROUND_UP(extent->height, 32),
      .blocks[2] = 1,
   };
   radv_compute_dispatch(cmd_buffer, &info);
   radv_image_view_finish(&src_iview);
   radv_image_view_finish(&dst_iview);
}

static void
encode_bc4(struct radv_cmd_buffer *cmd_buffer, VkImageLayout layout,
           const VkExtent3D *extent, const VkImageSubresourceLayers *subresource,
           struct radv_image *src_image, struct radv_image *dst_image)
{
   struct radv_device *device = cmd_buffer->device;
   struct radv_meta_state *state = &device->meta_state;

   struct radv_image_view src_iview, dst_iview;
   VkComponentMapping component = {
      .r = VK_COMPONENT_SWIZZLE_A,
      .g = VK_COMPONENT_SWIZZLE_ZERO,
      .b = VK_COMPONENT_SWIZZLE_ZERO,
      .a = VK_COMPONENT_SWIZZLE_ONE
   };
   image_view_init(device, src_image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, subresource->mipLevel,
                   subresource->baseArrayLayer, 1, &component, &src_iview);
   image_view_init(device, dst_image, VK_FORMAT_R16G16B16A16_UINT, VK_IMAGE_ASPECT_COLOR_BIT, subresource->mipLevel,
                   subresource->baseArrayLayer, 1, NULL, &dst_iview);

   cmd_buffer->state.flush_bits |=
      RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH |
      radv_src_access_flush(cmd_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, src_iview.image) |
      radv_dst_access_flush(cmd_buffer, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, dst_iview.image);

   struct vk_texcompress_bc4_write_descriptor_set write_desc_set;
   vk_texcompress_bc4_fill_write_descriptor_sets(state->bcn_encoded[BC4_ENCODE], &write_desc_set,
                                                  radv_image_view_to_handle(&src_iview), layout,
                                                  radv_image_view_to_handle(&dst_iview));

   radv_meta_push_descriptor_set(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->bcn_encoded[BC4_ENCODE]->p_layout,
                                 0, /* set number */
                                 2, write_desc_set.descriptor_set);

   VkPipeline pipeline =
      vk_texcompress_rgba_get_encode_pipeline(&device->vk, &state->alloc,
                                              state->bcn_encoded[BC4_ENCODE],
                                              radv_pipeline_cache_to_handle(&state->cache), BC4_ENCODE);
   if (pipeline == VK_NULL_HANDLE)
      return;

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   const unsigned alpha_channel_id = 0;  // r通道
   const unsigned use_norm = 0;
   const unsigned push_constants[2] = {alpha_channel_id, use_norm};
   radv_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), state->bcn_encoded[BC4_ENCODE]->p_layout,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants), &push_constants);

   struct radv_dispatch_info info = {
      .blocks[0] = 1,
      .blocks[1] = DIV_ROUND_UP(extent->width, 16),
      .blocks[2] = DIV_ROUND_UP(extent->height, 16),
   };
   radv_compute_dispatch(cmd_buffer, &info);
   radv_image_view_finish(&src_iview);
   radv_image_view_finish(&dst_iview);
}

static void
encode_bc3(struct radv_cmd_buffer *cmd_buffer,
           const VkExtent3D *extent, const VkImageSubresourceLayers *subresource,
           struct radv_image *bc1_image, struct radv_image *bc4_image, struct radv_image *dst_image)
{
   struct radv_device *device = cmd_buffer->device;
   struct radv_meta_state *state = &device->meta_state;

   struct radv_image_view bc1_iview, bc4_iview, dst_iview;
   VkComponentMapping component = {
      .r = VK_COMPONENT_SWIZZLE_R,
      .g = VK_COMPONENT_SWIZZLE_G,
      .b = VK_COMPONENT_SWIZZLE_ZERO,
      .a = VK_COMPONENT_SWIZZLE_ONE
   };
   image_view_init(device, bc1_image, VK_FORMAT_R32G32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, subresource->mipLevel,
                   subresource->baseArrayLayer, 1, &component, &bc1_iview);
   image_view_init(device, bc4_image, VK_FORMAT_R32G32_UINT, VK_IMAGE_ASPECT_COLOR_BIT, subresource->mipLevel,
                   subresource->baseArrayLayer, 1, &component, &bc4_iview);
   image_view_init(device, dst_image, VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_ASPECT_PLANE_1_BIT, subresource->mipLevel,
                   subresource->baseArrayLayer, 1, NULL, &dst_iview);

   cmd_buffer->state.flush_bits |=
      RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH |
      radv_src_access_flush(cmd_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, bc1_iview.image) |
      radv_src_access_flush(cmd_buffer, VK_ACCESS_TRANSFER_WRITE_BIT, bc4_iview.image) |
      radv_dst_access_flush(cmd_buffer, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, dst_iview.image);

   struct vk_texcompress_bcn_write_descriptor_set write_desc_set;
   vk_texcompress_bc3_fill_write_descriptor_sets(state->bcn_encoded[BC3_ENCODE], &write_desc_set,
                                                 radv_image_view_to_handle(&bc1_iview), radv_image_view_to_handle(&bc4_iview),
                                                 radv_image_view_to_handle(&dst_iview));

   radv_meta_push_descriptor_set(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->bcn_encoded[BC3_ENCODE]->p_layout,
                                 0, /* set number */
                                 3, write_desc_set.descriptor_set);

   VkPipeline pipeline =
      vk_texcompress_rgba_get_encode_pipeline(&device->vk, &state->alloc,
                                              state->bcn_encoded[BC3_ENCODE],
                                              radv_pipeline_cache_to_handle(&state->cache), BC3_ENCODE);
   if (pipeline == VK_NULL_HANDLE)
      return;

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   struct radv_dispatch_info info = {
      .blocks[0] = DIV_ROUND_UP(extent->width, 32),
      .blocks[1] = DIV_ROUND_UP(extent->height, 32),
      .blocks[2] = 1,
   };
   radv_compute_dispatch(cmd_buffer, &info);
   radv_image_view_finish(&bc1_iview);
   radv_image_view_finish(&bc4_iview);
   radv_image_view_finish(&dst_iview);
}

void
radv_meta_bcn_encoded(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout layout,
                      const VkImageSubresourceLayers *subresource, VkOffset3D offset, VkExtent3D extent)
{
   struct radv_device *device = cmd_buffer->device;
   struct radv_meta_saved_state saved_state;
   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS | RADV_META_SAVE_DESCRIPTORS);

   VkImage _image = radv_image_to_handle(image);
   VK_FROM_HANDLE(vk_image, vkImage, _image);

   extent = radv_sanitize_image_extent(image->type, extent);
   offset = radv_sanitize_image_offset(image->type, offset);

   VkExtent3D extent_copy = {
      .width = extent.width,
      .height = extent.height,
      .depth = 1,
   };

   RADV_FROM_HANDLE(radv_image, rgba_image, image->staging_images->image_rgba);
   RADV_FROM_HANDLE(radv_image, image1, image->staging_images->image_bc1);
   RADV_FROM_HANDLE(radv_image, image4, image->staging_images->image_bc4);

   encode_bc1(cmd_buffer, layout, &extent_copy, subresource, rgba_image, image1);

   encode_bc4(cmd_buffer, layout, &extent_copy, subresource, rgba_image, image4);

   encode_bc3(cmd_buffer, &extent_copy, subresource, image1, image4, image);

   radv_meta_restore(&saved_state, cmd_buffer);
}
