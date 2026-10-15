#include "radix_sort.h"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;

struct Options {
    std::string command;
    unsigned bits = 64, threads = 0, repeats = 3;
    size_t count = 1000000;
    bool signed_values = false, big_endian = false;
    std::vector<std::string> files;
};

void usage() {
    std::cout << "Usage:\n"
        "  intsort sort [options] INPUT OUTPUT\n"
        "  intsort bench [options]\n\n"
        "Sort raw fixed-width integers in ascending numeric order.\n"
        "  --bits N       Integer width: 8, 16, 32, 64 (default: 64)\n"
        "  --signed       Interpret two's-complement signed integers\n"
        "  --big-endian   Read/write big-endian files (sort only; default: little-endian)\n"
        "  --threads N    Worker limit, 1..256 (default: performance-core count)\n"
        "  --count N      Benchmark element count (default: 1000000)\n"
        "  --repeats N    Benchmark repetitions, 1..100 (default: 3)\n"
        "  --             End options (for filenames beginning with '-')\n";
}

size_t number(std::string_view s, const std::string& flag) {
    size_t value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (error != std::errc{} || end != s.data() + s.size() || s.empty() || value == 0)
        throw std::invalid_argument(flag + " requires a positive integer");
    return value;
}

Options parse(int argc, char** argv) {
    Options o;
    if (argc < 2) throw std::invalid_argument("expected 'sort' or 'bench'; use --help");
    o.command = argv[1];
    if (o.command != "sort" && o.command != "bench")
        throw std::invalid_argument("expected 'sort' or 'bench'");
    bool positional = false;
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (positional) { o.files.push_back(flag); continue; }
        if (flag == "--") { positional = true; continue; }
        if (flag == "--signed") { o.signed_values = true; continue; }
        if (flag == "--big-endian") { o.big_endian = true; continue; }
        if (flag == "--bits" || flag == "--threads" || flag == "--count" || flag == "--repeats") {
            if (++i == argc) throw std::invalid_argument("missing value for " + flag);
            const size_t n = number(argv[i], flag);
            if (flag == "--bits") {
                if (n != 8 && n != 16 && n != 32 && n != 64)
                    throw std::invalid_argument("--bits must be 8, 16, 32, or 64");
                o.bits = static_cast<unsigned>(n);
            } else if (flag == "--threads") {
                if (n > 256) throw std::invalid_argument("--threads must be 1..256");
                o.threads = static_cast<unsigned>(n);
            } else if (flag == "--repeats") {
                if (o.command != "bench" || n > 100)
                    throw std::invalid_argument("--repeats is for bench and must be 1..100");
                o.repeats = static_cast<unsigned>(n);
            } else {
                if (o.command != "bench") throw std::invalid_argument("--count is for bench only");
                o.count = n;
            }
        } else if (flag.starts_with('-')) {
            throw std::invalid_argument("unknown option: " + flag);
        } else o.files.push_back(flag);
    }
    if (o.command == "sort" && o.files.size() != 2)
        throw std::invalid_argument("sort requires INPUT and OUTPUT paths");
    if (o.command == "bench" && (!o.files.empty() || o.big_endian))
        throw std::invalid_argument("bench takes no file paths or --big-endian option");
    return o;
}

template<class T> std::vector<T> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open input: " + path);
    const auto end = file.tellg();
    if (end < 0) throw std::runtime_error("cannot determine input size");
    const auto bytes = static_cast<uintmax_t>(end);
    if (bytes % sizeof(T)) throw std::runtime_error("input size is not a multiple of the integer width");
    if (bytes > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()) ||
        bytes / sizeof(T) > std::vector<T>().max_size())
        throw std::runtime_error("input is too large for memory");
    std::vector<T> values(static_cast<size_t>(bytes / sizeof(T)));
    file.seekg(0);
    if (bytes && !file.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(bytes)))
        throw std::runtime_error("failed to read complete input");
    return values;
}

