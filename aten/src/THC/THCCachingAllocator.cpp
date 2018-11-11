#include "THCCachingAllocator.h"

#include <ATen/Context.h>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/Exceptions.h>

#include <cuda_runtime_api.h>
#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

//
// Yet another caching allocator for CUDA device allocations.
//
// - Allocations are associated with a stream. Once freed, blocks can be
//   re-allocated on the same stream, but not on any other stream.
// - The allocator attempts to find the smallest cached block that will fit the
//   requested size. If the block is larger than the requested size, it may be
//   split. If no block is found, the allocator will delegate to cudaMalloc.
// - If the cudaMalloc fails, the allocator will free all cached blocks that
//   are not split and retry the allocation.
// - Large (>1MB) and small allocation requests are handled separately. Large
//   allocation requests can be filled by a cudaMalloc call of the exact size.
//   Small requests will allocate and split a 1MB buffer, if necessary.
//
// With this allocator, allocations and frees should logically be considered
// "usages" of the memory segment associated with streams, just like kernel
// launches. The programmer must insert the proper synchronization if memory
// segments are used from multiple streams.
//
// The library provides a recordStream() function to help insert the correct
// synchronization when allocations are used on multiple streams. This will
// ensure that the block is not reused before each recorded stream completes
// work.
//

namespace {

typedef std::shared_ptr<THCStream> THCStreamPtr;
typedef std::set<THCStreamPtr> stream_set;

const size_t kRoundSmall = 512;     // round up small allocs to 512 bytes
const size_t kRoundLarge = 131072;  // round up large allocs to 128 KiB
const size_t kSmallAlloc = 1048576; // largest "small" allocation is 1 MiB

struct DeviceStats {
  uint64_t   amount_allocated;      // total amount allocated in bytes
  uint64_t   max_amount_allocated;  // max total amount allocated in bytes
  uint64_t   amount_cached;         // total amount in cache in bytes
  uint64_t   max_amount_cached;     // max total amount in cache in bytes

  DeviceStats() :
      amount_allocated(0), max_amount_allocated(0),
      amount_cached(0), max_amount_cached(0) { }

  void increaseAllocated(size_t delta) {
    amount_allocated += delta;
    max_amount_allocated = std::max(max_amount_allocated, amount_allocated);
  }

  void decreaseAllocated(size_t delta) {
    amount_allocated -= delta;
  }

  void increaseCached(size_t delta) {
    amount_cached += delta;
    max_amount_cached = std::max(max_amount_cached, amount_cached);
  }

  void decreaseCached(size_t delta) {
    amount_cached -= delta;
  }
};

struct Block {
  int           device;      // gpu
  cudaStream_t  stream;      // allocation stream
  stream_set    stream_uses; // streams on which the block was used
  size_t        size;        // block size in bytes
  char*         ptr;         // memory address
  bool          allocated;   // in-use flag
  Block*        prev;        // prev block if split from a larger allocation
  Block*        next;        // next block if split from a larger allocation
  int           event_count; // number of outstanding CUDA events

  Block(int device, cudaStream_t stream, size_t size, char* ptr=NULL) :
      device(device), stream(stream), stream_uses(), size(size), ptr(ptr),
      allocated(0), prev(NULL), next(NULL), event_count(0) { }
};

static bool BlockComparator(const Block* a, const Block* b)
{
  if (a->device != b->device) {
    return a->device < b->device;
  }
  if (a->stream != b->stream) {
    return (uintptr_t)a->stream < (uintptr_t)b->stream;
  }
  if (a->size != b->size) {
    return a->size < b->size;
  }
  return (uintptr_t)a->ptr < (uintptr_t)b->ptr;
}

static std::string format_size(uint64_t size) {
  std::ostringstream os;
  os.precision(2);
  os << std::fixed;
  if (size <= 1024) {
    os << size << " bytes";
  } else if (size <= 1048576) {
    os << (size / 1024.0);
    os << " KiB";
  } else if (size <= 1073741824ULL) {
    os << size / 1048576.0;
    os << " MiB";
  } else {
    os << size / 1073741824.0;
    os << " GiB";
  }
  return os.str();
}

} // namespace

struct THCCachingAllocator
{
  typedef bool (*Comparison)(const Block*, const Block*);
  typedef std::set<Block*, Comparison> FreeBlocks;

  // device statistics
  std::vector<DeviceStats> device_stats;

  // lock around all operations
  std::mutex mutex;

  // lock around calls to cudaFree (to prevent deadlocks with NCCL)
  std::mutex cuda_free_mutex;

  // cached blocks larger than 1 MB
  FreeBlocks large_blocks;

