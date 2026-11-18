# 01. LOB Engine 完整项目规划

> 本文档整理自「项目规划」目录下的 9 份规划设计资料，结合公司 `cx330` `neolob` 引擎与个人平台 `cn_tick_l2` 数据（QDC 网盘交付的深市逐笔委托 + 逐笔成交 + 十档快照三表），定义完整的 `C++ LOB 重建引擎 + Python 清洗/集成层` 方案。
>
> **端到端目标**：`cn_tick_l2` 原始 CSV → 重建完整订单簿 → 提取 19 个微观结构因子 → 聚合到分钟频 → 落盘到平台 `factors/minute/` 存储。

---

## 一、项目结构

### 1. C++ 引擎层（`libs/lob_engine/`）
```text
libs/lob_engine/
├── CMakeLists.txt
├── include/lob_engine/
│   ├── types.h        # Event, Side, EventType 基础类型
│   ├── order.h        # Order 节点（自带 void* level 回指）
│   ├── price_level.h  # PriceLevel（增量维护 volume/count + std::list 队列）
│   ├── order_book.h   # OrderBook（bids/asks std::map + order_index unordered_map）
│   ├── features.h     # BookSnapshot 结构体 + FeatureExtractor
│   └── order_pool.h   # OrderPool（free-list 对象池）
├── src/
│   ├── price_level.cpp
│   ├── order_book.cpp
│   └── features.cpp
├── pybind/
│   └── bind.cpp       # pybind11: extract_features(events) -> features_df
└── tests/
    ├── test_order_book.cpp  # 手算对拍 + 公司 10 场景
    └── test_features.cpp    # 特征值域断言
```

### 2. Python 清洗与因子层（在 `Quant_Platform` 内）
```text
scripts/ingestion/
└── tick_l2_cleaner.py     # cn_tick_l2 CSV → 统一 Event parquet

data/normalizers/
└── tick_l2_normalizer.py  # 沪深归一 + 集合竞价标记

research/factor_library/
└── lob_factors.py         # Factor ABC 实现，output_columns=19 列，调 lob_engine
```

---

## 二、C++ 核心数据结构

### 2.1 `Event`（清洗层产出，喂引擎的统一事件）
```cpp
enum class EventType : uint8_t { ADD = 1, CANCEL = 2, EXECUTE = 3 };
enum class Side : int8_t { BUY = 1, SELL = -1 };

struct Event {
    uint64_t timestamp; // 纳秒 (HHMMSSmmm × 1e6)
    uint64_t seq;       // ApplSeqNum（交易所委托号/成交编号，全局单调）
    uint64_t order_id;  // 本事件作用的委托号
    int64_t  price;     // ×10000 定点（直接用 QDC 原始值，不转 Price9）
    int64_t  volume;    // 股 (int64)
    EventType type;
    Side      side;
};
```

### 2.2 `Order`（簿中的活跃挂单节点）
```cpp
struct Order {
    uint64_t order_id;
    int64_t  price;           // ×10000 定点
    int64_t  volume;          // 剩余量（成交/撤单后递减）
    Side     side;
    uint64_t timestamp;
    void*    level = nullptr; // ★回指 PriceLevel（O(1) 定位，对标公司 BookOrder.level）
};
```

### 2.3 `PriceLevel`（单个价位的挂单队列）
```cpp
class PriceLevel {
public:
    int64_t price;
    int64_t total_volume = 0; // 增量维护（对标公司 PriceLevelSummary.volume_）
    int32_t order_count = 0;  // 增量维护（对标公司 PriceLevelSummary.order_count_）
    std::list<Order*> orders; // FIFO 队列（存指针，Order 由 pool 管理）

    void add(Order* o);                // 入队尾 + total_volume += o->volume + order_count++
    void reduce(Order* o, int64_t qty);// o->volume -= qty + total_volume -= qty
    void remove(Order* o);             // 整单移除 + total_volume -= o->volume + order_count--
    bool empty() const;
};
```

