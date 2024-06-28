/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.h"
#include "BLI_task.h"

#include "DNA_brush_types.h"

#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"

#include "sculpt_intern.hh"

#include "bmesh.hh"

#include <cmath>
#include <cstdlib>

namespace blender::ed::sculpt_paint::smooth {

float3 neighbor_coords_average_interior(const SculptSession &ss, PBVHVertRef vertex)
{
  float3 avg(0);
  int total = 0;
  int neighbor_count = 0;
  const bool is_boundary = SCULPT_vertex_is_boundary(ss, vertex);

  SculptVertexNeighborIter ni;
  SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
    neighbor_count++;
    if (is_boundary) {
      /* Boundary vertices use only other boundary vertices. */
      if (SCULPT_vertex_is_boundary(ss, ni.vertex)) {
        avg += SCULPT_vertex_co_get(ss, ni.vertex);
        total++;
      }
    }
    else {
      /* Interior vertices use all neighbors. */
      avg += SCULPT_vertex_co_get(ss, ni.vertex);
      total++;
    }
  }
  SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

  /* Do not modify corner vertices. */
  if (neighbor_count <= 2 && is_boundary) {
    return SCULPT_vertex_co_get(ss, vertex);
  }

  /* Avoid division by 0 when there are no neighbors. */
  if (total == 0) {
    return SCULPT_vertex_co_get(ss, vertex);
  }

  return avg / total;
}

void bmesh_four_neighbor_average(float avg[3], const float3 &direction, BMVert *v)
{
  float avg_co[3] = {0.0f, 0.0f, 0.0f};
  float tot_co = 0.0f;

  BMIter eiter;
  BMEdge *e;

  BM_ITER_ELEM (e, &eiter, v, BM_EDGES_OF_VERT) {
    if (BM_edge_is_boundary(e)) {
      copy_v3_v3(avg, v->co);
      return;
    }
    BMVert *v_other = (e->v1 == v) ? e->v2 : e->v1;
    float vec[3];
    sub_v3_v3v3(vec, v_other->co, v->co);
    madd_v3_v3fl(vec, v->no, -dot_v3v3(vec, v->no));
    normalize_v3(vec);

    /* fac is a measure of how orthogonal or parallel the edge is
     * relative to the direction. */
    float fac = dot_v3v3(vec, direction);
    fac = fac * fac - 0.5f;
    fac *= fac;
    madd_v3_v3fl(avg_co, v_other->co, fac);
    tot_co += fac;
  }

  /* In case vert has no Edge s. */
  if (tot_co > 0.0f) {
    mul_v3_v3fl(avg, avg_co, 1.0f / tot_co);

    /* Preserve volume. */
    float vec[3];
    sub_v3_v3(avg, v->co);
    mul_v3_v3fl(vec, v->no, dot_v3v3(avg, v->no));
    sub_v3_v3(avg, vec);
    add_v3_v3(avg, v->co);
  }
  else {
    zero_v3(avg);
  }
}

/* Generic functions for laplacian smoothing. These functions do not take boundary vertices into
 * account. */

float3 neighbor_coords_average(SculptSession &ss, PBVHVertRef vertex)
{
  float3 avg(0);
  int total = 0;

  SculptVertexNeighborIter ni;
  SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
    avg += SCULPT_vertex_co_get(ss, ni.vertex);
    total++;
  }
  SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

  if (total > 0) {
    return avg / total;
  }
  return SCULPT_vertex_co_get(ss, vertex);
}

float neighbor_mask_average(SculptSession &ss,
                            const SculptMaskWriteInfo write_info,
                            PBVHVertRef vertex)
{
  float avg = 0.0f;
  int total = 0;
  SculptVertexNeighborIter ni;
  switch (BKE_pbvh_type(*ss.pbvh)) {
    case PBVH_FACES: {
      SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
        avg += write_info.layer[ni.vertex.i];
        total++;
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
      return avg / total;
    }
    case PBVH_GRIDS: {
      SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
        avg += SCULPT_mask_get_at_grids_vert_index(
            *ss.subdiv_ccg, *BKE_pbvh_get_grid_key(*ss.pbvh), ni.vertex.i);
        total++;
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
      return avg / total;
    }
    case PBVH_BMESH: {
      Vector<BMVert *, 64> neighbors;
      for (BMVert *neighbor :
           vert_neighbors_get_bmesh(*reinterpret_cast<BMVert *>(vertex.i), neighbors))
      {
        avg += BM_ELEM_CD_GET_FLOAT(neighbor, write_info.bm_offset);
      }
      return avg / neighbors.size();
    }
  }
  BLI_assert_unreachable();
  return 0.0f;
}

