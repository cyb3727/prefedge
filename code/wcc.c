#include "graph_defs.h"
#include "balloon.h"
#include "prefetcher.h"

typedef struct wcc_metadata_st {
  unsigned long component;
  volatile unsigned long queue_next;
} wcc_metadata_t;

static volatile unsigned long queue_head = ULONG_MAX;
static volatile unsigned long vertex_position = 0;
static wcc_metadata_t *metadata;
static graph_t * volatile graph;

void prefetcher_random_callback(unsigned long *laf,
				unsigned long laf_size,
				unsigned long ift)
{
  unsigned long current_hoq;
  unsigned long entries = 0;
  /* Fill in inner-loop entries from BFS queue */
  current_hoq = queue_head;
  if(current_hoq != ULONG_MAX) {
    current_hoq = metadata[current_hoq].queue_next;
  }
  while(entries != ift && current_hoq != ULONG_MAX) {
    unsigned long page = graph->vertex_map[current_hoq].offset_edges;
    page = page >> (ASSUME_PAGE_SHIFT + 3); /* offset is in bits ! */
    if(laf[HASH_MODULO(page, laf_size)] != page) {
      laf[HASH_MODULO(page, laf_size)] = page;
      entries++;
    }
    current_hoq = metadata[current_hoq].queue_next;
  }
}


unsigned long prefetcher_sequential_callback(unsigned long* aux_offset)
{
  unsigned long offset = graph->vertex_map[vertex_position].offset_edges;
  return offset >> (ASSUME_PAGE_SHIFT + 3);
}

/* returns number of connected components */
static unsigned long wcc(graph_t *graph)
{
  unsigned long i = 0;
  unsigned long current_vertex;
  unsigned long components = 0;
  unsigned long queue_tail = ULONG_MAX;
  for(i = 0; i < graph->vertex_cnt; i++){
    vertex_position = i;
    if(metadata[i].component == 0) {
      components++;
      metadata[i].component = components;
      BFS_PUSH(queue_head, queue_tail, i, metadata);
      while(queue_head != ULONG_MAX) {
	current_vertex = BFS_POP(queue_head, queue_tail, metadata);
	edge_iterator_t iter;
	init_edge_iterator(graph, current_vertex, &iter);
	while(iter_step(graph, &iter) == 0) {
	    unsigned long target = iter.neighbour;
	    if(metadata[target].component == 0) {
	      metadata[target].component = 1;
	      BFS_PUSH(queue_head, queue_tail, target, metadata);
	    }
	}
      }
    }
  }
  return components;
}

int main(int argc, char **argv)
{
  unsigned long time_wcc, time_total, components;
  CLOCK_START(time_total);
  if(argc < 2) {
    fprintf(stderr, "Usage %s graph_name \n", argv[0]);
    exit(-1);
  }
#ifdef PREFETCHER
  bind_master();
  init_prefetcher(prefetcher_random_callback,
		  prefetcher_sequential_callback);
#endif
  graph = open_vertices(argv[1]);
  metadata = (wcc_metadata_t*)
    map_anon_memory(graph->vertex_cnt*sizeof(wcc_metadata_t), "vertex metadata");
  balloon_init();
  balloon_inflate(); /* Simulate semi-em conditions */
  open_cal(graph);
  /* Perhaps mmap /dev/null instead ? */
  memset(metadata, 0, graph->vertex_cnt*sizeof(wcc_metadata_t));
#ifdef PREFETCHER
  launch_prefetch_thread(graph->fd_calist);
#endif
  struct rusage ru_begin;
  getrusage(RUSAGE_SELF, &ru_begin);
  CLOCK_START(time_wcc);
  components = wcc(graph);
  CLOCK_STOP(time_wcc);
  struct rusage ru_end;
  getrusage(RUSAGE_SELF, &ru_end);
#ifdef PREFETCHER
  terminate_prefetch_thread();
  destroy_prefetcher();
#endif
  balloon_deflate();
  munmap(metadata, graph->vertex_cnt*sizeof(wcc_metadata_t));
  close_graph(graph);
  CLOCK_STOP(time_total);
  printf("COMPONENTS %lu\n", components);
  printf("TIME WCC %lu\n", time_wcc);
  printf("TIME TOTAL %lu\n", time_total);
  print_rusage_stats(stdout, &ru_begin, &ru_end);
  return 0;
}
