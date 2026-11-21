# Orderflow_engine

基于 C++20 的高性能订单流与限价订单簿（LOB）重建引擎：从 A 股 L2/L3 逐笔数据重建完整订单簿，并提取盘口微观结构因子。

## 项目定位

`Orderflow_engine` 作为量化研究架构中的底层 C++ 计算内核，由上层平台因子层（`Quant_Platform`）通过 Python 绑定调用：

- **上游输入**：经过清洗与沪深格式归一化的逐笔事件流（`Event`）
- **核心引擎**：实时维护买卖双侧限价订单簿（`OrderBook` / `PriceLevel` / `Order`）与竞价状态机
- **下游输出**：按固定采样周期提取盘口微观结构快照特征（19 维特征），供分钟频因子聚合与研究使用

## 当前进度

- **已完成**
  - CMake 构建系统（C++20，自动识别 `include/` 与 `src/`，支持静态库/头文件库自适应）
  - 核心基础类型定义（[types.h](include/orderflow_engine/types.h)：`EventType`、`Side`、`Event`）
  - 订单节点结构（[order.h](include/orderflow_engine/order.h)：`Order` 节点、侵入式双向指针 `prev`/`next` 与价位回指 `void* level`）
  - 价位队列管理（[price_level.h](include/orderflow_engine/price_level.h)、[price_level.cpp](src/price_level.cpp)：`PriceLevel` 侵入式双向链表与增量量价维护 `add_order` / `reduce_order` / `remove_order`）
- **进行中**
  - `OrderBook` 核心事件处理（`apply`、`on_add`、`on_cancel`、`on_execute`、`pending_trades` 与集合竞价状态机）
- **计划中**
  - 单元测试套件与十档快照对拍验证
  - 19 维微观结构特征提取器（`BookSnapshot` / `FeatureExtractor`）
  - `pybind11` Python 绑定接口
  - 高性能优化（`free-list` 对象池、`flat_hash_map`、有序连续数组等）

## 目录结构

```text
Orderflow_engine/
├── CMakeLists.txt
├── README.md
├── docs/
│   ├── DESIGN.md           # 架构设计与工程边界
│   ├── DATA_CONTRACT.md    # 原始数据契约与清洗归一规则
│   ├── FEATURES.md         # 19 个微观结构特征定义与约束
│   └── TESTING.md          # 单元测试场景与快照对拍验证方案
├── include/
│   └── orderflow_engine/
│       ├── types.h         # EventType, Side, Event
│       ├── order.h         # Order 活跃挂单节点
│       ├── price_level.h   # PriceLevel 单价位 FIFO 队列
│       └── order_book.h    # OrderBook 订单簿接口（开发中）
├── src/
│   └── price_level.cpp     # PriceLevel 实现
├── tests/                  # 单元测试（规划中）
└── pybind/                 # Python 绑定（规划中）
```

## 依赖环境

- **核心 C++ 引擎**：CMake >= 3.20、支持 C++20 的编译器（Clang / GCC / MSVC）
- **Python 集成层（规划中）**：Python 3.10+、`pybind11`、NumPy、PyArrow

## 构建与测试

### 编译构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### CMake 构建选项

| 选项 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `ORDERFLOW_ENGINE_BUILD_TESTS` | `OFF` | 构建单元测试（需存在 `tests/CMakeLists.txt`） |
| `ORDERFLOW_ENGINE_BUILD_EXAMPLES` | `OFF` | 构建示例程序（需存在 `examples/CMakeLists.txt`） |
| `ORDERFLOW_ENGINE_ENABLE_WARNINGS` | `ON` | 为静态库目标启用严格编译警告（`-Wall -Wextra -Wpedantic`） |

### 运行测试（规划中）

接入 `tests/` 目录后，可通过以下方式编译并运行全部测试：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DORDERFLOW_ENGINE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### 作为子项目集成

```cmake
add_subdirectory(Orderflow_engine)
target_link_libraries(your_target PRIVATE orderflow_engine::orderflow_engine)
```

## 项目文档

- [架构与设计决策](docs/DESIGN.md)：四层结构、核心数据结构选型、事件处理路径、竞价状态机与优化路线
- [数据契约与清洗规则](docs/DATA_CONTRACT.md)：`cn_tick_l2` 三表字段、沪深市场差异、归一规则与时段边界
- [微观结构特征定义](docs/FEATURES.md)：19 个盘口特征的定义、计算公式、采样频率与值域约束
- [验证方案](docs/TESTING.md)：10 个核心单元测试场景、十档快照对拍基准与最小手算序列
