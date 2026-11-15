# Orderflow_engine

基于 C++20 的高性能订单流（Order Flow）处理与市场微观结构引擎（处于早期初始化开发阶段）。

## 项目简介

`Orderflow_engine` 面向低延迟场景，致力于实现 L2/L3 逐笔行情事件处理、限价订单簿（Limit Order Book, LOB）重建以及实时订单流微观结构特征计算。

## 当前进展

项目目前处于基础脚手架搭建阶段：

- **核心基础类型**（`include/of_engine/types.h`）：
  - 定义 `orderflow_engine::EventType` 枚举（`ADD` 新增委托、`CANCEL` 撤单、`EXECUTE` 撮合成交、`MODIFY` 改单）
- **构建系统**（`CMakeLists.txt`）：
  - 要求 CMake 3.20+ 与 C++20 标准（`CMAKE_CXX_STANDARD 20`）
  - 提供库目标 `of_engine`（别名 `of_engine::of_engine`）：当 `src/` 下暂无源文件时自动作为 `INTERFACE` 纯头文件库暴露；添加 `.cpp` / `.cc` 后自动切换为 `STATIC` 静态库

## 目录结构

```text
Orderflow_engine/
├── CMakeLists.txt
├── README.md
└── include/
    └── of_engine/
        └── types.h
```

## 构建与集成

### 环境要求

- CMake >= 3.20
- 支持 C++20 的编译器（Clang / GCC / MSVC）

### 编译构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### CMake 构建选项

| 选项 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `OF_ENGINE_BUILD_TESTS` | `OFF` | 构建单元测试（需存在 `tests/CMakeLists.txt`） |
| `OF_ENGINE_BUILD_EXAMPLES` | `OFF` | 构建示例程序（需存在 `examples/CMakeLists.txt`） |
| `OF_ENGINE_ENABLE_WARNINGS` | `ON` | 为静态库目标启用严格编译警告 |

### 作为子项目集成

```cmake
add_subdirectory(Orderflow_engine)
target_link_libraries(your_target PRIVATE of_engine::of_engine)
```

## 后续开发规划

- [ ] 核心行情与订单事件数据结构（买卖方向 `Side`、逐笔委托/成交结构体、纳秒时间戳等）
- [ ] 高性能限价订单簿（LOB）重建与价位档位管理
- [ ] 订单流与微观结构指标计算（OFI、Volume Delta、Micro-price、买卖价差及流动性特征等）
- [ ] 单元测试与低延迟基准测试（Benchmark）
