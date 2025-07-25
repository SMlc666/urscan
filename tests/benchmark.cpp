#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <sstream>
#include <iomanip>
#include <cstring> // For std::memcpy
#include <random>
#include "ur/signature.hpp"
#include "plf_nanotimer.h"

// --- Utility Functions ---

// A simple random data generator
std::vector<std::byte> generate_random_data(size_t size) {
    std::vector<std::byte> data(size);
    std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> distrib(0, 255);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>(distrib(gen));
    }
    return data;
}

// Generates a random signature string with a given length and wildcard ratio
std::string generate_random_signature(size_t length, double wildcard_ratio, bool force_start_wildcard = false, bool force_end_solid = false) {
    std::stringstream ss;
    std::mt19937 gen(std::random_device{}());
    std::uniform_real_distribution<> ratio_dist(0.0, 1.0);
    std::uniform_int_distribution<> hex_dist(0, 255);

    for (size_t i = 0; i < length; ++i) {
        bool is_wildcard = ratio_dist(gen) < wildcard_ratio;
        if (force_start_wildcard && i < length / 2) {
            is_wildcard = true;
        }
        if (force_end_solid && i >= length - 2) { // Ensure last few bytes are solid
            is_wildcard = false;
        }

        if (is_wildcard) {
            ss << "?? ";
        } else {
            ss << std::hex << std::setw(2) << std::setfill('0') << hex_dist(gen) << " ";
        }
    }
    return ss.str();
}

// Helper to convert a signature string to a byte vector for test data injection.
std::vector<std::byte> pattern_from_string(std::string_view s) {
    std::vector<std::byte> pattern;
    pattern.reserve(s.length() / 3); // Approx.

    auto hex_to_val = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };

    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ' ') continue;
        if (s[i] == '?') {
            pattern.push_back(std::byte{0}); // Value doesn't matter for injection
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

void run_benchmark(const std::string& test_name, size_t sig_length, double wildcard_ratio, bool force_start_wildcard, bool force_end_solid, std::vector<std::byte>& data_buffer) {
    plf::nanotimer timer;
    const int construction_runs = 100;
    const int scan_runs = 10;

    std::cout << "\n--- " << test_name << " (Length: " << sig_length 
              << ", Wildcard Ratio: " << std::fixed << std::setprecision(2) << wildcard_ratio << ") ---" << std::endl;

    // 1. Generate a representative random signature for this config
    std::string signature_str = generate_random_signature(sig_length, wildcard_ratio, force_start_wildcard, force_end_solid);
    
    // 2. Benchmark runtime_signature construction
    double total_construction_time = 0;
    for (int i = 0; i < construction_runs; ++i) {
        std::string temp_sig_str = generate_random_signature(sig_length, wildcard_ratio, force_start_wildcard, force_end_solid);
        timer.start();
        volatile ur::runtime_signature sig(temp_sig_str);
        total_construction_time += timer.get_elapsed_ns();
    }
    double avg_construction_time = total_construction_time / construction_runs;
    std::cout << "Runtime Signature Construction (avg over " << construction_runs << " runs): " 
              << avg_construction_time / 1000.0 << " us" << std::endl;

    // 3. Benchmark runtime_signature scanning
    auto scan_sig = std::make_shared<ur::runtime_signature>(signature_str);
    
    auto pattern_to_place = pattern_from_string(signature_str);
    if (data_buffer.size() > pattern_to_place.size()) {
        size_t sig_pos = data_buffer.size() / 2;
        std::memcpy(data_buffer.data() + sig_pos, pattern_to_place.data(), pattern_to_place.size());
    }

    double total_scan_time = 0;
    for (int i = 0; i < scan_runs; ++i) {
        timer.start();
        volatile auto result = scan_sig->scan(data_buffer);
        total_scan_time += timer.get_elapsed_ns();
    }
    double avg_scan_time = total_scan_time / scan_runs;
    std::cout << "Runtime Signature Scan (avg over " << scan_runs << " runs):         " 
              << avg_scan_time / 1000.0 << " us" << std::endl;
}

void run_benchmark_for_size(size_t data_size) {
    std::cout << "\n=========================================================" << std::endl;
    std::cout << "Benchmarking with " << data_size / (1024 * 1024) << " MB of data." << std::endl;
    std::cout << "=========================================================" << std::endl;

    auto data_buffer = generate_random_data(data_size);

    // Define different signature characteristics to test
    run_benchmark("Standard Test", 16, 0.10, false, false, data_buffer);
    run_benchmark("Standard Test", 16, 0.50, false, false, data_buffer);
    run_benchmark("Standard Test", 64, 0.10, false, false, data_buffer);
    run_benchmark("Standard Test", 64, 0.50, false, false, data_buffer);
    
    // Add the special case for the user
    run_benchmark("USER SPECIAL CASE", 32, 0.50, true, true, data_buffer);
}

int main() {
    try {
        std::vector<size_t> test_sizes = {
            1 * 1024 * 1024,    // 1 MB
            10 * 1024 * 1024,   // 10 MB
            100 * 1024 * 1024,  // 100 MB
            233 * 1024 * 1024   // 233 MB
        };

        for (size_t size : test_sizes) {
            run_benchmark_for_size(size);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}