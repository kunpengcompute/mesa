/* Copyright (c) 2017-2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef VK_TEXCOMPRESS_BCN_H
#define VK_TEXCOMPRESS_BCN_H

#include "util/simple_mtx.h"
#include "vk_fence.h"
#include "vk_device.h"

#define VK_TEXCOMPRESS_BCN_NUM_COMPUTE_PIPELINES 3
#define VK_TEXCOMPRESS_BCN_WRITE_DESC_SET_COUNT 3  // 每个着色器最多有3个描述符集

struct vk_texcompress_bcn_write_descriptor_set {
   VkWriteDescriptorSet descriptor_set[VK_TEXCOMPRESS_BCN_WRITE_DESC_SET_COUNT];
   VkDescriptorImageInfo dst_desc_image_info;
   VkDescriptorImageInfo src_desc_image_info;
   VkDescriptorImageInfo src_desc_image_info_2;
   VkDescriptorBufferInfo bc1_buffer_info;
};

struct vk_texcompress_bc4_write_descriptor_set {
   VkWriteDescriptorSet descriptor_set[2];
   VkDescriptorImageInfo dst_desc_image_info;
   VkDescriptorImageInfo src_desc_image_info;
};

typedef struct vk_texcompress_bcn_image {
   VkImage image_dummy;
   VkImage image_rgba;
   VkImage image_bc1;
   VkImage image_bc4;
   VkDeviceMemory image_mem;
} bcn_image;

struct vk_texcompress_dummy_image {
   VkImage image_dummy;
   VkDeviceMemory image_mem;
};

typedef struct vk_texcompress_staging_images {
   struct vk_texcompress_bcn_image *current;
   struct vk_texcompress_staging_images* next;
} staging_images;

enum compute_pipeline_id {
   BC1_ENCODE = 0,
   BC4_ENCODE,
   BC3_ENCODE,
   MAX_PIPELINE_NUM
};

struct vk_texcompress_bcn_state {
   simple_mtx_t mutex;
   VkDescriptorSetLayout ds_layout;
   VkPipelineLayout p_layout;
   VkPipeline pipeline;
   VkShaderModule shader_module;
   VkSampler sampler;

   VkDeviceMemory bc1_table_mem;
   VkBuffer bc1_table_buf;
};

void
vk_texcompress_insert_head(staging_images **head, bcn_image *current);

void
vk_texcompress_delete_head(staging_images **head, struct vk_device *device);
   
VkResult
vk_texcompress_create_image(struct vk_device *device, VkAllocationCallbacks *allocator, VkDeviceMemory *pMem,
                            VkImage *image, VkExtent3D extent, VkFormat format, int size);

void bind_memory(struct vk_device *device, VkImage *image, VkDeviceMemory *pMem, int offset);

void
vk_texcompress_finish_image(struct vk_device *device, const VkAllocationCallbacks *allocator,
                            struct vk_texcompress_bcn_image *intermidate_image);

void
vk_texcompress_bc1_fill_write_descriptor_sets(struct vk_texcompress_bcn_state *bcn,
                                              struct vk_texcompress_bcn_write_descriptor_set *set,
                                              VkImageView src_img_view, VkImageLayout src_img_layout,
                                              VkImageView dst_img_view);

void
vk_texcompress_bc4_fill_write_descriptor_sets(struct vk_texcompress_bcn_state *bcn,
                                              struct vk_texcompress_bc4_write_descriptor_set *set,
                                              VkImageView src_img_view, VkImageLayout src_img_layout,
                                              VkImageView dst_img_view);

void
vk_texcompress_bc3_fill_write_descriptor_sets(struct vk_texcompress_bcn_state *bcn,
                                              struct vk_texcompress_bcn_write_descriptor_set *set,
                                              VkImageView bc1_src_img_view, VkImageView bc4_src_img_view,
                                              VkImageView dst_img_view);

VkPipeline
vk_texcompress_rgba_get_encode_pipeline(struct vk_device *device,
                                        VkAllocationCallbacks *allocator,
                                        struct vk_texcompress_bcn_state *bcn,
                                        VkPipelineCache pipeline_cache,
                                        enum compute_pipeline_id id);

VkResult
vk_texcompress_bcn_init(struct vk_device *device,
                        VkAllocationCallbacks *allocator,
                        VkPipelineCache pipeline_cache,
                        struct vk_texcompress_bcn_state *bcn[]);

void
vk_texcompress_bcn_finish(struct vk_device *device,
                          VkAllocationCallbacks *allocator,
                          struct vk_texcompress_bcn_state *bcn[]);


void
vk_texcompress_create_fence(struct vk_device *device, VkAllocationCallbacks *allocator, VkFence *fence);

void
vk_texcompress_wait_fence(struct vk_device *device, VkAllocationCallbacks *allocator, VkFence *fence);

#endif /* VK_TEXCOMPRESS_BCN_H */
