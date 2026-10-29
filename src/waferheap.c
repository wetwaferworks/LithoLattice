/* waferheap.c ... WaferHeap, a TLSF allocator written fresh for
 * LithoRoot from the two-level segregated fit design.
 *
 * Structure, in one paragraph: every block carries a two word header
 * (physical predecessor pointer, then size with flag bits in the low
 * bits vacated by alignment). Free blocks additionally store their
 * segregated-list links in the first two payload words. Free blocks
 * are indexed by a two-level bitmap scheme: the first level splits
 * sizes by power of two, the second splits each power-of-two range
 * into WH_SL_COUNT equal subranges, and two bitmaps (one first-level
 * word, one second-level word per first-level class) locate a
 * suitable block in a bounded number of bit operations. Freeing
 * coalesces with physical neighbors immediately, using the boundary
 * tags (the predecessor pointer plus a previous-is-free bit in every
 * header), so external fragmentation is repaired at free time, in
 * O(1), not by a later sweep.
 *
 * No dependencies beyond <stddef.h>, <stdint.h> and <string.h>.
 * No platform headers, no compiler intrinsics: the bit scans are
 * portable C99, isolated in three small functions below.
 */

#include "waferheap.h"

#include <string.h>

/* ---- configuration ---------------------------------------------------
 * WH_SL_LOG2: log2 of the second-level subdivision count. 4 gives 16
 * subranges per power-of-two range, which bounds the rounding waste
 * inside a range to under 1/16 of the range while keeping the
 * per-heap index small: WH_FL_COUNT * WH_SL_COUNT list heads plus
 * WH_FL_COUNT bitmap words.
 *
 * WH_ALIGN_LOG2: log2 of the guaranteed alignment (two words).
 *
 * WH_FL_COUNT: number of first-level classes. Derived so the largest
 * class covers the largest size_t value: classes below the small
 * limit share class 0, and each higher class steps one power of two.
 */
#define WH_SL_LOG2 4
#define WH_SL_COUNT (1 << WH_SL_LOG2)

#if defined(UINTPTR_MAX) && (UINTPTR_MAX > 0xffffffffu)
#define WH_PTR_BITS 64
#define WH_ALIGN_LOG2 4
#else
#define WH_PTR_BITS 32
#define WH_ALIGN_LOG2 3
#endif

#define WH_FL_COUNT (WH_PTR_BITS - WH_SL_LOG2 - WH_ALIGN_LOG2 + 1)

/* Sizes below this use the linear part of class 0 (one bucket per
 * alignment step); at and above it the log mapping takes over. */
#define WH_SMALL_LIMIT ((size_t)WH_SL_COUNT << WH_ALIGN_LOG2)

/* ---- blocks ------------------------------------------------------------
 * size_flags layout: the block size occupies the high bits (sizes
 * are WH_ALIGNMENT multiples, so the low WH_ALIGN_LOG2 bits are
 * spare); bit 0 marks the block free, bit 1 records that the
 * physical predecessor is free (the boundary tag that makes backward
 * coalescing O(1) together with prev_phys).
 */
#define WH_FLAG_FREE (size_t)0x1
#define WH_FLAG_PREV_FREE (size_t)0x2
#define WH_SIZE_MASK (~(size_t)(WH_ALIGNMENT - 1))

typedef struct wh_block {
  struct wh_block* prev_phys; /* physical predecessor header, or NULL */
  size_t size_flags;
  /* Free blocks only: segregated list links, in the payload area. */
  struct wh_block* next_free;
  struct wh_block* prev_free;
} wh_block_t;

#define WH_OVERHEAD (2u * sizeof(void*)) /* prev_phys + size_flags */
#define WH_MIN_PAYLOAD (2u * sizeof(void*)) /* room for the free links */
#define WH_MIN_BLOCK (WH_OVERHEAD + WH_MIN_PAYLOAD)

/* A region is one adopted extent. Its descriptor sits at the extent
 * base; blocks run from blocks_begin to a zero-size sentinel header
 * at blocks_end, so physical walks always terminate inside the
 * extent. */
typedef struct wh_region {
  struct wh_region* next;
  char* blocks_begin;
  char* blocks_end; /* address of the sentinel header */
} wh_region_t;

