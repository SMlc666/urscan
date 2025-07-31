#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <iomanip>
#include <cstring>
#include <random>
#include <memory>
#include <type_traits>
#include <sstream> // For std::stringstream
#include <algorithm> // For std::copy_n
#include <iterator> // For std::istream_iterator
#include "ur/signature.hpp"
#include "plf_nanotimer.h"

// --- Utility Functions ---

// Generates a random hex string representation of bytes (e.g., "DE AD BE EF")
std::string generate_random_hex(size_t num_bytes, std::mt19937& gen) {
    if (num_bytes == 0) return "";
    std::uniform_int_distribution<> distrib(0, 255);
    std::stringstream ss;
    for (size_t i = 0; i < num_bytes; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << std::uppercase << distrib(gen);
        if (i < num_bytes - 1) {
            ss << " ";
        }
    }
    return ss.str();
}

// Generates a buffer of random byte data.
std::vector<std::byte> generate_random_data(size_t size, std::mt19937& gen) {
    std::vector<std::byte> data(size);
    std::uniform_int_distribution<> distrib(0, 255);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>(distrib(gen));
    }
    return data;
}

// Helper to convert a signature string to a byte vector for test data injection.
std::vector<std::byte> pattern_from_string(std::string_view s) {
    std::vector<std::byte> pattern;
    pattern.reserve(s.length() / 3 + 1);

    auto hex_to_val = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0; // Should not happen with valid input
    };

    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ' ') continue;
        // This function is only for creating the ground-truth data,
        // so it should not encounter wildcards.
        if (s[i] == '?') continue;

        if (i + 1 >= s.size()) break;
        uint8_t high = hex_to_val(s[i++]);
        uint8_t low = hex_to_val(s[i]);
        pattern.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return pattern;
}


// --- Benchmark Core ---

// This function now only times construction and scanning. Data setup is external.
void run_benchmark(const std::string& test_name, const std::string& signature_str, const std::vector<std::byte>& data_buffer, uintptr_t expected_address) {
    plf::nanotimer timer;
    const int scan_runs = 10;

    std::cout << "\n--- Benchmarking: " << test_name << " ---" << std::endl;
    std::cout << "    Signature: " << signature_str << std::endl;

    // 1. Benchmark construction
    timer.start();
    auto scan_sig = std::make_shared<ur::runtime_signature>(signature_str);
    double construction_time = timer.get_elapsed_ns();
    std::cout << "    Construction Time: " << std::fixed << std::setprecision(3) << construction_time / 1000.0 << " us" << std::endl;

    // 2. Benchmark scanning
    double total_scan_time = 0;
    bool verification_done = false;
    for (int i = 0; i < scan_runs; ++i) {
        timer.start();
        // The result is made volatile to prevent the compiler from optimizing the call away.
        volatile auto result = scan_sig->scan(data_buffer);
        total_scan_time += timer.get_elapsed_ns();

        // Simple verification (only performed on the first run)
        if (!verification_done) {
            auto non_volatile_result = const_cast<std::remove_volatile_t<decltype(result)>&>(result);
            if (!non_volatile_result.has_value()) {
                std::cerr << "    [FAIL] Verification failed: Signature not found." << std::endl;
            } else if (*non_volatile_result != expected_address) {
                std::cerr << "    [FAIL] Verification failed: Found at incorrect address." << std::endl;
                std::cerr << "           Expected: " << reinterpret_cast<const void*>(expected_address)
                          << ", Got: " << reinterpret_cast<const void*>(*non_volatile_result) << std::endl;
            } else {
                std::cout << "    [OK] Verified: Signature found at correct address." << std::endl;
            }
            verification_done = true;
        }
    }
    double avg_scan_time = total_scan_time / scan_runs;
    std::cout << "    Average Scan Time (over " << scan_runs << " runs): "
              << avg_scan_time / 1000.0 << " us" << std::endl;
}

// Splits a string by spaces into a vector of tokens.
std::vector<std::string> get_tokens(const std::string& s) {
    std::stringstream ss(s);
    std::istream_iterator<std::string> begin(ss);
    std::istream_iterator<std::string> end;
    return std::vector<std::string>(begin, end);
}

// Joins a vector of string tokens back into a single space-separated string.
std::string join_tokens(const std::vector<std::string>& tokens) {
    std::stringstream ss;
    for (size_t i = 0; i < tokens.size(); ++i) {
        ss << tokens[i] << (i == tokens.size() - 1 ? "" : " ");
    }
    return ss.str();
}


