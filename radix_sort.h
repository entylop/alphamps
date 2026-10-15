#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <sys/sysctl.h>
#endif
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace intsort {

inline unsigned default_workers() {
#ifdef __APPLE__
    unsigned cores = 0;
    size_t size = sizeof(cores);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &cores, &size, nullptr, 0) == 0 && cores)
        return cores;
#endif
    return 1;
}

template<class F> void parallel_for(size_t count, F&& fn) {
#ifdef __APPLE__
    // Synchronous GCD tasks let macOS schedule work across asymmetric cores.
    if (count > 1) {
        dispatch_apply_f(count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                         &fn, [](void* context, size_t i) {
                             (*static_cast<std::remove_reference_t<F>*>(context))(i);
                         });
        return;
    }
#endif
    for (size_t i = 0; i < count; ++i) fn(i);
}

template<class T> constexpr T sign_mask(bool signed_values) {
    return signed_values ? T(T(1) << (sizeof(T) * 8 - 1)) : T(0);
}

template<class T> T byte_swap(T value) {
    if constexpr (sizeof(T) == 8) return __builtin_bswap64(value);
    if constexpr (sizeof(T) == 4) return __builtin_bswap32(value);
    if constexpr (sizeof(T) == 2) return __builtin_bswap16(value);
    return value;
}

// Convert file representation to sortable unsigned keys, or reverse that transform.
template<class T> void transform(T* data, size_t count, bool swap, bool signed_values,
                                bool encode) {
    const T mask = sign_mask<T>(signed_values);
    if (!swap && !mask) return;
    size_t i = 0;
#ifdef __ARM_NEON
    // Operate on 128 bits at a time, using native NEON byte reversals and XOR.
    std::array<T, 16 / sizeof(T)> masks;
    masks.fill(mask);
    const auto sign = vld1q_u8(reinterpret_cast<const uint8_t*>(masks.data()));
    for (; count - i >= masks.size(); i += masks.size()) {
        auto v = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
        if (!encode) v = veorq_u8(v, sign);
        if (swap) {
            if constexpr (sizeof(T) == 2) v = vrev16q_u8(v);
            if constexpr (sizeof(T) == 4) v = vrev32q_u8(v);
            if constexpr (sizeof(T) == 8) v = vrev64q_u8(v);
        }
        if (encode) v = veorq_u8(v, sign);
        vst1q_u8(reinterpret_cast<uint8_t*>(data + i), v);
    }
#endif
    for (; i < count; ++i) {
        T v = data[i];
        if (!encode) v ^= mask;
        if (swap) v = byte_swap(v);
        if (encode) v ^= mask;
        data[i] = v;
    }
}

template<class T> T varying_bits(const T* data, size_t count, T reference) {
    size_t i = 0;
    T bits = 0;
#ifdef __ARM_NEON
    std::array<T, 16 / sizeof(T)> lanes;
    lanes.fill(reference);
    const auto ref = vld1q_u8(reinterpret_cast<const uint8_t*>(lanes.data()));
    auto differences = vdupq_n_u8(0);
    for (; count - i >= lanes.size(); i += lanes.size()) {
        const auto v = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
        differences = vorrq_u8(differences, veorq_u8(v, ref));
    }
    vst1q_u8(reinterpret_cast<uint8_t*>(lanes.data()), differences);
    for (T lane : lanes) bits |= lane;
#endif
    for (; i < count; ++i) bits |= T(data[i] ^ reference);
    return bits;
}

template<class T> void scatter_chunk(const T* source, T* destination, size_t begin,
                                    size_t end, unsigned shift,
                                    std::array<size_t, 256>& positions) {
    constexpr size_t batch = 128 / sizeof(T);
    alignas(128) T buffers[256][batch];
    std::array<size_t, 256> used{};
    for (size_t i = begin; i < end; ++i) {
        const T value = source[i];
        const size_t digit = (value >> shift) & 255;
        buffers[digit][used[digit]++] = value;
        if (used[digit] == batch) {
            std::memcpy(destination + positions[digit], buffers[digit], 128);
            positions[digit] += batch;
            used[digit] = 0;
        }
    }
    for (size_t digit = 0; digit < 256; ++digit)
        std::memcpy(destination + positions[digit], buffers[digit], used[digit] * sizeof(T));
}

