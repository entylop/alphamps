#include "radix_sort.h"

#include <iostream>
#include <random>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T> void check(const std::vector<T>& input, unsigned workers) {
    auto expected = input;
    std::sort(expected.begin(), expected.end());
    // Offset the storage and place guards around it to catch SIMD/scatter overruns.
    std::vector<T> actual(input.size() + 2, T(123));
    std::copy(input.begin(), input.end(), actual.begin() + 1);
    intsort::radix_sort(actual.data() + 1, input.size(), workers);
    require(std::equal(expected.begin(), expected.end(), actual.begin() + 1), "radix order/permutation mismatch");
    require(actual.front() == T(123) && actual.back() == T(123), "guard overwritten");
}

template<class T> void test_type() {
    std::mt19937_64 rng(123);
    for (size_t n : {0, 1, 2, 15, 16, 17, 127, 128, 129, 32767, 32768, 65537, 131109,
                     262143, 262144, 262145, 1000003}) {
        std::vector<T> input(n);
        for (auto& value : input) value = static_cast<T>(rng());
        for (unsigned workers : {1, 2, 12}) check(input, workers);
    }
    std::vector<T> input(400003);
    for (int pattern = 0; pattern < 13; ++pattern) {
        for (size_t i = 0; i < input.size(); ++i) {
            switch (pattern) {
                case 0: input[i] = T(42); break;
                case 1: input[i] = T(i); break;
                case 2: input[i] = T(input.size() - i); break;
                case 3: input[i] = T(i % 2 ? 0 : std::numeric_limits<T>::max()); break;
                case 4: input[i] = T(rng() % 7); break;
                case 5: input[i] = T(T(rng() % 256) << (8 * (sizeof(T) - 1))); break;
                case 6: input[i] = T((T(rng() % 256) << (sizeof(T) > 1 ? 8 : 0)) ^ T(0x35)); break;
                // Highly skewed top byte, with random lower bytes.
                case 7: input[i] = T(T(rng()) >> 8) |
                    T(T(i % 1000 == 0 ? 255 : 0) << (8 * (sizeof(T) - 1))); break;
                // Four large top-byte buckets exercise the parallel LSD fallback.
                case 8: input[i] = T(T(rng()) >> 8) |
                    T(T(i % 4) << (8 * (sizeof(T) - 1))); break;
                // Many top-byte buckets with a constant middle byte.
                case 9: input[i] = T(T(rng()) & ~T(T(255) << (sizeof(T) > 2 ? 16 : 0))); break;
                case 10: input[i] = T(i); break;
                case 11: input[i] = T(input.size() - i); break;
                case 12: input[i] = T(T(0xa55a12349876c35aULL) ^
                    T(T(rng() % 256) << (sizeof(T) > 2 ? 8 : 0))); break;
            }
        }
        if (pattern == 10 || pattern == 11) {
            // Each worker's chunk remains ordered; only their shared boundary is wrong.
            const size_t boundary = input.size() / 12 + 1;
            std::swap(input[boundary - 1], input[boundary]);
        }
        for (unsigned workers : {1, 2, 12}) check(input, workers);
    }
    for (auto& value : input) value = static_cast<T>(rng());
    input[0] = 0;
    input[1] = std::numeric_limits<T>::max();
    input[2] = T(T(1) << (sizeof(T) * 8 - 1));
    for (bool swap : {false, true}) {
        for (bool signed_values : {false, true}) {
            auto actual = input, expected = input;
            std::sort(expected.begin(), expected.end(), [signed_values](T a, T b) {
                if (signed_values)
                    return std::bit_cast<std::make_signed_t<T>>(a) < std::bit_cast<std::make_signed_t<T>>(b);
                return a < b;
            });
            if (swap) for (auto& value : actual) value = intsort::byte_swap(value);
            intsort::transform(actual.data(), actual.size(), swap, signed_values, true);
            intsort::radix_sort(actual.data(), actual.size(), 12);
            intsort::transform(actual.data(), actual.size(), swap, signed_values, false);
            if (swap) for (auto& value : actual) value = intsort::byte_swap(value);
            require(actual == expected, "signed/endian transform mismatch");
        }
    }
    std::cout << "Passed " << sizeof(T) * 8 << "-bit tests\n";
}
} // namespace

int main() {
    try {
        test_type<uint8_t>();
        test_type<uint16_t>();
        test_type<uint32_t>();
        test_type<uint64_t>();
        intsort::radix_sort<uint64_t>(nullptr, 0);
        bool rejected = false;
        try { intsort::radix_sort<uint64_t>(nullptr, 2); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "null input was not rejected");
        std::cout << "All radix tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
