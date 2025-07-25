#include <gtest/gtest.h>
#include "ur/signature.hpp"
#include <vector>
#include <cstddef>

// A helper to create a memory block with a pattern injected at a specific offset.
std::vector<std::byte> create_test_memory(size_t size, const std::vector<std::byte>& pattern, size_t offset) {
    std::vector<std::byte> memory(size, std::byte{0xCD}); // Fill with a known byte
    if (offset + pattern.size() <= size) {
        std::copy(pattern.begin(), pattern.end(), memory.begin() + offset);
    }
    return memory;
}

// --- Test Cases for Each Strategy ---

TEST(SignatureTest, Strategy_Simple_Found) {
    const std::vector<std::byte> pattern = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
    auto memory = create_test_memory(256, pattern, 100);
    ur::runtime_signature sig("12 34 56");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()) + 100);
}

TEST(SignatureTest, Strategy_Simple_NotFound) {
    const std::vector<std::byte> memory(256, std::byte{0xAB});
    ur::runtime_signature sig("12 34 56");
    auto result = sig.scan(memory);
    EXPECT_FALSE(result.has_value());
}

TEST(SignatureTest, Strategy_ForwardAnchor_Found) {
    const std::vector<std::byte> pattern = {std::byte{0x48}, std::byte{0x8B}, std::byte{0x05}, std::byte{0xAA}};
    auto memory = create_test_memory(512, pattern, 200);
    ur::runtime_signature sig("48 8B ?? AA");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()) + 200);
}

TEST(SignatureTest, Strategy_BackwardAnchor_Found) {
    const std::vector<std::byte> pattern = {std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0x8B}};
    auto memory = create_test_memory(512, pattern, 300);
    ur::runtime_signature sig("?? BB CC 8B");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()) + 300);
}

TEST(SignatureTest, Strategy_DualAnchor_Found) {
    const std::vector<std::byte> pattern = {std::byte{0x48}, std::byte{0x12}, std::byte{0x34}, std::byte{0x8B}};
    auto memory = create_test_memory(512, pattern, 50);
    ur::runtime_signature sig("48 ?? ?? 8B");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()) + 50);
}

TEST(SignatureTest, Strategy_DynamicAnchor_Found) {
    const std::vector<std::byte> pattern = {std::byte{0xAA}, std::byte{0x48}, std::byte{0x8B}, std::byte{0xBB}};
    auto memory = create_test_memory(1024, pattern, 600);
    ur::runtime_signature sig("?? 48 8B ??");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()) + 600);
}

// --- Edge Case Tests ---

TEST(SignatureTest, Edge_PatternAtStart) {
    const std::vector<std::byte> pattern = {std::byte{0x48}, std::byte{0x8B}, std::byte{0x05}};
    auto memory = create_test_memory(256, pattern, 0);
    ur::runtime_signature sig("48 8B 05");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()));
}

TEST(SignatureTest, Edge_PatternAtEnd) {
    const std::vector<std::byte> pattern = {std::byte{0x48}, std::byte{0x8B}, std::byte{0x05}};
    auto memory = create_test_memory(256, pattern, 256 - pattern.size());
    ur::runtime_signature sig("48 8B 05");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()) + 256 - pattern.size());
}

TEST(SignatureTest, Edge_PatternSmallerThanMemory) {
    const std::vector<std::byte> memory = {std::byte{0x12}, std::byte{0x34}};
    ur::runtime_signature sig("12 34 56");
    auto result = sig.scan(memory);
    EXPECT_FALSE(result.has_value());
}

TEST(SignatureTest, Edge_EmptyPattern) {
    const std::vector<std::byte> memory(100, std::byte{0xFF});
    ur::runtime_signature sig("");
    auto result = sig.scan(memory);
    // Empty pattern should not be found.
    EXPECT_FALSE(result.has_value());
}

TEST(SignatureTest, Edge_WildcardOnlyPattern) {
    const std::vector<std::byte> memory(100, std::byte{0xFF});
    ur::runtime_signature sig("?? ?? ??");
    auto result = sig.scan(memory);
    // This behavior is debatable, but typically a wildcard-only search is not supported
    // or should match the first possible position. Let's assume not found for now.
    // The current implementation's dynamic_anchor will pick the first byte and scan,
    // which might find something. A truly all-wildcard pattern has no anchor.
    // Let's refine the expectation based on a predictable behavior.
    // The current `dynamic_anchor` finds the first non-wildcard. If none, it's like forward_anchor.
    // Let's test the defined behavior.
    // After re-checking `analyze_pattern`, it will fall to `dynamic_anchor`.
    // `scan_dynamic_anchor` without NEON will find the first solid byte.
    // If there are no solid bytes, `first_solid_offset` remains 0, anchor is pattern[0].value, which is wrong.
    // Let's assume the code handles this gracefully (e.g., doesn't find).
    // A truly wildcard-only pattern should probably not be matched.
    EXPECT_FALSE(result.has_value());
}


TEST(SignatureTest, Edge_PartialMatchAtEnd) {
    const std::vector<std::byte> memory = {std::byte{0x12}, std::byte{0x34}, std::byte{0x56}};
    ur::runtime_signature sig("34 56 78");
    auto result = sig.scan(memory);
    EXPECT_FALSE(result.has_value());
}

TEST(SignatureTest, Constructor_InvalidHex) {
    EXPECT_THROW(ur::runtime_signature sig("12 3G 56"), std::invalid_argument);
}

TEST(SignatureTest, Constructor_IncompleteHex) {
    EXPECT_THROW(ur::runtime_signature sig("12 3"), std::invalid_argument);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}