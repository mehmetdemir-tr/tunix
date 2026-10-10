#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "helpers.hpp"

namespace fs = std::filesystem;

namespace {

struct Tracked {
    static int alive;
    Tracked() { alive++; }
    ~Tracked() { alive--; }
    Tracked(const Tracked &) = delete;
    Tracked &operator=(const Tracked &) = delete;
};
int Tracked::alive = 0;

[[noreturn]] void throw_deep(int depth) {
    Tracked guard;
    if (depth == 0) throw std::runtime_error("bottom");
    throw_deep(depth - 1);
}

}

TEST(cxx, exceptions_unwind_and_run_destructors) {
    Tracked::alive = 0;
    std::string message;
    try {
        throw_deep(20);
    } catch (const std::runtime_error &error) { message = error.what(); }
    CHECK_EQ(message, std::string("bottom"));
    CHECK_EQ(Tracked::alive, 0);
}

TEST(cxx, exceptions_cross_threads_through_futures) {
    auto result = std::async(std::launch::async, [] {
        throw std::logic_error("from another thread");
        return 0;
    });
    std::string message;
    try {
        result.get();
    } catch (const std::logic_error &error) { message = error.what(); }
    CHECK_EQ(message, std::string("from another thread"));
}

TEST(cxx, async_tasks_run_in_parallel_and_return_values) {
    std::vector<std::future<long>> parts;
    for (long part = 0; part < 8; part++) {
        parts.push_back(std::async(std::launch::async, [part] {
            long sum = 0;
            for (long i = part * 100000; i < (part + 1) * 100000; i++) sum += i;
            return sum;
        }));
    }
    long total = 0;
    for (auto &part : parts) total += part.get();
    CHECK_EQ(total, 799999L * 800000L / 2);
}

TEST(cxx, filesystem_creates_walks_and_removes_a_tree) {
    posix_tests::TempDir dir;
    fs::create_directories("tree/a/b/c");
    std::ofstream("tree/a/file.txt") << "contents";
    std::ofstream("tree/a/b/c/deep.txt") << "deep";
    int files = 0;
    int directories = 0;
    for (const auto &entry : fs::recursive_directory_iterator("tree")) {
        if (entry.is_regular_file()) files++;
        if (entry.is_directory()) directories++;
    }
    CHECK_EQ(files, 2);
    CHECK_EQ(directories, 3);
    CHECK_EQ(fs::file_size("tree/a/file.txt"), (std::uintmax_t)8);
    fs::rename("tree/a/file.txt", "tree/moved.txt");
    CHECK(fs::exists("tree/moved.txt"));
    CHECK_EQ(fs::remove_all("tree"), (std::uintmax_t)6);
    CHECK(!fs::exists("tree"));
}

TEST(cxx, fstream_round_trips_text_and_binary) {
    posix_tests::TempDir dir;
    {
        std::ofstream out("numbers.txt");
        for (int i = 0; i < 1000; i++) out << i << '\n';
    }
    std::ifstream in("numbers.txt");
    long sum = 0;
    int value = 0;
    while (in >> value) sum += value;
    CHECK_EQ(sum, 999L * 1000L / 2);
    std::vector<double> data(4096);
    std::iota(data.begin(), data.end(), 0.5);
    std::ofstream("data.bin", std::ios::binary)
        .write(reinterpret_cast<const char *>(data.data()),
               (std::streamsize)(data.size() * sizeof(double)));
    std::vector<double> back(data.size());
    std::ifstream("data.bin", std::ios::binary)
        .read(reinterpret_cast<char *>(back.data()),
              (std::streamsize)(back.size() * sizeof(double)));
    CHECK(back == data);
}

TEST(cxx, random_device_draws_from_the_kernel) {
    std::random_device device;
    std::map<unsigned, int> seen;
    for (int i = 0; i < 64; i++) seen[device()]++;
    CHECK(seen.size() > 60);
}

TEST(cxx, steady_clock_measures_a_sleep) {
    auto before = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto elapsed = std::chrono::steady_clock::now() - before;
    CHECK(elapsed >= std::chrono::milliseconds(100));
    CHECK(elapsed < std::chrono::seconds(2));
}

TEST(cxx, containers_and_algorithms_work_at_scale) {
    std::mt19937 generator(1234);
    std::vector<int> values(200000);
    for (int &value : values) value = (int)(generator() % 1000000);
    std::sort(values.begin(), values.end());
    CHECK(std::is_sorted(values.begin(), values.end()));
    std::map<int, int> counts;
    for (int value : values) counts[value % 100]++;
    CHECK_EQ(counts.size(), (size_t)100);
    auto shared = std::make_shared<std::vector<int>>(values);
    std::weak_ptr<std::vector<int>> weak = shared;
    shared.reset();
    CHECK(weak.expired());
}

TEST(cxx, string_streams_format_and_parse) {
    std::ostringstream out;
    out << "pi=" << 3.25 << " hex=" << std::hex << 255;
    CHECK_EQ(out.str(), std::string("pi=3.25 hex=ff"));
    std::istringstream in("12 34.5 word");
    int whole = 0;
    double fraction = 0;
    std::string text;
    in >> whole >> fraction >> text;
    CHECK_EQ(whole, 12);
    CHECK(fraction == 34.5);
    CHECK_EQ(text, std::string("word"));
}
