# IsoAlloc Performance

## The Basics

Performance is a top priority for any memory allocator. Balancing those performance priorities with security is difficult. Anytime we introduce heavy handed security checks we usually have to pay a performance penalty. But we can be deliberate about our design choices and use good tooling to find places where we can mitigate those tradeoffs. In the end correct code is better than endless security mitigations.

## Configuration and Optimizations

IsoAlloc is only designed and tested for 64 bit, so we don't have to worry about portability hurting our performance. We can assume a large address space will always be present and we can optimize for simple things like always fetching 64 bits of memory as we iterate over an array. This remainder of this section is a basic overview of the performance optimizations in IsoAlloc.

Perhaps the most important optimization in IsoAlloc is the design choice to use a simple bitmap for tracking chunk states. Combining this with zones comprised of contiguous chunks of pages results in good performance at the cost of memory. This is in contrast to typical allocator designs full of linked list code that tends to result far more complex code, slow page faults, and exploitable designs.

All data fetches from a zone bitmap are 64 bits at a time which takes advantage of fast CPU pipelining. Fetching bits at a different bit width will result in slower performance by an order of magnitude in allocation intensive tests. All user chunks are 8 byte aligned no matter how big each chunk is. Accessing this memory with proper alignment will minimize CPU cache flushes.

When `PRE_POPULATE_PAGES` is enabled in the Makefile global caches, the root, and zone bitmaps (but not pages that hold user data) are created with `MAP_POPULATE` which instructs the kernel to pre-populate the page tables which reduces page faults and results in better performance. Note that by default at zone creation time user pages will have canaries written at random aligned offsets. This will cause page faults and populate those PTE's when the pages are first written to whether those pages are ever used at runtime or not. If you disable canaries it will result in lower RSS and a faster runtime performance.

The `MAX_ZONES` value in `conf.h` limits the total number of zones that can be allocated at runtime. If your program is being killed with OOM errors you can safely increase this value, however its max value is 65535. However it will result in a larger allocation for the `root->zones` array which holds meta data for each zone whether that zone is currently mapped and in use or not. To calculate the total number of bytes available for allocations you can do (`MAX_ZONES * ZONE_USER_SIZE`). Note that `ZONE_USER_SIZE` is not configurable in `conf.h`.

Default zones for common sizes are created in the library constructor. This helps speed up allocations for long running programs. New zones are created on demand when needed but this will incur a small performance penalty in the allocation path.

All chunk sizes are multiples of 32 with a minimum value of `SMALLEST_CHUNK_SZ` (32 by default, alignment and smallest chunk size should be in sync) and a maximum value of `SMALL_SIZE_MAX` up to 65536 by default. In a configuration with `SMALL_SIZE_MAX` set to 65536 zones will only be created for 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, and 65536. You can increase `SMALL_SIZE_MAX` up to 131072. If you choose to change this value be mindful of the pages that it can waste (e.g. allocating a chunk of 16385 bytes will result in returning a chunk of 32768 bytes).

Everything above `SMALL_SIZE_MAX` is allocated by the big zone path which has a limitation of 4 GB and a size granularity that is only limited by page size alignment. Big zones have meta data allocated separately. Guard pages for this meta data can be enabled or disabled using `BIG_ZONE_META_DATA_GUARD`. Likewise `BIG_ZONE_GUARD` can be used to enable or disable guard pages for big zone user data pages.

By default user chunks are not sanitized upon free. While this helps mitigate uninitialized memory vulnerabilities it is a very slow operation. You can enable this feature by changing the `SANITIZE_CHUNKS` flag in the Makefile.

When `USE_MLOCK` is enabled in the Makefile the meta data for the IsoAlloc root and other caches will be locked with `mlock`. This means this data will never be swapped to disk. We do this because using these data structures is required for both the alloc and free hot paths. This operation may fail if we are running inside a container with memory limits. Failure to lock the memory will not cause an abort and all failures will be silently ignored.

