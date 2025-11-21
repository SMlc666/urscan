#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <functional>
#include <limits>
#include <numeric>
#include <algorithm>
#include <memory>
#include <atomic>
#include <utility>
#include <bit>

// The UR_ENABLE_MULTITHREADING macro is the switch to enable/disable parallelism.
//#define UR_ENABLE_MULTITHREADING

// The UR_ENABLE_NEON_OPTIMIZATION macro is the switch to enable/disable NEON SIMD acceleration.
//#define UR_ENABLE_NEON_OPTIMIZATION

// The UR_ENABLE_HARDWARE_PREFETCH macro is the switch to enable/disable hardware prefetching.
//#define UR_ENABLE_HARDWARE_PREFETCH

#ifdef UR_ENABLE_MULTITHREADING
#include "thread_pool.hpp"
#include <future>
#endif

#if defined(UR_ENABLE_NEON_OPTIMIZATION) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace ur {

#ifdef UR_ENABLE_MULTITHREADING
inline ThreadPool& get_pool() {
    static ThreadPool pool;
    return pool;
}
#endif

struct pattern_byte {
    std::byte value{};
    bool is_wildcard = false;
};

inline constexpr uintptr_t npos = static_cast<uintptr_t>(-1);

namespace detail {
    // Enum to define which scanning strategy to use based on pattern structure.
    enum class scan_strategy {
        simple,           // No wildcards.
        forward_anchor,   // e.g., "48 8B ??"
        backward_anchor,  // e.g., "?? ?? 48 8B"
        dual_anchor,      // e.g., "48 ?? 8B"
        dynamic_anchor    // e.g., "?? 48 8B ??"
    };

#if defined(UR_ENABLE_NEON_OPTIMIZATION) && defined(__ARM_NEON)
    // Holds all the pre-computed properties needed for a NEON scan.
    struct neon_properties {
        bool has_anchor = false;
        std::byte anchor_byte{};
        size_t anchor_offset = 0;
        std::array<std::byte, 16> pattern16{};
        std::array<std::byte, 16> mask16{};
    };
#endif
}

class runtime_signature : public std::enable_shared_from_this<runtime_signature> {
private:
    using scanner_func_t = std::optional<uintptr_t> (runtime_signature::*)(std::span<const std::byte>, std::shared_ptr<std::atomic<bool>>) const;

    std::vector<pattern_byte> pattern_;
    detail::scan_strategy strategy_ = detail::scan_strategy::simple;
    std::byte first_byte_{}, last_byte_{};
    std::vector<std::byte> simple_pattern_;
    std::array<size_t, 256> horspool_table_{};

    void analyze_pattern() {
        if (pattern_.empty()) {
            strategy_ = detail::scan_strategy::simple; // Should ideally not be used, but as a fallback.
            return;
        }

        bool has_wildcards = false;
        for(const auto& p_byte : pattern_) {
            if (p_byte.is_wildcard) {
                has_wildcards = true;
                break;
            }
        }

        const bool starts_with_wildcard = pattern_.front().is_wildcard;
        const bool ends_with_wildcard = pattern_.back().is_wildcard;

        if (!has_wildcards) {
            // A pattern with no wildcards is a perfect candidate for Boyer-Moore-Horspool.
            strategy_ = detail::scan_strategy::simple;
            simple_pattern_.resize(pattern_.size());
            std::transform(pattern_.begin(), pattern_.end(), simple_pattern_.begin(), [](const auto& p){ return p.value; });

            // BMH pre-calculation
            const size_t pattern_len = simple_pattern_.size();
            if (pattern_len > 0) {
                horspool_table_.fill(pattern_len);
                for (size_t i = 0; i < pattern_len - 1; ++i) {
                    horspool_table_[static_cast<uint8_t>(simple_pattern_[i])] = pattern_len - 1 - i;
                }
            }
        } else if (!starts_with_wildcard && !ends_with_wildcard) {
            strategy_ = detail::scan_strategy::dual_anchor;
            first_byte_ = pattern_.front().value;
            last_byte_ = pattern_.back().value;
        } else if (!starts_with_wildcard) {
            strategy_ = detail::scan_strategy::forward_anchor;
            first_byte_ = pattern_.front().value;
        } else if (!ends_with_wildcard) {
            strategy_ = detail::scan_strategy::backward_anchor;
            last_byte_ = pattern_.back().value;
        } else {
            strategy_ = detail::scan_strategy::dynamic_anchor;
        }
    }

