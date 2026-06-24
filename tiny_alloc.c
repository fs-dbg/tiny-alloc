// tiny_alloc.c
// a small malloc/free clone running on top of a fixed static buffer.
// no real syscalls involved (no sbrk/mmap), just carving up one big array.
// good enough to understand how a free list allocator actually works,
// not good enough to replace real malloc lol

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define ARENA_SIZE (1024 * 64)   // 64kb playground, plenty for testing
#define ALIGN 8                  // keep blocks 8 byte aligned, pointers like that

// the arena is just a dumb byte array, this is our "heap"
static unsigned char arena[ARENA_SIZE];

// every allocation gets a little header stuck right before it in memory.
// so the actual layout in the arena looks like: [header][user data][header][user data]...
// free() just flips a flag in the header, it doesn't move anything around
typedef struct block_header {
    size_t size;              // size of the usable region after this header
    int free;                 // 1 if this block is up for grabs
    struct block_header *next; // next block in the list, list is just memory order
} block_header_t;

// pointer to start of the list, lives at the very front of the arena
static block_header_t *free_list_head = NULL;

// round a size up to the nearest multiple of ALIGN
// classic bit trick: add (align-1) then mask off the low bits
static size_t align_up(size_t n) {
    return (n + (ALIGN - 1)) & ~(ALIGN - 1);
}

// one time setup, carve the whole arena into a single big free block
static void arena_init(void) {
    free_list_head = (block_header_t *)arena;
    free_list_head->size = ARENA_SIZE - sizeof(block_header_t);
    free_list_head->free = 1;
    free_list_head->next = NULL;
}

// walk the list looking for the first block that's free and big enough.
// "first fit" strategy, not the smartest but simplest to reason about
static block_header_t *find_free_block(size_t size) {
    block_header_t *cur = free_list_head;
    while (cur) {
        if (cur->free && cur->size >= size) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL; // nothing fits, caller has to deal with that
}

// if a block is way bigger than what's needed, chop it in two
// so we don't waste a ton of space on small requests.
// only worth doing if the leftover chunk can actually hold a header + something
static void split_block(block_header_t *blk, size_t size) {
    size_t leftover = blk->size - size;
    if (leftover <= sizeof(block_header_t)) {
        return; // not worth splitting, just let it be a bit wasteful
    }

    unsigned char *raw = (unsigned char *)blk;
    block_header_t *new_blk = (block_header_t *)(raw + sizeof(block_header_t) + size);

    new_blk->size = leftover - sizeof(block_header_t);
    new_blk->free = 1;
    new_blk->next = blk->next;

    blk->size = size;
    blk->next = new_blk;
}

void *tiny_malloc(size_t size) {
    if (size == 0) return NULL;

    if (!free_list_head) {
        arena_init(); // lazy init on first call so caller doesn't have to remember
    }

    size = align_up(size);

    block_header_t *blk = find_free_block(size);
    if (!blk) {
        // out of memory in our fake arena, real malloc would ask the os for more
        // we just give up
        return NULL;
    }

    split_block(blk, size);
    blk->free = 0;

    // user gets a pointer just past the header, never sees the header itself
    return (void *)((unsigned char *)blk + sizeof(block_header_t));
}

// turn a user pointer back into the header sitting right before it
static block_header_t *header_from_ptr(void *ptr) {
    return (block_header_t *)((unsigned char *)ptr - sizeof(block_header_t));
}

// merge a block with its immediate neighbor in the list if that neighbor
// is also free. this is the thing that stops the heap from turning into
// a million tiny unusable fragments over time
static void coalesce(block_header_t *blk) {
    if (blk->next && blk->next->free) {
        blk->size += sizeof(block_header_t) + blk->next->size;
        blk->next = blk->next->next;
        // note: we only merge forward here, not backward.
        // a real allocator usually checks both directions, keeping this
        // one simple on purpose
    }
}

void tiny_free(void *ptr) {
    if (!ptr) return; // freeing null is a no-op, same as real free

    block_header_t *blk = header_from_ptr(ptr);
    blk->free = 1;

    coalesce(blk);
}

// quick and dirty way to see what the heap looks like, block by block.
// not part of "the allocator" really, just a debug helper
void tiny_dump(void) {
    block_header_t *cur = free_list_head;
    int i = 0;
    while (cur) {
        printf("block %d: size=%zu free=%d addr=%p\n",
               i, cur->size, cur->free, (void *)cur);
        cur = cur->next;
        i++;
    }
}

#ifdef TINY_ALLOC_DEMO
int main(void) {
    printf("-- fresh allocator --\n");
    tiny_dump();

    void *a = tiny_malloc(100);
    void *b = tiny_malloc(200);
    void *c = tiny_malloc(50);

    printf("\n-- after 3 allocs --\n");
    tiny_dump();

    tiny_free(b);
    printf("\n-- after freeing the middle one --\n");
    tiny_dump();

    void *d = tiny_malloc(180); // should NOT fit in b's old spot (200 vs 180 ok actually)
    printf("\n-- after another alloc, should reuse freed space --\n");
    tiny_dump();

    tiny_free(a);
    tiny_free(c);
    tiny_free(d);

    printf("\n-- after freeing everything, should coalesce back down --\n");
    tiny_dump();

    return 0;
}
#endif