By default IsoAlloc randomizes the freelist per zone which results in a small performance hit. You can disable this by setting `RANDOMIZE_FREELIST` to 0 in the Makefile.

When enabled `USE_SPINLOCK` will use spinlocks via `atomic_flag` instead of a pthread mutex. Performance and load testing of IsoAlloc has shown spinlocks are slightly slower than a mutex so it is not the preferred default option.

If you know your program will not require multi-threaded access to IsoAlloc you can disable threading support by setting the `THREAD_SUPPORT` define to 0 in the Makefile. This will remove all atomic/mutex lock/unlock operations from the allocator, which will result in significant performance gains in some programs. If you do require thread support then you may want to profile your program to determine what default zone sizes will benefit performance.

`DISABLE_CANARY` can be set to 1 to disable the creation and verification of canary chunks. This removes a useful security feature but will significantly improve performance and RSS.

By default IsoAlloc will attempt to use Huge Pages (for both Linux and Mac OS) for any allocations that are a multiple of 2 mb in size. This is the default huge page size on most systems but it might not be on yours. On Linux you can check the value for your system by running the following command:

```
cat /proc/meminfo | grep Hugepagesize
Hugepagesize:       2048 kB
```

If necessary you can adjust the value of `HUGE_PAGE_SZ` in `conf.h` to reflect the size on your system.

## Caches and Memoization

There are a few important caches and memoization techniques used in IsoAlloc. These significantly improve the performance of alloc/free hot paths and keep the design as simple as possible.

### Zone Free List

Each zone contains an array of bitslots that represent free chunks in that zone. The allocation hot path searches this list first for a free chunk that can satisfy the allocation request. Allocating chunks from this list is a lot faster than iterating through a zones bitmap for a free bitslot. This cache is refilled whenever it is low. Free'd chunks are added to this list after they've been in the quarantine.

### Chunk to Zone Lookup

The chunk-to-zone lookup table is a high hit rate cache for finding which zone owns a user chunk. It works by mapping the MSB of the chunk address to a zone index. Misses are gracefully handled and more common with a higher RSS and more mappings.

### MRU Zone Cache

It is not uncommon to write a program that uses multiple threads for different purposes. Some threads will never make an allocation request above or below a certain size. This thread local cache optimizes for this by storing a TLS array of the threads most recently used zones. These zones are checked in the `iso_find_zone_range` free path if the chunk-to-zone lookup fails.

### Thread Chunk Quarantine

This thread local cache speeds up the free hot path by quarantining chunks until a threshold has been met. Until that threshold is reached free's are very cheap. When the cache is emptied and the chunks free'd its still faster because we take advantage of keeping the zone meta data in a CPU cache line.

### Zone Lookup Table

Zones are linked by their `next_sz_index` member which tells the allocator where in the `_root->zones` array it can find the next zone that holds the same size chunks. This lookup table helps us find the first zone that holds a specific size in O(1) time. This is achieved by placing a zone's index value at that zones size index in the table, e.g. `zone_lookup_table[zone->size] = zone->index`, from there we just need to use the next zone's index member and walk it like a singly linked list to find other zones of that size. Zones are added to the front of the list as they are created.

## ARM NEON

IsoAlloc tries to use ARM Neon instructions to make loops in the hot path faster. If you want to disable any of this code at compile time and rely instead on more portable, but potentially slower code just set `DONT_USE_NEON` to 1 in the Makefile.

## Tests

I've spent a good amount of time testing IsoAlloc to ensure its reasonably fast compared to glibc/ptmalloc. But it is impossible for me to model or simulate all the different states a program that uses IsoAlloc may be in. This section briefly covers the existing performance related tests for IsoAlloc and the data I have collected so far.

The `malloc_cmp_test` build target will build 2 different versions of the test/tests.c program which runs roughly 1.4 million alloc/calloc/realloc operations. We free every other allocation and then free the remaining ones once the allocation loop is complete. This helps simulate some fragmentation in the heap. On average IsoAlloc is faster than ptmalloc but the difference is so close it will likely not be noticable.