    bool full_match_at(const std::byte* location) const {
        // For dual_anchor with no wildcards, we can use a fast memcmp.
        if (strategy_ == detail::scan_strategy::dual_anchor && !simple_pattern_.empty()) {
             return memcmp(location, simple_pattern_.data(), simple_pattern_.size()) == 0;
        }
        // Fallback for patterns with wildcards.
        size_t i = 0;
        const size_t size = pattern_.size();
        const pattern_byte* p_data = pattern_.data();
        
        // Aggressive unrolling (4 bytes per iteration)
        for (; i + 4 <= size; i += 4) {
            if ((!p_data[i].is_wildcard && p_data[i].value != location[i]) ||
                (!p_data[i+1].is_wildcard && p_data[i+1].value != location[i+1]) ||
                (!p_data[i+2].is_wildcard && p_data[i+2].value != location[i+2]) ||
                (!p_data[i+3].is_wildcard && p_data[i+3].value != location[i+3])) {
                return false;
            }
        }

        for (; i < size; ++i) {
            if (!p_data[i].is_wildcard && p_data[i].value != location[i]) {
                return false;
            }
        }
        return true;
    }

    std::optional<uintptr_t> scan_simple(std::span<const std::byte> memory_range, std::shared_ptr<std::atomic<bool>> found_flag) const {
        if (memory_range.size() < simple_pattern_.size() || simple_pattern_.empty()) {
            return std::nullopt;
        }

        const size_t pattern_len = simple_pattern_.size();
        const size_t scan_len = memory_range.size();
        const std::byte* const pattern_data = simple_pattern_.data();
        const std::byte* const scan_data = memory_range.data();
        const size_t last_pattern_idx = pattern_len - 1;

        size_t i = 0;
        while (i <= scan_len - pattern_len) {
            if (found_flag && found_flag->load(std::memory_order_relaxed)) {
                return std::nullopt;
            }

#ifdef UR_ENABLE_HARDWARE_PREFETCH
            __builtin_prefetch(scan_data + i, 0, 3);
#endif
            
            if (scan_data[i + last_pattern_idx] == pattern_data[last_pattern_idx]) {
                if (pattern_len == 1 || memcmp(pattern_data, scan_data + i, last_pattern_idx) == 0) {
                    if (found_flag) {
                        found_flag->store(true, std::memory_order_relaxed);
                    }
                    return reinterpret_cast<uintptr_t>(scan_data + i);
                }
            }

            i += horspool_table_[static_cast<uint8_t>(scan_data[i + last_pattern_idx])];
        }

        return std::nullopt;
    }

    std::optional<uintptr_t> scan_forward_anchor(std::span<const std::byte> memory_range, std::shared_ptr<std::atomic<bool>> found_flag) const {
        if (memory_range.size() < pattern_.size()) return std::nullopt;
        const auto scan_end = memory_range.data() + memory_range.size();

        for (const std::byte* p = memory_range.data(); p < scan_end; ++p) {
            p = static_cast<const std::byte*>(memchr(p, static_cast<int>(first_byte_), scan_end - p));
            if (!p) break;
            if (found_flag && found_flag->load(std::memory_order_relaxed)) return std::nullopt;
            if (static_cast<size_t>(scan_end - p) < pattern_.size()) break;
#ifdef UR_ENABLE_HARDWARE_PREFETCH
            __builtin_prefetch(p, 0, 3);
#endif
            if (full_match_at(p)) {
                if (found_flag) found_flag->store(true, std::memory_order_relaxed);
                return reinterpret_cast<uintptr_t>(p);
            }
        }
        return std::nullopt;
    }

    std::optional<uintptr_t> scan_backward_anchor(std::span<const std::byte> memory_range, std::shared_ptr<std::atomic<bool>> found_flag) const {
        if (memory_range.size() < pattern_.size()) return std::nullopt;
        const auto scan_end = memory_range.data() + memory_range.size();
        const size_t last_byte_offset = pattern_.size() - 1;

        for (const std::byte* p = memory_range.data(); p < scan_end; ++p) {
            p = static_cast<const std::byte*>(memchr(p, static_cast<int>(last_byte_), scan_end - p));
            if (!p) break;
            if (found_flag && found_flag->load(std::memory_order_relaxed)) return std::nullopt;
            
            const std::byte* potential_start = p - last_byte_offset;
            if (potential_start < memory_range.data()) continue;
            if (static_cast<size_t>(scan_end - potential_start) < pattern_.size()) continue;

#ifdef UR_ENABLE_HARDWARE_PREFETCH
            __builtin_prefetch(potential_start, 0, 3);
#endif

            if (full_match_at(potential_start)) {
                if (found_flag) found_flag->store(true, std::memory_order_relaxed);
                return reinterpret_cast<uintptr_t>(potential_start);
            }
        }
        return std::nullopt;
    }

