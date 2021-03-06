#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <stdint.h>

#include "xmalloc.h"

#ifndef __USE_MISC
#define __USE_MISC
#endif
#include <bits/mman.h>

//ATTRIBUTION: Nat Tuck, one_bucket.c, github.com/NatTuck/scratch-2020-09/blob/master/3650/p12a/01/one_bucket.c
typedef struct bucket {
  size_t size;
  struct bucket* next;
  void* mmap_start;
  int num_assigned;
  int num_bools;
  uint8_t* used;
  void* blocks;
} bucket;

long next_arena_ticket = 0;
bucket** arenas[4];
__thread int favorite_arena = -1;

static bucket* buckets[20];
static unsigned int sizes[20] = {2, 4, 8, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096 };

const size_t CHUNK_SIZE = 4096 * 4;
const size_t PAGE_SIZE = 4096;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void
print_bucket(bucket* b)
{
  if (!b) {
    printf("\nNull Bucket\n\n");
    return;
  }
  printf("\nBucket Info\n\n");

  printf("Bucket size: %zu\n", b->size);
  printf("Bucket next: %p\n", b->next);
  printf("Bucket mmap_start: %p\n", b->mmap_start);
  printf("Bucket location: %p\n", b);
  printf("Bucket num_bools: %d\n", b->num_bools);
  printf("Bucket num_assigned: %d\n", b->num_assigned);
  printf("Bucket used: %p\n", b->used);

  printf("Used list: ");
  for (int ii = 0; ii < b->num_bools; ++ii) {
    printf("%d ", b->used[ii]);
  }
  printf("\n");

  printf("Bucket blocks: %p\n", b->blocks);

  printf("\n");
}

bucket*
alloc_bucket(unsigned int bucket_size) {
  pthread_mutex_lock(&lock);

  intptr_t addr = (intptr_t) mmap(0, CHUNK_SIZE * 2, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  intptr_t mmap_start = addr;
  intptr_t ptr = addr;

  if (addr % CHUNK_SIZE != 0) {
    ptr = addr + (CHUNK_SIZE - (addr % CHUNK_SIZE));
  }

  addr = ptr;

  bucket* b = (bucket*) addr;
  b->size = bucket_size;
  b->next = 0;
  b->mmap_start = (void*) mmap_start;
  b->num_assigned = 0;

  int num_bools = 0;
  int remaining_size = CHUNK_SIZE - sizeof(bucket);

  //maybe speed up? O(large constant), could be faster with optimizations
  while (remaining_size > 0) {
    remaining_size -= (bucket_size + 1);
    if(remaining_size < 0) {
      break;
    }
    num_bools++;
  }

  //TODO add case for when you need to shift the pointer up to the point that there isnt enough room for any blocks

  //maybe shift up to account for alignment? num_bools is just a multiple of one
  b->used = (uint8_t*) (addr + sizeof(bucket));
  b->blocks = ((void*) b->used) + num_bools;
  b->num_bools = num_bools;

  pthread_mutex_unlock(&lock);
  return b;
}

//TODO implement checking adjacent buckets when multiple are mapped and linked
void*
xmalloc(size_t bytes)
{
  if (favorite_arena < 0) {
    favorite_arena = __atomic_fetch_add(&next_arena_ticket, 1, __ATOMIC_SEQ_CST);
  }

  pthread_mutex_lock(&lock);

  if (bytes + sizeof(bucket) >= 4096) {
    int num_chunks = CHUNK_SIZE / bytes + sizeof(bucket);
    printf("bytes = %zu, allocating %zu\n", bytes, bytes + sizeof(bucket));
    intptr_t b = (intptr_t) mmap(0, (num_chunks * CHUNK_SIZE) * 2, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    printf("mmaps at %p\n", (void*) b);

    if (b % CHUNK_SIZE != 0) {
      b += CHUNK_SIZE - (b % CHUNK_SIZE);
    }

    bucket* bb = (bucket*) b;
    bb->size = bytes + sizeof(bucket);

    void* bp = (void*) (b + sizeof(bucket));

    pthread_mutex_unlock(&lock);
    return bp;
  }

  int bucket_num = 0;

  if (bytes < sizes[0]) {
    bucket_num = 0;
  }
  else {
    for (int ii = 0; ii < 20; ++ii) {
      if (bytes == sizes[ii]) {
        bucket_num = ii;
        break;
      }
      else if (bytes > sizes[ii] && bytes < sizes[ii + 1]) {
        bytes = sizes[ii + 1];
        bucket_num = ii;
        break;
      }
    }
  }

  //TODO remove and readd once one is freed
  if (buckets[bucket_num] == 0) {
    pthread_mutex_unlock(&lock);
    buckets[bucket_num] = alloc_bucket(bytes);
    pthread_mutex_lock(&lock);
  }
  if (buckets[bucket_num]->num_assigned >= buckets[bucket_num]->num_bools) {
    pthread_mutex_unlock(&lock);
    bucket* b = alloc_bucket(bytes);
    pthread_mutex_lock(&lock);

    b->next = buckets[bucket_num];
    buckets[bucket_num] = b;
  }

  void* allocp;
  int ii = 0;

  for (uint8_t* used = buckets[bucket_num]->used; ; used++) {
    if (*used == 0) {
      *used = 1;
      allocp = buckets[bucket_num]->blocks + (ii * bytes);
      break;
    }
    ++ii;
  }

  buckets[bucket_num]->num_assigned++;
  pthread_mutex_unlock(&lock);
  return allocp;
}

//TODO insert bucket at head of bucket chain in list if valid
void
xfree(void* ptr)
{
  pthread_mutex_lock(&lock);

  intptr_t addr = (intptr_t) ptr;
  bucket* b = (bucket*) (addr - (addr % CHUNK_SIZE));

  if (b->size >= 4096) {
    munmap((void*) b, b->size);
    pthread_mutex_unlock(&lock);
    return;
  }


  intptr_t base = (intptr_t) (b->blocks);
  long ii = (addr - base) / b->size;

  b->used[ii] = 0;
  b->num_assigned--;

  int bucket_num = 0;
  for (; bucket_num < 20; ++bucket_num) {
    if (b->size == sizes[bucket_num]) {
      break;
    }
  }

  if (b->num_assigned == 0) {
    int ii = 0;

    for (; ii < 20; ++ii) {
      if (sizes[ii] == b->size) {
        break;
      }
    }

    buckets[bucket_num] = b->next;
    munmap(b, CHUNK_SIZE);
  }
  pthread_mutex_unlock(&lock);
}

void*
xrealloc(void* prev, size_t nn)
{
  void* newp = xmalloc(nn);
  intptr_t int_prev = (intptr_t) prev;

  pthread_mutex_lock(&lock);
  size_t old_size = int_prev - (int_prev % CHUNK_SIZE);

  size_t bytes_to_copy = old_size < nn ? old_size : nn;

  memcpy(newp, prev, bytes_to_copy);
  pthread_mutex_unlock(&lock);

  xfree(prev);
  
  prev = newp;
  
  return prev;
}
