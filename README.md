# sign

`sign` 是一个高性能、仅头文件的 C++20 内存特征码扫描库。它为在内存中快速定位字节序列（特征码）而设计，并针对速度进行了深度优化，利用了现代 C++、多线程和 SIMD 指令集。

## 核心特性

- **仅头文件**: 只需包含 `include/ur/signature.hpp` 即可轻松集成到任何项目中。
- **高性能**: 利用多线程和 ARM NEON SIMD 优化实现极速扫描。
- **IDA 风格特征码**: 支持带有通配符（`?`）的标准 IDA 风格特征码。
- **动态扫描策略**: 自动分析特征码模式，并选择最优的扫描策略（例如，前向锚点、后向锚点或双向锚点）以获得最佳性能。
- **现代 C++**: 使用 C++20 特性编写，代码现代、高效。
- **易于使用**: 简洁的 API 设计，使得执行内存扫描变得非常简单。

## 构建与运行

该项目使用 `xmake` 作为构建系统。

### 要求

- C++20 兼容的编译器（如 GCC 10+, Clang 11+）
- [xmake](https://xmake.io/)

### 步骤

1.  **克隆仓库**:
    ```bash
    git clone <repository_url>
    cd sign
    ```

2.  **构建项目**:
    ```bash
    xmake
    ```

3.  **运行单元测试**:
    ```bash
    xmake run tests
    ```

4.  **运行性能基准测试**:
    ```bash
    xmake run benchmark
    ```

## 使用示例

以下是如何使用 `sign` 库在内存区域中查找特征码的简单示例。

```cpp
#include <iostream>
#include <vector>
#include <ur/signature.hpp>

int main() {
    // 目标内存区域
    std::vector<std::uint8_t> memory_buffer = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9
    };

    // 定义一个 IDA 风格的特征码，使用 '?' 作为通配符
    const char* pattern = "48 89 5C 24 ? 57 48 83";

    try {
        // 创建一个 runtime_signature 实例
        ur::runtime_signature signature(pattern);

        // 在内存缓冲区中扫描特征码
        auto result = signature.scan(memory_buffer.data(), memory_buffer.size());

        if (result) {
            // 如果找到，打印匹配地址的偏移
            std::cout << "Pattern found at offset: " << (result - memory_buffer.data()) << std::endl;
        } else {
            std::cout << "Pattern not found." << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}
```

## API 简介

### `ur::runtime_signature`

这是库的核心类。

- **构造函数**: `runtime_signature(const char* pattern)`
  - 接收一个 IDA 风格的特征码字符串。如果模式无效，将抛出异常。

- **扫描方法**: `scan(const void* data, std::size_t size)`
  - 在给定的内存区域（`data`）和大小（`size`）中搜索特征码。
  - 如果找到匹配项，返回指向匹配位置开头的指针。
  - 如果未找到，返回 `nullptr`。

## 编译时选项

可以通过宏来启用或禁用特定功能：

- `UR_ENABLE_MULTITHREADING`: 启用多线程扫描以提高在大型内存区域上的性能。
- `UR_ENABLE_NEON_OPTIMIZATION`: 在支持的 ARM 平台上启用 NEON SIMD 优化以加速扫描。

这些宏可以在构建时通过 `xmake` 定义，例如：
```bash
xmake f -D UR_ENABLE_MULTITHREADING=true
xmake
```

## 依赖项

- **GTest**: 用于单元测试。
- **plf_nanotimer**: 用于性能基准测试。

所有依赖项都由 `xmake` 自动下载和管理。

## 许可证

该项目在 [LICENSE](LICENSE) 下获得许可。