    std::optional<uintptr_t> scan_dual_anchor(std::span<const std::byte> memory_range, std::shared_ptr<std::atomic<bool>> found_flag) const {
        if (memory_range.size() < pattern_.size()) return std::nullopt;
        const auto scan_end = memory_range.data() + memory_range.size();
        const size_t last_byte_offset = pattern_.size() - 1;

        for (const std::byte* p = memory_range.data(); p < scan_end; ++p) {
            p = static_cast<const std::byte*>(memchr(p, static_cast<int>(first_byte_), scan_end - p));
            if (!p) break;
            if (found_flag && found_flag->load(std::memory_order_relaxed)) return std::nullopt;
            if (static_cast<size_t>(scan_end - p) < pattern_.size()) break;
#ifdef UR_ENABLE_HARDWARE_PREFETCH
            __builtin_prefetch(p, 0, 3);
#endif
            if (p[last_byte_offset] == last_byte_ && full_match_at(p)) {
                if (found_flag) found_flag->store(true, std::memory_order_relaxed);
                return reinterpret_cast<uintptr_t>(p);
            }
        }
        return std::nullopt;
    }

#if defined(UR_ENABLE_NEON_OPTIMIZATION) && defined(__ARM_NEON)
    std::optional<uintptr_t> scan_dynamic_anchor(std::span<const std::byte> memory_range, std::shared_ptr<std::atomic<bool>> found_flag) const;
#else
    std::optional<uintptr_t> scan_dynamic_anchor(std::span<const std::byte> memory_range, std::shared_ptr<std::atomic<bool>> found_flag) const {
        size_t first_solid_offset = 0;
        for(size_t i = 0; i < pattern_.size(); ++i) { if(!pattern_[i].is_wildcard) { first_solid_offset = i; break; } }
        if (memory_range.size() < pattern_.size()) return std::nullopt;
        const auto scan_end = memory_range.data() + memory_range.size();
        const std::byte anchor = pattern_[first_solid_offset].value;
        for (const std::byte* p = memory_range.data(); p < scan_end; ++p) {
            p = static_cast<const std::byte*>(memchr(p, static_cast<int>(anchor), scan_end - p));
            if (!p) break;
            if (found_flag && found_flag->load(std::memory_order_relaxed)) return std::nullopt;
            const std::byte* potential_start = p - first_solid_offset;
            if (potential_start < memory_range.data() || static_cast<size_t>(scan_end - potential_start) < pattern_.size()) continue;
#ifdef UR_ENABLE_HARDWARE_PREFETCH
            __builtin_prefetch(potential_start, 0, 3);
#endif
            if (full_match_at(potential_start)) {
                if (found_flag) found_flag->store(true, std::memory_order_relaxed);
                return reinterpret_cast<uintptr_t>(potential_start);
            }
        }
        return std::nullopt;
    }
#endif

#ifdef UR_ENABLE_MULTITHREADING
    std::optional<uintptr_t> scan_multithreaded(std::span<const std::byte> memory_range, scanner_func_t core_scanner) const {
        const unsigned int num_threads = std::thread::hardware_concurrency();
        const size_t chunk_size = 65536 * 4;
        const size_t min_range_for_multithread = chunk_size;

        if (num_threads <= 1 || memory_range.size() < min_range_for_multithread) {
            return (this->*core_scanner)(memory_range, nullptr);
        }

        auto& pool = get_pool();
        std::vector<std::future<std::optional<uintptr_t>>> futures;
        auto found_flag = std::make_shared<std::atomic<bool>>(false);
        const size_t overlap = pattern_.size() > 1 ? pattern_.size() - 1 : 0;

        for (size_t start = 0; start < memory_range.size(); start += chunk_size) {
            if (found_flag->load(std::memory_order_acquire)) {
                break; 
            }
            size_t end = std::min(start + chunk_size + overlap, memory_range.size());
            if (start >= end || (end - start) < pattern_.size()) continue;
            
            std::span<const std::byte> chunk = memory_range.subspan(start, end - start);
            futures.push_back(pool.enqueue(core_scanner, shared_from_this(), chunk, found_flag));
        }

        std::optional<uintptr_t> first_result;
        for (auto& fut : futures) {
            if (auto result = fut.get(); result.has_value()) {
                if (!first_result.has_value() || result.value() < first_result.value()) {
                    first_result = result;
                }
            }
        }
        
        return first_result;
    }

