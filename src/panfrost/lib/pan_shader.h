/*
 * Copyright (C) 2021 Collabora, Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef __PAN_SHADER_H__
#define __PAN_SHADER_H__

#include "compiler/nir/nir.h"
#include "panfrost/util/pan_ir.h"

#include "pan_device.h"
#include "midgard_pack.h"

struct panfrost_device;

const nir_shader_compiler_options *
pan_shader_get_compiler_options(const struct panfrost_device *dev);

void
pan_shader_compile(const struct panfrost_device *dev,
                   nir_shader *nir,
                   const struct panfrost_compile_inputs *inputs,
                   struct util_dynarray *binary,
                   struct pan_shader_info *info);

static inline void
pan_shader_prepare_midgard_rsd(const struct pan_shader_info *info,
                               struct MALI_RENDERER_STATE *rsd)
{
        rsd->properties.uniform_buffer_count = info->ubo_count;
        rsd->properties.midgard.uniform_count = info->midgard.uniform_cutoff;
        rsd->properties.midgard.shader_has_side_effects = info->writes_global;
        rsd->properties.midgard.fp_mode = MALI_FP_MODE_GL_INF_NAN_ALLOWED;

        /* For fragment shaders, work register count, early-z, reads at draw-time */

        if (info->stage != MESA_SHADER_FRAGMENT)
                rsd->properties.midgard.work_register_count = info->work_reg_count;
}

static inline void
pan_shader_prepare_bifrost_rsd(const struct pan_shader_info *info,
                               struct MALI_RENDERER_STATE *rsd)
{
        unsigned fau_count = DIV_ROUND_UP(info->push.count, 2);

        switch (info->stage) {
        case MESA_SHADER_VERTEX:
                rsd->properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
                rsd->properties.uniform_buffer_count = info->ubo_count;

                rsd->preload.uniform_count = fau_count;
                rsd->preload.vertex.vertex_id = true;
                rsd->preload.vertex.instance_id = true;
                break;

        case MESA_SHADER_FRAGMENT:
                /* Early-Z set at draw-time */
                if (info->fs.writes_depth || info->fs.writes_stencil) {
                        rsd->properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_FORCE_LATE;
                        rsd->properties.bifrost.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_LATE;
                } else if (info->fs.can_discard) {
                        rsd->properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_FORCE_LATE;
                        rsd->properties.bifrost.pixel_kill_operation = MALI_PIXEL_KILL_WEAK_EARLY;
                } else {
                        rsd->properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
                        rsd->properties.bifrost.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
                }
                rsd->properties.uniform_buffer_count = info->ubo_count;
                rsd->properties.bifrost.shader_modifies_coverage = info->fs.can_discard;
                rsd->properties.bifrost.shader_wait_dependency_6 = info->bifrost.wait_6;
                rsd->properties.bifrost.shader_wait_dependency_7 = info->bifrost.wait_7;

                rsd->preload.uniform_count = fau_count;
                rsd->preload.fragment.fragment_position = info->fs.reads_frag_coord;
                rsd->preload.fragment.coverage = true;
                rsd->preload.fragment.primitive_flags = info->fs.reads_face;

                /* Contains sample ID and sample mask. Sample position and
                 * helper invocation are expressed in terms of the above, so
                 * preload for those too */
                rsd->preload.fragment.sample_mask_id =
                        info->fs.reads_sample_id |
                        info->fs.reads_sample_pos |
                        info->fs.reads_sample_mask_in |
                        info->fs.reads_helper_invocation;
                break;

        case MESA_SHADER_COMPUTE:
                rsd->properties.uniform_buffer_count = info->ubo_count;

                rsd->preload.uniform_count = fau_count;
                rsd->preload.compute.local_invocation_xy = true;
                rsd->preload.compute.local_invocation_z = true;
                rsd->preload.compute.work_group_x = true;
                rsd->preload.compute.work_group_y = true;
                rsd->preload.compute.work_group_z = true;
                rsd->preload.compute.global_invocation_x = true;
                rsd->preload.compute.global_invocation_y = true;
                rsd->preload.compute.global_invocation_z = true;
                break;

        default:
                unreachable("TODO");
        }
}

static inline void
pan_shader_prepare_rsd(const struct panfrost_device *dev,
                       const struct pan_shader_info *shader_info,
                       mali_ptr shader_ptr,
                       struct MALI_RENDERER_STATE *rsd)
{
        if (!pan_is_bifrost(dev))
                shader_ptr |= shader_info->midgard.first_tag;

        rsd->shader.shader = shader_ptr;
        rsd->shader.attribute_count = shader_info->attribute_count;
        rsd->shader.varying_count = shader_info->varyings.input_count +
                                   shader_info->varyings.output_count;
        rsd->shader.texture_count = shader_info->texture_count;
        rsd->shader.sampler_count = shader_info->texture_count;
        rsd->properties.shader_contains_barrier = shader_info->contains_barrier;

        if (shader_info->stage == MESA_SHADER_FRAGMENT) {
                rsd->properties.shader_contains_barrier |=
                        shader_info->fs.helper_invocations;
                rsd->properties.stencil_from_shader =
                        shader_info->fs.writes_stencil;
                rsd->properties.depth_source =
                        shader_info->fs.writes_depth ?
                        MALI_DEPTH_SOURCE_SHADER :
                        MALI_DEPTH_SOURCE_FIXED_FUNCTION;
        } else {
                rsd->properties.depth_source =
                        MALI_DEPTH_SOURCE_FIXED_FUNCTION;
        }

        if (pan_is_bifrost(dev))
                pan_shader_prepare_bifrost_rsd(shader_info, rsd);
        else
                pan_shader_prepare_midgard_rsd(shader_info, rsd);
}

#endif