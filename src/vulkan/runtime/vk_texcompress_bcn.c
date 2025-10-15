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

#include "vk_texcompress_bcn.h"
#include "util/bc1_tables.h"
#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_image.h"
#include "vk_physical_device.h"
#include "log/log.h"

void
vk_texcompress_insert_head(staging_images **head, bcn_image *current)
{
   staging_images *node = (staging_images *)malloc(sizeof(staging_images));
   if (node == NULL) {
      ALOGE("Failed to alloc staging_images node");
      return;
   }

   node->current = current;
   node->next = *head;
   *head = node;
}

void
vk_texcompress_delete_head(staging_images **head, struct vk_device *device)
{
   if (*head == NULL) {
      return;
   }
   staging_images *current_head = *head;
   vk_texcompress_finish_image(device, NULL, current_head->current);
   *head = current_head->next;
   free(current_head);
}

/* type_indexes_mask bits are set/clear for support memory type index as per
 * struct VkPhysicalDeviceMemoryProperties.memoryTypes[] */
static uint32_t
get_mem_type_index(struct vk_device *device, uint32_t type_indexes_mask,
                   VkMemoryPropertyFlags mem_property)
{
   const struct vk_physical_device_dispatch_table *disp = &device->physical->dispatch_table;
   VkPhysicalDevice _phy_device = vk_physical_device_to_handle(device->physical);

   VkPhysicalDeviceMemoryProperties2 props2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
      .pNext = NULL,
   };
   disp->GetPhysicalDeviceMemoryProperties2(_phy_device, &props2);

   for (uint32_t i = 0; i < props2.memoryProperties.memoryTypeCount; i++) {
      if ((type_indexes_mask & (1 << i)) &&
          ((props2.memoryProperties.memoryTypes[i].propertyFlags & mem_property) == mem_property)) {
         return i;
      }
   }

   return -1;
}

static VkResult
create_buffer(struct vk_device *device, VkAllocationCallbacks *allocator, struct vk_texcompress_bcn_state *bc1)
{
   VkDeviceSize size = sizeof(float) * (sizeof(stb__OMatch5) + sizeof(stb__OMatch6));
   VkResult result;
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkBufferCreateInfo buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   result =
      disp->CreateBuffer(_device, &buffer_create_info, allocator, &bc1->bc1_table_buf);
   if (unlikely(result != VK_SUCCESS))
      return result;

   VkBufferMemoryRequirementsInfo2 mem_req_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
      .buffer = bc1->bc1_table_buf,
   };
   VkMemoryRequirements2 mem_req = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
   };
   disp->GetBufferMemoryRequirements2(_device, &mem_req_info, &mem_req);

   uint32_t mem_type_index = get_mem_type_index(
      device, mem_req.memoryRequirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   if (mem_type_index == -1)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_req.memoryRequirements.size,
      .memoryTypeIndex = mem_type_index,
   };
   result = disp->AllocateMemory(_device, &alloc_info, allocator, &bc1->bc1_table_mem);
   if (unlikely(result != VK_SUCCESS))
      return result;

   result = disp->BindBufferMemory(_device, bc1->bc1_table_buf, bc1->bc1_table_mem, 0);
   if (unlikely(result != VK_SUCCESS))
      return result;

   void *data;
   disp->MapMemory(_device, bc1->bc1_table_mem, 0, size, 0, &data);
   float (*data_array)[2] = (float(*)[2])data;
   for (int i = 0; i < 256; i++) {
      for (int j = 0; j < 2; j++) {
         data_array[i][j] = (float)stb__OMatch5[i][j];
         data_array[i + 256][j] = (float)stb__OMatch6[i][j];
      }
   }
   disp->UnmapMemory(_device, bc1->bc1_table_mem);

   return result;
}

