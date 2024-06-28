/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"

#include "BLI_hash.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_vector.hh"
#include "BLI_task.h"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colorband.hh"
#include "BKE_colortools.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"

#include "IMB_colormanagement.hh"

#include "sculpt_intern.hh"

#include "IMB_imbuf.hh"

#include "bmesh.hh"

#include <cmath>
#include <cstdlib>

namespace blender::ed::sculpt_paint::color {

static void do_color_smooth_task(Object &ob,
                                 const Span<float3> vert_positions,
                                 const Span<float3> vert_normals,
                                 const Span<bool> hide_vert,
                                 const Span<float> mask,
                                 const Brush &brush,
                                 PBVHNode *node)
{
  SculptSession &ss = *ob.sculpt;
  const float bstrength = ss.cache->bstrength;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, test, brush.falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(nullptr);

  auto_mask::NodeData automask_data = auto_mask::node_begin(
      ob, ss.cache->automasking.get(), *node);

  const Span<int> verts = bke::pbvh::node_verts(*node);
  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    if (!hide_vert.is_empty() && hide_vert[verts[i]]) {
      continue;
    }
    if (!sculpt_brush_test_sq_fn(test, vert_positions[vert])) {
      continue;
    }

    auto_mask::node_update(automask_data, i);

    const float fade = bstrength *
                       SCULPT_brush_strength_factor(ss,
                                                    brush,
                                                    vert_positions[vert],
                                                    sqrtf(test.dist),
                                                    vert_normals[vert],
                                                    nullptr,
                                                    mask.is_empty() ? 0.0f : mask[vert],
                                                    PBVHVertRef{vert},
                                                    thread_id,
                                                    &automask_data);

    const float4 smooth_color = smooth::neighbor_color_average(ss, vert);
    float4 col = SCULPT_vertex_color_get(ss, vert);
    blend_color_interpolate_float(col, col, smooth_color, fade);
    SCULPT_vertex_color_set(ss, vert, col);
  }
}

static void do_paint_brush_task(Object &ob,
                                const Span<float3> vert_positions,
                                const Span<float3> vert_normals,
                                const Span<bool> hide_vert,
                                const Span<float> mask,
                                const Brush &brush,
                                const float (*mat)[4],
                                float *wet_mix_sampled_color,
                                PBVHNode *node)
{
  SculptSession &ss = *ob.sculpt;
  const float bstrength = fabsf(ss.cache->bstrength);

  SculptOrigVertData orig_data = SCULPT_orig_vert_data_init(ob, *node, undo::Type::Color);

  PBVHColorBufferNode *color_buffer = BKE_pbvh_node_color_buffer_get(node);

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, test, brush.falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(nullptr);

  float brush_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  copy_v3_v3(brush_color,
             ss.cache->invert ? BKE_brush_secondary_color_get(ss.scene, &brush) :
                                BKE_brush_color_get(ss.scene, &brush));

  IMB_colormanagement_srgb_to_scene_linear_v3(brush_color, brush_color);

  auto_mask::NodeData automask_data = auto_mask::node_begin(
      ob, ss.cache->automasking.get(), *node);

  if (brush.flag & BRUSH_USE_GRADIENT) {
    switch (brush.gradient_stroke_mode) {
      case BRUSH_GRADIENT_PRESSURE:
        BKE_colorband_evaluate(brush.gradient, ss.cache->pressure, brush_color);
        break;
      case BRUSH_GRADIENT_SPACING_REPEAT: {
        float coord = fmod(ss.cache->stroke_distance / brush.gradient_spacing, 1.0);
        BKE_colorband_evaluate(brush.gradient, coord, brush_color);
        break;
      }
      case BRUSH_GRADIENT_SPACING_CLAMP: {
        BKE_colorband_evaluate(
            brush.gradient, ss.cache->stroke_distance / brush.gradient_spacing, brush_color);
        break;
      }
    }
  }

  const Span<int> verts = bke::pbvh::node_unique_verts(*node);
  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    if (!hide_vert.is_empty() && hide_vert[vert]) {
      continue;
    }
    SCULPT_orig_vert_data_update(orig_data, i);

    bool affect_vertex = false;
    float distance_to_stroke_location = 0.0f;
    if (brush.tip_roundness < 1.0f) {
      affect_vertex = SCULPT_brush_test_cube(
          test, vert_positions[vert], mat, brush.tip_roundness, brush.tip_scale_x);
      distance_to_stroke_location = ss.cache->radius * test.dist;
    }
    else {
      affect_vertex = sculpt_brush_test_sq_fn(test, vert_positions[vert]);
      distance_to_stroke_location = sqrtf(test.dist);
    }

    if (!affect_vertex) {
      continue;
    }

    auto_mask::node_update(automask_data, i);

    float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                          brush,
                                                          vert_positions[vert],
                                                          distance_to_stroke_location,
                                                          vert_normals[vert],
                                                          nullptr,
                                                          mask.is_empty() ? 0.0f : mask[vert],
                                                          PBVHVertRef{vert},
                                                          thread_id,
                                                          &automask_data);

    /* Density. */
    float noise = 1.0f;
    const float density = ss.cache->paint_brush.density;
    if (density < 1.0f) {
      const float hash_noise = float(BLI_hash_int_01(ss.cache->density_seed * 1000 * vert));
      if (hash_noise > density) {
        noise = density * hash_noise;
        fade = fade * noise;
      }
    }

    /* Brush paint color, brush test falloff and flow. */
    float paint_color[4];
    float wet_mix_color[4];
    float buffer_color[4];

    mul_v4_v4fl(paint_color, brush_color, fade * ss.cache->paint_brush.flow);
    mul_v4_v4fl(wet_mix_color, wet_mix_sampled_color, fade * ss.cache->paint_brush.flow);

    /* Interpolate with the wet_mix color for wet paint mixing. */
    blend_color_interpolate_float(
        paint_color, paint_color, wet_mix_color, ss.cache->paint_brush.wet_mix);
    blend_color_mix_float(color_buffer->color[i], color_buffer->color[i], paint_color);

    /* Final mix over the original color using brush alpha. We apply auto-making again
     * at this point to avoid washing out non-binary masking modes like cavity masking. */
    float automasking = auto_mask::factor_get(
        ss.cache->automasking.get(), ss, PBVHVertRef{vert}, &automask_data);
    const float alpha = BKE_brush_alpha_get(ss.scene, &brush);
    mul_v4_v4fl(buffer_color, color_buffer->color[i], alpha * automasking);

    float4 col = SCULPT_vertex_color_get(ss, vert);
    IMB_blend_color_float(col, orig_data.col, buffer_color, IMB_BlendMode(brush.blend));
    col = math::clamp(col, 0.0f, 1.0f);
    SCULPT_vertex_color_set(ss, vert, col);
  }
}

