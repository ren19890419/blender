/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2018 Blender Foundation */

/** \file
 * \ingroup bke
 */

#include <mutex>

#include "atomic_ops.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_task.hh"

#include "BKE_customdata.h"
#include "BKE_key.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.h"
#include "BKE_subdiv.h"
#include "BKE_subdiv_eval.h"
#include "BKE_subdiv_foreach.hh"
#include "BKE_subdiv_mesh.hh"

#include "MEM_guardedalloc.h"

using blender::float2;
using blender::float3;
using blender::IndexRange;
using blender::MutableSpan;
using blender::Span;

/* -------------------------------------------------------------------- */
/** \name Subdivision Context
 * \{ */

struct SubdivMeshContext {
  const SubdivToMeshSettings *settings;
  const Mesh *coarse_mesh;
  const float (*coarse_positions)[3];
  blender::Span<MEdge> coarse_edges;
  blender::OffsetIndices<int> coarse_polys;
  blender::Span<int> coarse_corner_verts;

  Subdiv *subdiv;
  Mesh *subdiv_mesh;
  blender::MutableSpan<float3> subdiv_positions;
  blender::MutableSpan<MEdge> subdiv_edges;
  blender::MutableSpan<int> subdiv_poly_offsets;
  blender::MutableSpan<int> subdiv_corner_verts;
  blender::MutableSpan<int> subdiv_corner_edges;

  /* Cached custom data arrays for faster access. */
  int *vert_origindex;
  int *edge_origindex;
  int *loop_origindex;
  int *poly_origindex;
  /* UV layers interpolation. */
  int num_uv_layers;
  float2 *uv_layers[MAX_MTFACE];

  /* Original coordinates (ORCO) interpolation. */
  float (*orco)[3];
  float (*cloth_orco)[3];
  /* Per-subdivided vertex counter of averaged values. */
  int *accumulated_counters;
  bool have_displacement;

  /* Write optimal display edge tags into a boolean array rather than the final bit vector
   * to avoid race conditions when setting bits. */
  blender::Array<bool> subdiv_display_edges;

  /* Lazily initialize a map from vertices to connected edges. */
  std::mutex vert_to_edge_map_mutex;
  int *vert_to_edge_buffer;
  MeshElemMap *vert_to_edge_map;
};

static void subdiv_mesh_ctx_cache_uv_layers(SubdivMeshContext *ctx)
{
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  ctx->num_uv_layers = std::min(CustomData_number_of_layers(&subdiv_mesh->ldata, CD_PROP_FLOAT2),
                                MAX_MTFACE);
  for (int layer_index = 0; layer_index < ctx->num_uv_layers; layer_index++) {
    ctx->uv_layers[layer_index] = static_cast<float2 *>(CustomData_get_layer_n_for_write(
        &subdiv_mesh->ldata, CD_PROP_FLOAT2, layer_index, subdiv_mesh->totloop));
  }
}

static void subdiv_mesh_ctx_cache_custom_data_layers(SubdivMeshContext *ctx)
{
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  ctx->subdiv_positions = subdiv_mesh->vert_positions_for_write();
  ctx->subdiv_edges = subdiv_mesh->edges_for_write();
  ctx->subdiv_poly_offsets = subdiv_mesh->poly_offsets_for_write();
  ctx->subdiv_corner_verts = subdiv_mesh->corner_verts_for_write();
  ctx->subdiv_corner_edges = subdiv_mesh->corner_edges_for_write();
  /* Pointers to original indices layers. */
  ctx->vert_origindex = static_cast<int *>(
      CustomData_get_layer_for_write(&subdiv_mesh->vdata, CD_ORIGINDEX, subdiv_mesh->totvert));
  ctx->edge_origindex = static_cast<int *>(
      CustomData_get_layer_for_write(&subdiv_mesh->edata, CD_ORIGINDEX, subdiv_mesh->totedge));
  ctx->loop_origindex = static_cast<int *>(
      CustomData_get_layer_for_write(&subdiv_mesh->ldata, CD_ORIGINDEX, subdiv_mesh->totloop));
  ctx->poly_origindex = static_cast<int *>(
      CustomData_get_layer_for_write(&subdiv_mesh->pdata, CD_ORIGINDEX, subdiv_mesh->totpoly));
  /* UV layers interpolation. */
  subdiv_mesh_ctx_cache_uv_layers(ctx);
  /* Orco interpolation. */
  ctx->orco = static_cast<float(*)[3]>(
      CustomData_get_layer_for_write(&subdiv_mesh->vdata, CD_ORCO, subdiv_mesh->totvert));
  ctx->cloth_orco = static_cast<float(*)[3]>(
      CustomData_get_layer_for_write(&subdiv_mesh->vdata, CD_CLOTH_ORCO, subdiv_mesh->totvert));
}

static void subdiv_mesh_prepare_accumulator(SubdivMeshContext *ctx, int num_vertices)
{
  if (!ctx->have_displacement) {
    return;
  }
  ctx->accumulated_counters = static_cast<int *>(
      MEM_calloc_arrayN(num_vertices, sizeof(*ctx->accumulated_counters), __func__));
}