VkResult
vk_texcompress_create_image(struct vk_device *device, VkAllocationCallbacks *allocator, VkDeviceMemory *pMem,
                            VkImage *image, VkExtent3D extent, VkFormat format, int size)
{
   bool is_dummy = (size != 0);
   VkResult result;
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkImageCreateInfo image_create_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = extent,
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   result = disp->CreateImage(_device, &image_create_info, allocator, image);
   if (unlikely(result != VK_SUCCESS)) {
      return result;
   }

   if (is_dummy) {
      VkMemoryRequirements mem_requirements;
      disp->GetImageMemoryRequirements(_device, *image, &mem_requirements);

      uint32_t mem_type_index = get_mem_type_index(
         device, mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (mem_type_index == -1) {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      VkMemoryAllocateInfo alloc_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = size,
         .memoryTypeIndex = mem_type_index,
      };
      result = disp->AllocateMemory(_device, &alloc_info, allocator, pMem);
      if (unlikely(result != VK_SUCCESS)) {
         return result;
      }
   }
   if (is_dummy) {
      disp->DestroyImage(_device, *image, allocator);
   }
   return result;
}

void
vk_texcompress_finish_image(struct vk_device *device, const VkAllocationCallbacks *allocator,
                            struct vk_texcompress_bcn_image *intermidate_image)
{
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;
   disp->DestroyImage(vk_device_to_handle(device), intermidate_image->image_rgba, NULL);
   disp->DestroyImage(vk_device_to_handle(device), intermidate_image->image_bc1, NULL);
   disp->DestroyImage(vk_device_to_handle(device), intermidate_image->image_bc4, NULL);
   disp->FreeMemory(vk_device_to_handle(device), intermidate_image->image_mem, NULL);
   vk_free(&device->alloc, intermidate_image);
}

void bind_memory(struct vk_device *device, VkImage *image, VkDeviceMemory *pMem, int offset)
{
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;
   disp->BindImageMemory(_device, *image, *pMem, offset);
}

static VkResult
create_sampler(struct vk_device *device, VkAllocationCallbacks *allocator,
               VkSampler *sampler, VkFilter filter, VkSamplerMipmapMode mipmapMode)
{
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkSamplerCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = filter,
      .minFilter = filter,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
      .anisotropyEnable = VK_FALSE,
      .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
      .unnormalizedCoordinates = VK_FALSE,
      .compareEnable = VK_FALSE,
      .mipmapMode = mipmapMode,
   };
   VkResult result = disp->CreateSampler(_device, &create_info, allocator, sampler);
   return result;
}

static VkResult
create_bc1_layout(struct vk_device *device, VkAllocationCallbacks *allocator,
                  struct vk_texcompress_bcn_state *bcn)
{
   VkResult result;
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkDescriptorSetLayoutBinding bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = NULL,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = NULL,
      },
      {
         .binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = NULL,
      },
   };

   VkDescriptorSetLayoutCreateInfo ds_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount = ARRAY_SIZE(bindings),
      .pBindings = bindings,
   };

   result = disp->CreateDescriptorSetLayout(_device, &ds_create_info, allocator, &bcn->ds_layout);
   return result;
}

static VkResult
create_bc4_layout(struct vk_device *device, VkAllocationCallbacks *allocator,
                  struct vk_texcompress_bcn_state *bcn)
{
   VkResult result;
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkDescriptorSetLayoutBinding bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = NULL,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = NULL,
      },
   };

   VkDescriptorSetLayoutCreateInfo ds_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount = ARRAY_SIZE(bindings),
      .pBindings = bindings,
   };

   result = disp->CreateDescriptorSetLayout(_device, &ds_create_info, allocator, &bcn->ds_layout);
   return result;
}

static VkResult
create_bc3_layout(struct vk_device *device, VkAllocationCallbacks *allocator,
                  struct vk_texcompress_bcn_state *bcn)
{
   VkResult result;
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkDescriptorSetLayoutBinding bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = NULL,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = NULL,
      },
      {
         .binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = NULL,
      },
   };

   VkDescriptorSetLayoutCreateInfo ds_create_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
      .bindingCount = ARRAY_SIZE(bindings),
      .pBindings = bindings,
   };

   result = disp->CreateDescriptorSetLayout(_device, &ds_create_info, allocator, &bcn->ds_layout);
   return result;
}