struct SampleWetPaintData {
  int tot_samples;
  float color[4];
};

static void do_sample_wet_paint_task(SculptSession &ss,
                                     const Span<float3> vert_positions,
                                     const Span<bool> hide_vert,
                                     const Brush &brush,
                                     PBVHNode *node,
                                     SampleWetPaintData *swptd)
{
  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, test, brush.falloff_shape);

  test.radius *= brush.wet_paint_radius_factor;
  test.radius_squared = test.radius * test.radius;

  for (const int vert : bke::pbvh::node_unique_verts(*node)) {
    if (!hide_vert.is_empty() && hide_vert[vert]) {
      continue;
    }
    if (!sculpt_brush_test_sq_fn(test, vert_positions[vert])) {
      continue;
    }

    float4 col = SCULPT_vertex_color_get(ss, vert);

    add_v4_v4(swptd->color, col);
    swptd->tot_samples++;
  }
}

void do_paint_brush(PaintModeSettings &paint_mode_settings,
                    const Sculpt &sd,
                    Object &ob,
                    Span<PBVHNode *> nodes,
                    Span<PBVHNode *> texnodes)
{
  if (SCULPT_use_image_paint_brush(paint_mode_settings, ob)) {
    SCULPT_do_paint_brush_image(paint_mode_settings, sd, ob, texnodes);
    return;
  }

  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.sculpt;

  if (!SCULPT_has_colors(ss)) {
    return;
  }

  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    if (SCULPT_stroke_is_first_brush_step(*ss.cache)) {
      ss.cache->density_seed = float(BLI_hash_int_01(ss.cache->location[0] * 1000));
    }
    return;
  }

  BKE_curvemapping_init(brush.curve);

  float mat[4][4];

  /* If the brush is round the tip does not need to be aligned to the surface, so this saves a
   * whole iteration over the affected nodes. */
  if (brush.tip_roundness < 1.0f) {
    SCULPT_cube_tip_init(sd, ob, brush, mat);

    if (is_zero_m4(mat)) {
      return;
    }
  }

  /* Smooth colors mode. */
  const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
  const Span<float3> vert_positions = BKE_pbvh_get_vert_positions(*ss.pbvh);
  const Span<float3> vert_normals = BKE_pbvh_get_vert_normals(*ss.pbvh);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
  const VArraySpan mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
  if (ss.cache->alt_smooth) {
    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (const int i : range) {
        do_color_smooth_task(ob, vert_positions, vert_normals, hide_vert, mask, brush, nodes[i]);
      }
    });
    return;
  }

  /* Regular Paint mode. */

  /* Wet paint color sampling. */
  float4 wet_color(0);
  if (ss.cache->paint_brush.wet_mix > 0.0f) {
    const SampleWetPaintData swptd = threading::parallel_reduce(
        nodes.index_range(),
        1,
        SampleWetPaintData{},
        [&](const IndexRange range, SampleWetPaintData swptd) {
          for (const int i : range) {
            do_sample_wet_paint_task(ss, vert_positions, hide_vert, brush, nodes[i], &swptd);
          }
          return swptd;
        },
        [](const SampleWetPaintData &a, const SampleWetPaintData &b) {
          SampleWetPaintData joined{};
          joined.tot_samples = a.tot_samples + b.tot_samples;
          add_v4_v4v4(joined.color, a.color, b.color);
          return joined;
        });

    if (swptd.tot_samples > 0 && is_finite_v4(swptd.color)) {
      copy_v4_v4(wet_color, swptd.color);
      mul_v4_fl(wet_color, 1.0f / swptd.tot_samples);
      wet_color = math::clamp(wet_color, 0.0f, 1.0f);

      if (ss.cache->first_time) {
        copy_v4_v4(ss.cache->wet_mix_prev_color, wet_color);
      }
      blend_color_interpolate_float(wet_color,
                                    wet_color,
                                    ss.cache->wet_mix_prev_color,
                                    ss.cache->paint_brush.wet_persistence);
      copy_v4_v4(ss.cache->wet_mix_prev_color, wet_color);
      ss.cache->wet_mix_prev_color = math::clamp(ss.cache->wet_mix_prev_color, 0.0f, 1.0f);
    }
  }

  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (const int i : range) {
      do_paint_brush_task(
          ob, vert_positions, vert_normals, hide_vert, mask, brush, mat, wet_color, nodes[i]);
    }
  });
}

