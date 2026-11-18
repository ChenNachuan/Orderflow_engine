# 03. 增删查改核心：OrderBookBuilder 与 SSE/SZE Preferences

> 本文档整理自「增删查改的核心」目录下的 3 份分析记录，覆盖 `OrderBookBuilder` 增删改中枢、SSE `pending trade` 机制以及沪深 `Preferences` 差异枚举。

---

## 一、`OrderBookBuilder` —— LOB 的“增删改”中枢

`SSEOrderBook` 与 `SZEOrderBook` 的 `OnOrderAdd / OnOrderDel / OnTrade` 最终全部委托给 `OrderBookBuilder`：

```text
SSEOrderBook.DoOnOrderAdd()    -> builder_.AddLimitOrderCheckPendingTrade(&order)
SSEOrderBook.DoOnOrderUpdate() -> builder_.CancelLimitOrder(...)
SSEOrderBook.DoOnTrade()       -> builder_.AddTrade(&trade, BID_ASK)

SZEOrderBook.DoOnOrderAdd()    -> builder_.AddLimitOrder(&order)
SZEOrderBook.DoOnOrderDel()    -> builder_.FillLimitOrder(...)   // 撤单=填满剩余量
SZEOrderBook.DoOnTrade()       -> builder_.FillLimitOrder(...) + HandleTrade
```

### 关键 API 与 `lob_engine` 对应关系
| 公司 Builder API | `lob_engine` 对应方法 | 本质操作 |
| :--- | :--- | :--- |
| `AddLimitOrder(order*)` | `on_add(event)` | 新增挂单入簿 |
| `AddLimitOrderCheckPendingTrade` | 同上，但先扣 pending trade | SSE 特有 |
| `CancelLimitOrder(id,bid,p,v)` | `on_cancel(event)` | 撤单（减量） |
| `FillLimitOrder(id,bid,p,v)` | `on_execute(event)` | 成交（减量） |
| `AddTrade(trade*,type)` | `on_execute(event)` 的另一半 | 成交扣双边 |
| `RemoveOrder(order*)` | `on_cancel`（整单移除） | 整单删除 |
| `DeleteOrderByID(id)` | 同上 | 按 id 删除 |
| `ModifyOrder(order*)` | ——（A 股不用） | 改单 = 删 + 加 |

---

## 二、SSE `pending trade` 机制与 `AddTrade` 双边逻辑

### 2.1 为什么需要 `pending trade`？
上交所逐笔流中可能出现**成交先于委托到达**的乱序情况（或归并丢包/不完美）：
- `seq=100` 成交：`order_id=500` 成交 200 股（委托尚未入簿）
- `seq=101` 委托：`order_id=500` 委托买入 1000 股

**公司解法**：
1. 成交先到时，暂存为 `pending trade`（`AddTradeVolume` 存入 `trades_`）。
2. 委托到达时，调用 `AddLimitOrderCheckPendingTrade`：
```cpp
eureka::Status AddOrder(order_info_t* order) {
    volume_t early_trade_volume = 0;
    manager_.SubTradeVolume(order->GetOrderId(), early_trade_volume, nullptr);

    volume_t volume = order->GetVolume() - early_trade_volume;
    if (volume > 0) {
        order->SetVolume(volume);
        return manager_.AddOrderVolume(...); // 剩余量入簿
    }
    return eureka::Status::OK(); // 已全部成交，不入簿
}
```

### 2.2 `Builder::AddTrade` 双边扣减与 `bid_order_id < ask_order_id`
```cpp
eureka::Status AddTrade(trade_info_t* trade, ...) {
    // 1. 扣买方
    if (bid_order_id) {
        auto it = orders.m_find(bid_order_id);
        if (it != orders.m_end()) {
            ret = manager_.SubOrderVolume(bid_order_id, true, ...);
        } else {
            if (bid_order_id < ask_order_id) { // ★ 关键判断
                ret = manager_.AddTradeVolume(bid_order_id, ...);
            }
        }
    }
    // 2. 扣卖方（对称逻辑）
    if (ask_order_id) { ... }
}
```
- **含义**：逐笔流里 `order_id` 递增，较小的是先挂的被动方（理应已在簿上），较大的是后来的主动方（可能尚未入簿）。因此公司**只对较大 `order_id` 的一方**暂存 `pending trade`。

---

## 三、`SSE vs SZE Preferences` —— 市场差异完整枚举

### 3.1 两市完全相同的部分
- 类型统一：`order_id_t`、`trade_id_t`、`price_t`、`volume_t` 均为 `int64_t`
- 统一输入：`input_data_t = ubiqmessage::SZEOrderFlowField`（上交所逐笔在接入层也被归一成深交所格式）；`output_data_t = EquityMarketDataField`
- `price_factor = 1000`（OFF 模式，非 Price9），`volume_factor = 1`，`start_time = 9'30'00'000`
- `static constexpr bool kEnableMemoryPool = true;`（两市都开启内存池）

### 3.2 两市不同的部分（清洗层重点关注）
1. **差异 1：`OrderInfo` 类型**
   - SSE：`eureka::OrderInfoT<...>`（基础版，无市价单/本方最优概念）
   - SZE：`cx330::neolob::cn::SZEOrderInfo<...>`（扩展版，必须处理市价单 `'1'` 和本方最优 `'U'`）
2. **差异 2：价位容器（bid/ask level basket）**
   - SSE：`cx330::StdMap / RevStdMap`（`std::map` 红黑树）
   - SZE：`cx330::AssocVector / RevAssocVector`（排序 vector + 二分查找，因深市价位数通常更多、更 cache 友好）
3. **差异 3：`order` 容器**
   - SSE：`eureka::LevelContainer`（老版双 map）
   - SZE：`eureka::LevelContainerType2`（新版三结构，用 `ska::flat_hash_map + std::map`，开地址法比 `unordered_map` 更快）
4. **差异 4：SZE 按标的类型初始化**
   - SZE `Preferences` 维护 5 个 map：`stocks / cyb_stocks / bonds / etfs / indices`，分别处理主板 10%、创业板/科创板 20%、ETF 无涨跌停等收盘集合竞价 pending 逻辑。

---

## 四、对 `lob_engine` 的核心价值汇总
1. **`pending trade` 兜底（SSE 特有）**：`EXECUTE` 找不到 `order_id` 时暂存，后续 `ADD` 时扣减。
2. **`AssocVector` 替代 `std::map`**：M4 优化可将价位容器换为排序 vector。
3. **`ska::flat_hash_map`**：M4 的 `order_index` 可替换 `std::unordered_map`。
4. **`price_factor`**：`cn_tick_l2` 用 `×10000` 定点，与 OFF 模式 `×1000` 仅 scale 不同，不影响逻辑。
5. **统一输入格式**：公司 `SSE/SZE` 共用 `SZEOrderFlowField`，验证了 Python 清洗层将沪深归一为统一 `Event` 流的合理性。
6. **`kEnableMemoryPool = true`**：验证了对象池在 A 股 LOB 场景的必要性。
