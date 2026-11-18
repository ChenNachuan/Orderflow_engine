# 00. LOB Engine 项目总览与公司 cx330 对标架构

> 本文档整理自 `LOB_enigne` 主目录下的 5 份架构分析记录，对标公司 `cx330` / `NeoLOB` 生产代码与个人 `lob_engine` 工程。

---

## 一、`cx330` 完整代码架构（实测结构）

```text
cx330/
├── cx330/
│   ├── book/       ★ pulsar OrderBook（带 QueuePosition）
│   ├── neolob/     ★ NeoLOB 框架（21 个交易所的 OrderBook 实现）
│   ├── neobar/     NeoBar（K线引擎）
│   ├── umpmd/      UMP 行情管线（per-exchange stages）
│   ├── neomd/      行情载体 / 导出
│   └── common/     公共工具
├── submodules/
│   ├── sauvignon/  数据管线（内含 eureka）
│   └── ethereum/   数据源适配层
├── hubble/         应用框架（DataBus 等）
└── etc/            配置文件
```

---

## 二、`QueuePosition` —— LOB 引擎最终对标目标

- **源码路径**：`cx330/book/QueuePosition.hpp`
- **关键澄清**：`"OBQP"` 仅是 Confluence / 知识库中的产品或项目名称，在全部源码中字面量出现 **0 次**；代码中的真实实现是 `cx330::QueuePosition` 与 `GetQueuePosition()`。

```cpp
namespace cx330 {
struct QueuePosition {
    int64_t qty_ahead;      // 前方挂单总量
    int32_t orders_ahead;   // 前方挂单笔数
    int64_t qty_behind;     // 后方挂单总量
    int32_t orders_behind;  // 后方挂单笔数
};
}
```

- **调用位置**：`cx330/book/OrderBook.hpp`（第 1816 行）
  - `Status GetQueuePosition(order_id_t, QueuePosition*);`
  - `Status GetQueuePositionOfNode(...);`
  - 注意：仅在 **Full Mode**（含 `intrusive list` 的完整模式）可用，**Summary Mode**（无 per-order 迭代）不支持。

---

## 三、`NeoLOB` —— 21 个交易所的 OrderBook 引擎

- **路径**：`cx330/neolob/engine/instances/`（每个交易所一个实现目录）
  - `cn/`：`SSEOrderBook` + `SZEOrderBook`（**A 股直接参考**）
  - 其余包括：`nas/`、`nys/`、`cboe/`、`cme/`、`ice/`、`hkg/`、`tse/`、`nse/`、`crypto/`、`iex/`、`memx/`、`usc/`、`usbbo/`、`cnc/`、`cnb/`、`nsefut/`、`themis/`、`diy/` 等。
- **架构特点**：零虚函数，全 template 编译期多态。
- **入口**：`OrderBookCuratorSteward<Preferences>`
- **处理管线**：
  ```text
  Mediator.OnEvent() → OrderBook.OnEvent() → DoPreProcess
    → Builder → LevelManager → DoPostProcess
    → SnapshotUpdater → Emitter.Emit()
  ```
- `cn/SSEOrderBook` 与 `cn/SZEOrderBook` 处理沪深交易所逐笔数据（对应个人平台的 `cn_tick_l2`）。

---

## 四、`OrderBook` 的两种模式（Full Mode vs Summary Mode）

在 `cx330/book/OrderBook.hpp` 中提供两种模式：

| 维度 | Full Mode（完整模式） | Summary Mode（汇总模式） |
| :--- | :--- | :--- |
| 链表结构 | 含 per-order `intrusive list`（可遍历每个 order） | 无 per-order 迭代 |
| 节点内存对齐 | 64B cache-line 对齐节点 | 48B 节点（EBO 优化掉 `prev/next`） |
| 维护内容 | 价位聚合 + 订单级排队链 | 仅维护价位级聚合（`price, volume, count`） |
| `GetQueuePosition()` | 支持 | 不支持 |
| 适用场景 | OBQP 排队追踪、需要 order-level 信息的场景 | 只需盘口快照的场景（更快、更省内存） |

**对 `lob_engine` 的建议**：
- **M1~M3 阶段**仅计算盘口因子（`imbalance / spread / depth`），采用 **Summary 思路** 即可。
- 未来若扩展至**排队模拟**（模拟自有订单排队位置），再升级至 **Full Mode**。

---

## 五、`NeoLOB` 的 `Preferences/Traits` 体系（对标清洗层）

每个交易所拥有独立的 `Preferences` 文件：
- `cx330/neolob/ordbook/steward/SSEPreferences.hpp`（上交所）
- `cx330/neolob/ordbook/steward/SZEPreferences.hpp`（深交所）

`Preferences` 枚举了市场规则差异：
1. 集合竞价如何处理
2. 撤单事件所在数据流（如深市在成交流中通过成交代码 `'C'` 标识）
3. 价格精度（`×10000` vs `×1e9`）
4. 涨跌停板处理

> **设计映射**：公司通过 `Preferences` 模板特化做编译期分派；`lob_engine` 通过 Python 清洗层代码处理沪深差异，核心思路完全一致。

---

## 六、完整的 Replay 基础设施（5 个层次）

