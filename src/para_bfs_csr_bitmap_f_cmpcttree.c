/* ------------------------------------------------------------------------ *
 * This is part of NETALX.
 *
 * Originally written by Yuichiro Yasui
 * This version considerably modified by The GraphCREST Project
 *
 * Copyright (C) 2011-2013 Yuichiro Yasui
 * Copyright (C) 2013-2015 The GraphCREST Project, Tokyo Institute of Technology
 *
 * NETALX is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * ------------------------------------------------------------------------ */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#endif

#define _FILE_OFFSET_BITS 64


#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "generation.h"
#include "construction.h"
#include "para_bfs_csr.h"
#include "validation.h"
#include "statistics.h"
#include "dump.h"

#define PREFIX_BFS "[BFS]"
#define PREFIX "# "
#define PREFIX_TEPS "@ "


double current_seconds    = 0.0;
double current_trav_edges = 0.0;

const char *version(void) {
  return VERSION " (single,bitmap,offload,nntree)";
}

static struct bfs_t *allocate_bfs_data(struct graph_t *G);
static void prefetching_bfs_variables(struct graph_t *G, struct bfs_t *BFS);
static I64_t make_local_bfs_tree(struct graph_t *G, struct bfs_t *BFS, I64_t s, I64_t *thresholds,
                                 struct dumpfiles_t *DF_FG_s, struct dumpfiles_t *DF_FG_e,
                                 struct dump_buffer_t *BF);


struct bfs_info_t {
  long bfsid;			/* BFS ID */
  long root;			/* BFS root NodeID */
  double TEPS;		/* BFS time */
};

int teps64cmp(const void *a, const void *b) {
  const double _a = ((struct bfs_info_t *)a)->TEPS;
  const double _b = ((struct bfs_info_t *)b)->TEPS;
  if (_a > _b) return  1;
  if (_b > _a) return -1;
  /* if (_a == _b) return  0; */
  return 0;
}


struct stat_t *parallel_breadth_first_search(struct graph_t *G, struct edgelist_t *list)
{
  double elapsed_time = get_seconds();
  struct stat_t *stat = NULL;
  assert( stat = (struct stat_t *)calloc(list->numsrcs, sizeof(struct stat_t)) );
  struct bfs_t *BFS = NULL;
  assert( BFS = (struct bfs_t *)allocate_bfs_data(G) );

  int k;
  I64_t trav_edges = -1;
  I64_t thresholds[3] = { ALPHA_param, BETA_param, -1 };

  /* allocate and init file discripter */
  struct dumpfiles_t *DF_s = init_dumpfile_info_graph("", 1, 0);
  struct dumpfiles_t *DF_e = init_dumpfile_info_graph("", 0, 0);
  open_files_readmode(DF_s);
  open_files_readmode(DF_e);

  /* allocate buffer for read BG */
  struct dump_buffer_t *BF = alloc_dump_buffer(DF_e->num_files);


  /* energy_loop */
  if ( enable_energy_loop ) {
    printf("entering energy loop ...\n");
    double time_limit = getenvf((char *)"ENERGY_LOOP_LIMIT", 10.0); /* default: 10 seconds */
    struct ivtimer_t tm;
    I64_t num_sets = 0;
    int tmp_verbose = verbose;
    verbose = 0;
    tm_start(&tm);
    while ( tm_interval(&tm, 1000.0, time_limit * 1000.0) >= 0 ) {
      for (k = 0; k < list->numsrcs; ++k) {
        prefetching_bfs_variables(G, BFS);
        printf("[%03lld-%02d] ", num_sets, k+1);
        make_local_bfs_tree(G, BFS, list->srcs[k], thresholds, DF_s, DF_e, BF);
        if ( stat[k].bfs_time == 0.0 ||
            stat[k].trav_edges / stat[k].bfs_time < current_trav_edges / current_seconds ) {
          stat[k].bfs_time = current_seconds;
          stat[k].trav_edges = current_trav_edges;
        }
        printf("[tentative TEPS %e E/s] %.1fs\n",
               stat[k].trav_edges/stat[k].bfs_time, tm_elapsed(&tm) * 1e-3);
      }
      ++num_sets;
    }
    tm_stop(&tm);
    verbose = tmp_verbose;
    printf("done. (%.2f seconds)\n", tm_time(&tm) * 1e-3);

    output_graph500_results(stat, SCALE, 1ULL<<SCALE, edgefactor,
                            kron_param_A, kron_param_B, kron_param_C, kron_param_D,
                            generation_time, construction_time, list->numsrcs);
  }


