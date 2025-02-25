/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation */

/** \file
 * \ingroup bke
 */

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CLG_log.h"

#include "BLI_bitmap.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BLI_sys_types.h"

#include "BLI_edgehash.h"
#include "BLI_math_base.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.h"
#include "BKE_deform.h"
#include "BKE_mesh.hh"

#include "DEG_depsgraph.h"

#include "MEM_guardedalloc.h"

using blender::float3;
using blender::MutableSpan;
using blender::Span;

/* loop v/e are unsigned, so using max uint_32 value as invalid marker... */
#define INVALID_LOOP_EDGE_MARKER 4294967295u

static CLG_LogRef LOG = {"bke.mesh"};

void strip_loose_polysloops(Mesh *me, blender::BitSpan polys_to_remove);
void mesh_strip_edges(Mesh *me);

/* -------------------------------------------------------------------- */
/** \name Internal functions
 * \{ */

union EdgeUUID {
  uint32_t verts[2];
  int64_t edval;
};

struct SortFace {
  EdgeUUID es[4];
  uint index;
};

/* Used to detect polys (faces) using exactly the same vertices. */
/* Used to detect loops used by no (disjoint) or more than one (intersect) polys. */
struct SortPoly {
  int *verts;
  int numverts;
  int loopstart;
  uint index;
  bool invalid; /* Poly index. */
};

static void edge_store_assign(uint32_t verts[2], const uint32_t v1, const uint32_t v2)
{
  if (v1 < v2) {
    verts[0] = v1;
    verts[1] = v2;
  }
  else {
    verts[0] = v2;
    verts[1] = v1;
  }
}

static void edge_store_from_mface_quad(EdgeUUID es[4], MFace *mf)
{
  edge_store_assign(es[0].verts, mf->v1, mf->v2);
  edge_store_assign(es[1].verts, mf->v2, mf->v3);
  edge_store_assign(es[2].verts, mf->v3, mf->v4);
  edge_store_assign(es[3].verts, mf->v4, mf->v1);
}

static void edge_store_from_mface_tri(EdgeUUID es[4], MFace *mf)
{
  edge_store_assign(es[0].verts, mf->v1, mf->v2);
  edge_store_assign(es[1].verts, mf->v2, mf->v3);
  edge_store_assign(es[2].verts, mf->v3, mf->v1);
  es[3].verts[0] = es[3].verts[1] = UINT_MAX;
}

static int int64_cmp(const void *v1, const void *v2)
{
  const int64_t x1 = *(const int64_t *)v1;
  const int64_t x2 = *(const int64_t *)v2;

  if (x1 > x2) {
    return 1;
  }
  if (x1 < x2) {
    return -1;
  }

  return 0;
}

static int search_face_cmp(const void *v1, const void *v2)
{
  const SortFace *sfa = static_cast<const SortFace *>(v1);
  const SortFace *sfb = static_cast<const SortFace *>(v2);

  if (sfa->es[0].edval > sfb->es[0].edval) {
    return 1;
  }
  if (sfa->es[0].edval < sfb->es[0].edval) {
    return -1;
  }

  if (sfa->es[1].edval > sfb->es[1].edval) {
    return 1;
  }
  if (sfa->es[1].edval < sfb->es[1].edval) {
    return -1;
  }

  if (sfa->es[2].edval > sfb->es[2].edval) {
    return 1;
  }
  if (sfa->es[2].edval < sfb->es[2].edval) {
    return -1;
  }

  if (sfa->es[3].edval > sfb->es[3].edval) {
    return 1;
  }
  if (sfa->es[3].edval < sfb->es[3].edval) {
    return -1;
  }

  return 0;
}

/* TODO: check there is not some standard define of this somewhere! */
static int int_cmp(const void *v1, const void *v2)
{
  return *(int *)v1 > *(int *)v2 ? 1 : *(int *)v1 < *(int *)v2 ? -1 : 0;
}

static int search_poly_cmp(const void *v1, const void *v2)
{
  const SortPoly *sp1 = static_cast<const SortPoly *>(v1);
  const SortPoly *sp2 = static_cast<const SortPoly *>(v2);

  /* Reject all invalid polys at end of list! */
  if (sp1->invalid || sp2->invalid) {
    return sp1->invalid ? (sp2->invalid ? 0 : 1) : -1;
  }
  /* Else, sort on first non-equal verts (remember verts of valid polys are sorted). */
  const int max_idx = sp1->numverts > sp2->numverts ? sp2->numverts : sp1->numverts;
  for (int idx = 0; idx < max_idx; idx++) {
    const int v1_i = sp1->verts[idx];
    const int v2_i = sp2->verts[idx];
    if (v1_i != v2_i) {
      return (v1_i > v2_i) ? 1 : -1;
    }
  }
  return sp1->numverts > sp2->numverts ? 1 : sp1->numverts < sp2->numverts ? -1 : 0;
}