The following test was run in an Ubuntu 20.04.3 LTS (Focal Fossa) for ARM64 docker container with libc version 2.31-0ubuntu9.2 on a MacOS host. The kernel used was `Linux f7f23ca7dc44 5.10.76-linuxkit`.

```
Running IsoAlloc Performance Test

build/tests
iso_alloc/iso_free 1441616 tests completed in 0.168293 seconds
iso_calloc/iso_free 1441616 tests completed in 0.171274 seconds
iso_realloc/iso_free 1441616 tests completed in 0.231350 seconds

Running glibc/ptmalloc Performance Test

malloc/free 1441616 tests completed in 0.166813 seconds
calloc/free 1441616 tests completed in 0.223232 seconds
realloc/free 1441616 tests completed in 0.306684 seconds

Running jemalloc Performance Test

LD_PRELOAD=/code/mimalloc-bench/extern/jemalloc/lib/libjemalloc.so build/malloc_tests
malloc/free 1441616 tests completed in 0.064520 seconds
calloc/free 1441616 tests completed in 0.178228 seconds
realloc/free 1441616 tests completed in 0.271620 seconds

Running mimalloc Performance Test

LD_PRELOAD=/code/mimalloc-bench/extern/mimalloc/out/release/libmimalloc.so build/malloc_tests
malloc/free 1441616 tests completed in 0.085471 seconds
calloc/free 1441616 tests completed in 0.099644 seconds
realloc/free 1441616 tests completed in 0.143821 seconds

Running mimalloc-secure Performance Test

LD_PRELOAD=/code/mimalloc-bench/extern/mimalloc/out/secure/libmimalloc-secure.so build/malloc_tests
malloc/free 1441616 tests completed in 0.128479 seconds
calloc/free 1441616 tests completed in 0.148797 seconds
realloc/free 1441616 tests completed in 0.191719 seconds

Running tcmalloc Performance Test

LD_PRELOAD=/code/mimalloc-bench/extern/gperftools/.libs/libtcmalloc_minimal.so build/malloc_tests
malloc/free 1441616 tests completed in 0.093779 seconds
calloc/free 1441616 tests completed in 0.103634 seconds
realloc/free 1441616 tests completed in 0.131152 seconds

Running scudo Performance Test

LD_PRELOAD=/code/mimalloc-bench/extern/scudo/compiler-rt/lib/scudo/standalone/libscudo.so build/malloc_tests
malloc/free 1441616 tests completed in 0.227757 seconds
calloc/free 1441616 tests completed in 0.204610 seconds
realloc/free 1441616 tests completed in 0.258962 seconds

```

The same test run on an AWS t2.xlarge Ubuntu 20.04 instance with 4 `Intel(R) Xeon(R) CPU E5-2676 v3 @ 2.40GHz` CPUs and 16 gb of memory:

```
Running IsoAlloc Performance Test

iso_alloc/iso_free 1441616 tests completed in 0.147336 seconds
iso_calloc/iso_free 1441616 tests completed in 0.161482 seconds
iso_realloc/iso_free 1441616 tests completed in 0.244981 seconds

Running glibc malloc Performance Test

malloc/free 1441616 tests completed in 0.182437 seconds
calloc/free 1441616 tests completed in 0.246065 seconds
realloc/free 1441616 tests completed in 0.332292 seconds
```

Here is the same test as above on Mac OS 12.1

```
Running IsoAlloc Performance Test

build/tests
iso_alloc/iso_free 1441616 tests completed in 0.149818 seconds
iso_calloc/iso_free 1441616 tests completed in 0.183772 seconds
iso_realloc/iso_free 1441616 tests completed in 0.274413 seconds

Running system malloc Performance Test

build/malloc_tests
malloc/free 1441616 tests completed in 0.084803 seconds
calloc/free 1441616 tests completed in 0.194901 seconds
realloc/free 1441616 tests completed in 0.240934 seconds
```