1. **层次 1：OBQP 单进程 Replay**（`MockDataBus + cx330ReplayService + 5 路 FileReader + MergedPriorityQueue`）→ 验证核心逻辑。**`lob_engine` 对标层**：L2 逐笔流回放 + 十档快照对拍。
2. **层次 2：OBQP 三进程集成测试**（`uma_mdreplay` + `order_flow_replay` + `obqp_service`）→ 验证服务化订阅、积压与重启恢复。
3. **层次 3：`image-builder` 标准 Replay**（`/app/nebula_git/image-builder/etc/replay/replay-stock-cn.json` 等 per-market 配置化）。
4. **层次 4：`mm-luna` 精密 Replay**（`gen_replay.py → run_uni_replay.py → compare_replay_vs_kafka.py`，回放与 Kafka 实时对比）。
5. **层次 5：`eureka PcapReplayerIO`**（直接重放 pcap 文件 UDP 包，网络层 replay）。

---

## 七、`NeoLOB` 数据流管线与事件体系

```text
原始行情 (UMP 协议)
  → IChannel（行情接入）
  → Metronome（节拍器 / 定时器）
  → DecoderStage（解码：per-symbol node pool + order_id→node 索引）
  → LobStage（LOB 维护：每 symbol 一个 Book，价位链表 node↔node↔node）
  → BookObserver / BarStage / 自定义 IndicatorStage
  → Snapshot / BarData 输出
```

**事件类型**：
- 生命周期：`Init`、`Bod`（开盘）、`Eod`（收盘）、`Finalize`
- 原始 UMP：`RawUmpEvent`
- 归一化行情：`AddOrder`、`CancelOrder`、`ReplaceOrder`、`OrderExecuted`、`Trade`
- LOB 事件：`LobPreReset`、`LobPostReset`、`LobPreOrderBookUpdate`、`LobPostOrderBookUpdate`
- Bar 与快照：`BarEvent`、`SnapshotEvent`

**与 `lob_engine` 的映射**：
- `Event(ADD / CANCEL / EXECUTE)` ↔ `AddOrder / CancelOrder / OrderExecuted`
- `OrderBook.apply()` ↔ `LobStage`
- `FeatureExtractor` ↔ `BookObserver / IndicatorStage`
- 快照对拍 ↔ `SnapshotEvent`

---

## 八、补充发现：单测 API、预分配参数与微观结构因子

### 1. `QueuePositionTest.cpp` 核心用法
路径：`cx330/neolob/tests/orderbook/base/QueuePositionTest.cpp`

```cpp
OrderBook<BaseTypeDictionary> ob_;
ob_.Init(Config::ForPeakOrders(1024, 64)); // 预分配 1024 order, 64 价位

// 加三笔买单在同一价位 (100)
ob_.AddOrder(true, 1, 100, 300); // order_id=1, price=100, volume=300
ob_.AddOrder(true, 2, 100, 500); // order_id=2
ob_.AddOrder(true, 3, 100, 200); // order_id=3

// 查 order_id=2 的排队位置
QueuePosition pos;
ob_.GetQueuePosition(2, &pos);
// qty_ahead = 300, orders_ahead = 1
// qty_behind = 200, orders_behind = 1
```

### 2. `Config::ForPeakOrders` 预分配策略
- 公司采用“预估峰值一次分配”而非动态增长。
- A 股单只股票一天活跃挂单峰值约几万～十几万，M4 阶段可参考配置如 `ForPeakOrders(131072, 1024)`。

### 3. `Volab` 的 `HK HF` 指标（对应 M3 `FeatureExtractor`）
路径：`volab_grey/src/dlc/src/EventBricks/hf_indicators/`
- `hk_hf_common.h`（克隆自 `lux OrderTracker` 的 tick-test 买卖方向推断逻辑）
- `IDCS_HkTradeFlowFactors_Global.cpp`（港股成交流因子）
- `IDCS_HkMicrostructure_Global.cpp`（港股微观结构因子）

---

## 九、对 `lob_engine` 的指导意义汇总与文件阅读清单

### 核心指导意义
1. **读 `SSEOrderBook + SZEOrderBook`**：掌握沪深差异具体处理（深市撤单码 `'C'`、沪市委托缺失期降级等）。
2. **读 `SSEPreferences + SZEPreferences`**：获取市场规则差异完整枚举（集合竞价、涨跌停、价格精度等）。
3. **Summary Mode vs Full Mode**：明确 M3（盘口因子）与未来排队模拟的边界。
4. **Replay 对标**：`parquet 读取 → 按 seq 排序 → 驱动 OrderBook.apply()` 同构于公司单进程回放。
5. **Stage 分离**：`DecoderStage + LobStage` 分离即对应 `Python 清洗层 + C++ lob_engine` 分离。

### 建议精读文件优先级
- **最高优先级（直接指导 M1~M3）**：
  1. `cx330/neolob/engine/instances/cn/SSEOrderBook.hpp`
  2. `cx330/neolob/engine/instances/cn/SZEOrderBook.hpp`
  3. `cx330/neolob/ordbook/steward/SSEPreferences.hpp`
  4. `cx330/neolob/ordbook/steward/SZEPreferences.hpp`
  5. `cx330/book/OrderBook.hpp`
- **次要优先级（理解底层框架）**：
  6. `cx330/book/QueuePosition.hpp`
  7. `eureka/types/containers/lob/Level.hpp`
  8. `eureka/types/containers/lob/LevelContainer.hpp`
  9. `eureka/types/containers/list/IntrusiveList.hpp`