#define WH_REGION_HDR ((sizeof(wh_region_t) + WH_ALIGNMENT - 1) & \
                       ~(size_t)(WH_ALIGNMENT - 1))

struct wh_heap {
  uint32_t magic;
  wh_options_t opt;
  wh_region_t* regions;
  size_t fl_bitmap;
  uint32_t sl_bitmap[WH_FL_COUNT];
  wh_block_t* heads[WH_FL_COUNT][WH_SL_COUNT];
  size_t region_bytes;
  size_t used_bytes;
  size_t free_bytes;
  size_t peak_used_bytes;
  uint32_t live_blocks;
  uint32_t free_blocks_count;
};

#define WH_MAGIC 0x57484631u /* "WHF1" */

/* ---- portable bit scans --------------------------------------------------
 * Binary reduction, no tables, no intrinsics: constant bounded work
 * on every compiler, which is what the determinism story wants. */

static int wh_fls_size(size_t x) { /* index of highest set bit, or -1 */
  int r = 0;
  if (x == 0) return -1;
#if WH_PTR_BITS == 64
  if (x >> 32) { r += 32; x >>= 32; }
#endif
  if (x >> 16) { r += 16; x >>= 16; }
  if (x >> 8) { r += 8; x >>= 8; }
  if (x >> 4) { r += 4; x >>= 4; }
  if (x >> 2) { r += 2; x >>= 2; }
  if (x >> 1) { r += 1; }
  return r;
}

static int wh_ffs_size(size_t x) { /* index of lowest set bit, or -1 */
  int r = 0;
  if (x == 0) return -1;
#if WH_PTR_BITS == 64
  if ((x & (size_t)0xffffffffu) == 0) { r += 32; x >>= 32; }
#endif
  if ((x & 0xffffu) == 0) { r += 16; x >>= 16; }
  if ((x & 0xffu) == 0) { r += 8; x >>= 8; }
  if ((x & 0xfu) == 0) { r += 4; x >>= 4; }
  if ((x & 0x3u) == 0) { r += 2; x >>= 2; }
  if ((x & 0x1u) == 0) { r += 1; }
  return r;
}

static int wh_ffs_u32(uint32_t x) {
  int r = 0;
  if (x == 0) return -1;
  if ((x & 0xffffu) == 0) { r += 16; x >>= 16; }
  if ((x & 0xffu) == 0) { r += 8; x >>= 8; }
  if ((x & 0xfu) == 0) { r += 4; x >>= 4; }
  if ((x & 0x3u) == 0) { r += 2; x >>= 2; }
  if ((x & 0x1u) == 0) { r += 1; }
  return r;
}

/* ---- size mapping ---------------------------------------------------------
 * map_insert: the bucket a block of exactly this size belongs to.
 * map_search: the first bucket that can hold at least this size;
 * differs only in rounding the size up to the next bucket boundary
 * before mapping, so the search starts at a bucket whose smallest
 * member is still large enough. */

static void wh_map_insert(size_t size, int* fl, int* sl) {
  if (size < WH_SMALL_LIMIT) {
    *fl = 0;
    *sl = (int)(size >> WH_ALIGN_LOG2);
  } else {
    int f = wh_fls_size(size);
    *sl = (int)((size >> (f - WH_SL_LOG2)) - WH_SL_COUNT);
    *fl = f - (WH_SL_LOG2 + WH_ALIGN_LOG2) + 1;
  }
}

static void wh_map_search(size_t size, int* fl, int* sl) {
  if (size >= WH_SMALL_LIMIT) {
    int f = wh_fls_size(size);
    /* Round up by just under one subrange step at this magnitude. */
    size += ((size_t)1 << (f - WH_SL_LOG2)) - 1;
  }
  wh_map_insert(size, fl, sl);
}

/* ---- locking wrappers ----------------------------------------------------- */

static void wh_lock(wh_heap_t* h) {
  if (h->opt.lock) h->opt.lock(h->opt.lock_user);
}

static void wh_unlock(wh_heap_t* h) {
  if (h->opt.unlock) h->opt.unlock(h->opt.lock_user);
}

static int wh_valid(const wh_heap_t* h) {
  return h != NULL && h->magic == WH_MAGIC;
}

/* ---- block helpers (lock held by callers) --------------------------------- */

static size_t wh_block_size(const wh_block_t* b) {
  return b->size_flags & WH_SIZE_MASK;
}