This same test can be used with the `perf` utility to measure basic stats like page faults and CPU utilization using both heap implementations. The output below is on the same AWS t2.xlarge instance as above.

```
$ perf stat build/tests

iso_alloc/iso_free 1441616 tests completed in 0.416603 seconds
iso_calloc/iso_free 1441616 tests completed in 0.575822 seconds
iso_realloc/iso_free 1441616 tests completed in 0.679546 seconds

 Performance counter stats for 'build/tests':

           1709.07 msec task-clock                #    1.000 CPUs utilized
                 7      context-switches          #    0.004 K/sec
                 0      cpu-migrations            #    0.000 K/sec
            145562      page-faults               #    0.085 M/sec

       1.709414837 seconds time elapsed

       1.405068000 seconds user
       0.304239000 seconds sys

$ perf stat build/malloc_tests

malloc/free 1441616 tests completed in 0.359380 seconds
calloc/free 1441616 tests completed in 0.569044 seconds
realloc/free 1441616 tests completed in 0.597936 seconds

 Performance counter stats for 'build/malloc_tests':

           1528.51 msec task-clock                #    1.000 CPUs utilized
                 5      context-switches          #    0.003 K/sec
                 0      cpu-migrations            #    0.000 K/sec
            433055      page-faults               #    0.283 M/sec

       1.528795324 seconds time elapsed

       0.724352000 seconds user
       0.804371000 seconds sys

```

