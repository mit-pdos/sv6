// To build on Linux:
//  g++ -O3 -DLINUX -std=c++0x -Wall -g -I.. -pthread mapbench.cc -o mapbench

#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>

#if defined(LINUX)
#include "include/compiler.h"
#define CACHELINE 64
#define NCPU 256
#include <pthread.h>
#include <stdio.h>
#include "user/util.h"
#include <assert.h>
#include <sys/wait.h>
#include <atomic>
#include "include/xsys.h"
#else
#include "compiler.h"
#include "types.h"
#include "user.h"
#include "amd64.h"
#include "uspinlock.h"
#include "mtrace.h"
#include "pthread.h"
#include "bits.hh"
#include "rnd.hh"
#include "kstats.hh"
#include "xsys.h"
#endif

#define PGSIZE 4096

#define XSTR(x) #x
#define STR(x) XSTR(x)

enum { verbose = 0 };
enum { duration = 5 };
enum { fault = 1 };
enum { pipeline_width = 1 };

enum class bench_mode
{
  LOCAL, PIPELINE, GLOBAL, GLOBAL_FIXED
};

// XXX(Austin) Do this right.  Put these in a proper PMC library.
#if defined(HW_tom)
#define PERF_amd
#elif defined(HW_josmp) || defined(HW_ben)
#define PERF_intel
#endif

// PMCs
enum {
  pmc_llc_misses = 0x2e|(0x41<<8),
#if defined(PERF_intel)
  pmc_l2_cache_misses = 0x24|(0xAA<<8), // L2_RQSTS.MISS
  pmc_l2_prefetch_misses = 0x24|(0x80<<8), // L2_RQSTS.PREFETCH_MISS
  pmc_mem_load_retired_other_core_l2_hit_hitm = 0xcb|(0x08<<8),
  pmc_mem_load_retired_l3_miss = 0xcb|(0x10<<8),
#elif defined(PERF_amd)
  pmc_l2_cache_misses = 0x7e|((0x2|0x8)<<8),
#endif
};

#if !defined(LINUX) && !defined(HW_qemu)
#define RECORD_PMC pmc_l2_cache_misses
#define PMCNO 0
#endif

char * const base = (char*)0x100000000UL;

static int nthread, npg;
static bench_mode mode;

static pthread_barrier_t bar;

static volatile bool stop __mpalign__;
static __padout__ __attribute__((unused));

// For PIPELINE mode
static struct
{
  std::atomic<uint64_t> head __mpalign__;
  std::atomic<uint64_t> tail __mpalign__;
  __padout__;
} channels[NCPU];

// For GLOBAL mode
static struct
{
  std::atomic<uint64_t> round __mpalign__;
  std::atomic<uint64_t> left __mpalign__;
  __padout__;

  void wait()
  {
    uint64_t curround = round;
    if (--left) {
      while (round == curround && !stop)
        ;
    } else {
      left = nthread;
      ++round;
    }
  }
} gbarrier;

static uint64_t start_tscs[NCPU], stop_tscs[NCPU], iters[NCPU], pages[NCPU];
static std::atomic<uint64_t> total_underflows;
#ifdef RECORD_PMC
static uint64_t pmcs[NCPU];
#endif

void*
timer_thread(void *)
{
  sleep(duration);
  stop = true;
  return NULL;
}

#ifdef LINUX
static inline uint64_t
rdpmc(uint32_t ecx)
{
  uint32_t hi, lo;
  __asm volatile("rdpmc" : "=a" (lo), "=d" (hi) : "c" (ecx));
  return ((uint64_t) lo) | (((uint64_t) hi) << 32);
}
#endif

int
xread(int fd, const void *buf, size_t n)
{
  size_t pos = 0;
  while (pos < n) {
    int r = read(fd, (char*)buf + pos, n - pos);
    if (r < 0)
      die("read failed");
    if (r == 0)
      break;
    pos += n;
  }
  return pos;
}

#ifndef XV6_USER
struct kstats
{
  kstats operator-(const kstats &o) {
    return kstats{};
  }
};
#endif

static void
read_kstats(kstats *out)
{
#ifdef XV6_USER
  int fd = open("/dev/kstats", O_RDONLY);
  if (fd < 0)
    die("Couldn't open /dev/kstats");
  int r = xread(fd, out, sizeof *out);
  if (r != sizeof *out)
    die("Short read from /dev/kstats");
  close(fd);
#endif
}

static inline char *
pipeline_get_region(uint64_t channel, uint64_t step)
{
  return base + ((step % pipeline_width) * npg +
                 channel * npg * pipeline_width) * 0x10000000;
}