  /* parameter tuning */
  if (use_parameter_tuning) {
    verbose     = 0;
    statVERBOSE = 0;
    valiVERBOSE = 0;
    long *trav = NULL;
    struct bfs_info_t *bfs_info;
    assert( trav = (long *)calloc(list->numsrcs, sizeof(long)) );
    memset( trav, 0xff, list->numsrcs * sizeof(long) );
    assert( bfs_info = (struct bfs_info_t *)calloc(list->numsrcs, sizeof(struct bfs_info_t)) );

    for (int i = alpha_start; i <= alpha_end; ++i) {
      for (int j = beta_start; j <= beta_end; ++j) {

        const long alpha = 1UL << i;
        const long beta  = 1UL << j;
        struct stat_t *tuning = NULL;
        assert( tuning = (struct stat_t *)calloc(list->numsrcs, sizeof(struct stat_t)) );
        for (k = 0; k < list->numsrcs; ++k) {

          // -- drop pagacache !!!CORE MAJOR ONLY -- //
          for (int id = 0; id < G->num_graphs; id++) {
            drop_pagecache_file(DF_s->file_info_list[id].fname);
            drop_pagecache_file(DF_e->file_info_list[id].fname);
          }

          prefetching_bfs_variables(G, BFS);
          tuning[k].bfs_time = get_seconds();
          I64_t thres[3] = { alpha, beta, -1 };
          make_local_bfs_tree(G, BFS, list->srcs[k], thres, DF_s, DF_e, BF);
          tuning[k].bfs_time = get_seconds() - tuning[k].bfs_time;
          if (trav[k] < 0) {
            trav[k] = validate_bfs_tree(G, BFS, list, list->srcs[k]);
          }
          tuning[k].trav_edges = trav[k];

          bfs_info[k].bfsid = k;
          bfs_info[k].root  = list->srcs[k];
          bfs_info[k].TEPS  = 1.0 * trav[k] / tuning[k].bfs_time;
        }
        output_graph500_results(tuning, SCALE, 1ULL<<SCALE, edgefactor,
                                kron_param_A, kron_param_B, kron_param_C, kron_param_D,
                                generation_time, construction_time, list->numsrcs);
        printf("%s  SCALE=%d  edgefactor=%d  alpha=%ld  beta=%ld  np=%d  "
               "%.9e  %.9e  %.9e  %.9e  %.9e\n",
               use_RMAT_generator ? "R-MAT":"Kronecker",
               SCALE, edgefactor, alpha, beta,
               get_numa_num_threads(),
               minTEPS, fqTEPS, medianTEPS, tqTEPS, maxTEPS);
#if 0
        qsort(bfs_info, list->numsrcs, sizeof(struct bfs_info_t), teps64cmp);
        printf("@ ");
        for (k = 0; k < list->numsrcs; ++k) {
          printf("%ld ", bfs_info[k].bfsid);
        }
        printf("\n");
#endif
        free(tuning);
      }
    }
    free(trav);
    free(bfs_info);
    exit(1);
  }


  /* main BFS loop */
  for (k = 0; k < list->numsrcs; ++k) {
    /* prefetching */
    double tp = get_seconds();
    printf("prefetching ...");
    prefetching_bfs_variables(G, BFS);
    tp = get_seconds() - tp;
    printf(" done (%.2f seconds)\n", tp);

    /* breadth-first search */
    printf("making BFS tree with source %lld on G(n=%lld,m=%lld)"
           " by Hybrid BFS algorithm (alpha=%lld, beta=%lld) ...\n",
           list->srcs[k], G->n, G->m, thresholds[0], thresholds[1]);
    stat[k].bfs_time = get_seconds();
    I64_t lv = make_local_bfs_tree(G, BFS, list->srcs[k], thresholds, DF_s, DF_e, BF);
    stat[k].bfs_time = get_seconds() - stat[k].bfs_time;

    /* validdate hops */
    if (lv < 0) {
      printf("failed make_local_bfs_tree(%d: s=%lld)\n", k, list->srcs[k]+1);
      exit(1);
    }

    /* verification */
    printf("validating BFS tree ...");
    double tv = get_seconds();
    if (trav_edges < 0 || !SKIP_EVERY_VALIDATION) {
      trav_edges = validate_bfs_tree(G, BFS, list, list->srcs[k]);
      stat[k].trav_edges = trav_edges;
    } else {
      stat[k].trav_edges = trav_edges;
    }
    if (stat[k].trav_edges < 0) {
      fprintf(stderr, "[error] failed verification:"
              " bfs(%d:%lld) trav:%ld\n", k, list->srcs[k], (long)stat[k].trav_edges);
      exit(1);
    }
    tv = get_seconds() - tv;
    printf(" done (%.2f seconds)\n", tv);

    /* iteration result */
    double t = get_seconds() - elapsed_time;
    const int log10_n = intlog(10,G->n+1);
    const int log10_m = intlog(10,G->m);
    const int log10_numsrcs = intlog(10,list->numsrcs+1);
    printf(PREFIX_TEPS "[%.2fs] %0*d/%0*lld"
           " bfs(s=%*lld), %lld hops, %.3fs, %*ld edges, %e E/s\n\n",
           t,
           log10_numsrcs, k+1,
           log10_numsrcs, list->numsrcs,

           log10_n, list->srcs[k]+1,
           lv,
           stat[k].bfs_time,
           log10_m, (long)stat[k].trav_edges,
           (double)stat[k].trav_edges/stat[k].bfs_time);
  }

