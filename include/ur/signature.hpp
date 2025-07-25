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

// The UR_ENABLE_MULTITHREADING macro is the switch to enable/disable parallelism.
//#define UR_ENABLE_MULTITHREADING

// The UR_ENABLE_NEON_OPTIMIZATION macro is the switch to enable/disable NEON SIMD acceleration.
//#define UR_ENABLE_NEON_OPTIMIZATION

// The UR_ENABLE_HARDWARE_PREFETCH macro is the switch to enable/disable hardware prefetching.
//#define UR_ENABLE_HARDWARE_PREFETCH

#ifdef UR_ENABLE_MULTITHREADING
#include "thread_pool.hpp"
#include <future>
#include <atomic>
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
            // A pattern with no wildcards is a perfect candidate for dual_anchor.
            strategy_ = detail::scan_strategy::dual_anchor;
            first_byte_ = pattern_.front().value;
            last_byte_ = pattern_.back().value;
            // We still prepare simple_pattern_ for full_match_at if needed, or direct comparison.
            simple_pattern_.resize(pattern_.size());
            std::transform(pattern_.begin(), pattern_.end(), simple_pattern_.begin(), [](const auto& p){ return p.value; });

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
        for (size_t i = 0; i < pattern_.size(); ++i) {
            if (!pattern_[i].is_wildcard && pattern_[i].value != location[i]) {
                return false;
            }
        }
        return true;
    }

    std::optional<uintptr_t> scan_simple(std::span<const std::byte> memory_range, std::shared_ptr<std::atomic<bool>> found_flag) const {
        if (memory_range.size() < simple_pattern_.size() || simple_pattern_.empty()) {
            return std::nullopt;
        }

        const auto scan_end = memory_range.data() + memory_range.size();
        const std::byte first_byte = simple_pattern_.front();
        const size_t pattern_size = simple_pattern_.size();
        const std::byte* const pattern_data = simple_pattern_.data();

        for (const std::byte* p = memory_range.data(); p < scan_end; ++p) {
            p = static_cast<const std::byte*>(memchr(p, static_cast<int>(first_byte), scan_end - p));
            if (!p) {
                break;
            }

            if (found_flag && found_flag->load(std::memory_order_relaxed)) {
                return std::nullopt;
            }

            if (static_cast<size_t>(scan_end - p) < pattern_size) {
                break;
            }

#ifdef UR_ENABLE_HARDWARE_PREFETCH
            __builtin_prefetch(p, 0, 3);
#endif

            if (memcmp(p, pattern_data, pattern_size) == 0) {
                if (found_flag) {
                    found_flag->store(true, std::memory_order_relaxed);
                }
                return reinterpret_cast<uintptr_t>(p);
            }
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
        const size_t min_range_for_multithread = chunk_size * num_threads;
        if (num_threads <= 1 || memory_range.size() < min_range_for_multithread) {
            return (this->*core_scanner)(memory_range, nullptr);
        }
        auto& pool = get_pool();
        std::vector<std::future<std::optional<uintptr_t>>> futures;
        auto found_flag = std::make_shared<std::atomic<bool>>(false);
        const size_t overlap = pattern_.size() > 1 ? pattern_.size() - 1 : 0;
        for (size_t start = 0; start < memory_range.size(); start += chunk_size) {
            size_t end = std::min(start + chunk_size + overlap, memory_range.size());
            if (start >= end || (end - start) < pattern_.size()) continue;
            std::span<const std::byte> chunk = memory_range.subspan(start, end - start);
            futures.push_back(pool.enqueue(core_scanner, shared_from_this(), chunk, found_flag));
        }
        for (auto& fut : futures) {
            if (auto result = fut.get(); result.has_value()) {
                found_flag->store(true, std::memory_order_relaxed);
                return result;
            }
        }
        return std::nullopt;
    }
#endif

public:
    explicit runtime_signature(std::string_view s) {
        pattern_.reserve(s.length() / 2);
        auto hex_to_val = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            throw std::invalid_argument("Invalid hexadecimal character.");
        };
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == ' ') continue;
            if (s[i] == '?') {
                pattern_.push_back({.is_wildcard = true});
                if (i + 1 < s.size() && s[i+1] == '?') i++;
            } else {
                if (i + 1 >= s.size()) throw std::invalid_argument("Incomplete hex pair.");
                std::uint8_t high = hex_to_val(s[i++]);
                std::uint8_t low = hex_to_val(s[i]);
                pattern_.push_back({ .value = static_cast<std::byte>((high << 4) | low), .is_wildcard = false });
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
};

#if defined(UR_ENABLE_NEON_OPTIMIZATION) && defined(__ARM_NEON)
namespace detail {
    inline std::array<uint32_t, 256> calculate_dynamic_rarity(std::span<const std::byte> memory_range) {
        std::array<uint32_t, 256> frequencies{};
        constexpr size_t sample_stride = 4096;
        if (memory_range.size() < sample_stride) {
            for (const auto& byte : memory_range) frequencies[static_cast<uint8_t>(byte)]++;
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
    const std::byte* const end_pos = memory_range.data() + memory_range.size() - pattern_.size();
    const std::byte* const fast_scan_end_pos = memory_range.data() + memory_range.size() - 16;
    while (current_pos <= fast_scan_end_pos) {
        if (found_flag && found_flag->load(std::memory_order_relaxed)) return std::nullopt;
#ifdef UR_ENABLE_HARDWARE_PREFETCH
        __builtin_prefetch(current_pos + 64, 0, 0);
#endif
        const uint8x16_t v_mem = vld1q_u8(reinterpret_cast<const uint8_t*>(current_pos));
        const uint8x16_t v_cmp_result = vceqq_u8(v_mem, v_anchor);
        if (vmaxvq_u8(v_cmp_result) == 0) {
            current_pos += 16;
            continue;
        }
        uint8_t result_bytes[16];
        vst1q_u8(result_bytes, v_cmp_result);
        for (int i = 0; i < 16; ++i) {
            if (result_bytes[i] == 0xFF) {
                const std::byte* potential_start = current_pos + i - props.anchor_offset;
                if (potential_start < memory_range.data() || potential_start > end_pos) continue;
                const uint8x16_t v_mem16 = vld1q_u8(reinterpret_cast<const uint8_t*>(potential_start));
                const uint8x16_t v_masked_mem = vandq_u8(v_mem16, v_mask16);
                const uint8x16_t v_verify_result = vceqq_u8(v_masked_mem, v_pattern16);
                if (vminvq_u8(v_verify_result) == 0xFF) {
                    if (pattern_.size() <= 16 || full_match_at(potential_start)) {
                        if (found_flag) found_flag->store(true, std::memory_order_relaxed);
                        return reinterpret_cast<uintptr_t>(potential_start);
                    }
                }
            }
        }
        current_pos += 16;
    }
    std::span<const std::byte> tail_span{current_pos, static_cast<size_t>(memory_range.data() + memory_range.size() - current_pos)};
    const auto scan_end_tail = tail_span.data() + tail_span.size() - pattern_.size();
    for (const std::byte* p = tail_span.data(); p <= scan_end_tail; ++p) {
        if (full_match_at(p)) {
            if (found_flag) found_flag->store(true, std::memory_order_relaxed);
            return reinterpret_cast<uintptr_t>(p);
        }
    }
    return std::nullopt;
}
#endif

} // namespace ur