static VkResult
create_pipeline_layout(struct vk_device *device, VkAllocationCallbacks *allocator,
                       struct vk_texcompress_bcn_state *bcn, enum compute_pipeline_id id)
{
   VkResult result = VK_RESULT_MAX_ENUM;
   switch (id) {
   case BC1_ENCODE:
      result = create_bc1_layout(device, allocator, bcn);
      break;
   case BC4_ENCODE:
      result = create_bc4_layout(device, allocator, bcn);
      break;
   case BC3_ENCODE:
      result = create_bc3_layout(device, allocator, bcn);
      break;
   default:
      goto fail;
   }
   if (result != VK_SUCCESS) {
      goto fail;
   }
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;
   VkPipelineLayoutCreateInfo pl_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &bcn->ds_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &(VkPushConstantRange) {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8}
   };
   result = disp->CreatePipelineLayout(_device, &pl_create_info, allocator, &bcn->p_layout);
fail:
   return result;
}

static const uint32_t bc1_spv[] = {
#include "bc1_spv.h"
};

static const uint32_t bc4_spv[] = {
#include "bc4_spv.h"
};

static const uint32_t bc3_spv[] = {
#include "bc3_spv.h"
};

static VkResult
vk_bc1_create_shader_module(struct vk_device *device,
                             VkAllocationCallbacks *allocator,
                             struct vk_texcompress_bcn_state *bcn)
{
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkShaderModuleCreateInfo shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .codeSize = sizeof(bc1_spv),
      .pCode = bc1_spv,
   };

   return disp->CreateShaderModule(_device, &shader_module_create_info, allocator, &bcn->shader_module);
}

static VkResult
vk_bc4_create_shader_module(struct vk_device *device,
                             VkAllocationCallbacks *allocator,
                             struct vk_texcompress_bcn_state *bcn)
{
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkShaderModuleCreateInfo shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .codeSize = sizeof(bc4_spv),
      .pCode = bc4_spv,
   };

   return disp->CreateShaderModule(_device, &shader_module_create_info, allocator, &bcn->shader_module);
}

static VkResult
vk_bc3_create_shader_module(struct vk_device *device,
                             VkAllocationCallbacks *allocator,
                             struct vk_texcompress_bcn_state *bcn)
{
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkShaderModuleCreateInfo shader_module_create_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .pNext = NULL,
      .flags = 0,
      .codeSize = sizeof(bc3_spv),
      .pCode = bc3_spv,
   };

   return disp->CreateShaderModule(_device, &shader_module_create_info, allocator, &bcn->shader_module);
}

static VkResult
create_bcn_encode_pipeline(struct vk_device *device,
                           VkAllocationCallbacks *allocator,
                           struct vk_texcompress_bcn_state *bcn,
                           VkPipelineCache pipeline_cache)
{
   VkResult result;
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;
   VkPipeline pipeline;

   /* compute shader */
   VkPipelineShaderStageCreateInfo pipeline_shader_stage = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = bcn->shader_module,
      .pName = "main",
   };

   VkComputePipelineCreateInfo vk_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = pipeline_shader_stage,
      .flags = 0,
      .layout = bcn->p_layout,
   };

   result = disp->CreateComputePipelines(_device, pipeline_cache, 1, &vk_pipeline_info, allocator, &pipeline);
   if (result != VK_SUCCESS)
      return result;

   bcn->pipeline = pipeline;

   return result;
}

VkPipeline
vk_texcompress_rgba_get_encode_pipeline(struct vk_device *device, VkAllocationCallbacks *allocator,
                                        struct vk_texcompress_bcn_state *bcn, VkPipelineCache pipeline_cache,
                                        enum compute_pipeline_id id)
{
   VkResult result;

   simple_mtx_lock(&bcn->mutex);

   if (bcn->pipeline)
      goto unlock;

   if (id >= MAX_PIPELINE_NUM || id < 0)
      goto unlock;

   if (id == BC1_ENCODE && !bcn->shader_module) {
      result = vk_bc1_create_shader_module(device, allocator, bcn);
      if (result != VK_SUCCESS)
         goto unlock;
   }

   if (id == BC4_ENCODE && !bcn->shader_module) {
      result = vk_bc4_create_shader_module(device, allocator, bcn);
      if (result != VK_SUCCESS)
         goto unlock;
   }

   if (id == BC3_ENCODE && !bcn->shader_module) {
      result = vk_bc3_create_shader_module(device, allocator, bcn);
      if (result != VK_SUCCESS)
         goto unlock;
   }

   create_bcn_encode_pipeline(device, allocator, bcn, pipeline_cache);

unlock:
   simple_mtx_unlock(&bcn->mutex);
   return bcn->pipeline;
}