  // cached blocks 1 MB or smaller
  FreeBlocks small_blocks;

  // allocated blocks by device pointer
  std::unordered_map<void*, Block*> allocated_blocks;

  // outstanding cuda events
  std::deque<std::pair<cudaEvent_t, Block*>> cuda_events;

  THCCachingAllocator() :
      large_blocks(BlockComparator),
      small_blocks(BlockComparator) {}

  DeviceStats &get_stats_for_device(int device) {
    THAssert(device >= 0);
    if ((size_t) device >= device_stats.size()) {
      device_stats.resize(device + 1);
    }
    return device_stats.at(device);
  }

  /** allocates a block which is safe to use from the provided stream */
  void malloc(void** devPtr, size_t size, cudaStream_t stream)
  {
    std::lock_guard<std::mutex> lock(mutex);

    int device;
    AT_CUDA_CHECK(cudaGetDevice(&device));

    // process outstanding cudaEvents
    process_events();

    size = round_size(size);
    bool small = size <= kSmallAlloc;

    DeviceStats &stats = get_stats_for_device(device);

    Block search_key(device, stream, size);
    auto& free_blocks = small ? small_blocks : large_blocks;

    Block* block = NULL;
    Block* remaining = NULL;

    auto it = free_blocks.lower_bound(&search_key);
    if (it != free_blocks.end() && (*it)->device == device && (*it)->stream == stream) {
      block = *it;
      free_blocks.erase(it);
    } else {
      void* ptr;
      size_t alloc_size = small ? kSmallAlloc : size;
      cudaError_t err = cuda_malloc_retry(device, &ptr, alloc_size);
      if (err != cudaSuccess) {
        if (err == cudaErrorMemoryAllocation) {
          cudaGetLastError();  // clear CUDA error

          size_t device_free;
          size_t device_total;
          AT_CUDA_CHECK(cudaMemGetInfo(&device_free, &device_total));
          const auto& stats = get_stats_for_device(device);

          // "total capacity": total global memory on GPU
          // "already allocated": memory allocated by the program using the
          //                      caching allocator
          // "free": free memory as reported by the CUDA API
          // "cached": memory held by the allocator but not used by the program
          //
          // The "allocated" amount  does not include memory allocated outside
          // of the caching allocator, such as memory allocated by other programs
          // or memory held by the driver.
          //
          // The sum of "allocated" + "free" + "cached" may be less than the
          // total capacity due to memory held by the driver and usage by other
          // programs.
          //
          // Note that at this point cuda_malloc_retry has already returned all
          // possible "cached" memory to the driver. The only remaining "cached"
          // memory is split from a larger block that is partially in-use.
          AT_ERROR(
            "CUDA out of memory. Tried to allocate ", format_size(alloc_size),
            " (GPU ", device, "; ",
            format_size(device_total), " total capacity; ",
            format_size(stats.amount_allocated), " already allocated; ",
            format_size(device_free), " free; ",
            format_size(stats.amount_cached - stats.amount_allocated), " cached)");
        } else {
          AT_CUDA_CHECK(err);
        }
      }
      stats.increaseCached(alloc_size);
      block = new Block(device, stream, alloc_size, (char*)ptr);
    }

    if (block->size - size >= (small ? kRoundSmall : kSmallAlloc + 1)) {
      remaining = block;

      block = new Block(device, stream, size, block->ptr);
      block->prev = remaining->prev;
      if (block->prev) {
        block->prev->next = block;
      }
      block->next = remaining;

      remaining->prev = block;
      remaining->ptr += size;
      remaining->size -= size;
      free_blocks.insert(remaining);
    }

    block->allocated = true;
    allocated_blocks[block->ptr] = block;

    *devPtr = (void*)block->ptr;

    stats.increaseAllocated(block->size);
  }