static void subdiv_mesh_context_free(SubdivMeshContext *ctx)
{
  MEM_SAFE_FREE(ctx->accumulated_counters);
  MEM_SAFE_FREE(ctx->vert_to_edge_buffer);
  MEM_SAFE_FREE(ctx->vert_to_edge_map);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Loop custom data copy helpers
 * \{ */

struct LoopsOfPtex {
  /* First loop of the ptex, starts at ptex (0, 0) and goes in u direction. */
  int first_loop;
  /* Last loop of the ptex, starts at ptex (0, 0) and goes in v direction. */
  int last_loop;
  /* For quad coarse faces only. */
  int second_loop;
  int third_loop;
};

static void loops_of_ptex_get(LoopsOfPtex *loops_of_ptex,
                              const IndexRange coarse_poly,
                              const int ptex_of_poly_index)
{
  const int first_ptex_loop_index = coarse_poly.start() + ptex_of_poly_index;
  /* Loop which look in the (opposite) V direction of the current
   * ptex face.
   *
   * TODO(sergey): Get rid of using module on every iteration. */
  const int last_ptex_loop_index = coarse_poly.start() +
                                   (ptex_of_poly_index + coarse_poly.size() - 1) %
                                       coarse_poly.size();
  loops_of_ptex->first_loop = first_ptex_loop_index;
  loops_of_ptex->last_loop = last_ptex_loop_index;
  if (coarse_poly.size() == 4) {
    loops_of_ptex->second_loop = loops_of_ptex->first_loop + 1;
    loops_of_ptex->third_loop = loops_of_ptex->first_loop + 2;
  }
  else {
    loops_of_ptex->second_loop = -1;
    loops_of_ptex->third_loop = -1;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex custom data interpolation helpers
 * \{ */

/* TODO(sergey): Somehow de-duplicate with loops storage, without too much
 * exception cases all over the code. */

struct VerticesForInterpolation {
  /* This field points to a vertex data which is to be used for interpolation.
   * The idea is to avoid unnecessary allocations for regular faces, where
   * we can simply use corner vertices. */
  const CustomData *vertex_data;
  /* Vertices data calculated for ptex corners. There are always 4 elements
   * in this custom data, aligned the following way:
   *
   *   index 0 -> uv (0, 0)
   *   index 1 -> uv (0, 1)
   *   index 2 -> uv (1, 1)
   *   index 3 -> uv (1, 0)
   *
   * Is allocated for non-regular faces (triangles and n-gons). */
  CustomData vertex_data_storage;
  bool vertex_data_storage_allocated;
  /* Indices within vertex_data to interpolate for. The indices are aligned
   * with uv coordinates in a similar way as indices in loop_data_storage. */
  int vertex_indices[4];
};

static void vertex_interpolation_init(const SubdivMeshContext *ctx,
                                      VerticesForInterpolation *vertex_interpolation,
                                      const IndexRange coarse_poly)
{
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  if (coarse_poly.size() == 4) {
    vertex_interpolation->vertex_data = &coarse_mesh->vdata;
    vertex_interpolation->vertex_indices[0] = ctx->coarse_corner_verts[coarse_poly.start() + 0];
    vertex_interpolation->vertex_indices[1] = ctx->coarse_corner_verts[coarse_poly.start() + 1];
    vertex_interpolation->vertex_indices[2] = ctx->coarse_corner_verts[coarse_poly.start() + 2];
    vertex_interpolation->vertex_indices[3] = ctx->coarse_corner_verts[coarse_poly.start() + 3];
    vertex_interpolation->vertex_data_storage_allocated = false;
  }
  else {
    vertex_interpolation->vertex_data = &vertex_interpolation->vertex_data_storage;
    /* Allocate storage for loops corresponding to ptex corners. */
    CustomData_copy(&ctx->coarse_mesh->vdata,
                    &vertex_interpolation->vertex_data_storage,
                    CD_MASK_EVERYTHING.vmask,
                    CD_SET_DEFAULT,
                    4);
    /* Initialize indices. */
    vertex_interpolation->vertex_indices[0] = 0;
    vertex_interpolation->vertex_indices[1] = 1;
    vertex_interpolation->vertex_indices[2] = 2;
    vertex_interpolation->vertex_indices[3] = 3;
    vertex_interpolation->vertex_data_storage_allocated = true;
    /* Interpolate center of poly right away, it stays unchanged for all
     * ptex faces. */
    const float weight = 1.0f / float(coarse_poly.size());
    blender::Array<float, 32> weights(coarse_poly.size());
    blender::Array<int, 32> indices(coarse_poly.size());
    for (int i = 0; i < coarse_poly.size(); i++) {
      weights[i] = weight;
      indices[i] = ctx->coarse_corner_verts[coarse_poly.start() + i];
    }
    CustomData_interp(&coarse_mesh->vdata,
                      &vertex_interpolation->vertex_data_storage,
                      indices.data(),
                      weights.data(),
                      nullptr,
                      coarse_poly.size(),
                      2);
  }
}

static void vertex_interpolation_from_corner(const SubdivMeshContext *ctx,
                                             VerticesForInterpolation *vertex_interpolation,
                                             const IndexRange coarse_poly,
                                             const int corner)
{
  if (coarse_poly.size() == 4) {
    /* Nothing to do, all indices and data is already assigned. */
  }
  else {
    const CustomData *vertex_data = &ctx->coarse_mesh->vdata;
    LoopsOfPtex loops_of_ptex;
    loops_of_ptex_get(&loops_of_ptex, coarse_poly, corner);
    /* Ptex face corner corresponds to a poly loop with same index. */
    CustomData_copy_data(vertex_data,
                         &vertex_interpolation->vertex_data_storage,
                         ctx->coarse_corner_verts[coarse_poly.start() + corner],
                         0,
                         1);
    /* Interpolate remaining ptex face corners, which hits loops
     * middle points.
     *
     * TODO(sergey): Re-use one of interpolation results from previous
     * iteration. */
    const float weights[2] = {0.5f, 0.5f};
    const int first_loop_index = loops_of_ptex.first_loop;
    const int last_loop_index = loops_of_ptex.last_loop;
    const int first_indices[2] = {
        ctx->coarse_corner_verts[first_loop_index],
        ctx->coarse_corner_verts[coarse_poly.start() +
                                 (first_loop_index - coarse_poly.start() + 1) %
                                     coarse_poly.size()]};
    const int last_indices[2] = {ctx->coarse_corner_verts[first_loop_index],
                                 ctx->coarse_corner_verts[last_loop_index]};
    CustomData_interp(vertex_data,
                      &vertex_interpolation->vertex_data_storage,
                      first_indices,
                      weights,
                      nullptr,
                      2,
                      1);
    CustomData_interp(vertex_data,
                      &vertex_interpolation->vertex_data_storage,
                      last_indices,
                      weights,
                      nullptr,
                      2,
                      3);
  }
}

static void vertex_interpolation_end(VerticesForInterpolation *vertex_interpolation)
{
  if (vertex_interpolation->vertex_data_storage_allocated) {
    CustomData_free(&vertex_interpolation->vertex_data_storage, 4);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Loop custom data interpolation helpers
 * \{ */

struct LoopsForInterpolation {
  /* This field points to a loop data which is to be used for interpolation.
   * The idea is to avoid unnecessary allocations for regular faces, where
   * we can simply interpolate corner vertices. */
  const CustomData *loop_data;
  /* Loops data calculated for ptex corners. There are always 4 elements
   * in this custom data, aligned the following way:
   *
   *   index 0 -> uv (0, 0)
   *   index 1 -> uv (0, 1)
   *   index 2 -> uv (1, 1)
   *   index 3 -> uv (1, 0)
   *
   * Is allocated for non-regular faces (triangles and n-gons). */
  CustomData loop_data_storage;
  bool loop_data_storage_allocated;
  /* Indices within loop_data to interpolate for. The indices are aligned with
   * uv coordinates in a similar way as indices in loop_data_storage. */
  int loop_indices[4];
};

static void loop_interpolation_init(const SubdivMeshContext *ctx,
                                    LoopsForInterpolation *loop_interpolation,
                                    const IndexRange coarse_poly)
{
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  if (coarse_poly.size() == 4) {
    loop_interpolation->loop_data = &coarse_mesh->ldata;
    loop_interpolation->loop_indices[0] = coarse_poly.start() + 0;
    loop_interpolation->loop_indices[1] = coarse_poly.start() + 1;
    loop_interpolation->loop_indices[2] = coarse_poly.start() + 2;
    loop_interpolation->loop_indices[3] = coarse_poly.start() + 3;
    loop_interpolation->loop_data_storage_allocated = false;
  }
  else {
    loop_interpolation->loop_data = &loop_interpolation->loop_data_storage;
    /* Allocate storage for loops corresponding to ptex corners. */
    CustomData_copy(&ctx->coarse_mesh->ldata,
                    &loop_interpolation->loop_data_storage,
                    CD_MASK_EVERYTHING.lmask,
                    CD_SET_DEFAULT,
                    4);
    /* Initialize indices. */
    loop_interpolation->loop_indices[0] = 0;
    loop_interpolation->loop_indices[1] = 1;
    loop_interpolation->loop_indices[2] = 2;
    loop_interpolation->loop_indices[3] = 3;
    loop_interpolation->loop_data_storage_allocated = true;
    /* Interpolate center of poly right away, it stays unchanged for all
     * ptex faces. */
    const float weight = 1.0f / float(coarse_poly.size());
    blender::Array<float, 32> weights(coarse_poly.size());
    blender::Array<int, 32> indices(coarse_poly.size());
    for (int i = 0; i < coarse_poly.size(); i++) {
      weights[i] = weight;
      indices[i] = coarse_poly.start() + i;
    }
    CustomData_interp(&coarse_mesh->ldata,
                      &loop_interpolation->loop_data_storage,
                      indices.data(),
                      weights.data(),
                      nullptr,
                      coarse_poly.size(),
                      2);
  }
}

static void loop_interpolation_from_corner(const SubdivMeshContext *ctx,
                                           LoopsForInterpolation *loop_interpolation,
                                           const IndexRange coarse_poly,
                                           const int corner)
{
  if (coarse_poly.size() == 4) {
    /* Nothing to do, all indices and data is already assigned. */
  }
  else {
    const CustomData *loop_data = &ctx->coarse_mesh->ldata;
    LoopsOfPtex loops_of_ptex;
    loops_of_ptex_get(&loops_of_ptex, coarse_poly, corner);
    /* Ptex face corner corresponds to a poly loop with same index. */
    CustomData_free_elem(&loop_interpolation->loop_data_storage, 0, 1);
    CustomData_copy_data(
        loop_data, &loop_interpolation->loop_data_storage, coarse_poly.start() + corner, 0, 1);
    /* Interpolate remaining ptex face corners, which hits loops
     * middle points.
     *
     * TODO(sergey): Re-use one of interpolation results from previous
     * iteration. */
    const float weights[2] = {0.5f, 0.5f};
    const int base_loop_index = coarse_poly.start();
    const int first_loop_index = loops_of_ptex.first_loop;
    const int second_loop_index = base_loop_index +
                                  (first_loop_index - base_loop_index + 1) % coarse_poly.size();
    const int first_indices[2] = {first_loop_index, second_loop_index};
    const int last_indices[2] = {loops_of_ptex.last_loop, loops_of_ptex.first_loop};
    CustomData_interp(
        loop_data, &loop_interpolation->loop_data_storage, first_indices, weights, nullptr, 2, 1);
    CustomData_interp(
        loop_data, &loop_interpolation->loop_data_storage, last_indices, weights, nullptr, 2, 3);
  }
}

static void loop_interpolation_end(LoopsForInterpolation *loop_interpolation)
{
  if (loop_interpolation->loop_data_storage_allocated) {
    CustomData_free(&loop_interpolation->loop_data_storage, 4);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name TLS
 * \{ */

struct SubdivMeshTLS {
  bool vertex_interpolation_initialized;
  VerticesForInterpolation vertex_interpolation;
  int vertex_interpolation_coarse_poly_index;
  int vertex_interpolation_coarse_corner;

  bool loop_interpolation_initialized;
  LoopsForInterpolation loop_interpolation;
  int loop_interpolation_coarse_poly_index;
  int loop_interpolation_coarse_corner;
};

static void subdiv_mesh_tls_free(void *tls_v)
{
  SubdivMeshTLS *tls = static_cast<SubdivMeshTLS *>(tls_v);
  if (tls->vertex_interpolation_initialized) {
    vertex_interpolation_end(&tls->vertex_interpolation);
  }
  if (tls->loop_interpolation_initialized) {
    loop_interpolation_end(&tls->loop_interpolation);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Evaluation helper functions
 * \{ */

static void subdiv_vertex_orco_evaluate(const SubdivMeshContext *ctx,
                                        const int ptex_face_index,
                                        const float u,
                                        const float v,
                                        const int subdiv_vertex_index)
{
  if (ctx->orco || ctx->cloth_orco) {
    float vertex_data[6];
    BKE_subdiv_eval_vertex_data(ctx->subdiv, ptex_face_index, u, v, vertex_data);

    if (ctx->orco) {
      copy_v3_v3(ctx->orco[subdiv_vertex_index], vertex_data);
      if (ctx->cloth_orco) {
        copy_v3_v3(ctx->cloth_orco[subdiv_vertex_index], vertex_data + 3);
      }
    }
    else if (ctx->cloth_orco) {
      copy_v3_v3(ctx->cloth_orco[subdiv_vertex_index], vertex_data);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Accumulation helpers
 * \{ */

static void subdiv_accumulate_vertex_displacement(SubdivMeshContext *ctx,
                                                  const int ptex_face_index,
                                                  const float u,
                                                  const float v,
                                                  const int subdiv_vertex_index)
{
  /* Accumulate displacement. */
  Subdiv *subdiv = ctx->subdiv;
  float dummy_P[3], dPdu[3], dPdv[3], D[3];
  BKE_subdiv_eval_limit_point_and_derivatives(subdiv, ptex_face_index, u, v, dummy_P, dPdu, dPdv);

  /* NOTE: The subdivided mesh is allocated in this module, and its vertices are kept at zero
   * locations as a default calloc(). */
  BKE_subdiv_eval_displacement(subdiv, ptex_face_index, u, v, dPdu, dPdv, D);
  ctx->subdiv_positions[subdiv_vertex_index] += D;

  if (ctx->accumulated_counters) {
    ++ctx->accumulated_counters[subdiv_vertex_index];
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Callbacks
 * \{ */

static bool subdiv_mesh_topology_info(const SubdivForeachContext *foreach_context,
                                      const int num_vertices,
                                      const int num_edges,
                                      const int num_loops,
                                      const int num_polygons,
                                      const int * /*subdiv_polygon_offset*/)
{
  /* Multi-resolution grid data will be applied or become invalid after subdivision,
   * so don't try to preserve it and use memory. Crease values should also not be interpolated. */
  CustomData_MeshMasks mask = CD_MASK_EVERYTHING;
  mask.lmask &= ~CD_MASK_MULTIRES_GRIDS;
  /* Propagate edge creases so they can be used in another subdivision modifier (maintaining
   * existing behavior), but don't propagate vertex creases to avoid extra work when the result
   * isn't useful anyway. */
  mask.vmask &= ~CD_MASK_CREASE;

  SubdivMeshContext *subdiv_context = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  subdiv_context->subdiv_mesh = BKE_mesh_new_nomain_from_template_ex(
      subdiv_context->coarse_mesh, num_vertices, num_edges, 0, num_loops, num_polygons, mask);
  subdiv_mesh_ctx_cache_custom_data_layers(subdiv_context);
  subdiv_mesh_prepare_accumulator(subdiv_context, num_vertices);
  subdiv_context->subdiv_mesh->runtime->subsurf_face_dot_tags.clear();
  subdiv_context->subdiv_mesh->runtime->subsurf_face_dot_tags.resize(num_vertices);
  if (subdiv_context->settings->use_optimal_display) {
    subdiv_context->subdiv_display_edges = blender::Array<bool>(num_edges, false);
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Vertex subdivision process
 * \{ */

static void subdiv_vertex_data_copy(const SubdivMeshContext *ctx,
                                    const int coarse_vertex_index,
                                    const int subdiv_vertex_index)
{
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  CustomData_copy_data(
      &coarse_mesh->vdata, &ctx->subdiv_mesh->vdata, coarse_vertex_index, subdiv_vertex_index, 1);
}

static void subdiv_vertex_data_interpolate(const SubdivMeshContext *ctx,
                                           const int subdiv_vertex_index,
                                           const VerticesForInterpolation *vertex_interpolation,
                                           const float u,
                                           const float v)
{
  const float weights[4] = {(1.0f - u) * (1.0f - v), u * (1.0f - v), u * v, (1.0f - u) * v};
  CustomData_interp(vertex_interpolation->vertex_data,
                    &ctx->subdiv_mesh->vdata,
                    vertex_interpolation->vertex_indices,
                    weights,
                    nullptr,
                    4,
                    subdiv_vertex_index);
  if (ctx->vert_origindex != nullptr) {
    ctx->vert_origindex[subdiv_vertex_index] = ORIGINDEX_NONE;
  }
}

static void evaluate_vertex_and_apply_displacement_copy(const SubdivMeshContext *ctx,
                                                        const int ptex_face_index,
                                                        const float u,
                                                        const float v,
                                                        const int coarse_vertex_index,
                                                        const int subdiv_vertex_index)
{
  float3 &subdiv_position = ctx->subdiv_positions[subdiv_vertex_index];
  /* Displacement is accumulated in subdiv vertex position.
   * Needs to be backed up before copying data from original vertex. */
  float D[3] = {0.0f, 0.0f, 0.0f};
  if (ctx->have_displacement) {
    const float inv_num_accumulated = 1.0f / ctx->accumulated_counters[subdiv_vertex_index];
    copy_v3_v3(D, subdiv_position);
    mul_v3_fl(D, inv_num_accumulated);
  }
  /* Copy custom data and evaluate position. */
  subdiv_vertex_data_copy(ctx, coarse_vertex_index, subdiv_vertex_index);
  BKE_subdiv_eval_limit_point(ctx->subdiv, ptex_face_index, u, v, subdiv_position);
  /* Apply displacement. */
  subdiv_position += D;
  /* Evaluate undeformed texture coordinate. */
  subdiv_vertex_orco_evaluate(ctx, ptex_face_index, u, v, subdiv_vertex_index);
  /* Remove face-dot flag. This can happen if there is more than one subsurf modifier. */
  ctx->subdiv_mesh->runtime->subsurf_face_dot_tags[subdiv_vertex_index].reset();
}

static void evaluate_vertex_and_apply_displacement_interpolate(
    const SubdivMeshContext *ctx,
    const int ptex_face_index,
    const float u,
    const float v,
    VerticesForInterpolation *vertex_interpolation,
    const int subdiv_vertex_index)
{
  float3 &subdiv_position = ctx->subdiv_positions[subdiv_vertex_index];
  /* Displacement is accumulated in subdiv vertex position.
   * Needs to be backed up before copying data from original vertex. */
  float D[3] = {0.0f, 0.0f, 0.0f};
  if (ctx->have_displacement) {
    const float inv_num_accumulated = 1.0f / ctx->accumulated_counters[subdiv_vertex_index];
    copy_v3_v3(D, subdiv_position);
    mul_v3_fl(D, inv_num_accumulated);
  }
  /* Interpolate custom data and evaluate position. */
  subdiv_vertex_data_interpolate(ctx, subdiv_vertex_index, vertex_interpolation, u, v);
  BKE_subdiv_eval_limit_point(ctx->subdiv, ptex_face_index, u, v, subdiv_position);
  /* Apply displacement. */
  add_v3_v3(subdiv_position, D);
  /* Evaluate undeformed texture coordinate. */
  subdiv_vertex_orco_evaluate(ctx, ptex_face_index, u, v, subdiv_vertex_index);
}

static void subdiv_mesh_vertex_displacement_every_corner_or_edge(
    const SubdivForeachContext *foreach_context,
    void * /*tls*/,
    const int ptex_face_index,
    const float u,
    const float v,
    const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  subdiv_accumulate_vertex_displacement(ctx, ptex_face_index, u, v, subdiv_vertex_index);
}

static void subdiv_mesh_vertex_displacement_every_corner(
    const SubdivForeachContext *foreach_context,
    void *tls,
    const int ptex_face_index,
    const float u,
    const float v,
    const int /*coarse_vertex_index*/,
    const int /*coarse_poly_index*/,
    const int /*coarse_corner*/,
    const int subdiv_vertex_index)
{
  subdiv_mesh_vertex_displacement_every_corner_or_edge(
      foreach_context, tls, ptex_face_index, u, v, subdiv_vertex_index);
}

static void subdiv_mesh_vertex_displacement_every_edge(const SubdivForeachContext *foreach_context,
                                                       void *tls,
                                                       const int ptex_face_index,
                                                       const float u,
                                                       const float v,
                                                       const int /*coarse_edge_index*/,
                                                       const int /*coarse_poly_index*/,
                                                       const int /*coarse_corner*/,
                                                       const int subdiv_vertex_index)
{
  subdiv_mesh_vertex_displacement_every_corner_or_edge(
      foreach_context, tls, ptex_face_index, u, v, subdiv_vertex_index);
}

static void subdiv_mesh_vertex_corner(const SubdivForeachContext *foreach_context,
                                      void * /*tls*/,
                                      const int ptex_face_index,
                                      const float u,
                                      const float v,
                                      const int coarse_vertex_index,
                                      const int /*coarse_poly_index*/,
                                      const int /*coarse_corner*/,
                                      const int subdiv_vertex_index)
{
  BLI_assert(coarse_vertex_index != ORIGINDEX_NONE);
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  evaluate_vertex_and_apply_displacement_copy(
      ctx, ptex_face_index, u, v, coarse_vertex_index, subdiv_vertex_index);
}

static void subdiv_mesh_ensure_vertex_interpolation(SubdivMeshContext *ctx,
                                                    SubdivMeshTLS *tls,
                                                    const int coarse_poly_index,
                                                    const int coarse_corner)
{
  const IndexRange coarse_poly = ctx->coarse_polys[coarse_poly_index];
  /* Check whether we've moved to another corner or polygon. */
  if (tls->vertex_interpolation_initialized) {
    if (tls->vertex_interpolation_coarse_poly_index != coarse_poly_index ||
        tls->vertex_interpolation_coarse_corner != coarse_corner) {
      vertex_interpolation_end(&tls->vertex_interpolation);
      tls->vertex_interpolation_initialized = false;
    }
  }
  /* Initialize the interpolation. */
  if (!tls->vertex_interpolation_initialized) {
    vertex_interpolation_init(ctx, &tls->vertex_interpolation, coarse_poly);
  }
  /* Update it for a new corner if needed. */
  if (!tls->vertex_interpolation_initialized ||
      tls->vertex_interpolation_coarse_corner != coarse_corner) {
    vertex_interpolation_from_corner(ctx, &tls->vertex_interpolation, coarse_poly, coarse_corner);
  }
  /* Store settings used for the current state of interpolator. */
  tls->vertex_interpolation_initialized = true;
  tls->vertex_interpolation_coarse_poly_index = coarse_poly_index;
  tls->vertex_interpolation_coarse_corner = coarse_corner;
}

static void subdiv_mesh_vertex_edge(const SubdivForeachContext *foreach_context,
                                    void *tls_v,
                                    const int ptex_face_index,
                                    const float u,
                                    const float v,
                                    const int /*coarse_edge_index*/,
                                    const int coarse_poly_index,
                                    const int coarse_corner,
                                    const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  SubdivMeshTLS *tls = static_cast<SubdivMeshTLS *>(tls_v);
  subdiv_mesh_ensure_vertex_interpolation(ctx, tls, coarse_poly_index, coarse_corner);
  evaluate_vertex_and_apply_displacement_interpolate(
      ctx, ptex_face_index, u, v, &tls->vertex_interpolation, subdiv_vertex_index);
}

static bool subdiv_mesh_is_center_vertex(const IndexRange coarse_poly,
                                         const float u,
                                         const float v)
{
  if (coarse_poly.size() == 4) {
    if (u == 0.5f && v == 0.5f) {
      return true;
    }
  }
  else {
    if (u == 1.0f && v == 1.0f) {
      return true;
    }
  }
  return false;
}

static void subdiv_mesh_tag_center_vertex(const IndexRange coarse_poly,
                                          const int subdiv_vertex_index,
                                          const float u,
                                          const float v,
                                          Mesh *subdiv_mesh)
{
  if (subdiv_mesh_is_center_vertex(coarse_poly, u, v)) {
    subdiv_mesh->runtime->subsurf_face_dot_tags[subdiv_vertex_index].set();
  }
}

static void subdiv_mesh_vertex_inner(const SubdivForeachContext *foreach_context,
                                     void *tls_v,
                                     const int ptex_face_index,
                                     const float u,
                                     const float v,
                                     const int coarse_poly_index,
                                     const int coarse_corner,
                                     const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  SubdivMeshTLS *tls = static_cast<SubdivMeshTLS *>(tls_v);
  Subdiv *subdiv = ctx->subdiv;
  const IndexRange coarse_poly = ctx->coarse_polys[coarse_poly_index];
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  float3 &subdiv_position = ctx->subdiv_positions[subdiv_vertex_index];
  subdiv_mesh_ensure_vertex_interpolation(ctx, tls, coarse_poly_index, coarse_corner);
  subdiv_vertex_data_interpolate(ctx, subdiv_vertex_index, &tls->vertex_interpolation, u, v);
  BKE_subdiv_eval_final_point(subdiv, ptex_face_index, u, v, subdiv_position);
  subdiv_mesh_tag_center_vertex(coarse_poly, subdiv_vertex_index, u, v, subdiv_mesh);
  subdiv_vertex_orco_evaluate(ctx, ptex_face_index, u, v, subdiv_vertex_index);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge subdivision process
 * \{ */

static void subdiv_copy_edge_data(SubdivMeshContext *ctx,
                                  const int subdiv_edge_index,
                                  const int coarse_edge_index)
{
  if (coarse_edge_index == ORIGINDEX_NONE) {
    if (ctx->edge_origindex != nullptr) {
      ctx->edge_origindex[subdiv_edge_index] = ORIGINDEX_NONE;
    }
    return;
  }
  CustomData_copy_data(
      &ctx->coarse_mesh->edata, &ctx->subdiv_mesh->edata, coarse_edge_index, subdiv_edge_index, 1);
  if (ctx->settings->use_optimal_display) {
    ctx->subdiv_display_edges[subdiv_edge_index] = true;
  }
}

static void subdiv_mesh_edge(const SubdivForeachContext *foreach_context,
                             void * /*tls*/,
                             const int coarse_edge_index,
                             const int subdiv_edge_index,
                             const bool /*is_loose*/,
                             const int subdiv_v1,
                             const int subdiv_v2)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  subdiv_copy_edge_data(ctx, subdiv_edge_index, coarse_edge_index);
  ctx->subdiv_edges[subdiv_edge_index].v1 = subdiv_v1;
  ctx->subdiv_edges[subdiv_edge_index].v2 = subdiv_v2;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Loops creation/interpolation
 * \{ */

static void subdiv_interpolate_loop_data(const SubdivMeshContext *ctx,
                                         const int subdiv_loop_index,
                                         const LoopsForInterpolation *loop_interpolation,
                                         const float u,
                                         const float v)
{
  const float weights[4] = {(1.0f - u) * (1.0f - v), u * (1.0f - v), u * v, (1.0f - u) * v};
  CustomData_interp(loop_interpolation->loop_data,
                    &ctx->subdiv_mesh->ldata,
                    loop_interpolation->loop_indices,
                    weights,
                    nullptr,
                    4,
                    subdiv_loop_index);
  /* TODO(sergey): Set ORIGINDEX. */
}

static void subdiv_eval_uv_layer(SubdivMeshContext *ctx,
                                 const int corner_index,
                                 const int ptex_face_index,
                                 const float u,
                                 const float v)
{
  if (ctx->num_uv_layers == 0) {
    return;
  }
  Subdiv *subdiv = ctx->subdiv;
  for (int layer_index = 0; layer_index < ctx->num_uv_layers; layer_index++) {
    BKE_subdiv_eval_face_varying(
        subdiv, layer_index, ptex_face_index, u, v, ctx->uv_layers[layer_index][corner_index]);
  }
}

static void subdiv_mesh_ensure_loop_interpolation(SubdivMeshContext *ctx,
                                                  SubdivMeshTLS *tls,
                                                  const int coarse_poly_index,
                                                  const int coarse_corner)
{
  const IndexRange coarse_poly = ctx->coarse_polys[coarse_poly_index];
  /* Check whether we've moved to another corner or polygon. */
  if (tls->loop_interpolation_initialized) {
    if (tls->loop_interpolation_coarse_poly_index != coarse_poly_index ||
        tls->loop_interpolation_coarse_corner != coarse_corner) {
      loop_interpolation_end(&tls->loop_interpolation);
      tls->loop_interpolation_initialized = false;
    }
  }
  /* Initialize the interpolation. */
  if (!tls->loop_interpolation_initialized) {
    loop_interpolation_init(ctx, &tls->loop_interpolation, coarse_poly);
  }
  /* Update it for a new corner if needed. */
  if (!tls->loop_interpolation_initialized ||
      tls->loop_interpolation_coarse_corner != coarse_corner) {
    loop_interpolation_from_corner(ctx, &tls->loop_interpolation, coarse_poly, coarse_corner);
  }
  /* Store settings used for the current state of interpolator. */
  tls->loop_interpolation_initialized = true;
  tls->loop_interpolation_coarse_poly_index = coarse_poly_index;
  tls->loop_interpolation_coarse_corner = coarse_corner;
}

static void subdiv_mesh_loop(const SubdivForeachContext *foreach_context,
                             void *tls_v,
                             const int ptex_face_index,
                             const float u,
                             const float v,
                             const int /*coarse_loop_index*/,
                             const int coarse_poly_index,
                             const int coarse_corner,
                             const int subdiv_loop_index,
                             const int subdiv_vertex_index,
                             const int subdiv_edge_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  SubdivMeshTLS *tls = static_cast<SubdivMeshTLS *>(tls_v);
  subdiv_mesh_ensure_loop_interpolation(ctx, tls, coarse_poly_index, coarse_corner);
  subdiv_interpolate_loop_data(ctx, subdiv_loop_index, &tls->loop_interpolation, u, v);
  subdiv_eval_uv_layer(ctx, subdiv_loop_index, ptex_face_index, u, v);
  ctx->subdiv_corner_verts[subdiv_loop_index] = subdiv_vertex_index;
  ctx->subdiv_corner_edges[subdiv_loop_index] = subdiv_edge_index;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Polygons subdivision process
 * \{ */

static void subdiv_mesh_poly(const SubdivForeachContext *foreach_context,
                             void * /*tls*/,
                             const int coarse_poly_index,
                             const int subdiv_poly_index,
                             const int start_loop_index,
                             const int /*num_loops*/)
{
  BLI_assert(coarse_poly_index != ORIGINDEX_NONE);
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  CustomData_copy_data(
      &ctx->coarse_mesh->pdata, &ctx->subdiv_mesh->pdata, coarse_poly_index, subdiv_poly_index, 1);
  ctx->subdiv_poly_offsets[subdiv_poly_index] = start_loop_index;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Loose elements subdivision process
 * \{ */

static void subdiv_mesh_vertex_loose(const SubdivForeachContext *foreach_context,
                                     void * /*tls*/,
                                     const int coarse_vertex_index,
                                     const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  subdiv_vertex_data_copy(ctx, coarse_vertex_index, subdiv_vertex_index);
}

/* Get neighbor edges of the given one.
 * - neighbors[0] is an edge adjacent to edge->v1.
 * - neighbors[1] is an edge adjacent to edge->v2. */
static void find_edge_neighbors(const MEdge *coarse_edges,
                                const MeshElemMap *vert_to_edge_map,
                                const int edge_index,
                                const MEdge *neighbors[2])
{
  const MEdge *edge = &coarse_edges[edge_index];
  neighbors[0] = nullptr;
  neighbors[1] = nullptr;
  int neighbor_counters[2] = {0, 0};
  for (const int i : Span(vert_to_edge_map[edge->v1].indices, vert_to_edge_map[edge->v1].count)) {
    if (i == edge_index) {
      continue;
    }
    if (ELEM(edge->v1, coarse_edges[i].v1, coarse_edges[i].v2)) {
      neighbors[0] = &coarse_edges[i];
      ++neighbor_counters[0];
    }
  }
  for (const int i : Span(vert_to_edge_map[edge->v2].indices, vert_to_edge_map[edge->v2].count)) {
    if (i == edge_index) {
      continue;
    }
    if (ELEM(edge->v2, coarse_edges[i].v1, coarse_edges[i].v2)) {
      neighbors[1] = &coarse_edges[i];
      ++neighbor_counters[1];
    }
  }
  /* Vertices which has more than one neighbor are considered infinitely
   * sharp. This is also how topology factory treats vertices of a surface
   * which are adjacent to a loose edge. */
  if (neighbor_counters[0] > 1) {
    neighbors[0] = nullptr;
  }
  if (neighbor_counters[1] > 1) {
    neighbors[1] = nullptr;
  }
}

static void points_for_loose_edges_interpolation_get(const float (*coarse_positions)[3],
                                                     const MEdge *coarse_edge,
                                                     const MEdge *neighbors[2],
                                                     float points_r[4][3])
{
  /* Middle points corresponds to the edge. */
  copy_v3_v3(points_r[1], coarse_positions[coarse_edge->v1]);
  copy_v3_v3(points_r[2], coarse_positions[coarse_edge->v2]);
  /* Start point, duplicate from edge start if no neighbor. */
  if (neighbors[0] != nullptr) {
    if (neighbors[0]->v1 == coarse_edge->v1) {
      copy_v3_v3(points_r[0], coarse_positions[neighbors[0]->v2]);
    }
    else {
      copy_v3_v3(points_r[0], coarse_positions[neighbors[0]->v1]);
    }
  }
  else {
    sub_v3_v3v3(points_r[0], points_r[1], points_r[2]);
    add_v3_v3(points_r[0], points_r[1]);
  }
  /* End point, duplicate from edge end if no neighbor. */
  if (neighbors[1] != nullptr) {
    if (neighbors[1]->v1 == coarse_edge->v2) {
      copy_v3_v3(points_r[3], coarse_positions[neighbors[1]->v2]);
    }
    else {
      copy_v3_v3(points_r[3], coarse_positions[neighbors[1]->v1]);
    }
  }
  else {
    sub_v3_v3v3(points_r[3], points_r[2], points_r[1]);
    add_v3_v3(points_r[3], points_r[2]);
  }
}

void BKE_subdiv_mesh_interpolate_position_on_edge(const float (*coarse_positions)[3],
                                                  const MEdge *coarse_edges,
                                                  const MeshElemMap *vert_to_edge_map,
                                                  const int coarse_edge_index,
                                                  const bool is_simple,
                                                  const float u,
                                                  float pos_r[3])
{
  const MEdge *coarse_edge = &coarse_edges[coarse_edge_index];
  if (is_simple) {
    interp_v3_v3v3(pos_r, coarse_positions[coarse_edge->v1], coarse_positions[coarse_edge->v2], u);
  }
  else {
    /* Find neighbors of the coarse edge. */
    const MEdge *neighbors[2];
    find_edge_neighbors(coarse_edges, vert_to_edge_map, coarse_edge_index, neighbors);
    float points[4][3];
    points_for_loose_edges_interpolation_get(coarse_positions, coarse_edge, neighbors, points);
    float weights[4];
    key_curve_position_weights(u, weights, KEY_BSPLINE);
    interp_v3_v3v3v3v3(pos_r, points[0], points[1], points[2], points[3], weights);
  }
}

static void subdiv_mesh_vertex_of_loose_edge_interpolate(SubdivMeshContext *ctx,
                                                         const MEdge *coarse_edge,
                                                         const float u,
                                                         const int subdiv_vertex_index)
{
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  Mesh *subdiv_mesh = ctx->subdiv_mesh;
  /* This is never used for end-points (which are copied from the original). */
  BLI_assert(u > 0.0f);
  BLI_assert(u < 1.0f);
  const float interpolation_weights[2] = {1.0f - u, u};
  const int coarse_vertex_indices[2] = {int(coarse_edge->v1), int(coarse_edge->v2)};
  CustomData_interp(&coarse_mesh->vdata,
                    &subdiv_mesh->vdata,
                    coarse_vertex_indices,
                    interpolation_weights,
                    nullptr,
                    2,
                    subdiv_vertex_index);
  if (ctx->vert_origindex != nullptr) {
    ctx->vert_origindex[subdiv_vertex_index] = ORIGINDEX_NONE;
  }
}

static void subdiv_mesh_vertex_of_loose_edge(const SubdivForeachContext *foreach_context,
                                             void * /*tls*/,
                                             const int coarse_edge_index,
                                             const float u,
                                             const int subdiv_vertex_index)
{
  SubdivMeshContext *ctx = static_cast<SubdivMeshContext *>(foreach_context->user_data);
  const Mesh *coarse_mesh = ctx->coarse_mesh;
  const MEdge *coarse_edge = &ctx->coarse_edges[coarse_edge_index];
  const bool is_simple = ctx->subdiv->settings.is_simple;

  /* Lazily initialize a vertex to edge map to avoid quadratic runtime when subdividing loose
   * edges. Do this here to avoid the cost in common cases when there are no loose edges at all. */
  if (ctx->vert_to_edge_map == nullptr) {
    std::lock_guard lock{ctx->vert_to_edge_map_mutex};
    if (ctx->vert_to_edge_map == nullptr) {
      BKE_mesh_vert_edge_map_create(&ctx->vert_to_edge_map,
                                    &ctx->vert_to_edge_buffer,
                                    ctx->coarse_edges.data(),
                                    coarse_mesh->totvert,
                                    ctx->coarse_mesh->totedge);
    }
  }

  /* Interpolate custom data when not an end point.
   * This data has already been copied from the original vertex by #subdiv_mesh_vertex_loose. */
  if (!ELEM(u, 0.0, 1.0)) {
    subdiv_mesh_vertex_of_loose_edge_interpolate(ctx, coarse_edge, u, subdiv_vertex_index);
  }
  /* Interpolate coordinate. */
  BKE_subdiv_mesh_interpolate_position_on_edge(ctx->coarse_positions,
                                               ctx->coarse_edges.data(),
                                               ctx->vert_to_edge_map,
                                               coarse_edge_index,
                                               is_simple,
                                               u,
                                               ctx->subdiv_positions[subdiv_vertex_index]);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Initialization
 * \{ */

static void setup_foreach_callbacks(const SubdivMeshContext *subdiv_context,
                                    SubdivForeachContext *foreach_context)
{
  memset(foreach_context, 0, sizeof(*foreach_context));
  /* General information. */
  foreach_context->topology_info = subdiv_mesh_topology_info;
  /* Every boundary geometry. Used for displacement averaging. */
  if (subdiv_context->have_displacement) {
    foreach_context->vertex_every_corner = subdiv_mesh_vertex_displacement_every_corner;
    foreach_context->vertex_every_edge = subdiv_mesh_vertex_displacement_every_edge;
  }
  foreach_context->vertex_corner = subdiv_mesh_vertex_corner;
  foreach_context->vertex_edge = subdiv_mesh_vertex_edge;
  foreach_context->vertex_inner = subdiv_mesh_vertex_inner;
  foreach_context->edge = subdiv_mesh_edge;
  foreach_context->loop = subdiv_mesh_loop;
  foreach_context->poly = subdiv_mesh_poly;
  foreach_context->vertex_loose = subdiv_mesh_vertex_loose;
  foreach_context->vertex_of_loose_edge = subdiv_mesh_vertex_of_loose_edge;
  foreach_context->user_data_tls_free = subdiv_mesh_tls_free;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public entry point
 * \{ */

Mesh *BKE_subdiv_to_mesh(Subdiv *subdiv,
                         const SubdivToMeshSettings *settings,
                         const Mesh *coarse_mesh)
{
  using namespace blender;
  BKE_subdiv_stats_begin(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH);
  /* Make sure evaluator is up to date with possible new topology, and that
   * it is refined for the new positions of coarse vertices. */
  if (!BKE_subdiv_eval_begin_from_mesh(
          subdiv, coarse_mesh, nullptr, SUBDIV_EVALUATOR_TYPE_CPU, nullptr)) {
    /* This could happen in two situations:
     * - OpenSubdiv is disabled.
     * - Something totally bad happened, and OpenSubdiv rejected our
     *   topology.
     * In either way, we can't safely continue. */
    if (coarse_mesh->totpoly) {
      BKE_subdiv_stats_end(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH);
      return nullptr;
    }
  }
  /* Initialize subdivision mesh creation context. */
  SubdivMeshContext subdiv_context{};
  subdiv_context.settings = settings;

  subdiv_context.coarse_mesh = coarse_mesh;
  subdiv_context.coarse_positions = BKE_mesh_vert_positions(coarse_mesh);
  subdiv_context.coarse_edges = coarse_mesh->edges();
  subdiv_context.coarse_polys = coarse_mesh->polys();
  subdiv_context.coarse_corner_verts = coarse_mesh->corner_verts();

  subdiv_context.subdiv = subdiv;
  subdiv_context.have_displacement = (subdiv->displacement_evaluator != nullptr);
  /* Multi-threaded traversal/evaluation. */
  BKE_subdiv_stats_begin(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH_GEOMETRY);
  SubdivForeachContext foreach_context;
  setup_foreach_callbacks(&subdiv_context, &foreach_context);
  SubdivMeshTLS tls{};
  foreach_context.user_data = &subdiv_context;
  foreach_context.user_data_tls_size = sizeof(SubdivMeshTLS);
  foreach_context.user_data_tls = &tls;
  BKE_subdiv_foreach_subdiv_geometry(subdiv, &foreach_context, settings, coarse_mesh);
  BKE_subdiv_stats_end(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH_GEOMETRY);
  Mesh *result = subdiv_context.subdiv_mesh;

  /* Move the optimal display edge array to the final bit vector. */
  if (!subdiv_context.subdiv_display_edges.is_empty()) {
    const Span<bool> span = subdiv_context.subdiv_display_edges;
    BitVector<> &bit_vector = result->runtime->subsurf_optimal_display_edges;
    bit_vector.clear();
    bit_vector.resize(subdiv_context.subdiv_display_edges.size());
    threading::parallel_for_aligned(span.index_range(), 4096, 64, [&](const IndexRange range) {
      for (const int i : range) {
        bit_vector[i].set(span[i]);
      }
    });
  }

  if (subdiv->settings.is_simple) {
    /* In simple subdivision, min and max positions are not changed, avoid recomputing bounds. */
    result->runtime->bounds_cache = coarse_mesh->runtime->bounds_cache;
  }

  // BKE_mesh_validate(result, true, true);
  BKE_subdiv_stats_end(&subdiv->stats, SUBDIV_STATS_SUBDIV_TO_MESH);
  /* Using normals from the limit surface gives different results than Blender's vertex normal
   * calculation. Since vertex normals are supposed to be a consistent cache, don't bother
   * calculating them here. The work may have been pointless anyway if the mesh is deformed or
   * changed afterwards. */
  BLI_assert(BKE_mesh_vert_normals_are_dirty(result) || BKE_mesh_poly_normals_are_dirty(result));
  /* Free used memory. */
  subdiv_mesh_context_free(&subdiv_context);
  return result;
}

/** \} */