static void do_smear_brush_task(Object &ob,
                                const Span<float3> vert_positions,
                                const Span<float3> vert_normals,
                                const Span<bool> hide_vert,
                                const Span<float> mask,
                                const Brush &brush,
                                PBVHNode *node)
{
  SculptSession &ss = *ob.sculpt;
  const float bstrength = ss.cache->bstrength;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, test, brush.falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(nullptr);

  float brush_delta[3];

  if (brush.flag & BRUSH_ANCHORED) {
    copy_v3_v3(brush_delta, ss.cache->grab_delta_symmetry);
  }
  else {
    sub_v3_v3v3(brush_delta, ss.cache->location, ss.cache->last_location);
  }

  auto_mask::NodeData automask_data = auto_mask::node_begin(
      ob, ss.cache->automasking.get(), *node);

  const Span<int> verts = bke::pbvh::node_unique_verts(*node);
  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    if (!hide_vert.is_empty() && hide_vert[vert]) {
      continue;
    }
    if (!sculpt_brush_test_sq_fn(test, vert_positions[vert])) {
      continue;
    }

    auto_mask::node_update(automask_data, i);

    const float fade = bstrength *
                       SCULPT_brush_strength_factor(ss,
                                                    brush,
                                                    vert_positions[vert],
                                                    sqrtf(test.dist),
                                                    vert_normals[vert],
                                                    nullptr,
                                                    mask.is_empty() ? 0.0f : mask[vert],
                                                    PBVHVertRef{vert},
                                                    thread_id,
                                                    &automask_data);

    float current_disp[3];
    float current_disp_norm[3];

    const float3 &no = vert_normals[vert];

    switch (brush.smear_deform_type) {
      case BRUSH_SMEAR_DEFORM_DRAG:
        copy_v3_v3(current_disp, brush_delta);
        break;
      case BRUSH_SMEAR_DEFORM_PINCH:
        sub_v3_v3v3(current_disp, ss.cache->location, vert_positions[vert]);
        break;
      case BRUSH_SMEAR_DEFORM_EXPAND:
        sub_v3_v3v3(current_disp, vert_positions[vert], ss.cache->location);
        break;
    }

    /* Project into vertex plane. */
    madd_v3_v3fl(current_disp, no, -dot_v3v3(current_disp, no));

    normalize_v3_v3(current_disp_norm, current_disp);
    mul_v3_v3fl(current_disp, current_disp_norm, bstrength);

    float accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float totw = 0.0f;

    /*
     * NOTE: we have to do a nested iteration here to avoid
     * blocky artifacts on quad topologies.  The runtime cost
     * is not as bad as it seems due to neighbor iteration
     * in the sculpt code being cache bound; once the data is in
     * the cache iterating over it a few more times is not terribly
     * costly.
     */

    SculptVertexNeighborIter ni2;
    SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, PBVHVertRef{vert}, ni2) {
      const float *nco = SCULPT_vertex_co_get(ss, ni2.vertex);

      SculptVertexNeighborIter ni;
      SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, ni2.vertex, ni) {
        if (ni.index == vert) {
          continue;
        }

        float vertex_disp[3];
        float vertex_disp_norm[3];

        sub_v3_v3v3(vertex_disp, SCULPT_vertex_co_get(ss, ni.vertex), vert_positions[vert]);

        /* Weight by how close we are to our target distance from vd.co. */
        float w = (1.0f + fabsf(len_v3(vertex_disp) / bstrength - 1.0f));

        /* TODO: use cotangents (or at least face areas) here. */
        float len = len_v3v3(SCULPT_vertex_co_get(ss, ni.vertex), nco);
        if (len > 0.0f) {
          len = bstrength / len;
        }
        else { /* Coincident point. */
          len = 1.0f;
        }

        /* Multiply weight with edge lengths (in the future this will be
         * cotangent weights or face areas). */
        w *= len;

        /* Build directional weight. */

        /* Project into vertex plane. */
        madd_v3_v3fl(vertex_disp, no, -dot_v3v3(no, vertex_disp));
        normalize_v3_v3(vertex_disp_norm, vertex_disp);

        if (dot_v3v3(current_disp_norm, vertex_disp_norm) >= 0.0f) {
          continue;
        }

        const float *neighbor_color = ss.cache->prev_colors[ni.index];
        float color_interp = -dot_v3v3(current_disp_norm, vertex_disp_norm);

        /* Square directional weight to get a somewhat sharper result. */
        w *= color_interp * color_interp;

        madd_v4_v4fl(accum, neighbor_color, w);
        totw += w;
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
    }
    SCULPT_VERTEX_NEIGHBORS_ITER_END(ni2);

    if (totw != 0.0f) {
      mul_v4_fl(accum, 1.0f / totw);
    }

    float4 col = SCULPT_vertex_color_get(ss, vert);
    blend_color_interpolate_float(col, ss.cache->prev_colors[vert], accum, fade);
    SCULPT_vertex_color_set(ss, vert, col);
  }
}