static int wh_block_is_free(const wh_block_t* b) {
  return (b->size_flags & WH_FLAG_FREE) != 0;
}

static wh_block_t* wh_block_next_phys(wh_block_t* b) {
  return (wh_block_t*)((char*)b + wh_block_size(b));
}

static void* wh_block_ptr(wh_block_t* b) {
  return (char*)b + WH_OVERHEAD;
}

/* After b's size or free state changed, refresh the following
 * block's boundary tag. The following block may be a zero-size
 * sentinel, which still carries a valid header to update. */
static void wh_link_next(wh_heap_t* h, wh_block_t* b) {
  wh_block_t* next = wh_block_next_phys(b);
  (void)h;
  next->prev_phys = b;
  if (wh_block_is_free(b)) next->size_flags |= WH_FLAG_PREV_FREE;
  else next->size_flags &= ~WH_FLAG_PREV_FREE;
}

static void wh_insert_free(wh_heap_t* h, wh_block_t* b) {
  int fl, sl;
  wh_map_insert(wh_block_size(b), &fl, &sl);
  b->next_free = h->heads[fl][sl];
  b->prev_free = NULL;
  if (b->next_free) b->next_free->prev_free = b;
  h->heads[fl][sl] = b;
  h->fl_bitmap |= (size_t)1 << fl;
  h->sl_bitmap[fl] |= (uint32_t)1 << sl;
  h->free_bytes += wh_block_size(b) - WH_OVERHEAD;
  h->free_blocks_count += 1;
}

static void wh_remove_free(wh_heap_t* h, wh_block_t* b) {
  int fl, sl;
  wh_map_insert(wh_block_size(b), &fl, &sl);
  if (b->next_free) b->next_free->prev_free = b->prev_free;
  if (b->prev_free) b->prev_free->next_free = b->next_free;
  else h->heads[fl][sl] = b->next_free;
  if (h->heads[fl][sl] == NULL) {
    h->sl_bitmap[fl] &= ~((uint32_t)1 << sl);
    if (h->sl_bitmap[fl] == 0) h->fl_bitmap &= ~((size_t)1 << fl);
  }
  h->free_bytes -= wh_block_size(b) - WH_OVERHEAD;
  h->free_blocks_count -= 1;
}

/* Find and detach a free block of at least size payload capacity...
 * note the mapping works on whole block sizes, so callers pass the
 * needed block size (payload plus header), not the payload. */
static wh_block_t* wh_find_free(wh_heap_t* h, size_t block_size) {
  int fl, sl;
  wh_block_t* b;
  uint32_t sl_map;
  wh_map_search(block_size, &fl, &sl);
  sl_map = h->sl_bitmap[fl] & (~(uint32_t)0 << sl);
  if (sl_map == 0) {
    size_t fl_map = h->fl_bitmap & (~(size_t)0 << (fl + 1));
    if (fl_map == 0) return NULL;
    fl = wh_ffs_size(fl_map);
    sl_map = h->sl_bitmap[fl];
  }
  sl = wh_ffs_u32(sl_map);
  b = h->heads[fl][sl];
  if (b == NULL) return NULL; /* bitmap and list disagree: cannot happen */
  wh_remove_free(h, b);
  return b;
}

/* Carve size off the front of free block b (already detached). When
 * the remainder can stand as a block it becomes a new free block and
 * the following block's tags are fixed for both pieces; otherwise b
 * is handed over whole. */
static void wh_split(wh_heap_t* h, wh_block_t* b, size_t size) {
  size_t total = wh_block_size(b);
  if (total - size >= WH_MIN_BLOCK) {
    wh_block_t* rest = (wh_block_t*)((char*)b + size);
    rest->size_flags = (total - size) | WH_FLAG_FREE;
    rest->prev_phys = b;
    b->size_flags = size | (b->size_flags & WH_FLAG_PREV_FREE);
    wh_link_next(h, rest);
    wh_insert_free(h, rest);
  }
  wh_link_next(h, b);
}

/* Mark a detached free block as used and account for it. */
static void wh_take(wh_heap_t* h, wh_block_t* b) {
  b->size_flags &= ~WH_FLAG_FREE;
  wh_link_next(h, b);
  h->used_bytes += wh_block_size(b) - WH_OVERHEAD;
  if (h->used_bytes > h->peak_used_bytes) h->peak_used_bytes = h->used_bytes;
  h->live_blocks += 1;
}