static inline void
fill_desc_image_info_struct(VkDescriptorImageInfo *info, VkSampler sampler, VkImageView img_view,
                            VkImageLayout img_layout)
{
   info->sampler = sampler;
   info->imageView = img_view;
   info->imageLayout = img_layout;
}

static inline void
fill_write_descriptor_set_image(VkWriteDescriptorSet *set, uint8_t bind_i,
                                VkDescriptorType desc_type, VkDescriptorImageInfo *image_info)
{
   set->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   set->pNext = NULL;
   set->dstSet = VK_NULL_HANDLE;
   set->dstBinding = bind_i;
   set->dstArrayElement = 0;
   set->descriptorCount = 1;
   set->descriptorType = desc_type;
   set->pImageInfo = image_info;
   set->pBufferInfo = NULL;
   set->pTexelBufferView = NULL;
}

static inline void
fill_write_descriptor_set_buffer(VkWriteDescriptorSet *set,
                                 uint8_t bind_i, VkDescriptorType desc_type,
                                 VkDescriptorBufferInfo *buf_info)
{
   set->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   set->pNext = NULL;
   set->dstSet = VK_NULL_HANDLE;
   set->dstBinding = bind_i;
   set->dstArrayElement = 0;
   set->descriptorCount = 1;
   set->descriptorType = desc_type;
   set->pImageInfo = NULL;
   set->pBufferInfo = buf_info;
   set->pTexelBufferView = NULL;
}

void
vk_texcompress_bc1_fill_write_descriptor_sets(struct vk_texcompress_bcn_state *bcn,
                                               struct vk_texcompress_bcn_write_descriptor_set *set,
                                               VkImageView src_img_view, VkImageLayout src_img_layout,
                                               VkImageView dst_img_view)
{
   fill_desc_image_info_struct(&set->src_desc_image_info, bcn->sampler, src_img_view, src_img_layout);
   fill_write_descriptor_set_image(&set->descriptor_set[0], 0,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &set->src_desc_image_info);

   set->bc1_buffer_info.buffer = bcn->bc1_table_buf;
   set->bc1_buffer_info.offset = 0;
   set->bc1_buffer_info.range = VK_WHOLE_SIZE;
   fill_write_descriptor_set_buffer(&set->descriptor_set[1], 1,
                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &set->bc1_buffer_info);

   fill_desc_image_info_struct(&set->dst_desc_image_info, VK_NULL_HANDLE, dst_img_view, VK_IMAGE_LAYOUT_GENERAL);
   fill_write_descriptor_set_image(&set->descriptor_set[2], 2,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &set->dst_desc_image_info);
}

void
vk_texcompress_bc4_fill_write_descriptor_sets(struct vk_texcompress_bcn_state *bcn,
                                              struct vk_texcompress_bc4_write_descriptor_set *set,
                                              VkImageView src_img_view, VkImageLayout src_img_layout,
                                              VkImageView dst_img_view)
{
   fill_desc_image_info_struct(&set->src_desc_image_info, bcn->sampler, src_img_view, src_img_layout);
   fill_write_descriptor_set_image(&set->descriptor_set[0], 0,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &set->src_desc_image_info);

   fill_desc_image_info_struct(&set->dst_desc_image_info, VK_NULL_HANDLE, dst_img_view, VK_IMAGE_LAYOUT_GENERAL);
   fill_write_descriptor_set_image(&set->descriptor_set[1], 1,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &set->dst_desc_image_info);
}

void
vk_texcompress_bc3_fill_write_descriptor_sets(struct vk_texcompress_bcn_state *bcn,
                                               struct vk_texcompress_bcn_write_descriptor_set *set,
                                               VkImageView bc1_src_img_view, VkImageView bc4_src_img_view,
                                               VkImageView dst_img_view)
{
   fill_desc_image_info_struct(&set->src_desc_image_info, bcn->sampler, bc1_src_img_view, VK_IMAGE_LAYOUT_GENERAL);
   fill_write_descriptor_set_image(&set->descriptor_set[0], 0,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &set->src_desc_image_info);

   fill_desc_image_info_struct(&set->src_desc_image_info_2, bcn->sampler, bc4_src_img_view, VK_IMAGE_LAYOUT_GENERAL);
   fill_write_descriptor_set_image(&set->descriptor_set[1], 1,
                                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &set->src_desc_image_info_2);

   fill_desc_image_info_struct(&set->dst_desc_image_info, VK_NULL_HANDLE, dst_img_view, VK_IMAGE_LAYOUT_GENERAL);
   fill_write_descriptor_set_image(&set->descriptor_set[2], 2,
                                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &set->dst_desc_image_info);
}

