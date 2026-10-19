#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "framework.hpp"
#include "helpers.hpp"

using posix_tests::exit_code;
using posix_tests::TempDir;

namespace {

sigjmp_buf recovery;
void *volatile fault_address;
volatile uintptr_t handler_stack;

void on_fault(int, siginfo_t *info, void *) {
    int marker = 0;
    handler_stack = reinterpret_cast<uintptr_t>(&marker);
    fault_address = info->si_addr;
    siglongjmp(recovery, 1);
}

void catch_faults(int flags = 0) {
    struct sigaction action = {};
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
}

const long page = sysconf(_SC_PAGESIZE);

}

TEST(memory, mlock_accepts_mapped_pages_and_refuses_holes) {
    const size_t size = 4 * 4096;
    void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    CHECK_OK(mlock(mapping, size));
    CHECK_OK(munlock(mapping, size));
    CHECK_OK(munmap(static_cast<char *>(mapping) + 4096, 4096));
    CHECK_ERRNO(mlock(mapping, size), ENOMEM);
    CHECK_OK(munmap(mapping, size));
}

TEST(memory, anonymous_mappings_start_zeroed) {
    size_t length = 16 * (size_t)page;
    auto *bytes = static_cast<unsigned char *>(
        mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(bytes != MAP_FAILED);
    bool zero = true;
    for (size_t i = 0; i < length; i++) zero = zero && bytes[i] == 0;
    CHECK(zero);
    std::memset(bytes, 0xAB, length);
    CHECK_EQ((int)bytes[length - 1], 0xAB);
    CHECK_OK(munmap(bytes, length));
}

TEST(memory, a_shared_mapping_is_seen_across_fork) {
    auto *shared = static_cast<int *>(
        mmap(nullptr, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    CHECK(shared != MAP_FAILED);
    *shared = 1;
    pid_t child = fork();
    if (child == 0) {
        *shared = 2;
        _exit(0);
    }
    CHECK_EQ(exit_code(child), 0);
    CHECK_EQ(*shared, 2);
}

TEST(memory, a_private_mapping_is_copied_on_write) {
    auto *privately = static_cast<int *>(
        mmap(nullptr, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    *privately = 1;
    pid_t child = fork();
    if (child == 0) {
        *privately = 2;
        _exit(*privately == 2 ? 0 : 1);
    }
    CHECK_EQ(exit_code(child), 0);
    CHECK_EQ(*privately, 1);
}

TEST(memory, a_shared_file_mapping_writes_through_to_the_file) {
    TempDir dir;
    int fd = open("mapped", O_RDWR | O_CREAT, 0644);
    CHECK_OK(ftruncate(fd, page));
    auto *text =
        static_cast<char *>(mmap(nullptr, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    CHECK(text != MAP_FAILED);
    std::memcpy(text, "mapped write", 12);
    CHECK_OK(msync(text, (size_t)page, MS_SYNC));
    char back[13] = {};
    CHECK_EQ(pread(fd, back, 12, 0), 12L);
    CHECK_EQ(std::string(back), std::string("mapped write"));
    CHECK_EQ(pwrite(fd, "FILE", 4, 0), 4L);
    CHECK_EQ(std::string(text, 4), std::string("FILE"));
    munmap(text, (size_t)page);
    close(fd);
}

TEST(memory, a_private_file_mapping_does_not_change_the_file) {
    TempDir dir;
    int fd = open("mapped", O_RDWR | O_CREAT, 0644);
    CHECK_EQ(write(fd, "original", 8), 8L);
    auto *text = static_cast<char *>(
        mmap(nullptr, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0));
    CHECK(text != MAP_FAILED);
    CHECK_EQ(std::string(text, 8), std::string("original"));
    std::memcpy(text, "CHANGED!", 8);
    char back[9] = {};
    CHECK_EQ(pread(fd, back, 8, 0), 8L);
    CHECK_EQ(std::string(back), std::string("original"));
    munmap(text, (size_t)page);
    close(fd);
}

TEST(memory, writing_read_only_memory_raises_sigsegv_at_the_address) {
    catch_faults();
    auto *bytes = static_cast<char *>(
        mmap(nullptr, (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK_OK(mprotect(bytes, (size_t)page, PROT_READ));
    fault_address = nullptr;
    if (sigsetjmp(recovery, 1) == 0) {
        bytes[100] = 1;
        CHECK(!"the write did not fault");
    }
    CHECK(fault_address == bytes + 100);
    CHECK_OK(mprotect(bytes, (size_t)page, PROT_READ | PROT_WRITE));
    bytes[100] = 1;
    CHECK_EQ((int)bytes[100], 1);
}

TEST(memory, touching_an_unmapped_page_raises_sigsegv) {
    catch_faults();
    auto *bytes = static_cast<char *>(mmap(nullptr, 2 * (size_t)page, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK_OK(munmap(bytes + page, (size_t)page));
    fault_address = nullptr;
    if (sigsetjmp(recovery, 1) == 0) {
        volatile char value = bytes[page + 8];
        (void)value;
        CHECK(!"the read did not fault");
    }
    CHECK(fault_address == bytes + page + 8);
}

TEST(memory, sigaltstack_runs_the_handler_on_its_own_stack) {
    size_t size = 64 * 1024;
    auto *stack = static_cast<char *>(std::malloc(size));
    stack_t alternate = {};
    alternate.ss_sp = stack;
    alternate.ss_size = size;
    CHECK_OK(sigaltstack(&alternate, nullptr));
    catch_faults(SA_ONSTACK);
    handler_stack = 0;
    if (sigsetjmp(recovery, 1) == 0) {
        volatile char *nowhere = nullptr;
        *nowhere = 1;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(stack);
    CHECK(handler_stack >= base);
    CHECK(handler_stack < base + size);
}

TEST(memory, malloc_serves_large_and_small_blocks) {
    std::vector<void *> blocks;
    for (int i = 0; i < 10000; i++) blocks.push_back(std::malloc((size_t)(i % 512) + 1));
    for (void *block : blocks) std::free(block);
    size_t large = 64u << 20;
    auto *big = static_cast<unsigned char *>(std::malloc(large));
    CHECK(big != nullptr);
    std::memset(big, 0x5A, large);
    CHECK_EQ((int)big[large - 1], 0x5A);
    std::free(big);
}

TEST(memory, mmap_with_a_hint_and_map_fixed) {
    size_t length = 4 * (size_t)page;
    auto *area = static_cast<char *>(
        mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    std::memset(area, 1, length);
    auto *middle = static_cast<char *>(mmap(area + page, (size_t)page, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
    CHECK(middle == area + page);
    CHECK_EQ((int)middle[0], 0);
    CHECK_EQ((int)area[0], 1);
    CHECK_EQ((int)area[2 * page], 1);
    munmap(area, length);
}