/* ---- region adoption -------------------------------------------------------- */

static int wh_adopt_region(wh_heap_t* h, void* region, size_t region_bytes) {
  wh_region_t* r;
  wh_block_t* b;
  wh_block_t* sentinel;
  size_t usable;
  char* base;
  size_t skip;
  if (region == NULL) return 0;
  /* Extents are aligned up internally: a grow callback that hands
   * over slices of a larger buffer (the native pattern) is not
   * required to track our alignment, it only loses the skipped
   * bytes. Too small after alignment is still a refusal. */
  base = (char*)region;
  skip = (size_t)((WH_ALIGNMENT - ((uintptr_t)base & (WH_ALIGNMENT - 1))) &
                  (WH_ALIGNMENT - 1));
  if (region_bytes <= skip) return 0;
  base += skip;
  region_bytes -= skip;
  if (region_bytes < WH_REGION_HDR + WH_MIN_BLOCK + WH_OVERHEAD) return 0;
  r = (wh_region_t*)base;
  usable = region_bytes - WH_REGION_HDR;
  usable &= ~(size_t)(WH_ALIGNMENT - 1);
  r->blocks_begin = base + WH_REGION_HDR;
  r->blocks_end = r->blocks_begin + usable - WH_OVERHEAD;
  b = (wh_block_t*)r->blocks_begin;
  b->prev_phys = NULL;
  b->size_flags = (usable - WH_OVERHEAD) | WH_FLAG_FREE;
  sentinel = (wh_block_t*)r->blocks_end;
  sentinel->prev_phys = b;
  sentinel->size_flags = WH_FLAG_PREV_FREE; /* size 0, used, prev free */
  r->next = h->regions;
  h->regions = r;
  h->region_bytes += region_bytes;
  wh_insert_free(h, b);
  return 1;
}

/* Grow once through the callback and adopt whatever comes back.
 * Returns 1 when a region was adopted. Lock is held by the caller
 * on entry and on return, but (review M7) it is RELEASED around the
 * callback, so a grow implementation may allocate, log through
 * malloc, or otherwise re-enter the heap without deadlocking on a
 * non-recursive lock. Callers re-search the free lists afterwards
 * (another thread may have grown or freed meanwhile; two
 * concurrent growers simply both add a region). If the heap was
 * destroyed while unlocked the extent is not adopted. */
static int wh_try_grow(wh_heap_t* h, size_t min_extra) {
  size_t granted = 0;
  void* extent;
  wh_grow_fn grow = h->opt.grow;
  void* user = h->opt.grow_user;
  if (grow == NULL) return 0;
  wh_unlock(h);
  extent = grow(h, min_extra, &granted, user);
  wh_lock(h);
  if (extent == NULL || granted == 0 || h->magic != WH_MAGIC) return 0;
  return wh_adopt_region(h, extent, granted);
}

/* ---- pointer decoding -------------------------------------------------------
 * Plain allocations: the header is WH_OVERHEAD bytes before the
 * payload pointer. Over-aligned allocations (wh_aligned_alloc)
 * store, in the word immediately before the returned pointer, the
 * distance back to the header with the low bit set as a tag. The
 * word before a plain payload is the block's size_flags, whose bit
 * 0 (WH_FLAG_FREE) is 0 on every LIVE block, so a live plain block
 * cannot be mistaken for an over-aligned one. A FREED plain block
 * has that bit set by definition, and an interior pointer's
 * preceding word is payload data, so the raw decode can manufacture
 * a phantom candidate out of either. The candidate is therefore
 * never trusted on its own: wh_chain_valid proves it is a real
 * block boundary on its region's physical chain before any caller
 * acts on it, and a candidate that fails is foreign, which every
 * caller treats as a safe no-op. Returns the block header, or NULL
 * when the pointer does not decode to a proven block of this heap. */

/* Chain validation (audit fix A-1, 2026-10-28). The candidate must
 * start on the region's block grid, carry a whole-block size that
 * ends at or before the sentinel, contain the payload pointer
 * strictly inside its span, be named by its successor's boundary
 * tag, and be exactly where its predecessor ends (the first block
 * of a region has no predecessor). Every dereference is preceded
 * by a range check against the region, so a phantom candidate is
 * convicted without ever reading outside the region: mid-payload
 * bytes essentially never satisfy all five facts at once, because
 * the successor's prev_phys word would have to be the candidate's
 * own address sitting at exactly the candidate's claimed size. */
