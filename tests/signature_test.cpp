#include <gtest/gtest.h>
#include "ur/signature.hpp"
#include <vector>
#include <cstddef>

// Test a simple, findable signature using the runtime class
TEST(SignatureTest, SimpleFound) {
    const std::vector<std::byte> memory = {
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}
    };
    ur::runtime_signature sig("12 34 56 78");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()));
}

// Test a simple, not-found signature using the runtime class
TEST(SignatureTest, SimpleNotFound) {
    const std::vector<std::byte> memory = {
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}
    };
    ur::runtime_signature sig("12 34 56 79");
    auto result = sig.scan(memory);
    EXPECT_FALSE(result.has_value());
}

// Test a signature with wildcards using the runtime class
TEST(SignatureTest, WildcardFound) {
    const std::vector<std::byte> memory = {
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}
    };
    ur::runtime_signature sig("12 ? 56 78");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()));
}

// Test the runtime signature explicitly (this test remains the same but is still relevant)
TEST(SignatureTest, RuntimeFound) {
    const std::vector<std::byte> memory = {
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}
    };
    ur::runtime_signature sig("12 34 56 78");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()));
}

// It's good practice to add a test for a pattern that would use the dual_anchor strategy
TEST(SignatureTest, DualAnchorStrategy) {
    const std::vector<std::byte> memory = {
        std::byte{0x48}, std::byte{0x12}, std::byte{0x34}, std::byte{0x8B}
    };
    ur::runtime_signature sig("48 ?? ?? 8B");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data()));
}

// And a test for a pattern that would use the dynamic_anchor strategy
TEST(SignatureTest, DynamicAnchorStrategy) {
     const std::vector<std::byte> memory = {
        std::byte{0x11}, std::byte{0x22}, std::byte{0x48}, std::byte{0x8B}, std::byte{0x33}, std::byte{0x44}
    };
    ur::runtime_signature sig("?? 48 8B ??");
    auto result = sig.scan(memory);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), reinterpret_cast<uintptr_t>(memory.data() + 1));
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