The following benchmarks were collected from [mimalloc-bench](https://github.com/daanx/mimalloc-bench) with the default configuration of IsoAlloc. As you can see from the data IsoAlloc is competitive with jemalloc, tcmalloc, and glibc/ptmalloc for some benchmarks but clearly falls behind in the Redis benchmark. For any benchmark that IsoAlloc scores poorly on I was able to tweak its build to improve the CPU time and memory consumption. It's worth noting that IsoAlloc was able to stay competitive even with performing many security checks not present in other allocators.

```
# benchmark allocator elapsed rss user sys page-faults page-reclaims

cfrac       jemalloc    03.47 3948 3.46 0.00 0 422
cfrac       mimalloc    03.19 2688 3.18 0.00 0 337
cfrac       smimalloc   03.57 2860 3.56 0.00 0 375
cfrac       tcmalloc    03.25 7392 3.24 0.00 0 1325
cfrac       scudo       06.00 3920 5.99 0.00 0 561
cfrac       isoalloc    05.69 12920 5.58 0.10 0 3016

espresso    jemalloc    03.61 4508 3.56 0.00 5 553
espresso    mimalloc    03.43 3828 3.40 0.01 1 1299
espresso    smimalloc   03.65 5760 3.60 0.01 0 2682
espresso    tcmalloc    03.43 8132 3.39 0.01 0 1485
espresso    scudo       04.53 4028 4.49 0.01 0 514
espresso    isoalloc    04.59 48984 4.49 0.07 0 24276

barnes      jemalloc    01.93 59412 1.91 0.01 3 16646
barnes      mimalloc    01.91 57860 1.89 0.01 0 16539
barnes      smimalloc   01.98 57928 1.96 0.01 0 16557
barnes      tcmalloc    01.91 62664 1.89 0.01 0 17515
barnes      scudo       01.92 58940 1.91 0.01 0 16595
barnes      isoalloc    01.92 58328 1.91 0.01 0 16714

redis       jemalloc    5.019 31280 2.37 0.17 0 7268
redis       mimalloc    4.487 29204 2.07 0.20 0 6825
redis       smimalloc   4.909 30992 2.28 0.20 0 7410
redis       tcmalloc    4.675 37336 2.17 0.20 0 8682
redis       scudo       6.105 36968 2.85 0.23 0 8623
redis       isoalloc    8.658 62088 4.06 0.32 0 18099

cache-thrash1 jemalloc    01.28 3648 1.27 0.00 1 240
cache-thrash1 mimalloc    01.28 3408 1.28 0.00 0 197
cache-thrash1 smimalloc   01.28 3256 1.27 0.00 0 202
cache-thrash1 tcmalloc    01.27 7100 1.26 0.00 0 1127
cache-thrash1 scudo       01.27 3240 1.26 0.00 0 200
cache-thrash1 isoalloc    01.26 3460 1.26 0.00 0 363

cache-thrashN jemalloc    00.21 3936 1.64 0.00 0 360
cache-thrashN mimalloc    00.21 3516 1.63 0.00 0 239
cache-thrashN smimalloc   00.22 3584 1.68 0.01 0 249
cache-thrashN tcmalloc    02.74 6992 20.36 0.00 0 1151
cache-thrashN scudo       00.61 3164 2.53 0.00 0 237
cache-thrashN isoalloc    00.71 4032 5.63 0.00 0 472

larsonN     jemalloc    4.892 84172 39.71 0.20 1 52478
larsonN     mimalloc    4.360 98504 39.61 0.17 0 26372
larsonN     smimalloc   6.546 105724 39.77 0.16 3 27432
larsonN     tcmalloc    4.450 63464 39.57 0.21 0 15299
larsonN     scudo       44.707 33104 28.92 4.80 0 7826
larsonN     isoalloc    249.791 63996 7.09 17.51 0 15567

larsonN-sized jemalloc    4.872 84428 39.56 0.22 1 52874
larsonN-sized mimalloc    4.335 95388 39.82 0.13 0 25625
larsonN-sized smimalloc   6.332 106372 39.71 0.17 0 27642
larsonN-sized tcmalloc    4.230 64956 39.59 0.15 0 15669
larsonN-sized scudo       44.601 32900 28.68 4.65 0 7793
larsonN-sized isoalloc    363.176 70240 39.59 0.29 0 17222

mstressN    jemalloc    00.92 139772 2.10 1.76 1 984466
mstressN    mimalloc    00.43 352132 1.56 0.15 0 88171
mstressN    smimalloc   00.62 352204 1.85 0.67 0 95538      
mstressN    tcmalloc    00.51 147680 1.80 0.25 0 37111
mstressN    scudo       01.38 142068 3.23 1.63 0 616639
mstressN    isoalloc    03.11 225352 4.90 5.91 0 722991

xmalloc-testN jemalloc    2.307 64460 25.13 5.14 1 22975
xmalloc-testN mimalloc    0.513 82212 36.03 1.05 0 26689
xmalloc-testN smimalloc   0.857 73504 36.58 1.05 0 28285
xmalloc-testN tcmalloc    6.055 40824 9.31 18.77 0 9642
xmalloc-testN scudo       13.416 56708 10.06 14.30 0 16560
xmalloc-testN isoalloc    10.049 15268 7.19 20.91 0 3668

glibc-simple jemalloc    01.96 2984 1.95 0.00 1 313
glibc-simple mimalloc    01.50 1900 1.49 0.00 0 212
glibc-simple smimalloc   01.77 2032 1.76 0.00 0 229
glibc-simple tcmalloc    01.52 6880 1.52 0.00 0 1212
glibc-simple scudo       04.58 2776 4.58 0.00 0 281
glibc-simple isoalloc    04.21 10892 4.12 0.09 0 4674

glibc-thread jemalloc    6.772 4160 15.98 0.00 1 457
glibc-thread mimalloc    3.759 3320 15.98 0.00 0 585
glibc-thread smimalloc   9.012 17144 15.89 0.02 0 4018
glibc-thread tcmalloc    10.434 8508 15.99 0.00 0 1580
glibc-thread scudo       80.979 4076 15.90 0.01 0 582
glibc-thread isoalloc    374.692 2240 2.56 5.14 0 348
```

IsoAlloc isn't quite ready for performance sensitive server workloads. However it's more than fast enough for client side mobile/desktop applications with risky C/C++ attack surfaces. These environments have threat models similar to what IsoAlloc was designed for.