float4 neighbor_color_average(SculptSession &ss, const int vert)
{
  float4 avg(0);
  int total = 0;

  SculptVertexNeighborIter ni;
  SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, PBVHVertRef{vert}, ni) {
    float4 tmp = SCULPT_vertex_color_get(ss, ni.index);

    avg += tmp;
    total++;
  }
  SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

  if (total > 0) {
    return avg / total;
  }
  return SCULPT_vertex_color_get(ss, vert);
}

static void do_enhance_details_brush_task(Object &ob,
                                          const Sculpt &sd,
                                          const Brush &brush,
                                          PBVHNode *node)
{
  SculptSession &ss = *ob.sculpt;

  PBVHVertexIter vd;

  float bstrength = ss.cache->bstrength;
  CLAMP(bstrength, -1.0f, 1.0f);

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, test, brush.falloff_shape);

  const int thread_id = BLI_task_parallel_thread_id(nullptr);
  auto_mask::NodeData automask_data = auto_mask::node_begin(
      ob, ss.cache->automasking.get(), *node);

  BKE_pbvh_vertex_iter_begin (*ss.pbvh, node, vd, PBVH_ITER_UNIQUE) {
    if (!sculpt_brush_test_sq_fn(test, vd.co)) {
      continue;
    }

    auto_mask::node_update(automask_data, vd);

    const float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                                brush,
                                                                vd.co,
                                                                sqrtf(test.dist),
                                                                vd.no,
                                                                vd.fno,
                                                                vd.mask,
                                                                vd.vertex,
                                                                thread_id,
                                                                &automask_data);

    float disp[3];
    madd_v3_v3v3fl(disp, vd.co, ss.cache->detail_directions[vd.index], fade);
    SCULPT_clip(sd, ss, vd.co, disp);
  }
  BKE_pbvh_vertex_iter_end;
}

void enhance_details_brush(const Sculpt &sd, Object &ob, Span<PBVHNode *> nodes)
{
  SculptSession &ss = *ob.sculpt;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  SCULPT_vertex_random_access_ensure(ss);
  SCULPT_boundary_info_ensure(ob);

  if (SCULPT_stroke_is_first_brush_step(*ss.cache)) {
    const int totvert = SCULPT_vertex_count_get(ss);
    ss.cache->detail_directions = static_cast<float(*)[3]>(
        MEM_malloc_arrayN(totvert, sizeof(float[3]), "details directions"));

    for (int i = 0; i < totvert; i++) {
      PBVHVertRef vertex = BKE_pbvh_index_to_vertex(*ss.pbvh, i);
      const float3 avg = neighbor_coords_average(ss, vertex);
      sub_v3_v3v3(ss.cache->detail_directions[i], avg, SCULPT_vertex_co_get(ss, vertex));
    }
  }

  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (const int i : range) {
      do_enhance_details_brush_task(ob, sd, brush, nodes[i]);
    }
  });
}

/* HC Smooth Algorithm. */
/* From: Improved Laplacian Smoothing of Noisy Surface Meshes */

void surface_smooth_laplacian_step(SculptSession &ss,
                                   float *disp,
                                   const float co[3],
                                   float (*laplacian_disp)[3],
                                   const PBVHVertRef vertex,
                                   const float origco[3],
                                   const float alpha)
{
  float weigthed_o[3], weigthed_q[3], d[3];
  int v_index = BKE_pbvh_vertex_to_index(*ss.pbvh, vertex);

  const float3 laplacian_smooth_co = neighbor_coords_average(ss, vertex);

  mul_v3_v3fl(weigthed_o, origco, alpha);
  mul_v3_v3fl(weigthed_q, co, 1.0f - alpha);
  add_v3_v3v3(d, weigthed_o, weigthed_q);
  sub_v3_v3v3(laplacian_disp[v_index], laplacian_smooth_co, d);

  sub_v3_v3v3(disp, laplacian_smooth_co, co);
}

