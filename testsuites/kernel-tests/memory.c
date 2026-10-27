#define TEST_NAME "memory"
#include "test.h"

static int child_status(void (*body)(volatile u8 *), volatile u8 *page) {
    s64 child = fork();
    if (child == 0) {
        body(page);
        exit(0);
    }
    int status = -1;
    waitpid(child, &status, 0);
    return status;
}

static void read_page(volatile u8 *page) { (void)page[0]; }

static void write_page(volatile u8 *page) { page[0] = 1; }

static void anonymous_mapping(void) {
    enum { PAGES = 256 };
    u8 *area = mmap(0, PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    expect((s64)area > 0, "a 1 MiB anonymous mapping is granted");
    int zero = 1;
    for (u64 i = 0; i < PAGES * PAGE_SIZE; i++) zero &= area[i] == 0;
    expect(zero, "fresh anonymous memory reads as zero");
    for (u64 page = 0; page < PAGES; page++) area[page * PAGE_SIZE + page % 64] = (u8)page;
    int kept = 1;
    for (u64 page = 0; page < PAGES; page++) kept &= area[page * PAGE_SIZE + page % 64] == (u8)page;
    expect(kept, "every page keeps what was written to it");
    expect_eq(munmap(area, PAGES * PAGE_SIZE), 0, "munmap releases it");
    expect_eq(killed_by(child_status(read_page, area)), SIGSEGV,
              "touching an unmapped page raises SIGSEGV");
}

static void protection(void) {
    u8 *page = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    page[0] = 0x5a;
    expect_eq(mprotect(page, PAGE_SIZE, PROT_READ), 0, "mprotect makes a page read-only");
    expect_eq(killed_by(child_status(write_page, page)), SIGSEGV, "writing to it raises SIGSEGV");
    expect_eq(page[0], 0x5a, "reading it still works");
    munmap(page, PAGE_SIZE);
}

static void copy_on_write(void) {
    u8 *page = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    page[0] = 1;
    s64 child = fork();
    if (child == 0) {
        page[0] = 2;
        exit(page[0]);
    }
    int status;
    waitpid(child, &status, 0);
    expect_eq(exited_with(status), 2, "a child writes its own copy of a private page");
    expect_eq(page[0], 1, "the parent's copy is untouched");
    munmap(page, PAGE_SIZE);
}

static void shared_mapping(void) {
    volatile u8 *page = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS);
    page[0] = 1;
    s64 child = fork();
    if (child == 0) {
        page[0] = 7;
        exit(0);
    }
    int status;
    waitpid(child, &status, 0);
    expect_eq(page[0], 7, "a shared mapping shows the child's write to the parent");
    munmap((void *)page, PAGE_SIZE);
}

static void program_break(void) {
    u64 start = brk(0);
    expect(start > 0, "brk reports the current break");
    u64 end = brk(start + 64 * 1024);
    expect_eq((s64)end, (s64)(start + 64 * 1024), "the break grows by 64 KiB");
    volatile u8 *heap = (u8 *)start;
    heap[0] = 3;
    heap[64 * 1024 - 1] = 4;
    expect(heap[0] == 3 && heap[64 * 1024 - 1] == 4, "the grown heap is usable");
    expect_eq((s64)brk(start), (s64)start, "the break shrinks back");
}

static void probe_page(volatile u8 *page) { (void)page[0]; }

static void many_mappings(void) {
    enum { PAGES = 4096 };
    u8 *base = mmap(0, PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    expect((s64)base > 0, "a 16 MiB window is reserved");
    int placed = 1;
    for (u64 page = 0; page < PAGES; page++) {
        u8 *at = base + page * PAGE_SIZE;
        void *got =
            mmap(at, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
        placed &= got == at;
        if (got == at) at[0] = (u8)(page * 7);
    }
    expect(placed, "4096 single-page mappings replace the window one by one");
    int unmapped = 1;
    for (u64 page = 3; page < PAGES; page += 5)
        unmapped &= munmap(base + page * PAGE_SIZE, PAGE_SIZE) == 0;
    expect(unmapped, "every fifth page is unmapped again");
    expect_eq(mprotect(base + 100 * PAGE_SIZE, 50 * PAGE_SIZE, PROT_READ), 0,
              "mprotect spans many mappings and holes");
    int kept = 1;
    for (u64 page = 0; page < PAGES; page++) {
        if (page % 5 == 3) continue;
        kept &= base[page * PAGE_SIZE] == (u8)(page * 7);
    }
    expect(kept, "the surviving mappings keep their contents");
    expect_eq(killed_by(child_status(probe_page, base + 8 * PAGE_SIZE)), SIGSEGV,
              "a hole punched between mappings raises SIGSEGV");
    expect_eq(killed_by(child_status(write_page, base + 120 * PAGE_SIZE)), SIGSEGV,
              "a page inside the read-only range refuses writes");
    expect_eq(killed_by(child_status(write_page, base + 151 * PAGE_SIZE)), 0,
              "the page after the read-only range is still writable");
    void *again = mmap(base + 8 * PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
    expect(again == base + 8 * PAGE_SIZE, "a hole can be mapped again");
    expect_eq(base[8 * PAGE_SIZE], 0, "and reads as zero");
    expect_eq(munmap(base, PAGES * PAGE_SIZE), 0, "the whole window is unmapped at once");
    expect_eq(killed_by(child_status(probe_page, base + 2000 * PAGE_SIZE)), SIGSEGV,
              "nothing of it is left behind");
    u8 *fresh = mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    expect((s64)fresh > 0, "the address space still hands out new mappings");
    fresh[0] = 9;
    expect_eq(fresh[0], 9, "and they work");
    munmap(fresh, PAGE_SIZE);
}

static void run(int argc, char **argv) {
    (void)argc;
    (void)argv;
    anonymous_mapping();
    protection();
    copy_on_write();
    shared_mapping();
    program_break();
    many_mappings();
}