  /* free */
  for (k = 0; k < BFS->num_locals; ++k) {
    lfree(BFS->pool[k]);
  }
  //if (is_nonnuma_tree) free(BFS->bfs_local[0].tree);
  free(BFS->bfs_local);
  free(BFS->pool);
  free(BFS);

  /* close files */
  close_files(DF_s);
  close_files(DF_e);
  free_files(DF_s);
  free_files(DF_e);

  return stat;
}




/* ------------------------------------------------------------ *
 * allocate BFS data
 * ------------------------------------------------------------ */
static struct bfs_t *allocate_bfs_data(struct graph_t *G) {
  struct bfs_t *BFS = NULL;
  assert( BFS = (struct bfs_t  *)calloc(1, sizeof(struct bfs_t)) );
  BFS->num_locals = G->num_graphs;
  assert( BFS->bfs_local
         = (struct bfs_local_t *)calloc(BFS->num_locals+1, sizeof(struct bfs_local_t)) );
  assert( BFS->pool = (struct mempool_t *)calloc(BFS->num_locals+1, sizeof(struct mempool_t)) );

  const size_t spacing = 64;

  double t1, t2;
  t1 = get_seconds();
  int k;
  for (k = 0; k < G->num_graphs; ++k) {
    const I64_t n = G->n, bit_n = BIT_i(n);
    const I64_t range = G->BG_list[k].n, bit_range = BIT_i(range);
    size_t sz = 0;
    sz += (      (bit_range+1) * sizeof(UL_t)  + spacing);          /* visited   */
    sz += (      (bit_range+1) * sizeof(UL_t)  + spacing);          /* neighbors */
    sz += (          (bit_n+1) * sizeof(UL_t)  + spacing);          /* frontier  */
    sz += (          (range+1) * sizeof(I64_t) + spacing);          /* bfs-tree  */
    if (!is_dump_hops)    {
      sz += ((n+1) * sizeof(int) + spacing);                        /* hops      */
    }
    sz  = ROUNDUP( sz, hugepage_size() );
    BFS->pool[k] = lmalloc(sz, k);
    printf("[node%02d] BFS_tmp[%d] (%p) %.2f GB using mmap with mbind(MPOL_BIND:%8s)\n",
           k, k, BFS->pool[k].pool, (double)(BFS->pool[k].memsize) / (1ULL<<30),
           &num2bit((1ULL << k), 0)[56]);
  }
  force_pool_page_faults(BFS->pool);
  t2 = get_seconds();
  printf("numa node local allocation takes %.3f seconds\n", t2-t1);


  /* forward (start,end) / backward (start,end), tree, queue */
  for (k = 0; k < BFS->num_locals; ++k) {
    struct mempool_t *pool = &BFS->pool[k];
    struct bfs_local_t *LBFS = &BFS->bfs_local[k];
    LBFS->n                = G->n;
    LBFS->bit_n            = BIT_i(LBFS->n);
    LBFS->range            = G->BG_list[k].n;
    LBFS->bit_range        = BIT_i(LBFS->range);
    LBFS->offset           = G->BG_list[k].offset;
    LBFS->bit_offset       = BIT_i(LBFS->offset);

    size_t off = 0;
    LBFS->visited   = (UL_t *)&pool->pool[off];  off += (LBFS->bit_range+1) * sizeof(UL_t)  + spacing;
    LBFS->neighbors = (UL_t *)&pool->pool[off];  off += (LBFS->bit_range+1) * sizeof(UL_t)  + spacing;
    LBFS->frontier  = (UL_t *)&pool->pool[off];  off += (LBFS->bit_n+1)     * sizeof(UL_t)  + spacing;
    LBFS->tree      = (I64_t *)&pool->pool[off]; off += (LBFS->range+1)     * sizeof(I64_t) + spacing;
    if (!is_dump_hops) {
      LBFS->hops = (int *)&pool->pool[off]; off += (LBFS->n+1) * sizeof(int)   + spacing;
    }
    assert( ROUNDUP( off, hugepage_size() ) == pool->memsize );
  }

  return BFS;
}



/* ------------------------------------------------------------ *
 * prefetching_bfs_variables
 * ------------------------------------------------------------ */