static int wh_chain_valid(wh_region_t* r, wh_block_t* b, size_t off) {
  size_t size;
  wh_block_t* next;
  wh_block_t* prev;
  if ((const char*)b < r->blocks_begin || (const char*)b >= r->blocks_end)
    return 0;
  if (((size_t)((const char*)b - r->blocks_begin) & (WH_ALIGNMENT - 1)) != 0)
    return 0;
  size = wh_block_size(b);
  if (size < WH_MIN_BLOCK) return 0;
  if (size > (size_t)(r->blocks_end - (const char*)b)) return 0;
  if (off >= size) return 0; /* the payload start lives inside b */
  next = (wh_block_t*)((char*)b + size);
  if (next->prev_phys != b) return 0;
  prev = b->prev_phys;
  if ((const char*)b == r->blocks_begin) return prev == NULL;
  if (prev == NULL) return 0;
  if ((const char*)prev < r->blocks_begin || (const char*)prev >= (const char*)b)
    return 0;
  if (((size_t)((const char*)prev - r->blocks_begin) & (WH_ALIGNMENT - 1)) != 0)
    return 0;
  if (wh_block_size(prev) < WH_MIN_BLOCK) return 0;
  /* Ends-exactly-at-b, compared in offset space: prev's size
   * field is not yet trusted, and forming prev + size as a
   * pointer could overflow before the comparison ran. */
  if (wh_block_size(prev) != (size_t)((const char*)b - (const char*)prev))
    return 0;
  return 1;
}

static wh_block_t* wh_decode(wh_heap_t* h, const void* ptr, size_t* offset) {
  const char* p = (const char*)ptr;
  size_t word;
  size_t off;
  wh_block_t* b;
  wh_region_t* r;
  if (ptr == NULL) return NULL;
  /* Gate fix (Piece 3 review, 2026-10-28): validate region membership
   * BEFORE the first dereference. The decode below reads the word before
   * the payload; for a foreign pointer at the start of a mapped region
   * that read crosses below the mapping. A legitimate payload always
   * sits at least WH_OVERHEAD into a region's block range, so this
   * check rejects no real pointer and touches no memory. */
  r = NULL;
  {
    wh_region_t* it;
    for (it = h->regions; it != NULL; it = it->next) {
      if (p >= it->blocks_begin + WH_OVERHEAD && p < it->blocks_end) { r = it; break; }
    }
  }
  if (r == NULL) return NULL;
  word = *(const size_t*)(p - sizeof(size_t));
  if (word & (size_t)1) {
    off = word & ~(size_t)1;
    if (off < WH_OVERHEAD + sizeof(size_t)) return NULL;
    /* The candidate header must land inside this region. Test
     * the offset in integer space BEFORE forming the pointer: a
     * manufactured word can carry an offset so large that the
     * subtraction itself is undefined (pointer arithmetic
     * overflow; the sanitizer gate's bad-free cases trip
     * exactly that). */
    if (off > (size_t)(p - r->blocks_begin)) return NULL;
    b = (wh_block_t*)(p - off);
  } else {
    off = WH_OVERHEAD;
    b = (wh_block_t*)(p - WH_OVERHEAD);
  }
  if (!wh_chain_valid(r, b, off)) return NULL;
  if (offset) *offset = off;
  return b;
}

/* ---- internal allocation core (lock held) ----------------------------------- */

static void* wh_alloc_core(wh_heap_t* h, size_t block_size, int may_grow) {
  wh_block_t* b = wh_find_free(h, block_size);
  if (b == NULL && may_grow) {
    /* Ask for the need plus slack, so growth is amortized: the host
     * still decides (on wasm its cap is law), and a refusal or a
     * short grant that cannot serve the request ends the attempt. */
    size_t extra = block_size + block_size / 2 + WH_REGION_HDR + WH_OVERHEAD;
    if (wh_try_grow(h, extra)) b = wh_find_free(h, block_size);
  }
  if (b == NULL) return NULL;
  wh_split(h, b, block_size);
  wh_take(h, b);
  return wh_block_ptr(b);
}