VkResult
vk_texcompress_bcn_init(struct vk_device *device, VkAllocationCallbacks *allocator,
                        VkPipelineCache pipeline_cache,
                        struct vk_texcompress_bcn_state *bcn[])
{
   VkResult result;

   for (int pi = VK_TEXCOMPRESS_BCN_NUM_COMPUTE_PIPELINES - 1; pi >= 0; --pi) {
      /* memory to be freed as part of vk_bcn_decode_finish() */
      bcn[pi] = vk_zalloc(allocator, sizeof(struct vk_texcompress_bcn_state), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (bcn[pi] == NULL) {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
      simple_mtx_init(&(bcn[pi])->mutex, mtx_plain);
   }

   result = create_buffer(device, allocator, bcn[BC1_ENCODE]);
   if (result != VK_SUCCESS)
      goto fail;

   result = create_sampler(device, allocator, &bcn[BC1_ENCODE]->sampler,
                           VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR);
   if (result != VK_SUCCESS)
      goto fail;

   result = create_pipeline_layout(device, allocator, bcn[BC1_ENCODE], BC1_ENCODE);
   if (result != VK_SUCCESS)
      goto fail;

   result = create_sampler(device, allocator, &bcn[BC4_ENCODE]->sampler,
                           VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR);
   if (result != VK_SUCCESS)
      goto fail;

   result = create_pipeline_layout(device, allocator, bcn[BC4_ENCODE], BC4_ENCODE);
   if (result != VK_SUCCESS)
      goto fail;

   // 由于VK_FORMAT_R32G32_UINT格式的imageView格式不包含VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, 采用nearest
   result = create_sampler(device, allocator, &bcn[BC3_ENCODE]->sampler,
                           VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST);
   if (result != VK_SUCCESS)
      goto fail;

   result = create_pipeline_layout(device, allocator, bcn[BC3_ENCODE], BC3_ENCODE);
   if (result != VK_SUCCESS)
      goto fail;

fail:
   return result;
}

void
vk_texcompress_bcn_finish(struct vk_device *device,
                          VkAllocationCallbacks *allocator,
                          struct vk_texcompress_bcn_state *bcn[])
{
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   for (int pi = VK_TEXCOMPRESS_BCN_NUM_COMPUTE_PIPELINES - 1; pi >= 0; --pi) {
      if (bcn[pi]->pipeline)
         disp->DestroyPipeline(_device, bcn[pi]->pipeline, allocator);

      disp->DestroyPipelineLayout(_device, bcn[pi]->p_layout, allocator);
      disp->DestroyShaderModule(_device, bcn[pi]->shader_module, allocator);
      disp->DestroyDescriptorSetLayout(_device, bcn[pi]->ds_layout, allocator);

      if (pi == BC1_ENCODE) {
         disp->DestroyBuffer(_device, bcn[pi]->bc1_table_buf, allocator);
         disp->FreeMemory(_device, bcn[pi]->bc1_table_mem, allocator);
      }
      vk_free(allocator, bcn[pi]);
   }
}

void
vk_texcompress_create_fence(struct vk_device *device, VkAllocationCallbacks *allocator, VkFence *fence)
{
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkFenceCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
   };
   VkResult result = disp->CreateFence(_device, &create_info, allocator, fence);
   if (result != VK_SUCCESS) {
      ALOGE("Failed to create fence");
   }
}

void
vk_texcompress_wait_fence(struct vk_device *device, VkAllocationCallbacks *allocator, VkFence *fence)
{
   VkDevice _device = vk_device_to_handle(device);
   const struct vk_device_dispatch_table *disp = &device->dispatch_table;

   VkResult result = disp->WaitForFences(_device, 1, fence, VK_TRUE, UINT64_MAX);
   if (result != VK_SUCCESS) {
      ALOGE("Failed to wait for fence");
   }
}