static int search_polyloop_cmp(const void *v1, const void *v2)
{
  const SortPoly *sp1 = static_cast<const SortPoly *>(v1);
  const SortPoly *sp2 = static_cast<const SortPoly *>(v2);

  /* Reject all invalid polys at end of list! */
  if (sp1->invalid || sp2->invalid) {
    return sp1->invalid && sp2->invalid ? 0 : sp1->invalid ? 1 : -1;
  }
  /* Else, sort on loopstart. */
  return sp1->loopstart > sp2->loopstart ? 1 : sp1->loopstart < sp2->loopstart ? -1 : 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh Validation
 * \{ */

#define PRINT_MSG(...) \
  if (do_verbose) { \
    CLOG_INFO(&LOG, 1, __VA_ARGS__); \
  } \
  ((void)0)

#define PRINT_ERR(...) \
  do { \
    is_valid = false; \
    if (do_verbose) { \
      CLOG_ERROR(&LOG, __VA_ARGS__); \
    } \
  } while (0)

/* NOLINTNEXTLINE: readability-function-size */
bool BKE_mesh_validate_arrays(Mesh *mesh,
                              float (*vert_positions)[3],
                              uint totvert,
                              MEdge *edges,
                              uint totedge,
                              MFace *mfaces,
                              uint totface,
                              int *corner_verts,
                              int *corner_edges,
                              uint totloop,
                              int *poly_offsets,
                              uint totpoly,
                              MDeformVert *dverts, /* assume totvert length */
                              const bool do_verbose,
                              const bool do_fixes,
                              bool *r_changed)
{
#define REMOVE_EDGE_TAG(_me) \
  { \
    _me->v2 = _me->v1; \
    free_flag.edges = do_fixes; \
  } \
  (void)0
#define IS_REMOVED_EDGE(_me) (_me->v2 == _me->v1)

#define REMOVE_LOOP_TAG(corner) \
  { \
    corner_edges[corner] = INVALID_LOOP_EDGE_MARKER; \
    free_flag.polyloops = do_fixes; \
  } \
  (void)0
  blender::BitVector<> polys_to_remove(totpoly);

  blender::bke::AttributeWriter<int> material_indices =
      mesh->attributes_for_write().lookup_for_write<int>("material_index");
  blender::MutableVArraySpan<int> material_indices_span(material_indices.varray);

#if 0
  const blender::OffsetIndices<int> polys({poly_offsets, totpoly + 1});
  for (const int i : polys.index_range()) {
    BLI_assert(polys[i].size() > 2);
  }
#endif

  MEdge *edge;
  uint i, j;
  int *v;

  bool is_valid = true;

  union {
    struct {
      int verts : 1;
      int verts_weight : 1;
      int loops_edge : 1;
    };
    int as_flag;
  } fix_flag;

  union {
    struct {
      int edges : 1;
      int faces : 1;
      /* This regroups loops and polys! */
      int polyloops : 1;
      int mselect : 1;
    };
    int as_flag;
  } free_flag;

  union {
    struct {
      int edges : 1;
    };
    int as_flag;
  } recalc_flag;

  EdgeHash *edge_hash = BLI_edgehash_new_ex(__func__, totedge);

  BLI_assert(!(do_fixes && mesh == nullptr));

  fix_flag.as_flag = 0;
  free_flag.as_flag = 0;
  recalc_flag.as_flag = 0;

  PRINT_MSG("verts(%u), edges(%u), loops(%u), polygons(%u)", totvert, totedge, totloop, totpoly);

  if (totedge == 0 && totpoly != 0) {
    PRINT_ERR("\tLogical error, %u polygons and 0 edges", totpoly);
    recalc_flag.edges = do_fixes;
  }

  for (i = 0; i < totvert; i++) {
    for (j = 0; j < 3; j++) {
      if (!isfinite(vert_positions[i][j])) {
        PRINT_ERR("\tVertex %u: has invalid coordinate", i);

        if (do_fixes) {
          zero_v3(vert_positions[i]);

          fix_flag.verts = true;
        }
      }
    }
  }

  for (i = 0, edge = edges; i < totedge; i++, edge++) {
    bool remove = false;

    if (edge->v1 == edge->v2) {
      PRINT_ERR("\tEdge %u: has matching verts, both %u", i, edge->v1);
      remove = do_fixes;
    }
    if (edge->v1 >= totvert) {
      PRINT_ERR("\tEdge %u: v1 index out of range, %u", i, edge->v1);
      remove = do_fixes;
    }
    if (edge->v2 >= totvert) {
      PRINT_ERR("\tEdge %u: v2 index out of range, %u", i, edge->v2);
      remove = do_fixes;
    }

    if ((edge->v1 != edge->v2) && BLI_edgehash_haskey(edge_hash, edge->v1, edge->v2)) {
      PRINT_ERR("\tEdge %u: is a duplicate of %d",
                i,
                POINTER_AS_INT(BLI_edgehash_lookup(edge_hash, edge->v1, edge->v2)));
      remove = do_fixes;
    }

    if (remove == false) {
      if (edge->v1 != edge->v2) {
        BLI_edgehash_insert(edge_hash, edge->v1, edge->v2, POINTER_FROM_INT(i));
      }
    }
    else {
      REMOVE_EDGE_TAG(edge);
    }
  }

  if (mfaces && !poly_offsets) {
#define REMOVE_FACE_TAG(_mf) \
  { \
    _mf->v3 = 0; \
    free_flag.faces = do_fixes; \
  } \
  (void)0
#define CHECK_FACE_VERT_INDEX(a, b) \
  if (mf->a == mf->b) { \
    PRINT_ERR("    face %u: verts invalid, " STRINGIFY(a) "/" STRINGIFY(b) " both %u", i, mf->a); \
    remove = do_fixes; \
  } \
  (void)0
#define CHECK_FACE_EDGE(a, b) \
  if (!BLI_edgehash_haskey(edge_hash, mf->a, mf->b)) { \
    PRINT_ERR("    face %u: edge " STRINGIFY(a) "/" STRINGIFY(b) " (%u,%u) is missing edge data", \
              i, \
              mf->a, \
              mf->b); \
    recalc_flag.edges = do_fixes; \
  } \
  (void)0

    MFace *mf;
    MFace *mf_prev;

    SortFace *sort_faces = (SortFace *)MEM_callocN(sizeof(SortFace) * totface, "search faces");
    SortFace *sf;
    SortFace *sf_prev;
    uint totsortface = 0;

    PRINT_ERR("No Polys, only tessellated Faces");

    for (i = 0, mf = mfaces, sf = sort_faces; i < totface; i++, mf++) {
      bool remove = false;
      int fidx;
      uint fv[4];

      fidx = mf->v4 ? 3 : 2;
      do {
        fv[fidx] = *(&(mf->v1) + fidx);
        if (fv[fidx] >= totvert) {
          PRINT_ERR("\tFace %u: 'v%d' index out of range, %u", i, fidx + 1, fv[fidx]);
          remove = do_fixes;
        }
      } while (fidx--);

      if (remove == false) {
        if (mf->v4) {
          CHECK_FACE_VERT_INDEX(v1, v2);
          CHECK_FACE_VERT_INDEX(v1, v3);
          CHECK_FACE_VERT_INDEX(v1, v4);

          CHECK_FACE_VERT_INDEX(v2, v3);
          CHECK_FACE_VERT_INDEX(v2, v4);

          CHECK_FACE_VERT_INDEX(v3, v4);
        }
        else {
          CHECK_FACE_VERT_INDEX(v1, v2);
          CHECK_FACE_VERT_INDEX(v1, v3);

          CHECK_FACE_VERT_INDEX(v2, v3);
        }

        if (remove == false) {
          if (totedge) {
            if (mf->v4) {
              CHECK_FACE_EDGE(v1, v2);
              CHECK_FACE_EDGE(v2, v3);
              CHECK_FACE_EDGE(v3, v4);
              CHECK_FACE_EDGE(v4, v1);
            }
            else {
              CHECK_FACE_EDGE(v1, v2);
              CHECK_FACE_EDGE(v2, v3);
              CHECK_FACE_EDGE(v3, v1);
            }
          }

          sf->index = i;

          if (mf->v4) {
            edge_store_from_mface_quad(sf->es, mf);

            qsort(sf->es, 4, sizeof(int64_t), int64_cmp);
          }
          else {
            edge_store_from_mface_tri(sf->es, mf);
            qsort(sf->es, 3, sizeof(int64_t), int64_cmp);
          }

          totsortface++;
          sf++;
        }
      }

      if (remove) {
        REMOVE_FACE_TAG(mf);
      }
    }

    qsort(sort_faces, totsortface, sizeof(SortFace), search_face_cmp);

    sf = sort_faces;
    sf_prev = sf;
    sf++;

    for (i = 1; i < totsortface; i++, sf++) {
      bool remove = false;

      /* on a valid mesh, code below will never run */
      if (memcmp(sf->es, sf_prev->es, sizeof(sf_prev->es)) == 0) {
        mf = mfaces + sf->index;

        if (do_verbose) {
          mf_prev = mfaces + sf_prev->index;

          if (mf->v4) {
            PRINT_ERR("\tFace %u & %u: are duplicates (%u,%u,%u,%u) (%u,%u,%u,%u)",
                      sf->index,
                      sf_prev->index,
                      mf->v1,
                      mf->v2,
                      mf->v3,
                      mf->v4,
                      mf_prev->v1,
                      mf_prev->v2,
                      mf_prev->v3,
                      mf_prev->v4);
          }
          else {
            PRINT_ERR("\tFace %u & %u: are duplicates (%u,%u,%u) (%u,%u,%u)",
                      sf->index,
                      sf_prev->index,
                      mf->v1,
                      mf->v2,
                      mf->v3,
                      mf_prev->v1,
                      mf_prev->v2,
                      mf_prev->v3);
          }
        }

        remove = do_fixes;
      }
      else {
        sf_prev = sf;
      }

      if (remove) {
        REMOVE_FACE_TAG(mf);
      }
    }

    MEM_freeN(sort_faces);

#undef REMOVE_FACE_TAG
#undef CHECK_FACE_VERT_INDEX
#undef CHECK_FACE_EDGE
  }

  /* Checking loops and polys is a bit tricky, as they are quite intricate...
   *
   * Polys must have:
   * - a valid loopstart value.
   * - a valid totloop value (>= 3 and loopstart+totloop < me.totloop).
   *
   * Loops must have:
   * - a valid v value.
   * - a valid e value (corresponding to the edge it defines with the next loop in poly).
   *
   * Also, loops not used by polys can be discarded.
   * And "intersecting" loops (i.e. loops used by more than one poly) are invalid,
   * so be sure to leave at most one poly per loop!
   */
  {
    BLI_bitmap *vert_tag = BLI_BITMAP_NEW(mesh->totvert, __func__);

    SortPoly *sort_polys = (SortPoly *)MEM_callocN(sizeof(SortPoly) * totpoly,
                                                   "mesh validate's sort_polys");
    SortPoly *prev_sp, *sp = sort_polys;
    int prev_end;

    for (const int64_t i : blender::IndexRange(totpoly)) {
      const int poly_start = poly_offsets[i];
      const int poly_size = poly_offsets[i + 1] - poly_start;
      sp->index = i;

      /* Material index, isolated from other tests here. While large indices are clamped,
       * negative indices aren't supported by drawing, exporters etc.
       * To check the indices are in range, use #BKE_mesh_validate_material_indices */
      if (material_indices && material_indices_span[i] < 0) {
        PRINT_ERR("\tPoly %u has invalid material (%d)", sp->index, material_indices_span[i]);
        if (do_fixes) {
          material_indices_span[i] = 0;
        }
      }

      if (poly_start < 0 || poly_size < 3) {
        /* Invalid loop data. */
        PRINT_ERR(
            "\tPoly %u is invalid (loopstart: %d, totloop: %d)", sp->index, poly_start, poly_size);
        sp->invalid = true;
      }
      else if (poly_start + poly_size > totloop) {
        /* Invalid loop data. */
        PRINT_ERR(
            "\tPoly %u uses loops out of range "
            "(loopstart: %d, loopend: %d, max number of loops: %u)",
            sp->index,
            poly_start,
            poly_start + poly_size - 1,
            totloop - 1);
        sp->invalid = true;
      }
      else {
        /* Poly itself is valid, for now. */
        int v1, v2; /* v1 is prev loop vert idx, v2 is current loop one. */
        sp->invalid = false;
        sp->verts = v = (int *)MEM_mallocN(sizeof(int) * poly_size, "Vert idx of SortPoly");
        sp->numverts = poly_size;
        sp->loopstart = poly_start;

        /* Ideally we would only have to do that once on all vertices
         * before we start checking each poly, but several polys can use same vert,
         * so we have to ensure here all verts of current poly are cleared. */
        for (j = 0; j < poly_size; j++) {
          const int vert = corner_verts[sp->loopstart + j];
          if (vert < totvert) {
            BLI_BITMAP_DISABLE(vert_tag, vert);
          }
        }

        /* Test all poly's loops' vert idx. */
        for (j = 0; j < poly_size; j++, v++) {
          const int vert = corner_verts[sp->loopstart + j];
          if (vert >= totvert) {
            /* Invalid vert idx. */
            PRINT_ERR("\tLoop %u has invalid vert reference (%d)", sp->loopstart + j, vert);
            sp->invalid = true;
          }
          else if (BLI_BITMAP_TEST(vert_tag, vert)) {
            PRINT_ERR("\tPoly %u has duplicated vert reference at corner (%u)", uint(i), j);
            sp->invalid = true;
          }
          else {
            BLI_BITMAP_ENABLE(vert_tag, vert);
          }
          *v = vert;
        }

        if (sp->invalid) {
          sp++;
          continue;
        }

        /* Test all poly's loops. */
        for (j = 0; j < poly_size; j++) {
          const int corner = sp->loopstart + j;
          const int vert = corner_verts[corner];
          const int edge_i = corner_edges[corner];
          v1 = vert;
          v2 = corner_verts[sp->loopstart + (j + 1) % poly_size];
          if (!BLI_edgehash_haskey(edge_hash, v1, v2)) {
            /* Edge not existing. */
            PRINT_ERR("\tPoly %u needs missing edge (%d, %d)", sp->index, v1, v2);
            if (do_fixes) {
              recalc_flag.edges = true;
            }
            else {
              sp->invalid = true;
            }
          }
          else if (edge_i >= totedge) {
            /* Invalid edge idx.
             * We already know from previous text that a valid edge exists, use it (if allowed)! */
            if (do_fixes) {
              int prev_e = edge_i;
              corner_edges[corner] = POINTER_AS_INT(BLI_edgehash_lookup(edge_hash, v1, v2));
              fix_flag.loops_edge = true;
              PRINT_ERR("\tLoop %d has invalid edge reference (%d), fixed using edge %d",
                        corner,
                        prev_e,
                        corner_edges[corner]);
            }
            else {
              PRINT_ERR("\tLoop %d has invalid edge reference (%d)", corner, edge_i);
              sp->invalid = true;
            }
          }
          else {
            edge = &edges[edge_i];
            if (IS_REMOVED_EDGE(edge) ||
                !((edge->v1 == v1 && edge->v2 == v2) || (edge->v1 == v2 && edge->v2 == v1))) {
              /* The pointed edge is invalid (tagged as removed, or vert idx mismatch),
               * and we already know from previous test that a valid one exists,
               * use it (if allowed)! */
              if (do_fixes) {
                int prev_e = edge_i;
                corner_edges[corner] = POINTER_AS_INT(BLI_edgehash_lookup(edge_hash, v1, v2));
                fix_flag.loops_edge = true;
                PRINT_ERR(
                    "\tPoly %u has invalid edge reference (%d, is_removed: %d), fixed using edge "
                    "%d",
                    sp->index,
                    prev_e,
                    IS_REMOVED_EDGE(edge),
                    corner_edges[corner]);
              }
              else {
                PRINT_ERR("\tPoly %u has invalid edge reference (%d)", sp->index, edge_i);
                sp->invalid = true;
              }
            }
          }
        }

        if (!sp->invalid) {
          /* Needed for checking polys using same verts below. */
          qsort(sp->verts, sp->numverts, sizeof(int), int_cmp);
        }
      }
      sp++;
    }

    MEM_freeN(vert_tag);

    /* Second check pass, testing polys using the same verts. */
    qsort(sort_polys, totpoly, sizeof(SortPoly), search_poly_cmp);
    sp = prev_sp = sort_polys;
    sp++;

    for (i = 1; i < totpoly; i++, sp++) {
      int p1_nv = sp->numverts, p2_nv = prev_sp->numverts;
      const int *p1_v = sp->verts, *p2_v = prev_sp->verts;

      if (sp->invalid) {
        /* Break, because all known invalid polys have been put at the end
         * by qsort with search_poly_cmp. */
        break;
      }

      /* Test same polys. */
      if ((p1_nv == p2_nv) && (memcmp(p1_v, p2_v, p1_nv * sizeof(*p1_v)) == 0)) {
        if (do_verbose) {
          /* TODO: convert list to string */
          PRINT_ERR("\tPolys %u and %u use same vertices (%d", prev_sp->index, sp->index, *p1_v);
          for (j = 1; j < p1_nv; j++) {
            PRINT_ERR(", %d", p1_v[j]);
          }
          PRINT_ERR("), considering poly %u as invalid.", sp->index);
        }
        else {
          is_valid = false;
        }
        sp->invalid = true;
      }
      else {
        prev_sp = sp;
      }
    }

    /* Third check pass, testing loops used by none or more than one poly. */
    qsort(sort_polys, totpoly, sizeof(SortPoly), search_polyloop_cmp);
    sp = sort_polys;
    prev_sp = nullptr;
    prev_end = 0;
    for (i = 0; i < totpoly; i++, sp++) {
      /* Free this now, we don't need it anymore, and avoid us another loop! */
      if (sp->verts) {
        MEM_freeN(sp->verts);
      }

      /* Note above prev_sp: in following code, we make sure it is always valid poly (or nullptr).
       */
      if (sp->invalid) {
        if (do_fixes) {
          polys_to_remove[sp->index].set();
          /* DO NOT REMOVE ITS LOOPS!!!
           * As already invalid polys are at the end of the SortPoly list, the loops they
           * were the only users have already been tagged as "to remove" during previous
           * iterations, and we don't want to remove some loops that may be used by
           * another valid poly! */
        }
      }
      /* Test loops users. */
      else {
        /* Unused loops. */
        if (prev_end < sp->loopstart) {
          int corner;
          for (j = prev_end, corner = prev_end; j < sp->loopstart; j++, corner++) {
            PRINT_ERR("\tLoop %u is unused.", j);
            if (do_fixes) {
              REMOVE_LOOP_TAG(corner);
            }
          }
          prev_end = sp->loopstart + sp->numverts;
          prev_sp = sp;
        }
        /* Multi-used loops. */
        else if (prev_end > sp->loopstart) {
          PRINT_ERR("\tPolys %u and %u share loops from %d to %d, considering poly %u as invalid.",
                    prev_sp->index,
                    sp->index,
                    sp->loopstart,
                    prev_end,
                    sp->index);
          if (do_fixes) {
            polys_to_remove[sp->index].set();
            /* DO NOT REMOVE ITS LOOPS!!!
             * They might be used by some next, valid poly!
             * Just not updating prev_end/prev_sp vars is enough to ensure the loops
             * effectively no more needed will be marked as "to be removed"! */
          }
        }
        else {
          prev_end = sp->loopstart + sp->numverts;
          prev_sp = sp;
        }
      }
    }
    /* We may have some remaining unused loops to get rid of! */
    if (prev_end < totloop) {
      int corner;
      for (j = prev_end, corner = prev_end; j < totloop; j++, corner++) {
        PRINT_ERR("\tLoop %u is unused.", j);
        if (do_fixes) {
          REMOVE_LOOP_TAG(corner);
        }
      }
    }

    MEM_freeN(sort_polys);
  }

  BLI_edgehash_free(edge_hash, nullptr);

  /* fix deform verts */
  if (dverts) {
    MDeformVert *dv;
    for (i = 0, dv = dverts; i < totvert; i++, dv++) {
      MDeformWeight *dw;

      for (j = 0, dw = dv->dw; j < dv->totweight; j++, dw++) {
        /* NOTE: greater than max defgroups is accounted for in our code, but not < 0. */
        if (!isfinite(dw->weight)) {
          PRINT_ERR("\tVertex deform %u, group %u has weight: %f", i, dw->def_nr, dw->weight);
          if (do_fixes) {
            dw->weight = 0.0f;
            fix_flag.verts_weight = true;
          }
        }
        else if (dw->weight < 0.0f || dw->weight > 1.0f) {
          PRINT_ERR("\tVertex deform %u, group %u has weight: %f", i, dw->def_nr, dw->weight);
          if (do_fixes) {
            CLAMP(dw->weight, 0.0f, 1.0f);
            fix_flag.verts_weight = true;
          }
        }

        /* Not technically incorrect since this is unsigned, however,
         * a value over INT_MAX is almost certainly caused by wrapping an uint. */
        if (dw->def_nr >= INT_MAX) {
          PRINT_ERR("\tVertex deform %u, has invalid group %u", i, dw->def_nr);
          if (do_fixes) {
            BKE_defvert_remove_group(dv, dw);
            fix_flag.verts_weight = true;

            if (dv->dw) {
              /* re-allocated, the new values compensate for stepping
               * within the for loop and may not be valid */
              j--;
              dw = dv->dw + j;
            }
            else { /* all freed */
              break;
            }
          }
        }
      }
    }
  }

#undef REMOVE_EDGE_TAG
#undef IS_REMOVED_EDGE
#undef REMOVE_LOOP_TAG
#undef REMOVE_POLY_TAG

  if (mesh) {
    if (free_flag.faces) {
      BKE_mesh_strip_loose_faces(mesh);
    }

    if (free_flag.polyloops) {
      strip_loose_polysloops(mesh, polys_to_remove);
    }

    if (free_flag.edges) {
      mesh_strip_edges(mesh);
    }

    if (recalc_flag.edges) {
      BKE_mesh_calc_edges(mesh, true, false);
    }
  }

  if (mesh && mesh->mselect) {
    MSelect *msel;

    for (i = 0, msel = mesh->mselect; i < mesh->totselect; i++, msel++) {
      int tot_elem = 0;

      if (msel->index < 0) {
        PRINT_ERR(
            "\tMesh select element %u type %d index is negative, "
            "resetting selection stack.\n",
            i,
            msel->type);
        free_flag.mselect = do_fixes;
        break;
      }

      switch (msel->type) {
        case ME_VSEL:
          tot_elem = mesh->totvert;
          break;
        case ME_ESEL:
          tot_elem = mesh->totedge;
          break;
        case ME_FSEL:
          tot_elem = mesh->totpoly;
          break;
      }

      if (msel->index > tot_elem) {
        PRINT_ERR(
            "\tMesh select element %u type %d index %d is larger than data array size %d, "
            "resetting selection stack.\n",
            i,
            msel->type,
            msel->index,
            tot_elem);

        free_flag.mselect = do_fixes;
        break;
      }
    }

    if (free_flag.mselect) {
      MEM_freeN(mesh->mselect);
      mesh->mselect = nullptr;
      mesh->totselect = 0;
    }
  }

  material_indices_span.save();
  material_indices.finish();

  PRINT_MSG("%s: finished\n\n", __func__);

  *r_changed = (fix_flag.as_flag || free_flag.as_flag || recalc_flag.as_flag);

  BLI_assert((*r_changed == false) || (do_fixes == true));

  return is_valid;
}

static bool mesh_validate_customdata(CustomData *data,
                                     eCustomDataMask mask,
                                     const uint totitems,
                                     const bool do_verbose,
                                     const bool do_fixes,
                                     bool *r_change)
{
  bool is_valid = true;
  bool has_fixes = false;
  int i = 0;

  PRINT_MSG("%s: Checking %d CD layers...\n", __func__, data->totlayer);

  /* Set dummy values so the layer-type is always initialized on first access. */
  int layer_num = -1;
  int layer_num_type = -1;

  while (i < data->totlayer) {
    CustomDataLayer *layer = &data->layers[i];
    const eCustomDataType type = eCustomDataType(layer->type);
    bool ok = true;

    /* Count layers when the type changes. */
    if (layer_num_type != type) {
      layer_num = CustomData_number_of_layers(data, type);
      layer_num_type = type;
    }

    /* Validate active index, for a time this could be set to a negative value, see: #105860. */
    int *active_index_array[] = {
        &layer->active,
        &layer->active_rnd,
        &layer->active_clone,
        &layer->active_mask,
    };
    for (int *active_index : Span(active_index_array, ARRAY_SIZE(active_index_array))) {
      if (*active_index < 0) {
        PRINT_ERR("\tCustomDataLayer type %d has a negative active index (%d)\n",
                  layer->type,
                  *active_index);
        if (do_fixes) {
          *active_index = 0;
          has_fixes = true;
        }
      }
      else {
        if (*active_index >= layer_num) {
          PRINT_ERR("\tCustomDataLayer type %d has an out of bounds active index (%d >= %d)\n",
                    layer->type,
                    *active_index,
                    layer_num);
          if (do_fixes) {
            BLI_assert(layer_num > 0);
            *active_index = layer_num - 1;
            has_fixes = true;
          }
        }
      }
    }

    if (CustomData_layertype_is_singleton(type)) {
      if (layer_num > 1) {
        PRINT_ERR("\tCustomDataLayer type %d is a singleton, found %d in Mesh structure\n",
                  type,
                  layer_num);
        ok = false;
      }
    }

    if (mask != 0) {
      eCustomDataMask layer_typemask = CD_TYPE_AS_MASK(type);
      if ((layer_typemask & mask) == 0) {
        PRINT_ERR("\tCustomDataLayer type %d which isn't in the mask\n", type);
        ok = false;
      }
    }

    if (ok == false) {
      if (do_fixes) {
        CustomData_free_layer(data, type, 0, i);
        has_fixes = true;
      }
    }

    if (ok) {
      if (CustomData_layer_validate(layer, totitems, do_fixes)) {
        PRINT_ERR("\tCustomDataLayer type %d has some invalid data\n", type);
        has_fixes = do_fixes;
      }
      i++;
    }
  }

  PRINT_MSG("%s: Finished (is_valid=%d)\n\n", __func__, int(!has_fixes));

  *r_change = has_fixes;

  return is_valid;
}

bool BKE_mesh_validate_all_customdata(CustomData *vdata,
                                      const uint totvert,
                                      CustomData *edata,
                                      const uint totedge,
                                      CustomData *ldata,
                                      const uint totloop,
                                      CustomData *pdata,
                                      const uint totpoly,
                                      const bool check_meshmask,
                                      const bool do_verbose,
                                      const bool do_fixes,
                                      bool *r_change)
{
  bool is_valid = true;
  bool is_change_v, is_change_e, is_change_l, is_change_p;
  CustomData_MeshMasks mask = {0};
  if (check_meshmask) {
    mask = CD_MASK_MESH;
  }

  is_valid &= mesh_validate_customdata(
      vdata, mask.vmask, totvert, do_verbose, do_fixes, &is_change_v);
  is_valid &= mesh_validate_customdata(
      edata, mask.emask, totedge, do_verbose, do_fixes, &is_change_e);
  is_valid &= mesh_validate_customdata(
      ldata, mask.lmask, totloop, do_verbose, do_fixes, &is_change_l);
  is_valid &= mesh_validate_customdata(
      pdata, mask.pmask, totpoly, do_verbose, do_fixes, &is_change_p);

  const int tot_uvloop = CustomData_number_of_layers(ldata, CD_PROP_FLOAT2);
  if (tot_uvloop > MAX_MTFACE) {
    PRINT_ERR(
        "\tMore UV layers than %d allowed, %d last ones won't be available for render, shaders, "
        "etc.\n",
        MAX_MTFACE,
        tot_uvloop - MAX_MTFACE);
  }

  /* check indices of clone/stencil */
  if (do_fixes && CustomData_get_clone_layer(ldata, CD_PROP_FLOAT2) >= tot_uvloop) {
    CustomData_set_layer_clone(ldata, CD_PROP_FLOAT2, 0);
    is_change_l = true;
  }
  if (do_fixes && CustomData_get_stencil_layer(ldata, CD_PROP_FLOAT2) >= tot_uvloop) {
    CustomData_set_layer_stencil(ldata, CD_PROP_FLOAT2, 0);
    is_change_l = true;
  }

  *r_change = (is_change_v || is_change_e || is_change_l || is_change_p);

  return is_valid;
}

bool BKE_mesh_validate(Mesh *me, const bool do_verbose, const bool cddata_check_mask)
{
  bool changed;

  if (do_verbose) {
    CLOG_INFO(&LOG, 0, "MESH: %s", me->id.name + 2);
  }

  BKE_mesh_validate_all_customdata(&me->vdata,
                                   me->totvert,
                                   &me->edata,
                                   me->totedge,
                                   &me->ldata,
                                   me->totloop,
                                   &me->pdata,
                                   me->totpoly,
                                   cddata_check_mask,
                                   do_verbose,
                                   true,
                                   &changed);
  MutableSpan<float3> positions = me->vert_positions_for_write();
  MutableSpan<MEdge> edges = me->edges_for_write();
  MutableSpan<int> poly_offsets = me->poly_offsets_for_write();
  MutableSpan<int> corner_verts = me->corner_verts_for_write();
  MutableSpan<int> corner_edges = me->corner_edges_for_write();

  BKE_mesh_validate_arrays(
      me,
      reinterpret_cast<float(*)[3]>(positions.data()),
      positions.size(),
      edges.data(),
      edges.size(),
      (MFace *)CustomData_get_layer_for_write(&me->fdata, CD_MFACE, me->totface),
      me->totface,
      corner_verts.data(),
      corner_edges.data(),
      corner_verts.size(),
      poly_offsets.data(),
      me->totpoly,
      me->deform_verts_for_write().data(),
      do_verbose,
      true,
      &changed);

  if (changed) {
    DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY_ALL_MODES);
    return true;
  }

  return false;
}

bool BKE_mesh_is_valid(Mesh *me)
{
  const bool do_verbose = true;
  const bool do_fixes = false;

  bool is_valid = true;
  bool changed = true;

  is_valid &= BKE_mesh_validate_all_customdata(
      &me->vdata,
      me->totvert,
      &me->edata,
      me->totedge,
      &me->ldata,
      me->totloop,
      &me->pdata,
      me->totpoly,
      false, /* setting mask here isn't useful, gives false positives */
      do_verbose,
      do_fixes,
      &changed);

  MutableSpan<float3> positions = me->vert_positions_for_write();
  MutableSpan<MEdge> edges = me->edges_for_write();
  MutableSpan<int> poly_offsets = me->poly_offsets_for_write();
  MutableSpan<int> corner_verts = me->corner_verts_for_write();
  MutableSpan<int> corner_edges = me->corner_edges_for_write();

  is_valid &= BKE_mesh_validate_arrays(
      me,
      reinterpret_cast<float(*)[3]>(positions.data()),
      positions.size(),
      edges.data(),
      edges.size(),
      (MFace *)CustomData_get_layer_for_write(&me->fdata, CD_MFACE, me->totface),
      me->totface,
      corner_verts.data(),
      corner_edges.data(),
      corner_verts.size(),
      poly_offsets.data(),
      me->totpoly,
      me->deform_verts_for_write().data(),
      do_verbose,
      do_fixes,
      &changed);

  BLI_assert(changed == false);

  return is_valid;
}

bool BKE_mesh_validate_material_indices(Mesh *me)
{
  const int mat_nr_max = max_ii(0, me->totcol - 1);
  bool is_valid = true;

  blender::bke::AttributeWriter<int> material_indices =
      me->attributes_for_write().lookup_for_write<int>("material_index");
  blender::MutableVArraySpan<int> material_indices_span(material_indices.varray);
  for (const int i : material_indices_span.index_range()) {
    if (material_indices_span[i] < 0 || material_indices_span[i] > mat_nr_max) {
      material_indices_span[i] = 0;
      is_valid = false;
    }
  }
  material_indices_span.save();
  material_indices.finish();

  if (!is_valid) {
    DEG_id_tag_update(&me->id, ID_RECALC_GEOMETRY_ALL_MODES);
    return true;
  }

  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh Stripping (removing invalid data)
 * \{ */

void BKE_mesh_strip_loose_faces(Mesh *me)
{
  /* NOTE: We need to keep this for edge creation (for now?), and some old `readfile.c` code. */
  MFace *f;
  int a, b;
  MFace *mfaces = (MFace *)CustomData_get_layer_for_write(&me->fdata, CD_MFACE, me->totface);

  for (a = b = 0, f = mfaces; a < me->totface; a++, f++) {
    if (f->v3) {
      if (a != b) {
        memcpy(&mfaces[b], f, sizeof(mfaces[b]));
        CustomData_copy_data(&me->fdata, &me->fdata, a, b, 1);
      }
      b++;
    }
  }
  if (a != b) {
    CustomData_free_elem(&me->fdata, b, a - b);
    me->totface = b;
  }
}

void strip_loose_polysloops(Mesh *me, blender::BitSpan polys_to_remove)
{
  MutableSpan<int> poly_offsets = me->poly_offsets_for_write();
  MutableSpan<int> corner_edges = me->corner_edges_for_write();

  int a, b;
  /* New loops idx! */
  int *new_idx = (int *)MEM_mallocN(sizeof(int) * me->totloop, __func__);

  for (a = b = 0; a < me->totpoly; a++) {
    bool invalid = false;
    int start = poly_offsets[a];
    int size = poly_offsets[a + 1] - start;
    int stop = start + size;

    if (polys_to_remove[a]) {
      invalid = true;
    }
    else if (stop > me->totloop || stop < start || size < 0) {
      invalid = true;
    }
    else {
      /* If one of the poly's loops is invalid, the whole poly is invalid! */
      if (corner_edges.slice(start, size).as_span().contains(INVALID_LOOP_EDGE_MARKER)) {
        invalid = true;
      }
    }

    if (size >= 3 && !invalid) {
      if (a != b) {
        CustomData_copy_data(&me->pdata, &me->pdata, a, b, 1);
      }
      b++;
    }
  }
  if (a != b) {
    CustomData_free_elem(&me->pdata, b, a - b);
    me->totpoly = b;
  }

  /* And now, get rid of invalid loops. */
  int corner = 0;
  for (a = b = 0; a < me->totloop; a++, corner++) {
    if (corner_edges[corner] != INVALID_LOOP_EDGE_MARKER) {
      if (a != b) {
        CustomData_copy_data(&me->ldata, &me->ldata, a, b, 1);
      }
      new_idx[a] = b;
      b++;
    }
    else {
      /* XXX Theoretically, we should be able to not do this, as no remaining poly
       *     should use any stripped loop. But for security's sake... */
      new_idx[a] = -a;
    }
  }
  if (a != b) {
    CustomData_free_elem(&me->ldata, b, a - b);
    me->totloop = b;
  }

  poly_offsets[me->totpoly] = me->totloop;

  /* And now, update polys' start loop index. */
  /* NOTE: At this point, there should never be any poly using a striped loop! */
  for (const int i : blender::IndexRange(me->totpoly)) {
    poly_offsets[i] = new_idx[poly_offsets[i]];
    BLI_assert(poly_offsets[i] >= 0);
  }

  MEM_freeN(new_idx);
}

void mesh_strip_edges(Mesh *me)
{
  MEdge *e;
  int a, b;
  uint *new_idx = (uint *)MEM_mallocN(sizeof(int) * me->totedge, __func__);
  MutableSpan<MEdge> edges = me->edges_for_write();

  for (a = b = 0, e = edges.data(); a < me->totedge; a++, e++) {
    if (e->v1 != e->v2) {
      if (a != b) {
        memcpy(&edges[b], e, sizeof(edges[b]));
        CustomData_copy_data(&me->edata, &me->edata, a, b, 1);
      }
      new_idx[a] = b;
      b++;
    }
    else {
      new_idx[a] = INVALID_LOOP_EDGE_MARKER;
    }
  }
  if (a != b) {
    CustomData_free_elem(&me->edata, b, a - b);
    me->totedge = b;
  }

  /* And now, update loops' edge indices. */
  /* XXX We hope no loop was pointing to a striped edge!
   *     Else, its e will be set to INVALID_LOOP_EDGE_MARKER :/ */
  MutableSpan<int> corner_edges = me->corner_edges_for_write();
  for (const int i : corner_edges.index_range()) {
    corner_edges[i] = new_idx[corner_edges[i]];
  }

  MEM_freeN(new_idx);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh Edge Calculation
 * \{ */

void BKE_mesh_calc_edges_tessface(Mesh *mesh)
{
  const int numFaces = mesh->totface;
  EdgeSet *eh = BLI_edgeset_new_ex(__func__, BLI_EDGEHASH_SIZE_GUESS_FROM_POLYS(numFaces));
  MFace *mfaces = (MFace *)CustomData_get_layer_for_write(&mesh->fdata, CD_MFACE, mesh->totface);

  MFace *mf = mfaces;
  for (int i = 0; i < numFaces; i++, mf++) {
    BLI_edgeset_add(eh, mf->v1, mf->v2);
    BLI_edgeset_add(eh, mf->v2, mf->v3);

    if (mf->v4) {
      BLI_edgeset_add(eh, mf->v3, mf->v4);
      BLI_edgeset_add(eh, mf->v4, mf->v1);
    }
    else {
      BLI_edgeset_add(eh, mf->v3, mf->v1);
    }
  }

  const int numEdges = BLI_edgeset_len(eh);

  /* write new edges into a temporary CustomData */
  CustomData edgeData;
  CustomData_reset(&edgeData);
  CustomData_add_layer(&edgeData, CD_MEDGE, CD_SET_DEFAULT, numEdges);
  CustomData_add_layer(&edgeData, CD_ORIGINDEX, CD_SET_DEFAULT, numEdges);

  MEdge *ege = (MEdge *)CustomData_get_layer_for_write(&edgeData, CD_MEDGE, mesh->totedge);
  int *index = (int *)CustomData_get_layer_for_write(&edgeData, CD_ORIGINDEX, mesh->totedge);

  EdgeSetIterator *ehi = BLI_edgesetIterator_new(eh);
  for (int i = 0; BLI_edgesetIterator_isDone(ehi) == false;
       BLI_edgesetIterator_step(ehi), i++, ege++, index++) {
    BLI_edgesetIterator_getKey(ehi, &ege->v1, &ege->v2);
    *index = ORIGINDEX_NONE;
  }
  BLI_edgesetIterator_free(ehi);

  /* free old CustomData and assign new one */
  CustomData_free(&mesh->edata, mesh->totedge);
  mesh->edata = edgeData;
  mesh->totedge = numEdges;

  BLI_edgeset_free(eh);
}

/** \} */