    std::optional<uintptr_t> scan_ranges_multithreaded(const std::vector<std::pair<const void*, const void*>>& ranges, scanner_func_t core_scanner) const {
        const unsigned int num_threads = std::thread::hardware_concurrency();
        if (num_threads <= 1 || ranges.empty()) {
            for (const auto& range : ranges) {
                const std::byte* start = static_cast<const std::byte*>(range.first);
                const std::byte* end = static_cast<const std::byte*>(range.second);
                if (start >= end) continue;
                auto res = (this->*core_scanner)(std::span<const std::byte>(start, end - start), nullptr);
                if (res) return res;
            }
            return std::nullopt;
        }

        auto& pool = get_pool();
        std::vector<std::future<std::optional<uintptr_t>>> futures;
        auto found_flag = std::make_shared<std::atomic<bool>>(false);
        
        const size_t chunk_size = 65536 * 4;
        const size_t overlap = pattern_.size() > 1 ? pattern_.size() - 1 : 0;

        for (const auto& range : ranges) {
            if (found_flag->load(std::memory_order_acquire)) break;
            
            const std::byte* start_ptr = static_cast<const std::byte*>(range.first);
            const std::byte* end_ptr = static_cast<const std::byte*>(range.second);
            if (start_ptr >= end_ptr) continue;
            
            size_t len = end_ptr - start_ptr;
            if (len < pattern_.size()) continue;
            
            std::span<const std::byte> mem_span(start_ptr, len);

            // If the range is small enough, treat as single task
            if (len <= chunk_size * 2) {
                 futures.push_back(pool.enqueue(core_scanner, shared_from_this(), mem_span, found_flag));
            } else {
                 // Chunk large ranges
                 for (size_t i = 0; i < len; i += chunk_size) {
                    if (found_flag->load(std::memory_order_acquire)) break;
                    size_t end = std::min(i + chunk_size + overlap, len);
                    if (i >= end || (end - i) < pattern_.size()) continue;
                    
                    futures.push_back(pool.enqueue(core_scanner, shared_from_this(), mem_span.subspan(i, end - i), found_flag));
                 }
            }
        }

        std::optional<uintptr_t> first_result;
        for (auto& fut : futures) {
            if (auto result = fut.get(); result.has_value()) {
                if (!first_result.has_value() || result.value() < first_result.value()) {
                    first_result = result;
                }
            }
        }
        
        return first_result;
    }
#endif

public:
    explicit runtime_signature(std::string_view s) {
        pattern_.reserve(s.length() / 2);
        const char* p = s.data();
        const char* end = p + s.size();

        auto hex_to_val = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            // This function should not be called with invalid characters, but as a safeguard:
            throw std::invalid_argument("Invalid hexadecimal character.");
        };
        