void surface_smooth_displace_step(SculptSession &ss,
                                  float *co,
                                  float (*laplacian_disp)[3],
                                  const PBVHVertRef vertex,
                                  const float beta,
                                  const float fade)
{
  float b_avg[3] = {0.0f, 0.0f, 0.0f};
  float b_current_vertex[3];
  int total = 0;
  SculptVertexNeighborIter ni;
  SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
    add_v3_v3(b_avg, laplacian_disp[ni.index]);
    total++;
  }

  SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
  if (total > 0) {
    int v_index = BKE_pbvh_vertex_to_index(*ss.pbvh, vertex);

    mul_v3_v3fl(b_current_vertex, b_avg, (1.0f - beta) / total);
    madd_v3_v3fl(b_current_vertex, laplacian_disp[v_index], beta);
    mul_v3_fl(b_current_vertex, clamp_f(fade, 0.0f, 1.0f));
    sub_v3_v3(co, b_current_vertex);
  }
}

static void do_surface_smooth_brush_laplacian_task(Object &ob, const Brush &brush, PBVHNode *node)
{
  SculptSession &ss = *ob.sculpt;
  const float bstrength = ss.cache->bstrength;
  float alpha = brush.surface_smooth_shape_preservation;

  PBVHVertexIter vd;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, test, brush.falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(nullptr);

  SculptOrigVertData orig_data = SCULPT_orig_vert_data_init(ob, *node, undo::Type::Position);
  auto_mask::NodeData automask_data = auto_mask::node_begin(
      ob, ss.cache->automasking.get(), *node);

  BKE_pbvh_vertex_iter_begin (*ss.pbvh, node, vd, PBVH_ITER_UNIQUE) {
    SCULPT_orig_vert_data_update(orig_data, vd);
    if (!sculpt_brush_test_sq_fn(test, vd.co)) {
      continue;
    }

    auto_mask::node_update(automask_data, vd);

    const float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                                brush,
                                                                vd.co,
                                                                sqrtf(test.dist),
                                                                vd.no,
                                                                vd.fno,
                                                                vd.mask,
                                                                vd.vertex,
                                                                thread_id,
                                                                &automask_data);

    float disp[3];
    surface_smooth_laplacian_step(
        ss, disp, vd.co, ss.cache->surface_smooth_laplacian_disp, vd.vertex, orig_data.co, alpha);
    madd_v3_v3fl(vd.co, disp, clamp_f(fade, 0.0f, 1.0f));
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_surface_smooth_brush_displace_task(Object &ob, const Brush &brush, PBVHNode *node)
{
  SculptSession &ss = *ob.sculpt;
  const float bstrength = ss.cache->bstrength;
  const float beta = brush.surface_smooth_current_vertex;

  PBVHVertexIter vd;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, test, brush.falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(nullptr);
  auto_mask::NodeData automask_data = auto_mask::node_begin(
      ob, ss.cache->automasking.get(), *node);

  BKE_pbvh_vertex_iter_begin (*ss.pbvh, node, vd, PBVH_ITER_UNIQUE) {
    if (!sculpt_brush_test_sq_fn(test, vd.co)) {
      continue;
    }

    auto_mask::node_update(automask_data, vd);

    const float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                                brush,
                                                                vd.co,
                                                                sqrtf(test.dist),
                                                                vd.no,
                                                                vd.fno,
                                                                vd.mask,
                                                                vd.vertex,
                                                                thread_id,
                                                                &automask_data);
    surface_smooth_displace_step(
        ss, vd.co, ss.cache->surface_smooth_laplacian_disp, vd.vertex, beta, fade);
  }
  BKE_pbvh_vertex_iter_end;
}

void do_surface_smooth_brush(const Sculpt &sd, Object &ob, Span<PBVHNode *> nodes)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  for (int i = 0; i < brush.surface_smooth_iterations; i++) {
    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (const int i : range) {
        do_surface_smooth_brush_laplacian_task(ob, brush, nodes[i]);
      }
    });
    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (const int i : range) {
        do_surface_smooth_brush_displace_task(ob, brush, nodes[i]);
      }
    });
  }
}

}  // namespace blender::ed::sculpt_paint::smooth
