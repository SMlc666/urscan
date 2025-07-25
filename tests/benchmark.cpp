#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <iomanip>
#include <cstring>
#include <random>
#include <memory>
#include <type_traits>
#include "ur/signature.hpp"
#include "plf_nanotimer.h"

// --- Utility Functions ---

// Generates a buffer of random byte data.
std::vector<std::byte> generate_random_data(size_t size) {
    std::vector<std::byte> data(size);
    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> distrib(0, 255);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>(distrib(gen));
    }
    return data;
}

// Helper to convert a signature string to a byte vector for test data injection.
// This version correctly handles wildcards for injection purposes.
std::vector<std::byte> pattern_from_string(std::string_view s) {
    std::vector<std::byte> pattern;
    pattern.reserve(s.length() / 3);

    auto hex_to_val = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0; // Should not happen with valid input
    };

    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ' ') continue;
        if (s[i] == '?') {
            // For data injection, we can't use a wildcard.
            // We'll just use a placeholder byte. The signature will still use the wildcard.
            pattern.push_back(std::byte{0x90}); // A common NOP instruction byte
            if (i + 1 < s.size() && s[i+1] == '?') i++;
        } else {
            if (i + 1 >= s.size()) break;
            uint8_t high = hex_to_val(s[i++]);
            uint8_t low = hex_to_val(s[i]);
            pattern.push_back(static_cast<std::byte>((high << 4) | low));
        }
    }
    return pattern;
}


// --- Benchmark Core ---

void run_benchmark(const std::string& test_name, const std::string& signature_str, std::vector<std::byte>& data_buffer) {
    plf::nanotimer timer;
    const int scan_runs = 10;

    std::cout << "\n--- Benchmarking: " << test_name << " ---" << std::endl;
    std::cout << "    Signature: " << signature_str << std::endl;

    // 1. Benchmark construction (less critical, but good to have)
    timer.start();
    auto scan_sig = std::make_shared<ur::runtime_signature>(signature_str);
    double construction_time = timer.get_elapsed_ns();
    std::cout << "    Construction Time: " << std::fixed << std::setprecision(3) << construction_time / 1000.0 << " us" << std::endl;

    // 2. Prepare data buffer by injecting the pattern
    auto pattern_to_place = pattern_from_string(signature_str);
    if (data_buffer.size() > pattern_to_place.size()) {
        // Place signature in the middle third of the buffer to ensure it's not at an edge
        size_t offset = data_buffer.size() / 3 + (rand() % (data_buffer.size() / 3));
        std::memcpy(data_buffer.data() + offset, pattern_to_place.data(), pattern_to_place.size());
    }

    // 3. Benchmark scanning
    double total_scan_time = 0;
    for (int i = 0; i < scan_runs; ++i) {
        timer.start();
        // The result is made volatile to prevent the compiler from optimizing the call away.
        volatile auto result = scan_sig->scan(data_buffer);
        total_scan_time += timer.get_elapsed_ns();
        // Simple verification
        if (!const_cast<std::remove_volatile_t<decltype(result)>&>(result).has_value() && i == 0) {
             std::cerr << "Warning: Scan did not find the injected signature for test '" << test_name << "'." << std::endl;
        }
    }
    double avg_scan_time = total_scan_time / scan_runs;
    std::cout << "    Average Scan Time (over " << scan_runs << " runs): "
              << avg_scan_time / 1000.0 << " us" << std::endl;
}

void run_all_benchmarks_for_size(size_t data_size) {
    std::cout << "\n=========================================================" << std::endl;
    std::cout << "Benchmarking with " << data_size / (1024 * 1024) << " MB of data." << std::endl;
    std::cout << "=========================================================" << std::endl;

    auto data_buffer = generate_random_data(data_size);

    // --- Define Representative Signatures for Each Strategy ---

    // 1. Simple: No wildcards. Should be the fastest.
    run_benchmark("Simple Strategy", "48 89 5C 24 08 57 48 83 EC 20", data_buffer);

    // 2. Forward Anchor: Starts with solid bytes, ends with wildcards.
    run_benchmark("Forward Anchor Strategy", "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 41 56 ?? ?? ?? ??", data_buffer);

    // 3. Backward Anchor: Starts with wildcards, ends with solid bytes.
    run_benchmark("Backward Anchor Strategy", "?? ?? ?? ?? 48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 41 56", data_buffer);

    // 4. Dual Anchor: Solid bytes at both ends, wildcards in the middle.
    run_benchmark("Dual Anchor Strategy", "48 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B", data_buffer);

    // 5. Dynamic Anchor: Wildcards at both ends, solid bytes in the middle.
    run_benchmark("Dynamic Anchor Strategy", "?? ?? 48 8B C4 48 89 58 08 48 89 70 10 ?? ??", data_buffer);
}

int main() {
    try {
        srand(time(0)); // Seed for random placement of signature

        std::vector<size_t> test_sizes = {
            1 * 1024 * 1024,    // 1 MB
            10 * 1024 * 1024,   // 10 MB
            50 * 1024 * 1024,   // 50 MB
            100 * 1024 * 1024   // 100 MB
        };

        for (size_t size : test_sizes) {
            run_all_benchmarks_for_size(size);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}