// Rename only after the complete output is written; input and output may be the same path.
template<class T> void write_file(const std::string& path, const std::vector<T>& values) {
    std::string temporary = path + ".tmp.XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) throw std::runtime_error("cannot create output beside: " + path);
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        std::remove(temporary.c_str());
        throw std::runtime_error("cannot open temporary output");
    }
    const bool written = values.empty() ||
        std::fwrite(values.data(), sizeof(T), values.size(), file) == values.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::remove(temporary.c_str());
        throw std::runtime_error("failed to write complete output");
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::remove(temporary.c_str());
        throw std::runtime_error("cannot replace output: " + error.message());
    }
}

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

template<class T> void sort_file(const Options& o) {
    auto values = read_file<T>(o.files[0]);
    const bool swap = o.big_endian != (std::endian::native == std::endian::big);
    const auto start = Clock::now();
    intsort::transform(values.data(), values.size(), swap, o.signed_values, true);
    intsort::radix_sort(values.data(), values.size(), o.threads);
    intsort::transform(values.data(), values.size(), swap, o.signed_values, false);
    const double seconds = elapsed(start);
    write_file(o.files[1], values);
    std::cerr << "Sorted " << values.size() << " x " << o.bits << "-bit integers in "
              << std::fixed << std::setprecision(3) << seconds * 1000 << " ms (excluding file I/O)\n";
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

template<class T> void benchmark(const Options& o) {
    if (o.count > std::vector<T>().max_size()) throw std::length_error("--count is too large");
    std::mt19937_64 rng(42);
    std::vector<T> input(o.count);
    for (auto& value : input) value = static_cast<T>(rng());
    std::vector<double> radix_times, std_times;
    const T mask = intsort::sign_mask<T>(o.signed_values);
    auto less = [mask](T a, T b) { return T(a ^ mask) < T(b ^ mask); };
    for (unsigned repeat = 0; repeat < o.repeats; ++repeat) {
        auto actual = input, expected = input;
        const auto radix = [&] {
            const auto start = Clock::now();
            intsort::transform(actual.data(), actual.size(), false, o.signed_values, true);
            intsort::radix_sort(actual.data(), actual.size(), o.threads);
            intsort::transform(actual.data(), actual.size(), false, o.signed_values, false);
            radix_times.push_back(elapsed(start));
        };
        const auto comparison = [&] {
            const auto start = Clock::now();
            if (o.signed_values) std::sort(expected.begin(), expected.end(), less);
            else std::sort(expected.begin(), expected.end());
            std_times.push_back(elapsed(start));
        };
        if (repeat % 2) { comparison(); radix(); }
        else { radix(); comparison(); }
        if (actual != expected) throw std::runtime_error("benchmark verification failed");
    }
    const double radix = median(radix_times), comparison = median(std_times);
    std::cout << "Uniform random " << (o.signed_values ? "signed " : "unsigned ")
              << o.bits << "-bit integers, n=" << o.count
              << ", worker limit=" << (o.threads ? o.threads : intsort::default_workers())
              << ", repeats=" << o.repeats << "\n"
              << std::fixed << std::setprecision(3)
              << "radix median:    " << radix * 1000 << " ms, "
              << (o.count * sizeof(T) / 1e9) / radix << " GB/s (input bytes / time)\n"
              << "std::sort median: " << comparison * 1000 << " ms\n"
              << "speedup:          " << comparison / radix << "x\n"
              << "Verified exact equality with std::sort on every repetition.\n";
}

template<class T> void run(const Options& options) {
    if (options.command == "bench") benchmark<T>(options);
    else sort_file<T>(options);
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc && std::string_view(argv[i]) != "--"; ++i) {
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
            usage();
            return 0;
        }
    }
    try {
        const auto o = parse(argc, argv);
        switch (o.bits) {
            case 8: run<uint8_t>(o); break;
            case 16: run<uint16_t>(o); break;
            case 32: run<uint32_t>(o); break;
            case 64: run<uint64_t>(o); break;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "intsort: " << error.what() << '\n';
        return 1;
    }
}