        while (p < end) {
            if (*p == ' ') {
                p++;
                continue;
            }

            if (*p == '?') {
                pattern_.push_back({.is_wildcard = true});
                p++;
                if (p < end && *p == '?') { // Handles the second '?' in a '??' pair
                    p++;
                }
                continue;
            }

            if (isxdigit(static_cast<unsigned char>(p[0])) && (p + 1 < end) && isxdigit(static_cast<unsigned char>(p[1]))) {
                std::uint8_t high = hex_to_val(p[0]);
                std::uint8_t low = hex_to_val(p[1]);
                pattern_.push_back({ .value = static_cast<std::byte>((high << 4) | low), .is_wildcard = false });
                p += 2;
            } else {
                throw std::invalid_argument("Invalid pattern format: expected a hex pair or a wildcard.");
            }
        }
        pattern_.shrink_to_fit();
        analyze_pattern();
    }

    std::optional<uintptr_t> scan(std::span<const std::byte> memory_range) const {
        if (pattern_.empty()) {
            return std::nullopt;
        }
        scanner_func_t scanner;
        switch (strategy_) {
            case detail::scan_strategy::simple:
                scanner = &runtime_signature::scan_simple;
                break;
            case detail::scan_strategy::forward_anchor:
                scanner = &runtime_signature::scan_forward_anchor;
                break;
            case detail::scan_strategy::backward_anchor:
                scanner = &runtime_signature::scan_backward_anchor;
                break;
            case detail::scan_strategy::dual_anchor:
                scanner = &runtime_signature::scan_dual_anchor;
                break;
            case detail::scan_strategy::dynamic_anchor:
            default:
                scanner = &runtime_signature::scan_dynamic_anchor;
                break;
        }
#ifdef UR_ENABLE_MULTITHREADING
        return scan_multithreaded(memory_range, scanner);
#else
        return (this->*scanner)(memory_range, nullptr);
#endif
    }

    std::optional<uintptr_t> scan(const std::vector<std::pair<const void*, const void*>>& ranges) const {
        if (pattern_.empty() || ranges.empty()) {
            return std::nullopt;
        }
        scanner_func_t scanner;
        switch (strategy_) {
            case detail::scan_strategy::simple:
                scanner = &runtime_signature::scan_simple;
                break;
            case detail::scan_strategy::forward_anchor:
                scanner = &runtime_signature::scan_forward_anchor;
                break;
            case detail::scan_strategy::backward_anchor:
                scanner = &runtime_signature::scan_backward_anchor;
                break;
            case detail::scan_strategy::dual_anchor:
                scanner = &runtime_signature::scan_dual_anchor;
                break;
            case detail::scan_strategy::dynamic_anchor:
            default:
                scanner = &runtime_signature::scan_dynamic_anchor;
                break;
        }
#ifdef UR_ENABLE_MULTITHREADING
        return scan_ranges_multithreaded(ranges, scanner);
#else
        for (const auto& range : ranges) {
             const std::byte* start = static_cast<const std::byte*>(range.first);
             const std::byte* end = static_cast<const std::byte*>(range.second);
             if (start >= end) continue;
             auto res = (this->*scanner)(std::span<const std::byte>(start, end - start), nullptr);
             if (res) return res;
        }
        return std::nullopt;
#endif
    }
};

#if defined(UR_ENABLE_NEON_OPTIMIZATION) && defined(__ARM_NEON)
namespace detail {
    inline std::array<uint32_t, 256> calculate_dynamic_rarity(std::span<const std::byte> memory_range) {
        std::array<uint32_t, 256> frequencies{};
        constexpr size_t sample_stride = 4096;
        if (memory_range.size() < sample_stride) {
            const size_t size = memory_range.size();
            const std::byte* data = memory_range.data();
            size_t i = 0;
            for (; i + 8 <= size; i += 8) {
                frequencies[static_cast<uint8_t>(data[i])]++;
                frequencies[static_cast<uint8_t>(data[i+1])]++;
                frequencies[static_cast<uint8_t>(data[i+2])]++;
                frequencies[static_cast<uint8_t>(data[i+3])]++;
                frequencies[static_cast<uint8_t>(data[i+4])]++;
                frequencies[static_cast<uint8_t>(data[i+5])]++;
                frequencies[static_cast<uint8_t>(data[i+6])]++;
                frequencies[static_cast<uint8_t>(data[i+7])]++;
            }
            for (; i < size; ++i) {
                frequencies[static_cast<uint8_t>(data[i])]++;
            }
        } else {
            for (size_t i = 0; i < memory_range.size(); i += sample_stride) {
                frequencies[static_cast<uint8_t>(memory_range[i])]++;
            }
        }
        return frequencies;
    }

    inline neon_properties find_best_anchor_and_build_props(
        const std::vector<pattern_byte>& pattern,
        const std::array<uint32_t, 256>& frequencies) {
        neon_properties props{};
        uint32_t best_score = std::numeric_limits<uint32_t>::max();
        for (size_t i = 0; i < pattern.size() && i < 16; ++i) {
            if (!pattern[i].is_wildcard) {
                const auto byte_val = static_cast<uint8_t>(pattern[i].value);
                const uint32_t current_score = frequencies[byte_val] + (i * 2);
                if (current_score < best_score) {
                    best_score = current_score;
                    props.has_anchor = true;
                    props.anchor_byte = pattern[i].value;
                    props.anchor_offset = i;
                }
            }
        }
        if(props.has_anchor) {
            for (size_t i = 0; i < 16 && i < pattern.size(); ++i) {
                props.pattern16[i] = pattern[i].is_wildcard ? std::byte{0} : pattern[i].value;
                props.mask16[i] = pattern[i].is_wildcard ? std::byte{0} : std::byte{0xFF};
            }
        }
        return props;
    }
}