// Within a most-significant-byte bucket, all remaining passes stay cache-local.
template<class T> void sort_bucket(T* source, T* output, size_t count, unsigned top_shift) {
    if (count < 128) {
        std::sort(source, source + count);
        std::memcpy(output, source, count * sizeof(T));
        return;
    }
    std::array<std::array<size_t, 256>, sizeof(T)> histograms{};
    // Counts do not change between local passes: collect all of them in one scan.
    for (size_t i = 0; i < count; ++i) {
        const T value = source[i];
        for (unsigned byte = 0; byte + 1 < sizeof(T); ++byte)
            if (byte * 8 < top_shift) ++histograms[byte][(value >> (byte * 8)) & 255];
    }
    T* destination = output;
    for (unsigned byte = 0; byte < top_shift / 8; ++byte) {
        auto& positions = histograms[byte];
        if (std::find(positions.begin(), positions.end(), count) != positions.end()) continue;
        size_t offset = 0;
        for (auto& position : positions) {
            const size_t size = position;
            position = offset;
            offset += size;
        }
        for (size_t i = 0; i < count; ++i) {
            const T value = source[i];
            destination[positions[(value >> (byte * 8)) & 255]++] = value;
        }
        std::swap(source, destination);
    }
    if (source != output) std::memcpy(output, source, count * sizeof(T));
}