/* Round a payload request up to a whole block size, with guards.
 * Requests above half the address space are refused outright: no
 * arena can serve them, and the refusal keeps every later addition
 * (rounding, header, grow slack) far away from wraparound. */
static int wh_block_size_for(size_t size, size_t* out) {
  size_t payload = size < WH_MIN_PAYLOAD ? WH_MIN_PAYLOAD : size;
  size_t total;
  if (size > (size_t)-1 / 2) return 0;
  payload = (payload + WH_ALIGNMENT - 1) & ~(size_t)(WH_ALIGNMENT - 1);
  total = payload + WH_OVERHEAD;
  if (total < payload) return 0; /* wrapped */
  *out = total;
  return 1;
}

/* ---- public: lifecycle -------------------------------------------------------- */

size_t wh_heap_context_size(void) {
  return (sizeof(struct wh_heap) + WH_ALIGNMENT - 1) &
         ~(size_t)(WH_ALIGNMENT - 1);
}

wh_heap_t* wh_heap_init(void* context_mem, void* arena, size_t arena_bytes,
                        const wh_options_t* options) {
  wh_heap_t* h;
  if (context_mem == NULL) return NULL;
  if (((uintptr_t)context_mem & (WH_ALIGNMENT - 1)) != 0) return NULL;
  h = (wh_heap_t*)context_mem;
  memset(h, 0, sizeof(*h));
  if (options) h->opt = *options;
  h->magic = WH_MAGIC;
  if (!wh_adopt_region(h, arena, arena_bytes)) {
    h->magic = 0;
    return NULL;
  }
  return h;
}

wh_heap_t* wh_heap_create(void* arena, size_t arena_bytes,
                          const wh_options_t* options) {
  size_t ctx = wh_heap_context_size();
  char* base;
  if (arena == NULL) return NULL;
  if (((uintptr_t)arena & (WH_ALIGNMENT - 1)) != 0) return NULL;
  if (arena_bytes < ctx + WH_REGION_HDR + WH_MIN_BLOCK + WH_OVERHEAD)
    return NULL;
  base = (char*)arena;
  return wh_heap_init(base, base + ctx, arena_bytes - ctx, options);
}

void wh_heap_destroy(wh_heap_t* heap) {
  if (!wh_valid(heap)) return;
  wh_lock(heap);
  heap->magic = 0;
  wh_unlock(heap);
}

int wh_heap_add_region(wh_heap_t* heap, void* region, size_t region_bytes) {
  int ok;
  if (!wh_valid(heap)) return 0;
  wh_lock(heap);
  ok = wh_adopt_region(heap, region, region_bytes);
  wh_unlock(heap);
  return ok;
}

/* ---- public: allocation -------------------------------------------------------- */

void* wh_malloc(wh_heap_t* heap, size_t size) {
  size_t block_size;
  void* out;
  if (!wh_valid(heap)) return NULL;
  if (!wh_block_size_for(size, &block_size)) return NULL;
  wh_lock(heap);
  out = wh_alloc_core(heap, block_size, 1);
  wh_unlock(heap);
  return out;
}

void wh_free(wh_heap_t* heap, void* ptr) {
  wh_block_t* b;
  wh_block_t* next;
  if (!wh_valid(heap) || ptr == NULL) return;
  wh_lock(heap);
  b = wh_decode(heap, ptr, NULL);
  if (b == NULL || wh_block_is_free(b)) {
    wh_unlock(heap);
    return;
  }
  heap->used_bytes -= wh_block_size(b) - WH_OVERHEAD;
  heap->live_blocks -= 1;
  b->size_flags |= WH_FLAG_FREE;

  /* Immediate coalescing, boundary tags both directions. Each
   * neighbor merge restores one list removal, so free stays O(1). */
  next = wh_block_next_phys(b);
  if (wh_block_size(next) != 0 && wh_block_is_free(next)) {
    wh_remove_free(heap, next);
    b->size_flags = (wh_block_size(b) + wh_block_size(next)) |
                    WH_FLAG_FREE | (b->size_flags & WH_FLAG_PREV_FREE);
  }
  if (b->size_flags & WH_FLAG_PREV_FREE) {
    wh_block_t* prev = b->prev_phys;
    if (prev != NULL && wh_block_is_free(prev)) {
      wh_remove_free(heap, prev);
      prev->size_flags = (wh_block_size(prev) + wh_block_size(b)) |
                         WH_FLAG_FREE | (prev->size_flags & WH_FLAG_PREV_FREE);
      b = prev;
    }
  }
  wh_link_next(heap, b);
  wh_insert_free(heap, b);
  wh_unlock(heap);
}