inline std::optional<uintptr_t> runtime_signature::scan_dynamic_anchor(std::span<const std::byte> memory_range, std::shared_ptr<std::atomic<bool>> found_flag) const {
    if (memory_range.size() < pattern_.size()) return std::nullopt;

    auto frequencies = detail::calculate_dynamic_rarity(memory_range);
    auto props = detail::find_best_anchor_and_build_props(pattern_, frequencies);
    if (!props.has_anchor) {
        return scan_forward_anchor(memory_range, found_flag);
    }

    const uint8x16_t v_anchor = vdupq_n_u8(static_cast<uint8_t>(props.anchor_byte));
    const uint8x16_t v_pattern16 = vld1q_u8(reinterpret_cast<const uint8_t*>(props.pattern16.data()));
    const uint8x16_t v_mask16 = vld1q_u8(reinterpret_cast<const uint8_t*>(props.mask16.data()));

    const std::byte* current_pos = memory_range.data();
    const std::byte* const range_end = memory_range.data() + memory_range.size();
    const std::byte* const end_pos = range_end - pattern_.size();
    const std::byte* const fast_scan_end_pos = (range_end >= memory_range.data() + 16) ? range_end - 16 : memory_range.data();
    const std::byte* const fast_scan_end_pos_64 = (range_end >= memory_range.data() + 64) ? range_end - 64 : memory_range.data();

    auto check_block = [&](const std::byte* pos, uint8x16_t cmp_res) -> std::optional<uintptr_t> {
        if (vmaxvq_u8(cmp_res) == 0) return std::nullopt;

        const uint8x8_t packed = vshrn_n_u16(vreinterpretq_u16_u8(cmp_res), 4);
        uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(packed), 0);

        while (mask != 0) {
            const int bit_pos = std::countr_zero(mask);
            const int i = bit_pos >> 2;

            // Clear the processed nibble
            mask &= ~(0xFULL << bit_pos);

            const std::byte* potential_start = pos + i - props.anchor_offset;
            if (potential_start < memory_range.data() || potential_start > end_pos) continue;

            if (potential_start + 16 <= range_end) {
                const uint8x16_t v_mem16 = vld1q_u8(reinterpret_cast<const uint8_t*>(potential_start));
                const uint8x16_t v_masked_mem = vandq_u8(v_mem16, v_mask16);
                const uint8x16_t v_verify_result = vceqq_u8(v_masked_mem, v_pattern16);

                if (vminvq_u8(v_verify_result) == 0xFF) {
                    if (pattern_.size() <= 16 || full_match_at(potential_start)) {
                        if (found_flag) found_flag->store(true, std::memory_order_release);
                        return reinterpret_cast<uintptr_t>(potential_start);
                    }
                }
            } else {
                if (full_match_at(potential_start)) {
                    if (found_flag) found_flag->store(true, std::memory_order_release);
                    return reinterpret_cast<uintptr_t>(potential_start);
                }
            }
        }
        return std::nullopt;
    };

    // Aggressive unrolling: process 64 bytes at a time
    while (current_pos <= fast_scan_end_pos_64) {
        if (found_flag && found_flag->load(std::memory_order_acquire)) return std::nullopt;

#ifdef UR_ENABLE_HARDWARE_PREFETCH
        __builtin_prefetch(current_pos + 128, 0, 0);
#endif

        const uint8x16_t v_mem0 = vld1q_u8(reinterpret_cast<const uint8_t*>(current_pos));
        const uint8x16_t v_mem1 = vld1q_u8(reinterpret_cast<const uint8_t*>(current_pos + 16));
        const uint8x16_t v_mem2 = vld1q_u8(reinterpret_cast<const uint8_t*>(current_pos + 32));
        const uint8x16_t v_mem3 = vld1q_u8(reinterpret_cast<const uint8_t*>(current_pos + 48));

        const uint8x16_t v_cmp0 = vceqq_u8(v_mem0, v_anchor);
        const uint8x16_t v_cmp1 = vceqq_u8(v_mem1, v_anchor);
        const uint8x16_t v_cmp2 = vceqq_u8(v_mem2, v_anchor);
        const uint8x16_t v_cmp3 = vceqq_u8(v_mem3, v_anchor);

        // Fast check: if no anchor is found in any of the 4 blocks, skip 64 bytes
        const uint8x16_t v_any = vorrq_u8(vorrq_u8(v_cmp0, v_cmp1), vorrq_u8(v_cmp2, v_cmp3));
        if (vmaxvq_u8(v_any) == 0) {
            current_pos += 64;
            continue;
        }

        if (auto res = check_block(current_pos, v_cmp0)) return res;
        current_pos += 16;
        if (auto res = check_block(current_pos, v_cmp1)) return res;
        current_pos += 16;
        if (auto res = check_block(current_pos, v_cmp2)) return res;
        current_pos += 16;
        if (auto res = check_block(current_pos, v_cmp3)) return res;
        current_pos += 16;
    }

    while (current_pos <= fast_scan_end_pos) {
        if (found_flag && found_flag->load(std::memory_order_acquire)) return std::nullopt;

#ifdef UR_ENABLE_HARDWARE_PREFETCH
        __builtin_prefetch(current_pos + 64, 0, 0);
#endif
        const uint8x16_t v_mem = vld1q_u8(reinterpret_cast<const uint8_t*>(current_pos));
        const uint8x16_t v_cmp_result = vceqq_u8(v_mem, v_anchor);

        if (auto res = check_block(current_pos, v_cmp_result)) return res;
        current_pos += 16;
    }

    const auto scan_end_tail = range_end - pattern_.size();
    while(current_pos <= scan_end_tail) {
        if (found_flag && found_flag->load(std::memory_order_acquire)) return std::nullopt;
        if (full_match_at(current_pos)) {
            if (found_flag) found_flag->store(true, std::memory_order_release);
            return reinterpret_cast<uintptr_t>(current_pos);
        }
        current_pos++;
    }

    return std::nullopt;
}
#endif