void*
thr(void *arg)
{
  const uintptr_t cpu = (uintptr_t)arg;

  const uint64_t inchan = cpu;
  const uint64_t outchan = (cpu + 1) % nthread;

  if (setaffinity(cpu) < 0)
    die("setaffinity err");

  pthread_barrier_wait(&bar);

  start_tscs[cpu] = rdtsc();
  uint64_t myiters = 0, mypages = 0, myunderflows = 0;
#ifdef RECORD_PMC
  uint64_t pmc1 = rdpmc(PMCNO);
#endif

  switch (mode) {
  case bench_mode::LOCAL:
    while (!stop) {
      volatile char *p = base + cpu * npg * 0x100000;
      if (mmap((void *) p, npg * PGSIZE, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
        die("%d: map failed", cpu);

      if (fault)
        for (int j = 0; j < npg * PGSIZE; j += PGSIZE)
          p[j] = '\0';

      if (munmap((void *) p, npg * PGSIZE) < 0)
        die("%d: unmap failed\n", cpu);

      ++myiters;
    }
    mypages = myiters * npg;
    break;

  case bench_mode::PIPELINE: {
    while (!stop) {
      bool underflow = true;

      // Fill the outgoing pipeline
      uint64_t target = channels[outchan].tail + pipeline_width;
      for (; channels[outchan].head < target; ++channels[outchan].head) {
        underflow = false;

        volatile char *p = pipeline_get_region(outchan, channels[outchan].head);
        if (mmap((void *) p, npg * PGSIZE, PROT_READ|PROT_WRITE,
                 MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
          die("%d: map failed", cpu);

        if (fault)
          for (int j = 0; j < npg * PGSIZE; j += PGSIZE)
            p[j] = '\0';
      }

      // Consume incoming pipeline
      target = channels[inchan].head;
      for (; channels[inchan].tail < target; ++channels[inchan].tail) {
        underflow = false;

        volatile char *p = pipeline_get_region(inchan, channels[inchan].tail);

        if (fault)
          for (int j = 0; j < npg * PGSIZE; j += PGSIZE)
            p[j] = '\0';

        if (munmap((void *) p, npg * PGSIZE) < 0)
          die("%d: unmap failed\n", cpu);

        ++myiters;
      }

      if (underflow)
        ++myunderflows;
    }
    mypages = myiters * npg * 2;
    break;
  }

  case bench_mode::GLOBAL: {
    while (!stop) {
      // Map my part of the "hash table".  After the first iteration,
      // this will also clear the old mapping.
      volatile char *p = (base + cpu * npg * PGSIZE);
      if (mmap((void *) p, npg * PGSIZE, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
        die("%d: map failed", cpu);

      // Wait for all cores to finish mapping the "hash table".
      gbarrier.wait();
      if (stop)
        break;

      // Fault in random pages
      uint64_t *touched = (uint64_t*)malloc(1 + nthread * npg / 8);
      for (int i = 0; i < nthread * npg; ++i) {
        size_t pg = rnd() % (nthread * npg);
        if (!(touched[pg / 64] & (1ull << (pg % 64)))) {
          base[PGSIZE * pg] = '\0';
          touched[pg / 64] |= 1ull << (pg % 64);
          ++mypages;
        }
      }

      // Wait for all cores to finish faulting
      gbarrier.wait();

      ++myiters;
    }
    break;
  }

  case bench_mode::GLOBAL_FIXED: {
    volatile char *p = (base + (cpu * npg / nthread) * PGSIZE);
    volatile char *p2 = (base + ((cpu + 1) * npg / nthread) * PGSIZE);
    if (cpu == nthread - 1)
      p2 = base + npg * PGSIZE;

    while (!stop) {
      // Map my part of the "hash table".
      if (mmap((void *) p, p2 - p, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
        die("%d: map failed", cpu);

      // Wait for all cores to finish mapping the "hash table".
      gbarrier.wait();
      if (stop)
        break;

      // Fault in random pages
      uint64_t *touched = (uint64_t*)malloc(1 + npg / 8);
      for (int i = 0; i < npg; ++i) {
        size_t pg = rnd() % npg;
        if (!(touched[pg / 64] & (1ull << (pg % 64)))) {
          base[PGSIZE * pg] = '\0';
          touched[pg / 64] |= 1ull << (pg % 64);
          ++mypages;
        }
      }

      // Wait for all cores to finish faulting
      gbarrier.wait();
      if (stop)
        break;

      // Unmap
      if (munmap((void *) p, p2 - p) < 0)
        die("%d: unmap failed\n", cpu);

      ++myiters;
    }
    break;
  }
  }
  stop_tscs[cpu] = rdtsc();
#ifdef RECORD_PMC
  pmcs[cpu] = rdpmc(PMCNO) - pmc1;
#endif
  iters[cpu] = myiters;
  pages[cpu] = mypages;
  total_underflows += myunderflows;
  return nullptr;
}

uint64_t
summarize_tsc(const char *label, uint64_t tscs[], unsigned count)
{
  uint64_t min = tscs[0], max = tscs[0], total = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (tscs[i] < min)
      min = tscs[i];
    if (tscs[i] > max)
      max = tscs[i];
    total += tscs[i];
  }
  printf("%lu cycles %s skew\n", max - min, label);
  return total/count;
}

template<class T>
T
sum(T v[], unsigned count)
{
  T res{};
  for (unsigned i = 0; i < count; ++i)
    res += v[i];
  return res;
}

int
main(int argc, char **argv)
{
  if (argc < 3)
    die("usage: %s nthreads local|pipeline|global [npg]", argv[0]);

  nthread = atoi(argv[1]);

  if (strcmp(argv[2], "local") == 0)
    mode = bench_mode::LOCAL;
  else if (strcmp(argv[2], "pipeline") == 0)
    mode = bench_mode::PIPELINE;
  else if (strcmp(argv[2], "global") == 0)
    mode = bench_mode::GLOBAL;
  else if (strcmp(argv[2], "global-fixed") == 0)
    mode = bench_mode::GLOBAL_FIXED;
  else
    die("bad mode argument");

  if (argc >= 4)
    npg = atoi(argv[3]);
  else if (mode == bench_mode::GLOBAL_FIXED)
    npg = 64 * 80;
  else
    npg = 1;

  printf("# --cores=%d --duration=%ds --mode=%s --fault=%s",
         nthread, duration,
         mode == bench_mode::LOCAL ? "local" :
         mode == bench_mode::PIPELINE ? "pipeline" :
         mode == bench_mode::GLOBAL ? "global" :
         mode == bench_mode::GLOBAL_FIXED ? "global-fixed" : "UNKNOWN",
         fault ? "true" : "false");
  if (mode == bench_mode::GLOBAL_FIXED)
    printf(" --totalpg=%d", npg);
  else
    printf(" --npg=%d", npg);
  if (mode == bench_mode::PIPELINE)
    printf(" --pipeline-width=%d", pipeline_width);
  printf("\n");

#ifdef RECORD_PMC
  perf_start(PERF_SEL_USR|PERF_SEL_OS|PERF_SEL_ENABLE|RECORD_PMC, 0);
#endif

  gbarrier.left = nthread;

  pthread_t timer;
  pthread_create(&timer, NULL, timer_thread, NULL);

  pthread_t* tid = (pthread_t*) malloc(sizeof(*tid)*nthread);

  pthread_barrier_init(&bar, 0, nthread);

  for(int i = 0; i < nthread; i++)
    xthread_create(&tid[i], 0, thr, (void*)(uintptr_t) i);

  struct kstats kstats_before, kstats_after;
  read_kstats(&kstats_before);

  xpthread_join(timer);
  for(int i = 0; i < nthread; i++)
    xpthread_join(tid[i]);

  read_kstats(&kstats_after);

  // Summarize
  uint64_t start_avg = summarize_tsc("start", start_tscs, nthread);
  uint64_t stop_avg = summarize_tsc("stop", stop_tscs, nthread);
  uint64_t iter = sum(iters, nthread);

  printf("%lu cycles\n", stop_avg - start_avg);
  printf("%lu iterations\n", iter);
  printf("%lu page touches\n", sum(pages, nthread));
  if (mode == bench_mode::PIPELINE)
    printf("%lu underflows\n", total_underflows.load());
#ifdef RECORD_PMC
  printf("%lu total %s\n", sum(pmcs, nthread), STR(RECORD_PMC)+4);
#endif

#ifdef XV6_USER
  struct kstats kstats = kstats_after - kstats_before;

  printf("%lu TLB shootdowns\n", kstats.tlb_shootdown_count);
  printf("%f TLB shootdowns/page touch\n",
         (double)kstats.tlb_shootdown_count / sum(pages, nthread));
  printf("%f TLB shootdowns/iteration\n",
         (double)kstats.tlb_shootdown_count / iter);
  if (kstats.tlb_shootdown_count) {
    printf("%f targets/TLB shootdown\n",
           (double)kstats.tlb_shootdown_targets / kstats.tlb_shootdown_count);
    printf("%lu cycles/TLB shootdown\n",
           kstats.tlb_shootdown_cycles / kstats.tlb_shootdown_count);
  }

  printf("%lu page faults\n", kstats.page_fault_count);
  printf("%f page faults/page touch\n",
         (double)kstats.page_fault_count / sum(pages, nthread));
  printf("%f page faults/iteration\n",
         (double)kstats.page_fault_count / iter);
  if (kstats.page_fault_count)
    printf("%lu cycles/page fault\n",
           kstats.page_fault_cycles / kstats.page_fault_count);

  printf("%lu mmaps\n", kstats.mmap_count);
  printf("%f mmaps/page touch\n",
         (double)kstats.mmap_count / sum(pages, nthread));
  printf("%f mmaps/iteration\n",
         (double)kstats.mmap_count / iter);
  if (kstats.mmap_count)
    printf("%lu cycles/mmap\n",
           kstats.mmap_cycles / kstats.mmap_count);

  printf("%lu munmaps\n", kstats.munmap_count);
  printf("%f munmaps/page touch\n",
         (double)kstats.munmap_count / sum(pages, nthread));
  printf("%f munmaps/iteration\n",
         (double)kstats.munmap_count / iter);
  if (kstats.munmap_count)
    printf("%lu cycles/munmap\n",
           kstats.munmap_cycles / kstats.munmap_count);
#endif

  printf("%lu cycles/iteration\n",
         (sum(stop_tscs, nthread) - sum(start_tscs, nthread))/iter);
  printf("\n");
  sleep(5);
}