void* wh_realloc(wh_heap_t* heap, void* ptr, size_t size) {
  wh_block_t* b;
  size_t want;
  size_t offset = 0;
  if (!wh_valid(heap)) return NULL;
  if (ptr == NULL) return wh_malloc(heap, size);
  if (size == 0) {
    wh_free(heap, ptr);
    return NULL;
  }
  if (!wh_block_size_for(size, &want)) return NULL;
  wh_lock(heap);
  b = wh_decode(heap, ptr, &offset);
  if (b == NULL) {
    wh_unlock(heap);
    return NULL;
  }
  if (offset != WH_OVERHEAD) {
    /* Over-aligned block: geometry is not the plain one, so take
     * the always-correct path of allocate, copy, free. */
    size_t old_usable = (size_t)((char*)b + wh_block_size(b) - (char*)ptr);
    size_t copy = old_usable < size ? old_usable : size;
    void* fresh;
    wh_unlock(heap);
    fresh = wh_malloc(heap, size);
    if (fresh != NULL) {
      memcpy(fresh, ptr, copy);
      wh_free(heap, ptr);
    }
    return fresh;
  }
  if (want <= wh_block_size(b)) {
    /* Shrink (or same): split in place when the tail stands alone. */
    if (wh_block_size(b) - want >= WH_MIN_BLOCK) {
      wh_block_t* rest = (wh_block_t*)((char*)b + want);
      size_t old = wh_block_size(b);
      heap->used_bytes -= old - want;
      rest->size_flags = (old - want) | WH_FLAG_FREE;
      rest->prev_phys = b;
      b->size_flags = want | (b->size_flags & WH_FLAG_PREV_FREE);
      wh_link_next(heap, rest);
      /* The freshly freed tail may absorb a free successor. */
      {
        wh_block_t* after = wh_block_next_phys(rest);
        if (wh_block_size(after) != 0 && wh_block_is_free(after)) {
          wh_remove_free(heap, after);
          rest->size_flags = (wh_block_size(rest) + wh_block_size(after)) |
                             WH_FLAG_FREE |
                             (rest->size_flags & WH_FLAG_PREV_FREE);
          wh_link_next(heap, rest);
        }
      }
      wh_insert_free(heap, rest);
    }
    wh_unlock(heap);
    return ptr;
  }
  /* Grow: try absorbing a free physical successor first. */
  {
    wh_block_t* next = wh_block_next_phys(b);
    if (wh_block_size(next) != 0 && wh_block_is_free(next)) {
      size_t old = wh_block_size(b);
      size_t merged = old + wh_block_size(next);
      if (merged >= want) {
        wh_remove_free(heap, next);
        b->size_flags = merged | (b->size_flags & WH_FLAG_PREV_FREE);
        heap->used_bytes += merged - old;
        if (heap->used_bytes > heap->peak_used_bytes)
          heap->peak_used_bytes = heap->used_bytes;
        wh_link_next(heap, b);
        wh_split(heap, b, want); /* may carve a free remainder */
        /* The split remainder went back to the free accounting, so
         * used_bytes (raised to the merged whole above) must settle
         * on the block's final size. Zero when no split happened. */
        heap->used_bytes -= merged - wh_block_size(b);
        wh_unlock(heap);
        return ptr;
      }
    }
  }
  /* Move: allocate, copy the old payload capacity, free. */
  {
    size_t old_payload = wh_block_size(b) - WH_OVERHEAD;
    size_t copy = old_payload < size ? old_payload : size;
    void* fresh;
    wh_unlock(heap);
    fresh = wh_malloc(heap, size);
    if (fresh != NULL) {
      memcpy(fresh, ptr, copy);
      wh_free(heap, ptr);
    }
    return fresh;
  }
}

void* wh_calloc(wh_heap_t* heap, size_t count, size_t size) {
  size_t total;
  void* p;
  if (count != 0 && size > (size_t)-1 / count) return NULL;
  total = count * size;
  p = wh_malloc(heap, total);
  if (p != NULL) memset(p, 0, total < WH_MIN_PAYLOAD ? WH_MIN_PAYLOAD : total);
  return p;
}