template <size_t N>
struct fixed_string {
    char data[N]{};
    size_t size = N;
    consteval fixed_string(const char(&str)[N]) {
        std::copy_n(str, N, data);
    }
};

template <fixed_string Signature>
class static_signature {
private:
    static constexpr auto pattern_ = []() consteval {
        std::array<pattern_byte, 256> p_bytes{};
        size_t count = 0;
        const char* p = Signature.data;
        const char* end = p + Signature.size - 1;

        auto hex_to_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        
        while (p < end) {
            if (*p == ' ') {
                p++;
                continue;
            }

            if (*p == '?') {
                if (count >= 256) throw std::logic_error("Pattern exceeds maximum length of 256 bytes.");
                p_bytes[count++] = {.is_wildcard = true};
                p++;
                if (p < end && *p == '?') {
                    p++;
                }
                continue;
            }

            if (p + 1 < end) {
                int high = hex_to_val(p[0]);
                int low = hex_to_val(p[1]);
                if (high != -1 && low != -1) {
                    if (count >= 256) throw std::logic_error("Pattern exceeds maximum length of 256 bytes.");
                    p_bytes[count++] = { .value = static_cast<std::byte>((high << 4) | low), .is_wildcard = false };
                    p += 2;
                } else {
                    throw std::logic_error("Invalid hexadecimal character in pattern.");
                }
            } else {
                 if (*p != '\0') { // Check for dangling character
                    throw std::logic_error("Incomplete hex pair at the end of the pattern.");
                }
                break; // End of string
            }
        }
        
        std::array<pattern_byte, 256> final_pattern{};
        for(size_t i = 0; i < count; ++i) final_pattern[i] = p_bytes[i];
        
        return std::pair(final_pattern, count);
    }();

    static constexpr size_t pattern_size_ = pattern_.second;
    static constexpr auto pattern_data_ = pattern_.first;

    static constexpr detail::scan_strategy strategy_ = []() consteval {
        if (pattern_size_ == 0) return detail::scan_strategy::simple;

        bool has_wildcards = false;
        for(size_t i = 0; i < pattern_size_; ++i) {
            if (pattern_data_[i].is_wildcard) {
                has_wildcards = true;
                break;
            }
        }

        if (!has_wildcards) return detail::scan_strategy::simple;
        if (!pattern_data_[0].is_wildcard && !pattern_data_[pattern_size_ - 1].is_wildcard) return detail::scan_strategy::dual_anchor;
        if (!pattern_data_[0].is_wildcard) return detail::scan_strategy::forward_anchor;
        if (!pattern_data_[pattern_size_ - 1].is_wildcard) return detail::scan_strategy::backward_anchor;
        return detail::scan_strategy::dynamic_anchor;
    }();