static void do_smear_store_prev_colors_task(SculptSession &ss, PBVHNode *node, float4 *prev_colors)
{
  for (const int vert : bke::pbvh::node_unique_verts(*node)) {
    prev_colors[vert] = SCULPT_vertex_color_get(ss, vert);
  }
}

void do_smear_brush(const Sculpt &sd, Object &ob, Span<PBVHNode *> nodes)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.sculpt;

  if (!SCULPT_has_colors(ss) || ss.cache->bstrength == 0.0f) {
    return;
  }

  const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
  const Span<float3> vert_positions = BKE_pbvh_get_vert_positions(*ss.pbvh);
  const Span<float3> vert_normals = BKE_pbvh_get_vert_normals(*ss.pbvh);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
  const VArraySpan mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);

  if (ss.cache->prev_colors.is_empty()) {
    ss.cache->prev_colors = Array<float4>(mesh.verts_num);
    for (int i = 0; i < mesh.verts_num; i++) {
      ss.cache->prev_colors[i] = SCULPT_vertex_color_get(ss, i);
    }
  }

  BKE_curvemapping_init(brush.curve);

  /* Smooth colors mode. */
  if (ss.cache->alt_smooth) {
    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (const int i : range) {
        do_color_smooth_task(ob, vert_positions, vert_normals, hide_vert, mask, brush, nodes[i]);
      }
    });
  }
  else {
    /* Smear mode. */
    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (const int i : range) {
        do_smear_store_prev_colors_task(ss, nodes[i], ss.cache->prev_colors.data());
      }
    });
    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (const int i : range) {
        do_smear_brush_task(ob, vert_positions, vert_normals, hide_vert, mask, brush, nodes[i]);
      }
    });
  }
}

}  // namespace blender::ed::sculpt_paint::color