### 2.4 `OrderBook`（买卖两侧订单簿）
```cpp
class OrderBook {
public:
    std::map<int64_t, PriceLevel, std::greater<int64_t>> bids; // 买侧降序（best bid = begin）
    std::map<int64_t, PriceLevel> asks;                        // 卖侧升序（best ask = begin）

    // ★ O(1) 定位（对标公司 LevelContainerNew 的 flat_hash_map）
    std::unordered_map<uint64_t, Order*> order_index;

    OrderPool pool; // 对象池

    // 状态（对标公司 OrderBookCommon）
    bool is_good = true;             // order not found 等异常后变 false
    int64_t ob_state = 0;            // 不一致计数（crossed book 等）
    bool in_batch_auction = true;    // 集合竞价状态

    // ★ pending trade 兜底（对标公司 SSE AddLimitOrderCheckPendingTrade）
    std::unordered_map<uint64_t, int64_t> pending_trades; // order_id -> 已成交量

    void apply(const Event& e); // 唯一入口
    std::optional<int64_t> best_bid() const;
    std::optional<int64_t> best_ask() const;

private:
    void on_add(const Event& e);
    void on_cancel(const Event& e);
    void on_execute(const Event& e);
    void change_auction_status(uint64_t timestamp);
};
```

### 2.5 `OrderPool`（free-list 对象池）
```cpp
class OrderPool {
    static constexpr size_t BLOCK_SIZE = 4096;
    std::vector<std::unique_ptr<Order[]>> blocks;
    Order* free_head = nullptr;

public:
    Order* allocate();            // free list pop 或分配新 block
    void deallocate(Order* p);    // free list push（复用释放内存 next 指针）
};
```

### 2.6 `BookSnapshot` + `FeatureExtractor`（19 个微观结构特征）
```cpp
struct BookSnapshot {
    uint64_t timestamp;
    uint64_t seq;
    int64_t best_bid, best_ask, mid_price, spread;
    int64_t bid_depth_1, ask_depth_1;
    int64_t bid_depth_5, ask_depth_5;
    int64_t bid_depth_10, ask_depth_10;
    double  imbalance_1, imbalance_5, imbalance_10;
    int32_t bid_levels, ask_levels;
    double  bid_vwap_5, ask_vwap_5;
    double  slope_bid_5, slope_ask_5;
};

class FeatureExtractor {
public:
    BookSnapshot extract(const OrderBook& book, uint64_t ts, uint64_t seq) const;
    static int64_t sum_depth(const OrderBook& book, Side side, int n);
    static double  vwap(const OrderBook& book, Side side, int n);
    static double  depth_slope(const OrderBook& book, Side side, int n);
};
```

---

## 三、事件处理核心逻辑

### 3.1 `apply()` 分派与一致性检查（对标 `SSE/SZE OnEvent` + `DoPostProcess`）
```cpp
void OrderBook::apply(const Event& e) {
    change_auction_status(e.timestamp);

    switch (e.type) {
    case EventType::ADD:     on_add(e);     break;
    case EventType::CANCEL:  on_cancel(e);  break;
    case EventType::EXECUTE: on_execute(e); break;
    }

    if (!in_batch_auction) {
        if (is_good && best_bid() && best_ask() && *best_bid() >= *best_ask()) {
            ob_state++; // crossed book
        } else {
            ob_state = 0;
        }
    } else {
        ob_state++;
    }
}
```

### 3.2 `on_add()`（含 `pending trade` 兜底）
```cpp
void OrderBook::on_add(const Event& e) {
    Order* o = pool.allocate();
    *o = Order{e.order_id, e.price, e.volume, e.side, e.timestamp, nullptr};

    // ★ pending trade 兜底（SSE 成交可能先于委托到达）
    auto pt = pending_trades.find(e.order_id);
    if (pt != pending_trades.end()) {
        o->volume -= pt->second;
        pending_trades.erase(pt);
        if (o->volume <= 0) { pool.deallocate(o); return; }
    }

    auto& levels = (e.side == Side::BUY) ? bids : asks;
    auto& level = levels[e.price];
    level.price = e.price;
    level.add(o); // o->level = &level 在 add() 内设置
    order_index[e.order_id] = o;
}
```

### 3.3 `on_cancel()` / `on_execute()`（含负值防御与 O(1) 定位）
```cpp
void OrderBook::on_cancel(const Event& e) {
    auto it = order_index.find(e.order_id);
    if (it == order_index.end()) { is_good = false; return; }

    Order* o = it->second;
    auto* lv = static_cast<PriceLevel*>(o->level);
    auto& levels = (o->side == Side::BUY) ? bids : asks;

    lv->remove(o);
    if (lv->empty()) levels.erase(lv->price);
    order_index.erase(it);
    pool.deallocate(o);
}

void OrderBook::on_execute(const Event& e) {
    auto it = order_index.find(e.order_id);
    if (it == order_index.end()) {
        // ★ pending trade 兜底：成交先于委托到达
        pending_trades[e.order_id] += e.volume;
        return;
    }

    Order* o = it->second;
    auto* lv = static_cast<PriceLevel*>(o->level);
    auto& levels = (o->side == Side::BUY) ? bids : asks;

    if (o->volume < e.volume) { is_good = false; return; } // ★ 负值防御

    lv->reduce(o, e.volume);
    if (o->volume <= 0) {
        lv->remove(o);
        if (lv->empty()) levels.erase(lv->price);
        order_index.erase(it);
        pool.deallocate(o);
    }
}
```