static void prefetching_bfs_variables(struct graph_t *G, struct bfs_t *BFS) {
  OMP("omp parallel num_threads(get_numa_num_threads())") {
    int id = omp_get_thread_num();
    int nodeid = get_numa_nodeid(id);
    int coreid = get_numa_vircoreid(id);
    int lcores = get_numa_online_cores(nodeid);
    pinned(USE_HYBRID_AFFINITY, id);
    OMP("omp barrier");

    I64_t j, ls, le;
    struct bfs_local_t *LBFS = &BFS->bfs_local[nodeid];

    /* G is not used. */
    G = G;

    UL_t *frontier = LBFS->frontier;
    partial_range(LBFS->n, 0, lcores, coreid, &ls, &le);
    ls = BIT_i(ls);
    le = BIT_i(le);
    for (j = ls; j < le; ++j) {
      frontier[j] = 0;
    }

    UL_t *neighbors = LBFS->neighbors;
    partial_range(LBFS->range, 0, lcores, coreid, &ls, &le);
    /* partial_range(LBFS->range, LBFS->offset, lcores, coreid, &ls, &le); */
    ls = BIT_i(ls);
    le = BIT_i(le);
    for (j = ls; j < le; ++j) {
      neighbors[j] = 0;
    }

    UL_t *visited = LBFS->visited;
    partial_range(LBFS->range, 0, lcores, coreid, &ls, &le);
    /* partial_range(LBFS->range, LBFS->offset, lcores, coreid, &ls, &le); */
    ls = BIT_i(ls);
    le = BIT_i(le);
    for (j = ls; j < le; ++j) {
      visited[j] = 0;
    }
#if 0
    // set zero-degree nodes's visited flag.
    OMP("omp barrier");
    partial_range(LBFS->range, 0, lcores, coreid, &ls, &le);
    struct subgraph_t  *BG   = &G->BG_list[nodeid];
    const I64_t *BG_start    = &BG->start[0];
    for (j = ls; j < le; ++j) {
      if ((BG_start[j+1] - BG_start[j]) == 0)
        SET_BITMAP(visited, j);
    }
#endif
    I64_t *tree = LBFS->tree;
    partial_range(LBFS->range, 0, lcores, coreid, &ls, &le);
    //partial_range(LBFS->range, LBFS->offset, lcores, coreid, &ls, &le);
    for (j = ls; j < le; ++j) {
      tree[j] = -1;
    }

    clear_affinity();
  }
}



/* ------------------------------------------------------------ *
 * parallel BFS
 * ------------------------------------------------------------ */
static inline int get_bit_offset(UL_t *bits) {
  int i = __builtin_ctzll(*bits)+1;
  *bits >>= i-1;
  *bits >>= 1;
  return i;
}


enum {
  ALGO_TOPDOWN, ALGO_BOTTOMUP,
};

struct hist_t {
  int algorithm;
  long frontier_nodes;
  long NQ_size[MAX_NODES];
  long scanned_edges;
  long scanned_edges_onmem;
  long scanned_edges_exmem;
#if PROFILE_DETAIL == 1
  long scanned_vertex_onmem;
  long scanned_vertex_exmem;
#endif
  long pred_topdown_edges;
  long pred_bottomup_edges;
  int flag;
  double elapsed_time;
  double merge_time;
};

