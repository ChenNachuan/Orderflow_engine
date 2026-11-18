# 04. 完整内核：LevelManager、PriceLevelData 与三层架构

> 本文档整理自「完整内核」目录下的 5 份资料，剖析 LOB 引擎“增删查”三件套的完整内核实现及公司 3 层架构图谱。

---

## 一、`LevelManager` —— 增量维护价位聚合值的内核

### 1.1 `AddOrderVolumeHelper`（新增挂单入价位）
```cpp
template <typename LevelsT>
Status AddOrderVolumeHelper(LevelsT& levels, order_id_t id, price_t price,
                            volume_t volume, order_info_t* order) {
    auto it = levels.lower_bound(price);
    if (it != levels.end() && !levels.key_comp()(price, it->first)) {
        // 价位已存在 -> 增量更新
        it->second.AddVolume(volume);     // total_volume += volume
        it->second.AddOrderCount(1);      // order_count += 1
    } else {
        // 价位不存在 -> 创建新价位（hint-insert 避免二次查找）
        auto& lvl = levels.insert(it, {price, {}})->second;
        lvl.SetPrice(price);
        lvl.SetVolume(volume);
        lvl.SetOrderCount(1);
    }
    data_->orders().Add(order_id, price, order);
    return Status::OK();
}
```
- **核心优化**：使用 `lower_bound(price) + insert(hint, ...)` 替代 `levels[price]`，每次 `ADD` 节省一次 `O(log N)` 查找。

### 1.2 `SubOrderVolumeHelper`（成交 / 撤单共用扣量）
```cpp
template <typename LevelsT, typename OrderIDIter>
Status SubOrderVolumeHelper(LevelsT& levels, order_id_t id,
                            OrderIDIter order_mit, price_t price, volume_t volume) {
    price_t original_price = order_mit.level_key(); // 从 order index 拿到真实价位
    auto level_it = levels.find(original_price);

    auto& order = order_mit.value();
    auto& level = level_it->second;

    order.SetVolume(order.GetVolume() - volume); // ① order 减量
    level.SubVolume(volume);                     // ② 价位总量减量

    if (order.GetVolume() == 0) {                // ③ order 量归零 -> 删除
        data_->orders().EraseIDComplete(order_id);
        level.SubOrderCount(1);
    }
    if (level.GetVolume() == 0) {                // ④ 价位量归零 -> 删除价位
        levels.erase(level_it);
    }

    // ★ 防御性负值检测
    if (order.GetVolume() < 0) return ORDER_VOL_NEGATIVE;
    if (level.GetVolume() < 0) return LEVEL_VOL_NEGATIVE;
    return OK;
}
```
- **负值防御意义**：遇到重复撤单或重复成交等脏数据时，防止静默负值污染后续所有特征；检测到负值即返回错误状态并跳过该标的。

### 1.3 `AddTrade` 完整逻辑（SSE `pending trade`）
- 逐笔流中 `order_id` 递增：较小 `id` 为被动方（先挂，理应已在簿上，若不在属异常不存 pending）；较大 `id` 为主动方（后进，不在簿上属正常，调用 `AddTradeVolume` 存入 `trades_`）。
- 后续 `AddOrder` 到达时由 `AddLimitOrderCheckPendingTrade() -> SubTradeVolume()` 先扣掉 pending 量，剩余量再入簿。

---

## 二、`PriceLevelData` —— 数据存储层（V1 → V2 演进）

| 版本 | 结构组成 | 特点 |
| :--- | :--- | :--- |
| **V1（`PriceLevelData`）** | `bids_`、`asks_`、`orders_`、`trades_` 并列独立容器 | 增删需分别维护两处 |
| **V2（`PriceLevelDataV2`）** | `orders_`（`LevelContainerNew`）内部统一容纳 `bids/asks` + `trades_` | 更紧凑、一致性更好，消除 `bids` 与 `orders` 不同步的 bug |

- **对 M4 的启发**：参考 V2 设计，将 `bids / asks / order_index` 收拢至统一类中管理。

---

## 三、`OrderBookAccessor` —— 查询接口层

常用查询 API 与 M3 `FeatureExtractor` 映射：
- `GetBestBidPrice()` / `GetBestAskPrice()` → 取 `bids/asks.begin()->first`
- `IsPriceLevelGood()` / `IsPriceCross()` → 检测 `bestBid >= bestAsk`（crossed book）
- `bid_begin() / bid_end()`、`ask_begin() / ask_end()` → 迭代前 10 档求 depth
- `order_mfind(id)` / `order_mbegin() / order_mend()` → O(1) `flat_hash_map` 查找
- `order_begin_at(price) / order_end_at(price)` → 遍历指定价位内的所有 order

---

## 四、公司 `cx330 neolob` 三层架构图谱与对照

```text
OrderBookSSE / SZE（入口层）
  ├── 事件分派：OnEvent -> switch(FieldType) -> OnOrderAdd/OnTrade/OnOrderDel
  ├── 集合竞价状态机：ChangeAuctionStatus
  ├── 深市特有：trading_orders_ / pending / market order promote
  └── 委托给 Builder

OrderBookBuilder（操作层）
  ├── AddLimitOrder / AddLimitOrderCheckPendingTrade
  ├── FillLimitOrder / CancelLimitOrder / AddTrade
  └── 委托给 LevelManager

LevelManager（存储层）
  ├── AddOrderVolumeHelper / SubOrderVolumeHelper
  ├── AddTradeVolume / SubTradeVolume
  └── 操作 PriceLevelData (bids/asks/orders/trades)

OrderBookAccessor（查询层）
  └── GetBestBid/AskPrice, IsPriceLevelGood, 按 id/价位遍历
```

**`lob_engine` 对照**：
- `OrderBook.apply()` = 入口层 + 操作层（专注 A 股可合在一起）
- `PriceLevel.add/reduce` = 存储层（`LevelManager`）
- `FeatureExtractor` = 查询层（`Accessor`）

---

## 五、最值得学习的三个核心工程点
1. **`lower_bound + hint-insert`**：避免 `std::map` 插入新价位时的二次查找（M4 优化采用）。
2. **负值防御**：`order.volume < 0` 或 `level.volume < 0` 时返回错误状态并标记异常（M1 必须加）。
3. **SSE `pending trade` 兜底**：`EXECUTE` 找不到 `order_id` 时记警告并暂存，防止数据丢包或 seq 不连续导致静默错误。