### 3.4 集合竞价状态机（对标 `ChangeAuctionStatus`）
```cpp
static constexpr uint64_t OPEN_AUCTION_END   = 93000000ULL * 1000000ULL;  // 09:30:00.000
static constexpr uint64_t CLOSE_AUCTION_START = 145700000ULL * 1000000ULL; // 14:57:00.000

void OrderBook::change_auction_status(uint64_t ts) {
    if (in_batch_auction && ts >= OPEN_AUCTION_END) {
        in_batch_auction = false; // -> 连续竞价
    }
    if (!in_batch_auction && ts > CLOSE_AUCTION_START) {
        in_batch_auction = true;  // -> 收盘集合竞价（14:57 后跳过或特殊处理）
    }
}
```

---

## 四、Python 清洗层

### 4.1 `cn_tick_l2` CSV → 统一 Event 流（`scripts/ingestion/tick_l2_cleaner.py`）
对标公司 `SZEEventExtractor`：
- **逐笔委托 CSV → `ADD` Event**：
  - `seq = order_id = int(row['交易所委托号'])`
  - `price = int(row['委托价格'])`（已是 `×10000`，不转 Price9）
  - `volume = int(row['委托数量'])`
  - `side = BUY if row['委托代码']=='B' else SELL`
  - 委托类型 `'U'`（本方最优）→ `price` 取清洗时查上一帧快照的当前 `best_bid / best_ask`
- **逐笔成交 CSV → `EXECUTE` 或 `CANCEL`**：
  - 若 `row['成交代码'] == 'C'`（深市撤单，对标 SZE `SubFunction='4'`）：`order_id = 非零的那个序号（买序号 or 卖序号）` → `CANCEL` Event
  - 否则（真成交，对标 SZE `SubFunction='F'`）：拆成两条 `EXECUTE`（买序号 `BUY` + 卖序号 `SELL`）
- **时间戳**：`HHMMSSmmm`（9 位整数）→ `uint64` 纳秒（`×1e6`）
- **归并**：委托流 + 成交流按 `ApplSeqNum` k-way merge → 单调有序 Event 流

### 4.2 沪深差异处理（`data/normalizers/tick_l2_normalizer.py`）
- **深市（SZE，现有样本）**：撤单在成交表（`成交代码='C'`）；委托类型 `'0'` 限价 / `'U'` 本方最优（转限价） / `'1'` 市价（极少）。
- **沪市（SSE，2021-07 后才有委托流）**：撤单在委托流（`OrderKind='D'`）；成交流只有成交；`pending trade` 兜底尤为重要（SSE 委托与成交可能乱序）。
- **集合竞价标记**：
  - `09:15~09:30` 开盘集合竞价 → `in_batch_auction=true`
  - `09:30~14:57` 连续竞价
  - `14:57~15:00` 收盘集合竞价 → 简化处理：跳过或只提取快照因子

---

## 五、`pybind11` 接口与因子注册

### 5.1 `pybind11` 接口（`pybind/bind.cpp`）
```cpp
py::array extract_features(py::array_t<Event> events, int snapshot_interval_ms) {
    OrderBook book;
    FeatureExtractor extractor;
    std::vector<BookSnapshot> snapshots;

    auto* ptr = events.data();
    uint64_t last_snap_ts = 0;
    for (ssize_t i = 0; i < events.size(); ++i) {
        book.apply(ptr[i]);
        if (ptr[i].timestamp - last_snap_ts >= snapshot_interval_ms * 1000000ULL) {
            snapshots.push_back(extractor.extract(book, ptr[i].timestamp, ptr[i].seq));
            last_snap_ts = ptr[i].timestamp;
        }
    }
    return snapshots_to_numpy(snapshots);
}

PYBIND11_MODULE(lob_engine, m) {
    m.def("extract_features", &extract_features,
          py::arg("events"), py::arg("snapshot_interval_ms") = 3000);
}
```
Python 侧聚合：
```python
features_df = extract_features(events_array, snapshot_interval_ms=3000)
features_df["minute"] = features_df["timestamp"].dt.floor("1min")
minute_features = features_df.groupby("minute").last()  # 分钟末状态
```