  void free(void* ptr)
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!ptr) {
      return;
    }

    auto it = allocated_blocks.find(ptr);
    if (it == allocated_blocks.end()) {
      AT_ERROR("invalid device pointer: ", ptr);
    }

    Block* block = it->second;
    allocated_blocks.erase(it);
    block->allocated = false;

    get_stats_for_device(block->device).decreaseAllocated(block->size);
    if (!block->stream_uses.empty()) {
      insert_events(block);
    } else {
      free_block(block);
    }
  }

  /** returns cached blocks to the system allocator */
  void emptyCache()
  {
    std::lock_guard<std::mutex> lock(mutex);
    free_blocks(large_blocks, large_blocks.begin(), large_blocks.end());
    free_blocks(small_blocks, small_blocks.begin(), small_blocks.end());
  }

  void* getBaseAllocation(void* ptr, size_t* outSize)
  {
    std::lock_guard<std::mutex> lock(mutex);
    Block* block = find_allocated_block(ptr);
    if (!block) {
      THError("invalid device pointer: %p", ptr);
    }
    while (block->prev) {
      block = block->prev;
    }
    void *basePtr = block->ptr;
    if (outSize) {
      size_t size = 0;
      while (block) {
        size += block->size;
        block = block->next;
      }
      *outSize = size;
    }
    return basePtr;
  }

  // Accumulates sizes of all memory blocks for given device in given free list
  void cacheInfoAux(FreeBlocks& blocks, int dev_id, size_t* total, size_t* largest)
  {
    Block search_key(dev_id, 0, 0);
    auto it = blocks.lower_bound(&search_key);
    for (; it != blocks.end() && *it && (*it)->device == dev_id; ++it) {
      size_t blocksize = (*it)->size;
      *total += blocksize;
      if (blocksize > *largest) {
        *largest = blocksize;
      }
    }
  }

  void cacheInfo(int dev_id, size_t* total, size_t* largest)
  {
    std::lock_guard<std::mutex> lock(mutex);
    cacheInfoAux(large_blocks, dev_id, total, largest);
    cacheInfoAux(small_blocks, dev_id, total, largest);
  }

  void recordStream(void* ptr, THCStream* stream)
  {
    std::lock_guard<std::mutex> lock(mutex);
    Block* block = find_allocated_block(ptr);
    if (!block) {
      THError("invalid device pointer: %p", ptr);
    }
    if (THCStream_stream(stream) == block->stream) {
      // ignore uses on the allocation stream, since those don't require any
      // special synchronization
      return;
    }
    THCStream_retain(stream);
    block->stream_uses.insert(THCStreamPtr(stream, &THCStream_free));
  }

  /** moves a block into the free block list */
  void free_block(Block* block)
  {
    THAssert(!block->allocated && block->event_count == 0);
    bool small = block->size <= kSmallAlloc;
    auto& free_blocks = small ? small_blocks : large_blocks;
    try_merge_blocks(block, block->prev, free_blocks);
    try_merge_blocks(block, block->next, free_blocks);
    free_blocks.insert(block);
  }

  /** combine previously split blocks */
  void try_merge_blocks(Block* dst, Block* src, FreeBlocks& free_blocks)
  {
    if (!src || src->allocated || src->event_count > 0) {
      return;
    }
    if (dst->prev == src) {
      dst->ptr = src->ptr;
      dst->prev = src->prev;
      if (dst->prev) {
        dst->prev->next = dst;
      }
    } else {
      dst->next = src->next;
      if (dst->next) {
        dst->next->prev = dst;
      }
    }
    dst->size += src->size;
    free_blocks.erase(src);
    delete src;
  }

  size_t round_size(size_t size)
  {
    if (size < kRoundSmall) {
      size = kRoundSmall;
    } else if (size < kSmallAlloc) {
      size += kRoundSmall - 1 - (size - 1) % kRoundSmall;
    } else {
      size += kRoundLarge - 1 - (size - 1) % kRoundLarge;
    }
    return size;
  }

  cudaError_t cuda_malloc_retry(int device, void** devPtr, size_t size)
  {
    // Try cudaMalloc. If cudaMalloc fails, frees all non-split cached blocks
    // and retries.
    cudaError_t err = cudaMalloc(devPtr, size);
    if (err != cudaSuccess) {
      cudaGetLastError();  // reset the last CUDA error
      free_cached_blocks(device);
      err = cudaMalloc(devPtr, size);
      if (err != cudaSuccess) {
        return err;
      }
    }
    return cudaSuccess;
  }

  void free_cached_blocks(int device)
  {
    // Free all non-split cached blocks on device
    Block lower_bound(device, NULL, 0);
    Block upper_bound(device + 1, NULL, 0);

    free_blocks(
        large_blocks,
        large_blocks.lower_bound(&lower_bound),
        large_blocks.lower_bound(&upper_bound));
    free_blocks(
        small_blocks,
        small_blocks.lower_bound(&lower_bound),
        small_blocks.lower_bound(&upper_bound));
  }

  void free_blocks(FreeBlocks& blocks, FreeBlocks::iterator it, FreeBlocks::iterator end)
  {
    // Frees all non-split blocks between `it` and `end`
    std::lock_guard<std::mutex> lock(cuda_free_mutex);
    while (it != end) {
      Block* block = *it;
      if (!block->prev && !block->next) {
        AT_CUDA_CHECK(cudaFree((void*)block->ptr));
        get_stats_for_device(block->device).decreaseCached(block->size);
        auto cur = it;
        ++it;
        blocks.erase(cur);
        delete block;
      } else {
        ++it;
      }
    }
  }

  Block* find_allocated_block(void *ptr) {
    auto it = allocated_blocks.find(ptr);
    if (it == allocated_blocks.end()) {
      return NULL;
    }
    return it->second;
  }

  void insert_events(Block* block)
  {
    int prev_device;
    AT_CUDA_CHECK(cudaGetDevice(&prev_device));

    std::set<THCStreamPtr> streams(std::move(block->stream_uses));
    THAssert(block->stream_uses.empty());
    for (auto it = streams.begin(); it != streams.end(); ++it) {
      auto& stream = *it;
      AT_CUDA_CHECK(cudaSetDevice(THCStream_device(stream.get())));

      cudaEvent_t event;
      AT_CUDA_CHECK(cudaEventCreateWithFlags(&event, cudaEventDisableTiming));
      AT_CUDA_CHECK(cudaEventRecord(event, THCStream_stream(stream.get())));

      block->event_count++;
      cuda_events.emplace_back(event, block);
    }

    cudaSetDevice(prev_device);
  }

  void process_events()
  {
    // Process outstanding cudaEvents. Events that are completed are removed
    // from the queue, and the 'event_count' for the corresponding allocation
    // is decremented. Stops at the first event which has not been completed.
    // Since events on different devices or streams may occur out of order,
    // the processing of some events may be delayed.
    while (!cuda_events.empty()) {
      auto& e = cuda_events.front();
      cudaEvent_t event = e.first;
      Block* block = e.second;

      cudaError_t err = cudaEventQuery(event);
      if (err == cudaErrorNotReady) {
        break;
      } else if (err != cudaSuccess) {
        AT_CUDA_CHECK(err);
      }

      AT_CUDA_CHECK(cudaEventDestroy(event));

      block->event_count--;
      if (block->event_count == 0) {
        free_block(block);
      }
      cuda_events.pop_front();
    }
  }
};