    static constexpr std::array<size_t, 256> horspool_table_ = []() consteval {
        std::array<size_t, 256> table{};
        if constexpr (strategy_ == detail::scan_strategy::simple && pattern_size_ > 0) {
            table.fill(pattern_size_);
            for (size_t i = 0; i < pattern_size_ - 1; ++i) {
                table[static_cast<uint8_t>(pattern_data_[i].value)] = pattern_size_ - 1 - i;
            }
        }
        return table;
    }();

    static constexpr std::byte first_byte_ = (pattern_size_ > 0) ? pattern_data_[0].value : std::byte{0};
    static constexpr std::byte last_byte_ = (pattern_size_ > 0) ? pattern_data_[pattern_size_ - 1].value : std::byte{0};

    template <size_t... Is>
    static constexpr bool full_match_at_impl(const std::byte* location, std::index_sequence<Is...>) {
        return ((pattern_data_[Is].is_wildcard || pattern_data_[Is].value == location[Is]) && ...);
    }

    static constexpr bool full_match_at(const std::byte* location) {
        if constexpr (strategy_ == detail::scan_strategy::simple) {
            // For BMH, the last byte is already checked, so we compare the rest.
            return memcmp(location, &pattern_data_[0].value, pattern_size_ - 1) == 0;
        } else {
            return full_match_at_impl(location, std::make_index_sequence<pattern_size_>{});
        }
    }

public:
    static std::optional<uintptr_t> scan(std::span<const std::byte> memory_range) {
        if (pattern_size_ == 0 || memory_range.size() < pattern_size_) {
            return std::nullopt;
        }

        const auto scan_end = memory_range.data() + memory_range.size();
        const auto scan_data = memory_range.data();
        const auto scan_len = memory_range.size();

        if constexpr (strategy_ == detail::scan_strategy::simple) {
            const size_t last_pattern_idx = pattern_size_ - 1;
            size_t i = 0;
            while (i <= scan_len - pattern_size_) {
                const std::byte last_byte = scan_data[i + last_pattern_idx];
                if (last_byte == last_byte_) {
                    if (pattern_size_ == 1 || full_match_at(scan_data + i)) {
                        return reinterpret_cast<uintptr_t>(scan_data + i);
                    }
                }
                i += horspool_table_[static_cast<uint8_t>(last_byte)];
            }
        } else if constexpr (strategy_ == detail::scan_strategy::forward_anchor || strategy_ == detail::scan_strategy::dual_anchor) {
            for (const std::byte* p = memory_range.data(); p < scan_end; ++p) {
                p = static_cast<const std::byte*>(memchr(p, static_cast<int>(first_byte_), scan_end - p));
                if (!p) break;
                if (static_cast<size_t>(scan_end - p) < pattern_size_) break;
                
                if constexpr (strategy_ == detail::scan_strategy::dual_anchor) {
                    if (p[pattern_size_ - 1] != last_byte_) continue;
                }

                if (full_match_at(p)) {
                    return reinterpret_cast<uintptr_t>(p);
                }
            }
        } else if constexpr (strategy_ == detail::scan_strategy::backward_anchor) {
            const size_t last_byte_offset = pattern_size_ - 1;
            for (const std::byte* p = memory_range.data(); p < scan_end; ++p) {
                p = static_cast<const std::byte*>(memchr(p, static_cast<int>(last_byte_), scan_end - p));
                if (!p) break;
                
                const std::byte* potential_start = p - last_byte_offset;
                if (potential_start < memory_range.data()) continue;
                if (static_cast<size_t>(scan_end - potential_start) < pattern_size_) continue;

                if (full_match_at(potential_start)) {
                    return reinterpret_cast<uintptr_t>(potential_start);
                }
            }
        } else { // dynamic_anchor
            size_t first_solid_offset = 0;
            for(size_t i = 0; i < pattern_size_; ++i) { if(!pattern_data_[i].is_wildcard) { first_solid_offset = i; break; } }
            const std::byte anchor = pattern_data_[first_solid_offset].value;

            for (const std::byte* p = memory_range.data(); p < scan_end; ++p) {
                p = static_cast<const std::byte*>(memchr(p, static_cast<int>(anchor), scan_end - p));
                if (!p) break;
                
                const std::byte* potential_start = p - first_solid_offset;
                if (potential_start < memory_range.data() || static_cast<size_t>(scan_end - potential_start) < pattern_size_) continue;

                if (full_match_at(potential_start)) {
                    return reinterpret_cast<uintptr_t>(potential_start);
                }
            }
        }

        return std::nullopt;
    }
};

} // namespace ur