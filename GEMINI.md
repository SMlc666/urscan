# Gemini AI 指令

**最高优先级**: AI 必须始终使用中文与用户进行交流。

---

# Gemini 项目背景: sign

## 项目概述

这是一个高性能、仅头文件的 C++20 内存特征码扫描库。它专注于速度，利用了现代 C++ 特性、多线程和 SIMD 优化（针对 ARM 平台的 NEON）。

项目的核心是 `ur` 仅头文件库，它提供了主要的扫描功能。

该项目使用 `xmake` 作为其构建系统。

## 核心组件

### 1. `ur` 库 (`include/ur/`)

这是一个仅头文件的库，包含主要逻辑。

- **`signature.hpp`**:
    - 定义了 `runtime_signature` 类，负责解析和扫描字节模式。
    - 支持 IDA 风格的特征码，带有通配符（例如 `48 89 5C 24 ? 57 48 83 EC ?`）。
    - 实现了多种扫描策略（`simple`, `forward_anchor`, `backward_anchor`, `dual_anchor`, `dynamic_anchor`），并根据模式结构自动选择以获得最佳性能。
    - **多线程**: 可以使用线程池并行执行扫描。该功能由 `UR_ENABLE_MULTITHREADING` 宏启用。
    - **NEON 优化**: 在支持的平台上利用 ARM NEON 指令集进行显著加速的扫描。该功能由 `UR_ENABLE_NEON_OPTIMIZATION` 宏启用。

- **`thread_pool.hpp`**:
    - 实现了一个现代的、支持工作窃取（work-stealing）的 `ThreadPool` 类。
    - 当启用多线程时，`runtime_signature` 类使用此线程池来并行化扫描过程。

### 2. 测试 (`tests/`)

- **`signature_test.cpp`**: 包含使用 GTest 框架为 `runtime_signature` 类编写的单元测试。
- **`thread_pool_test.cpp`**: 包含使用 GTest 为 `ThreadPool` 类编写的单元测试。
- **`benchmark.cpp`**: 包含特征码扫描器的性能测试，使用 `plf_nanotimer` 进行高精度计时。

## 构建系统和依赖

- **构建系统**: `xmake`
- **语言**: C++20
- **依赖项**:
    - `gtest`: 用于单元测试。
    - `plf_nanotimer`: 用于性能基准测试。

这些依赖项由 `xmake` 通过 `xmake.lua` 文件中的 `add_requires` 指令进行管理。

## 如何构建和运行

该项目完全由 `xmake` 管理。

- **构建所有目标**:
  ```bash
  xmake
  ```

- **运行单元测试**:
  ```bash
  xmake run tests
  ```

- **运行性能基准测试**:
  ```bash
  xmake run benchmark
  ```