THCCachingAllocator caching_allocator;

static void CudaCachingDeleter(void* ptr) {
  caching_allocator.free(ptr);
}

// NB: I decided not to fold this into THCCachingAllocator, because the latter
// has a lot more methods and it wasn't altogether clear that they should
// actually be publically exposed
struct CudaCachingAllocator : public at::Allocator {
  at::DataPtr allocate(size_t size) const override {
    int device;
    THCudaCheck(cudaGetDevice(&device));
    void* r = nullptr;
    if (size != 0) {
      caching_allocator.malloc(&r, size, at::cuda::getCurrentCUDAStream(device));
    }
    return {r, r, &CudaCachingDeleter, at::Device(at::DeviceType::CUDA, device)};
  }
  at::DeleterFnPtr raw_deleter() const override {
    return &CudaCachingDeleter;
  }
};

CudaCachingAllocator device_allocator;

THC_API at::Allocator* THCCachingAllocator_get(void)
{
  return &device_allocator;
}

THC_API void THCCachingAllocator_emptyCache(void) {
  caching_allocator.emptyCache();
}

THC_API void THCCachingAllocator_cacheInfo(int dev_id, size_t* cachedAndFree, size_t* largestBlock) {
  caching_allocator.cacheInfo(dev_id, cachedAndFree, largestBlock);
}

THC_API void* THCCachingAllocator_getBaseAllocation(void *ptr, size_t *size)
{
  return caching_allocator.getBaseAllocation(ptr, size);
}

THC_API void THCCachingAllocator_recordStream(void *ptr, THCStream* stream)
{
  caching_allocator.recordStream(ptr, stream);
}

THC_API std::mutex* THCCachingAllocator_getCudaFreeMutex()
{
  return &caching_allocator.cuda_free_mutex;
}

static inline void assertValidDevice(int device) {
  int device_count;
  THCudaCheck(cudaGetDeviceCount(&device_count));
  THAssertMsg(0 <= device && device < device_count, "Invalid device argument.");
}

THC_API uint64_t THCCachingAllocator_currentMemoryAllocated(int device)
{
  assertValidDevice(device);
  return caching_allocator.get_stats_for_device(device).amount_allocated;
}

THC_API uint64_t THCCachingAllocator_maxMemoryAllocated(int device) {
  assertValidDevice(device);
  return caching_allocator.get_stats_for_device(device).max_amount_allocated;
}

THC_API uint64_t THCCachingAllocator_currentMemoryCached(int device)
{
  assertValidDevice(device);
  return caching_allocator.get_stats_for_device(device).amount_cached;
}

THC_API uint64_t THCCachingAllocator_maxMemoryCached(int device) {
  assertValidDevice(device);
  return caching_allocator.get_stats_for_device(device).max_amount_cached;
}