void* wh_aligned_alloc(wh_heap_t* heap, size_t alignment, size_t size) {
  size_t block_size;
  wh_block_t* b;
  char* raw;
  char* aligned;
  size_t offset;
  if (!wh_valid(heap)) return NULL;
  if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0)
    return NULL;
  if (alignment <= WH_ALIGNMENT) return wh_malloc(heap, size);
  /* Over-allocate so an aligned payload start exists with room for
   * the tagged back-offset word just before it. The extra word in
   * the request covers the worst-case padding arithmetic exactly:
   * pad below alignment, plus the back-offset word itself. */
  if (size > (size_t)-1 - alignment - sizeof(size_t)) return NULL;
  if (!wh_block_size_for(size + alignment + sizeof(size_t), &block_size))
    return NULL;
  wh_lock(heap);
  b = NULL;
  {
    void* mem = wh_alloc_core(heap, block_size, 1);
    if (mem != NULL) b = (wh_block_t*)((char*)mem - WH_OVERHEAD);
    raw = (char*)mem;
  }
  if (b == NULL) {
    wh_unlock(heap);
    return NULL;
  }
  aligned = (char*)(((uintptr_t)(raw + sizeof(size_t)) + alignment - 1) &
                    ~(uintptr_t)(alignment - 1));
  offset = (size_t)(aligned - (char*)b);
  *(size_t*)(aligned - sizeof(size_t)) = offset | (size_t)1;
  wh_unlock(heap);
  return aligned;
}

size_t wh_malloc_usable_size(wh_heap_t* heap, const void* ptr) {
  wh_block_t* b;
  size_t result = 0;
  if (!wh_valid(heap) || ptr == NULL) return 0;
  wh_lock(heap);
  b = wh_decode(heap, ptr, NULL);
  if (b != NULL && !wh_block_is_free(b))
    result = (size_t)((char*)b + wh_block_size(b) - (const char*)ptr);
  wh_unlock(heap);
  return result;
}

/* ---- public: introspection ------------------------------------------------------ */

void wh_heap_stats(wh_heap_t* heap, wh_stats_t* out_stats) {
  int fl, sl;
  if (out_stats == NULL) return;
  memset(out_stats, 0, sizeof(*out_stats));
  if (!wh_valid(heap)) return;
  wh_lock(heap);
  out_stats->region_bytes = heap->region_bytes;
  out_stats->used_bytes = heap->used_bytes;
  out_stats->free_bytes = heap->free_bytes;
  out_stats->peak_used_bytes = heap->peak_used_bytes;
  out_stats->live_blocks = heap->live_blocks;
  out_stats->free_blocks = heap->free_blocks_count;
  out_stats->largest_free_block = 0;
  for (fl = 0; fl < WH_FL_COUNT; fl++) {
    for (sl = 0; sl < WH_SL_COUNT; sl++) {
      wh_block_t* b;
      for (b = heap->heads[fl][sl]; b != NULL; b = b->next_free) {
        size_t payload = wh_block_size(b) - WH_OVERHEAD;
        if (payload > out_stats->largest_free_block)
          out_stats->largest_free_block = payload;
      }
    }
  }
  wh_unlock(heap);
}

/* ---- default heap layer ------------------------------------------------------------ */

static wh_heap_t* wh_default = NULL;

void wh_set_default_heap(wh_heap_t* heap) { wh_default = heap; }

wh_heap_t* wh_default_heap(void) { return wh_default; }

void* wh_libc_malloc(size_t size) {
  return wh_default ? wh_malloc(wh_default, size) : NULL;
}

void wh_libc_free(void* ptr) {
  if (wh_default) wh_free(wh_default, ptr);
}

void* wh_libc_realloc(void* ptr, size_t size) {
  return wh_default ? wh_realloc(wh_default, ptr, size) : NULL;
}

void* wh_libc_calloc(size_t count, size_t size) {
  return wh_default ? wh_calloc(wh_default, count, size) : NULL;
}

void* wh_libc_aligned_alloc(size_t alignment, size_t size) {
  return wh_default ? wh_aligned_alloc(wh_default, alignment, size) : NULL;
}