### 5.2 因子注册（`research/factor_library/lob_factors.py`）
- 类名：`LOBFactors(Factor)`，`name = "lob_microstructure"`，`frequency = "minute"`，`version = "1.0"`
- 依赖：`["tick_l2_events"]`
- 19 个输出列：`best_bid`, `best_ask`, `mid_price`, `spread`, `bid_depth_1`, `ask_depth_1`, `bid_depth_5`, `ask_depth_5`, `bid_depth_10`, `ask_depth_10`, `imbalance_1`, `imbalance_5`, `imbalance_10`, `bid_levels`, `ask_levels`, `bid_vwap_5`, `ask_vwap_5`, `slope_bid_5`, `slope_ask_5`
- 落盘路径：`$QUANT_DATA_ROOT/factors/minute/lob_microstructure/v1.0/{YYYY-MM}.parquet`

---

## 六、验证方案

1. **单元测试（10 个场景，对标公司 `QueuePositionTest`）**：
   - `SingleOrder`、`OrderNotFound`、`FrontOfQueue`、`MiddleOfQueue`、`BackOfQueue`、`AskSide`、`MultiplePriceLevels`、`AfterCancel`、`AfterPartialCancel`、`PendingTrade`
2. **L2 十档快照对拍（核心验证）**：
   - 以 `cn_tick_l2` 十档快照表（`行情.csv`）为黄金基准，在每个快照时点比对重建的 `bids/asks` 前 10 档申买价/量与申卖价/量。
   - 交叉验证：`AmazingData` L1 五档快照 ⇔ 重建前五档。
3. **特征值域断言**：
   - `imbalance ∈ [-1, 1]`
   - `spread ≥ 0`（负值 = crossed book，标记异常）
   - `depth ≥ 0`
   - `vwap_bid ≤ best_bid ≤ best_ask ≤ vwap_ask`
   - `bid_levels, ask_levels ≥ 0`

---

## 七、数据存储与硬件规划（1TB 外置硬盘方案）

- **盘上常驻分配**：
  - 原始切片 `~500GB`：中证 800 × 近 3 年（2024~2026）的 L2 三表 parquet
  - 成品区 `~200GB`：分钟频因子（全市场流式遍历产出）
  - 加工区 `~150GB`：逐日滚动（下 1 天 7z → 解压 → 重建 → 删）
  - 余量 `~150GB`
- **目录统一（symlink）**：
  - `$QUANT_DATA_ROOT/asset/`（SSD 热数据）
  - `$QUANT_DATA_ROOT/factors/`（SSD 因子）
  - `$QUANT_DATA_ROOT/orderflow/ -> /Volumes/ExtDisk/orderflow/`（外置硬盘冷数据）
- **下载策略**：QDC `cn_tick_l2` 按交易日 7z 打包（全市场 ~5GB/天），支持 `7z x pkg.7z {中证800目录列表}` 只解压池内证券，清洗成 parquet 落盘后删 7z（或联系客服按证券定制交付）。

---

## 八、工程边界与简化决策

### 不做（公司有，但当前不需要）
- `Price9` 转换（直接用 `×10000`）
- `UMP` 协议解析（直接读 CSV / parquet）
- `OrderBookCuratorSteward / Mediator`（Python `for` 循环管多票）
- `Listener` 回调链（无实时下游消费者）
- `trading_orders_` cross 未决池（清洗层 seq 排序消除大部分 cross）
- 市价单 `kOrdType_Market` promote（A 股极少，忽略）
- 收盘集合竞价 pending 队列（14:57 后跳过）
- `BranchlessAssocVector`（`std::map` 够用，M5 再考虑）
- `AffinityNodePool` 16GB arena（经典 free-list 够用）
- `cache-line` 64B 对齐（离线单线程不需要防 false sharing）
- `GetQueuePosition`（仅做盘口因子，不追踪排队位置）

### 做了（直接从公司借鉴）
- `void* level` 回指（O(1) 定位价位）
- `order_index` `unordered_map`（O(1) 按 `id` 找 `order`）
- 增量维护 `total_volume / order_count`（避免每次遍历）
- `free-list` 对象池（消灭 `new / delete`）
- `pending trade` 兜底（SSE 乱序防护）
- `is_good_ + ob_state_` 状态检测（异常标记）
- 三段集合竞价状态机（09:15 / 09:30 / 14:57）
- 深市撤单码 `'C'` 归一 + `'U'` 转限价
- 10 场景单元测试矩阵 + L2 十档快照对拍验证