static I64_t make_local_bfs_tree(struct graph_t *G, struct bfs_t *BFS, I64_t s, I64_t *thresholds,
                                 struct dumpfiles_t *DF_BG_s, struct dumpfiles_t *DF_BG_e, struct dump_buffer_t *BF) {
  double elapsed_offset = get_seconds();
#if PROFILE == 1
#define MAX_HISTS 256
  struct hist_t hist[MAX_HISTS];
  memset(hist, 0x00, sizeof(struct hist_t) * MAX_HISTS);

  I64_t total_scanned_edges_onmem;
  I64_t total_scanned_edges_exmem;

 #if PROFILE_DETAIL == 1
    I64_t total_scanned_vertex_onmem;
    I64_t total_scanned_vertex_exmem;
  #endif
#endif

  I64_t hops = -1;
  I64_t master_queue_count    = 1; /* for shared queue size */
  I64_t shared_topdown_edges  = 0; /* for parameter estimating */

  assert( BIT_j(G->n) == 0 );

  OMP("omp parallel num_threads(get_numa_num_threads())") {
    int id = omp_get_thread_num();
    int nodeid = get_numa_nodeid(id);
    int coreid = get_numa_vircoreid(id);
    int lcores = get_numa_online_cores(nodeid);
    pinned(USE_HYBRID_AFFINITY, id);
    struct subgraph_t  *BG   = &G->BG_list[nodeid];
    struct bfs_local_t *LBFS = &BFS->bfs_local[nodeid];
    const int    num_graphs  =  G->num_graphs;
    const I64_t  log_c       =  log2( G->chunk );
    const I64_t  n           =  G->n;
    const I64_t  offset      =  LBFS->offset;
    const I64_t  range       =  LBFS->range;
    const I64_t  bit_offset  =  LBFS->bit_offset;
    const I64_t *BG_start    = &BG->start[0-offset];
    const I64_t *BG_end      =  BG->end;
    I64_t fs, fe;
    I64_t bs, be;
    I64_t *tree              = &LBFS->tree[0 - offset];
    UL_t  *visited           = &LBFS->visited[0 - bit_offset];
    UL_t  *frontier          =  LBFS->frontier;
    UL_t  *neighbors         = &LBFS->neighbors[0 - bit_offset];

    int algo = ALGO_TOPDOWN;
    I64_t ptop_edges = 0;
#ifdef BOTTOMUP_ONLY
    algo = ALGO_BOTTOMUP;
#endif
    const I64_t alpha_param = thresholds[0], beta_param = thresholds[1];

    I64_t i, j, ls, le;
    I64_t start = 0, end = 1, level = 0;
    I64_t thread_queue_count = 0;
    I64_t range_ls, range_le, bit_range_ls, bit_range_le, bit_n_ls, bit_n_le;

    /* file discripter for graph */
    int fd_start = DF_BG_s->file_info_list[id].fd;
    int fd_end   = DF_BG_e->file_info_list[id].fd;

    /* for buffered I/O */
    I64_t rm_e, read_length_e;
    I64_t *read_buf_e = BF->buffer_list[id].buf; // NUMA optimized buffer
    const I64_t buf_read_length_TD = MIN((I64_t)BF->buffer_list[id].length, (I64_t)DUMP_BUF_READ_LENGTH_TD);
    const I64_t buf_read_length_BU = MIN((I64_t)BF->buffer_list[id].length, (I64_t)DUMP_BUF_READ_LENGTH_BU);
    I64_t start_buf[2];


    /* for profile */
#if PROFILE == 1
    I64_t scanned_edges_onmem;
    I64_t scanned_edges_exmem;
  #if PROFILE_DETAIL == 1
    I64_t scanned_vertex_onmem;
    I64_t scanned_vertex_exmem;
  #endif
#endif
    I64_t frontier_size = 1;

    partial_range(LBFS->n, 0, lcores, coreid, &bit_n_ls, &bit_n_le);
    partial_range(range, LBFS->offset, lcores, coreid, &range_ls, &range_le);
    // BIT_i :  exm.) 0 ~ 63 -> 0,  64 ~ 127 -> 1
    bit_range_ls = BIT_i(range_ls);
    bit_range_le = BIT_i(range_le);
    bit_n_ls = BIT_i(bit_n_ls);
    bit_n_le = BIT_i(bit_n_le);

    for (i = bit_n_ls; i < bit_n_le; ++i) {
      frontier[i] = 0;
    }

    /* [TODO] neighbor[] and visited[] are packed by 'struct node_t'. */
    for (i = bit_range_ls; i < bit_range_le; ++i) {
      neighbors[i] = 0;
    }
    for (i = bit_range_ls; i < bit_range_le; ++i) {
      visited[i] = 0;
    }
    for (i = range_ls; i < range_le; ++i) {
      tree[i] = -1;
    }

    OMP("omp barrier");

    if (coreid == 0) {
      SET_BITMAP(frontier, s);
    }
    if (range_ls <= s && s < range_le) {
      SET_BITMAP(visited, s);
      tree[s] = s;
    }
    OMP("omp barrier");


    /* -------------------- */
    /* BFS */
    /* -------------------- */
    for (level = 0; end != start; ++level) { /* bfs loop */
#if PROFILE == 1
      scanned_edges_onmem = 0;
      scanned_edges_exmem = 0;
      if (id == 0) {
        total_scanned_edges_onmem  = 0;
        total_scanned_edges_exmem  = 0;
        hist[level].algorithm      = algo;
        hist[level].frontier_nodes = end - start;
        hist[level].elapsed_time   = get_seconds();
      }
  #if PROFILE_DETAIL == 1
      scanned_vertex_onmem = 0;
      scanned_vertex_exmem = 0;
      if (id == 0) {
        total_scanned_vertex_onmem = 0;
        total_scanned_vertex_exmem = 0;
      }
  #endif


#if 0
  // -- clear pagecache at every level --- //
      if (coreid == 0) {

        const double ts = get_seconds();

        drop_pagecache_file(DF_BG_s->file_info_list[id].fname);
        drop_pagecache_file(DF_BG_e->file_info_list[id].fname);

        const double droping_time = get_seconds() - ts;
        printf("drop pagecache takes %lf seconds\n", droping_time);

      }
      OMP("omp barrier");
#endif


#endif


      if ( algo == ALGO_TOPDOWN ) {

        /*------------------------------------
           Top-Down Step for small frontier
         -------------------------------------*/

        if (id == 0) shared_topdown_edges = 0;
        ptop_edges = 0;
        for (i = bit_range_ls; i < bit_range_le; ++i) {
          neighbors[i] = 0;
        }
        OMP("omp barrier");

        ls = bit_range_ls;
        le = bit_range_le;
        /* ls = bit_n_ls; */
        /* le = bit_n_le; */
        for (i = ls; i < le; ++i) {
          UL_t fron = frontier[i];
          I64_t v = BIT_v(i,0);
          I64_t k = -1;

          while (fron != 0) {
            k += get_bit_offset(&fron);

            /* ------------------------------- read from memory ------------------------------- */
            fs = BG_start[v+k];
            fe = BG_start[v+k+1];

#if PROFILE == 1
            scanned_edges_onmem += fe - fs;
           #if PROFILE_DETAIL == 1
            ++scanned_vertex_onmem;
           #endif
#endif
            for (j = fs; j < fe; ++j) {
              const I64_t w = BG_end[j];
              const int u = (int)( w >> log_c );

              const I64_t target_bit_offset = BFS->bfs_local[ u ].bit_offset;
              UL_t  *target_visited = &( BFS->bfs_local[ u ].visited[0 - target_bit_offset] );
              if ( ! ISSET_BITMAP(target_visited,w) ) {
                if ( ! IS_TEST_AND_SET_BITMAP(target_visited,w) ) {
                  //const I64_t *target_BG_start = &( G->BG_list[u].start[0 - target_offset] );
                  UL_t *target_neighbors = &( BFS->bfs_local[ u ].neighbors[0 - target_bit_offset] );

                  ptop_edges += 16;
                  const I64_t target_offset = BFS->bfs_local[ u ].offset;
                  I64_t *target_tree = &BFS->bfs_local[ u ].tree[0 - target_offset];
                  target_tree[w] = v+k;
                  TEST_AND_SET_BITMAP(target_neighbors, w);
                  ++thread_queue_count;
                }
              }
            }

            /* ------------------------------- read from external memory ------------------------------- */
            if (fe-fs < max_onmem_edges) {
              continue ;  // go to next vertex
            }

            // -- read csr-index data from file -- //
            lseek(fd_start, sizeof(I64_t)*(v+k-offset), SEEK_SET);
            read(fd_start, start_buf, sizeof(I64_t)*2);
            fs = start_buf[0] + (fe-fs);
            fe = start_buf[1];

#if PROFILE == 1
            scanned_edges_exmem += fe - fs;
          #if PROFILE_DETAIL == 1
            ++scanned_vertex_exmem;
          #endif
#endif

            lseek(fd_end, sizeof(I64_t)*fs, SEEK_SET);
            rm_e = fe - fs; // fe - fs is degree of vertex; sequential area in file

            // -- search unvisited vertecies -- //
            while(rm_e > 0) {

              // -- buffered read for edges -- //
              read_length_e = MIN(rm_e, buf_read_length_TD);
              rm_e -= read_length_e;
              read(fd_end, read_buf_e, sizeof(I64_t)*read_length_e);


              for (j = 0; j < read_length_e; ++j) {
                const I64_t w = read_buf_e[j];
                const int u = (int)( w >> log_c );

                const I64_t target_bit_offset = BFS->bfs_local[ u ].bit_offset;
                UL_t  *target_visited = &( BFS->bfs_local[ u ].visited[0 - target_bit_offset] );

                if ( ! ISSET_BITMAP(target_visited, w) ) { // w is unvisited
                  if ( ! IS_TEST_AND_SET_BITMAP(target_visited, w) ) { // visit w
                    UL_t *target_neighbors = &( BFS->bfs_local[ u ].neighbors[0 - target_bit_offset] );

                    ptop_edges += 16;
                    const I64_t target_offset = BFS->bfs_local[ u ].offset;
                    I64_t *target_tree = &BFS->bfs_local[ u ].tree[0 - target_offset];
                    target_tree[w] = v+k;

                    TEST_AND_SET_BITMAP(target_neighbors, w);
                    ++thread_queue_count;

                  }
                }
              }

            }
          }
        }


      } else {


        /*------------------------------------
           Bottom-Up Step for large frontier
         -------------------------------------*/

        ls = bit_range_ls;
        le = bit_range_le;

        for (i = ls; i < le; ++i) {
          UL_t neighbors_i = 0;
          ///UL_t zero_i = 0;
          UL_t vst = ~( visited[i] );
          I64_t w = BIT_v(i,0);
          I64_t k = -1;

          while (vst != 0) {
            k += get_bit_offset(&vst);

            /* -------------------- read from memory -------------------- */
            bs = BG_start[w+k];
            be = BG_start[w+k+1];
#if PROFILE == 1 && PROFILE_DETAIL == 1
          ++scanned_vertex_onmem;
#endif

            for (j = bs; j < be; ++j) {
              I64_t tail = BG_end[j];
#if PROFILE == 1
              ++scanned_edges_onmem;
#endif
              if ( ISSET_BITMAP(frontier, tail) ) {
                tree[w+k] = tail;
                neighbors_i |= 1ULL << k;
                ++thread_queue_count;
                goto next_vertex_btm;
              }
            }



            /* ---------------------- read from external memory --------------------- */
             if (be-bs < max_onmem_edges) {
              goto next_vertex_btm;  // go to next vertex
            }

            // -- read csr-index data from file -- //
            lseek(fd_start, sizeof(I64_t)*(w+k-offset), SEEK_SET);
            read(fd_start, start_buf, sizeof(I64_t)*2);
            bs = start_buf[0] + (be-bs);
            be = start_buf[1];

#if PROFILE_DETAIL == 1 && PROFILE_DETAIL == 1
            ++scanned_vertex_exmem;
#endif

            lseek(fd_end, sizeof(I64_t)*bs, SEEK_SET);
            rm_e = be - bs; // be - bs is degree of vertex; sequential area in file

            // -- search fronter -- //
            while(rm_e > 0) {

              // -- buffered read edges -- //
              read_length_e = MIN(rm_e, buf_read_length_BU);
              rm_e -= read_length_e;
              read(fd_end, read_buf_e, sizeof(I64_t)*read_length_e);


              for (j = 0; j < read_length_e; ++j) {
                I64_t tail = read_buf_e[j];
#if PROFILE == 1
                ++scanned_edges_exmem;
#endif
                if ( ISSET_BITMAP(frontier, tail) ) { // frontier is found
                  tree[w+k] = tail;
                  neighbors_i |= 1ULL << k;
                  ++thread_queue_count;
                  goto next_vertex_btm; // goto next vertex
                }
              }
            } // end of reading BG from file

next_vertex_btm: ;

          }

          visited[i] |= neighbors_i;
          neighbors[i] = neighbors_i;

        } // end of bottom-up approarch

      }


      /* merge queue */
      __sync_fetch_and_add(&shared_topdown_edges, ptop_edges);
#if PROFILE == 1
      __sync_fetch_and_add(&total_scanned_edges_onmem, scanned_edges_onmem);
      __sync_fetch_and_add(&total_scanned_edges_exmem, scanned_edges_exmem);
    #if PROFILE_DETAIL == 1
      __sync_fetch_and_add(&total_scanned_vertex_onmem, scanned_vertex_onmem);
      __sync_fetch_and_add(&total_scanned_vertex_exmem, scanned_vertex_exmem);
    #endif
#endif
      __sync_fetch_and_add(&master_queue_count, thread_queue_count);
      OMP("omp barrier");	/* node-local-barrier */
      I64_t neighbor_size = master_queue_count - end;

      /* ------------------------------ switching algorithm ------------------------------ */
#define EXACT   0x01
#define GROWING 0x02
      int flag = 0;
      I64_t topdown_edges = 0;
      I64_t bottomup_edges = /* visited_nodes + */ (n - end) * edgefactor + frontier_size;
      if ( frontier_size < neighbor_size ) {
        if ( algo == ALGO_TOPDOWN ) {
          /* exact neighbor_edges */
          topdown_edges = shared_topdown_edges;
          algo = topdown_edges * alpha_param < bottomup_edges ? ALGO_TOPDOWN : ALGO_BOTTOMUP;
          flag = EXACT | GROWING;
        } else {
          /* approx. neighbor_edges */
          topdown_edges = frontier_size * edgefactor;
          algo = ALGO_BOTTOMUP;
          flag = GROWING;
        }
      } else {
        topdown_edges = frontier_size * edgefactor;
        algo = topdown_edges * beta_param < bottomup_edges ? ALGO_TOPDOWN : ALGO_BOTTOMUP;
        /* algo = frontier_size * beta_param < (n-end) ? ALGO_TOPDOWN : ALGO_BOTTOMUP; */
        flag = 0;
      }
#ifdef BOTTOMUP_ONLY
      algo = ALGO_BOTTOMUP;
#endif
      /* OMP("omp barrier"); */
      /* ------------------------------ switching algorithm ------------------------------ */

#if PROFILE == 1
      if (id == 0) {
        hist[level].merge_time    = get_seconds();
        hist[level].scanned_edges_onmem = total_scanned_edges_onmem;
        hist[level].scanned_edges_exmem = total_scanned_edges_exmem;
        hist[level].scanned_edges = total_scanned_edges_onmem + total_scanned_edges_exmem;
        #if PROFILE_DETAIL == 1
          hist[level].scanned_vertex_onmem = total_scanned_vertex_onmem;
          hist[level].scanned_vertex_exmem = total_scanned_vertex_exmem;
        #endif
      }
#endif

      /* ------------------------------ swap(CQ,NQ) ------------------------------ */
      for (i = bit_n_ls; i < bit_n_le; ++i) {
        frontier[i] = 0;
      }
      OMP("omp barrier");
      I64_t k;
      for (k = 0; k < num_graphs; ++k) {
        UL_t *target = BFS->bfs_local[ (nodeid+k+1) % num_graphs ].frontier;
        for (i = bit_range_ls; i < bit_range_le; ++i) {
          target[i] |= neighbors[i];
        }
        OMP("omp barrier");
      }
      /* ------------------------------ swap(CQ,NQ) ------------------------------ */

#if PROFILE == 1
      if (coreid == 0) {
        hist[level].NQ_size[nodeid] = master_queue_count-end;
      }
      if (id == 0) {
        hist[level].pred_topdown_edges  = topdown_edges;
        hist[level].pred_bottomup_edges = bottomup_edges;
        hist[level].flag                = flag;
        hist[level].merge_time     = get_seconds() - hist[level].merge_time;
        hist[level].elapsed_time   = get_seconds() - hist[level].elapsed_time;
      }
#endif
      OMP("omp barrier");

      start = end;
      end = master_queue_count;
      thread_queue_count = 0;
      frontier_size = end - start;

      OMP("omp barrier");
    } /* bfs loop */

    if (id == 0) {
      hops = level-1;
    }

    clear_affinity();
  }

#if PROFILE == 1
  elapsed_offset = get_seconds() - elapsed_offset;
  I64_t i;
  long frontier_nodes = 0, scanned_edges = 0;
  long scanned_edges_onmem = 0, scanned_edges_exmem = 0;
  #if PROFILE_DETAIL == 1
    long scanned_vertex_onmem = 0, scanned_vertex_exmem = 0;
  #endif
  double merge_time = 0.0;
  for (i = 0; i <= hops; ++i) {
    frontier_nodes        += hist[i].frontier_nodes;
    scanned_edges         += hist[i].scanned_edges;
    scanned_edges_onmem   += hist[i].scanned_edges_onmem;
    scanned_edges_exmem   += hist[i].scanned_edges_exmem;
#if PROFILE_DETAIL == 1
    scanned_vertex_onmem  += hist[i].scanned_vertex_onmem;
    scanned_vertex_exmem  += hist[i].scanned_vertex_exmem;
#endif
    merge_time            += hist[i].merge_time;
  }
  if ( verbose ) {
    int j;
    printf(PREFIX "%2s  %9s  %8s (%8s)  %11s ( %*s )  "
           "%12s (%5s)  %10s  %14s     %14s   %8s   %14s   %14s",
           "Lv", "algorithm", "time", "merge", "frontier", 9 * G->num_graphs, " ",
           "TE", "%", "TE/|CQ|", "next-TD[E]", "next-BU[E]", "TEPS", "OnMem-TE", "ExMem-TE");
  #if PROFILE_DETAIL == 1
    printf("  %14s  %14s", "OnMem-SCND-VX", "ExMem-SCND-VX");
  #endif
    printf("\n");

    for (i = 0; i <= hops; ++i) {
#define MAX_MSGS (1<<12)
      char *msg = alloca(MAX_MSGS);
      *msg = '\0';
      for (j = 0; j < G->num_graphs; ++j)
        strcatfmt(msg, "%8lld ", hist[i].NQ_size[j]);
        printf(PREFIX "%2lld  %9s  %6.1fms (%6.1fms)  %11ld ( %s )  "
             "%12ld (%5.2f)  %10.3e  %14ld %c%c  %14ld  %5.2fGE/s  %14ld  %14ld",
             i,
             !hist[i].algorithm ? "TopDown":"BottomUp",
             hist[i].elapsed_time * 1e3,
             hist[i].merge_time * 1e3,
             hist[i].frontier_nodes,
             msg,

             hist[i].scanned_edges,
             100.0 * hist[i].scanned_edges / G->m,
             1.0 * hist[i].scanned_edges / hist[i].frontier_nodes,
             hist[i].pred_topdown_edges,
             hist[i].flag & EXACT   ? 'E' : 'A',
             hist[i].flag & GROWING ? 'G' : 'S',
             hist[i].pred_bottomup_edges,
             (double)hist[i].frontier_nodes * edgefactor / hist[i].elapsed_time / 1e9,
             hist[i].scanned_edges_onmem,
             hist[i].scanned_edges_exmem);

      #if PROFILE_DETAIL == 1
        printf("  %14ld  %14ld", hist[i].scanned_vertex_onmem, hist[i].scanned_vertex_exmem);
      #endif
        printf("\n");

    }

    printf(PREFIX "%2s  %9s  %6.1fms (%6.1f %%)  %11ld %*s      %12ld (%5.2f)  %5.2fGE/s  %14ld  %14ld",
           "",
           "Total",
           elapsed_offset * 1e3,
           (double)merge_time / elapsed_offset * 100.0,
           frontier_nodes,
           9 * G->num_graphs, " ",
           scanned_edges,
           (double)scanned_edges * 100 / G->m,
           (double)scanned_edges / elapsed_offset / 1e9,
           scanned_edges_onmem,
           scanned_edges_exmem);
  #if PROFILE_DETAIL == 1
    printf("  %14ld  %14ld", scanned_vertex_onmem, scanned_vertex_exmem);
  #endif
    printf("\n");
  }
#endif
  if ( verbose ) {
    printf("TEPS ratio: %e E/s\n", G->m/2.0/elapsed_offset);
    /* } else { */
    /*   printf("TEPS ratio: %e E/s  ", G->m/2.0/elapsed_offset); */
  }
  current_seconds    = elapsed_offset;
  current_trav_edges = 1.0 * G->m / 2.0;

  return hops;
}