void run_all_benchmarks_for_size(size_t data_size, std::mt19937& gen) {
    std::cout << "\n=========================================================" << std::endl;
    std::cout << "Benchmarking with " << data_size / (1024 * 1024) << " MB of data." << std::endl;
    std::cout << "=========================================================" << std::endl;

    // 1. Generate a single, shared data buffer for this size category.
    auto data_buffer = generate_random_data(data_size, gen);

    // 2. Generate one "master" signature that will be the ground truth.
    const size_t signature_length = 20; // bytes
    std::string base_signature_str = generate_random_hex(signature_length, gen);
    auto base_pattern_bytes = pattern_from_string(base_signature_str);

    // 3. Inject the master signature at a known location and get its address.
    uintptr_t expected_address = 0;
    if (data_buffer.size() >= base_pattern_bytes.size()) {
        size_t offset = data_buffer.size() / 2; // Place in the middle
        std::memcpy(data_buffer.data() + offset, base_pattern_bytes.data(), base_pattern_bytes.size());
        expected_address = reinterpret_cast<uintptr_t>(data_buffer.data() + offset);
    }

    std::cout << "Injected Base Signature: " << base_signature_str << std::endl;
    std::cout << "Expected Address: " << reinterpret_cast<const void*>(expected_address) << std::endl;

    auto base_tokens = get_tokens(base_signature_str);

    // --- All strategies below will scan the SAME data_buffer ---

    // 1. Simple Strategy: No wildcards. The exact base signature.
    // This will now be classified as Dual Anchor, which is the optimal strategy.
    run_benchmark("Simple Strategy (now Dual Anchor)", base_signature_str, data_buffer, expected_address);

    // 2. Forward Anchor Strategy: Solid start, wildcard end.
    {
        auto tokens = base_tokens;
        for (size_t i = 16; i < signature_length; ++i) tokens[i] = "??";
        run_benchmark("Forward Anchor Strategy", join_tokens(tokens), data_buffer, expected_address);
    }

    // 3. Backward Anchor Strategy: Wildcard start, solid end.
    {
        auto tokens = base_tokens;
        for (size_t i = 0; i < 4; ++i) tokens[i] = "??";
        run_benchmark("Backward Anchor Strategy", join_tokens(tokens), data_buffer, expected_address);
    }

    // 4. Dual Anchor Strategy: Solid ends, wildcard middle.
    {
        auto tokens = base_tokens;
        for (size_t i = 2; i < signature_length - 2; ++i) tokens[i] = "??";
        run_benchmark("Dual Anchor Strategy", join_tokens(tokens), data_buffer, expected_address);
    }

    // 5. Dynamic Anchor Strategy: Wildcard ends, solid middle.
    {
        auto tokens = base_tokens;
        tokens[0] = tokens[1] = "??";
        tokens[signature_length - 1] = tokens[signature_length - 2] = "??";
        run_benchmark("Dynamic Anchor Strategy", join_tokens(tokens), data_buffer, expected_address);
    }
}

void run_frequent_first_byte_benchmark(size_t data_size, std::mt19937& gen) {
    std::cout << "\n=========================================================" << std::endl;
    std::cout << "Benchmarking with frequent first byte (" << data_size / (1024 * 1024) << " MB data)" << std::endl;
    std::cout << "=========================================================" << std::endl;

    // 1. Define the frequent byte
    const std::byte frequent_byte{0xAA};
    const std::string frequent_byte_str = "AA";

    // 2. Create the data buffer and fill it with a high percentage of the frequent byte
    auto data_buffer = generate_random_data(data_size, gen);
    // Overwrite a large portion of the buffer with the frequent byte
    std::fill(data_buffer.begin(), data_buffer.begin() + data_size / 2, frequent_byte);
    // Shuffle to distribute the frequent byte somewhat randomly
    std::shuffle(data_buffer.begin(), data_buffer.end(), gen);
    std::cout << "    Generated data with a high frequency of '" << frequent_byte_str << "'." << std::endl;

    // 3. Create a signature starting with the frequent byte
    const size_t signature_length = 20;
    std::string signature_suffix = generate_random_hex(signature_length - 1, gen);
    std::string signature_str = frequent_byte_str + " " + signature_suffix;
    auto pattern_bytes = pattern_from_string(signature_str);

    // 4. Inject signature and get address
    uintptr_t expected_address = 0;
    if (data_buffer.size() >= pattern_bytes.size()) {
        size_t offset = data_buffer.size() / 3; // Place somewhere in the latter 2/3
        std::memcpy(data_buffer.data() + offset, pattern_bytes.data(), pattern_bytes.size());
        expected_address = reinterpret_cast<uintptr_t>(data_buffer.data() + offset);
    }

    std::cout << "    Injected Signature: " << signature_str << std::endl;
    std::cout << "    Expected Address: " << reinterpret_cast<const void*>(expected_address) << std::endl;

    // 5. Run the benchmark. This signature will use the "Forward Anchor" strategy.
    run_benchmark("Frequent First Byte (Forward Anchor)", signature_str, data_buffer, expected_address);
}

int main() {
    try {
        // Use a single, seeded random generator for the whole benchmark suite
        // for better reproducibility.
        std::mt19937 gen(std::random_device{}());

        std::vector<size_t> test_sizes = {
            1 * 1024 * 1024,    // 1 MB
            10 * 1024 * 1024,   // 10 MB
            50 * 1024 * 1024,   // 50 MB
            100 * 1024 * 1024   // 100 MB
        };

        for (size_t size : test_sizes) {
            run_all_benchmarks_for_size(size, gen);
            run_frequent_first_byte_benchmark(size, gen);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}