// Hybrid MSD/LSD radix sort, in numeric unsigned order. One full-size scratch buffer.
template<class T> void radix_sort(T* data, size_t count, unsigned requested_workers = 0) {
    static_assert(std::is_unsigned_v<T> && std::is_integral_v<T> && sizeof(T) <= 8);
    if (requested_workers > 256) throw std::invalid_argument("threads must be at most 256");
    if (count < 2) return;
    if (!data) throw std::invalid_argument("null data pointer");
    if (count > std::numeric_limits<size_t>::max() / sizeof(T))
        throw std::length_error("input is too large");
    if (count < 128) {
        std::sort(data, data + count);
        return;
    }
    // Small inputs avoid GCD overhead; enough work per task amortizes scheduling.
    const size_t workers = std::min<size_t>(requested_workers ? requested_workers : default_workers(),
                                           std::max<size_t>(1, count / 32768));
    struct alignas(128) Worker {
        std::array<size_t, 256> bins{};
        T variation = 0;
    };
    std::vector<Worker> state(workers);
    const size_t base = count / workers, remainder = count % workers;
    auto boundary = [&](size_t w) { return base * w + std::min(w, remainder); };
    const bool maybe_ascending = data[0] <= data[count / 2] && data[count / 2] <= data[count - 1];
    const bool maybe_descending = data[0] >= data[count / 2] && data[count / 2] >= data[count - 1];
    bool ascending = maybe_ascending, descending = maybe_descending;
    if (maybe_ascending != maybe_descending) {
        std::vector<unsigned char> order(workers);
        parallel_for(workers, [&](size_t w) {
            const size_t begin = boundary(w), end = boundary(w + 1);
            // Include the previous chunk's last value when checking boundaries.
            const T* first = data + (begin ? begin - 1 : 0);
            const T* last = data + end;
            if (maybe_ascending && std::is_sorted(first, last)) order[w] |= 1;
            if (maybe_descending && std::is_sorted(first, last,
                    [](T a, T b) { return a > b; })) order[w] |= 2;
        });
        for (auto flags : order) {
            ascending = ascending && (flags & 1);
            descending = descending && (flags & 2);
        }
        if (ascending) return;
        if (descending) {
            parallel_for(workers, [&](size_t w) {
                const size_t half = count / 2;
                for (size_t i = half * w / workers; i < half * (w + 1) / workers; ++i)
                    std::swap(data[i], data[count - 1 - i]);
            });
            return;
        }
    }
    parallel_for(workers, [&](size_t w) {
        const size_t begin = boundary(w), end = boundary(w + 1);
        state[w].variation = varying_bits(data + begin, end - begin, data[0]);
    });
    T variation = 0;
    for (const auto& s : state) variation |= s.variation;
    if (!variation) return;
    const unsigned top_shift = (std::bit_width(variation) - 1) & ~7u;
    const bool count_only = (variation & T(T(255) << top_shift)) == variation;
    bool use_buckets = false;
    std::array<size_t, 257> boundaries{};
    if (count_only || (count >= 262144 && top_shift > 0)) {
        parallel_for(workers, [&](size_t w) {
            auto& bins = state[w].bins;
            for (size_t i = boundary(w); i < boundary(w + 1); ++i)
                ++bins[(data[i] >> top_shift) & 255];
        });
        size_t largest = 0;
        for (size_t digit = 0; digit < 256; ++digit) {
            size_t size = 0;
            for (const auto& s : state) size += s.bins[digit];
            largest = std::max(largest, size);
            boundaries[digit + 1] = boundaries[digit] + size;
        }
        if (count_only) {
            const T prefix = data[0] & ~T(T(255) << top_shift);
            parallel_for(workers, [&](size_t w) {
                for (size_t digit = 0; digit < 256; ++digit) {
                    const size_t begin = std::max(boundary(w), boundaries[digit]);
                    const size_t end = std::min(boundary(w + 1), boundaries[digit + 1]);
                    if (begin < end)
                        std::fill(data + begin, data + end, T(prefix | T(T(digit) << top_shift)));
                }
            });
            return;
        }
        // A heavily skewed digit would serialize this path; retain parallel LSD.
        use_buckets = largest <= std::min(count / 4, std::max<size_t>(32768, count / workers / 2));
    }
    // Every destination element is written before it is read. Avoid serial zeroing.
    std::unique_ptr<T[]> scratch(new T[count]);
    T* source = data;
    T* destination = scratch.get();
    if (use_buckets) {
        for (size_t digit = 0; digit < 256; ++digit) {
            size_t offset = boundaries[digit];
            for (auto& s : state) {
                const size_t size = s.bins[digit];
                s.bins[digit] = offset;
                offset += size;
            }
        }
        parallel_for(workers, [&](size_t w) {
            scatter_chunk(data, scratch.get(), boundary(w), boundary(w + 1),
                          top_shift, state[w].bins);
        });
        std::atomic<size_t> next_bucket{0};
        parallel_for(workers, [&](size_t) {
            for (;;) {
                const size_t digit = next_bucket.fetch_add(1, std::memory_order_relaxed);
                if (digit >= 256) break;
                const size_t begin = boundaries[digit], end = boundaries[digit + 1];
                if (begin != end)
                    sort_bucket(scratch.get() + begin, data + begin, end - begin, top_shift);
            }
        });
        return;
    }
    for (unsigned shift = 0; shift < sizeof(T) * 8; shift += 8) {
        if (((variation >> shift) & 255) == 0) continue;
        parallel_for(workers, [&](size_t w) {
            auto& bins = state[w].bins;
            bins.fill(0);
            for (size_t i = boundary(w); i < boundary(w + 1); ++i)
                ++bins[(source[i] >> shift) & 255];
        });
        // Thread chunks retain input order, so their disjoint bucket slices are stable.
        size_t offset = 0;
        for (size_t digit = 0; digit < 256; ++digit) {
            for (auto& s : state) {
                const size_t size = s.bins[digit];
                s.bins[digit] = offset;
                offset += size;
            }
        }
        parallel_for(workers, [&](size_t w) {
            scatter_chunk(source, destination, boundary(w), boundary(w + 1), shift, state[w].bins);
        });
        std::swap(source, destination);
    }
    if (source != data) std::memcpy(data, source, count * sizeof(T));
}

} // namespace intsort
