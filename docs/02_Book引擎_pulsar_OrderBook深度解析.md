# 02. Book 引擎：pulsar OrderBook 深度解析

> 本文档整理自「Book 引擎」目录下的 6 份源码解析记录，深入剖析 `cx330/book/OrderBook.hpp`（2383 行）——`NeoLOB` 底层真正的 Book 引擎。

---

## 一、`BookOrder` 节点：`intrusive + cache-line + 条件编译三合一`

路径：`cx330/book/OrderBook.hpp:93-125`

```cpp
template <..., bool kSummaryMode = false, bool kEventBus = false>
struct alignas((kSummaryMode || kEventBus) ? alignof(int64_t) : kCacheLineSize) BookOrder
    : public ORDER_T,                          // 继承市场特定字段 (order_id/price/volume/is_bid)
      public BookOrderQueueLinks<!kSummaryMode, ...> // 条件继承 prev/next
{
    void* level = nullptr; // ★回指 PriceLevel 的指针（O(1) 找到所在价位）
};
```

### 三种模式下的内存布局对比
1. **Full Mode（独立进程，默认）**：
   - `alignas(64)`（强制 64 字节 cache-line 对齐，防多线程 false sharing）
   - `ORDER_T` (~32B) + `prev/next` (16B) + `void* level` (8B) + payload + padding = **64B / 节点**（10M 峰值 order ≈ 640MB pool）
2. **Summary Mode（USC 统一簿）**：
   - `alignas(8)`，通过 EBO 优化掉 `prev/next`
   - `ORDER_T` (~32B) + `void* level` (8B) = **48B / 节点**（省 25% 内存，不支持 `GetQueuePosition`）
3. **EventBus Mode（单线程 event bus）**：
   - `alignas(8)`，保留 `prev/next` 但不做 64B 对齐（单线程无需防 false sharing），约 **~56B / 节点**。

### 关键设计：`void* level` 回指
- 撤单时：`order -> order->level` 直接拿到 `PriceLevel`，实现 **O(1)** 定位，无需再在 `bids/asks` map 中 `find(price)`。
- 比外部维护 `OrderIter{level*, list::iterator}` 更紧凑，省内存。

---

## 二、`PriceLevel`：`intrusive list` 的真实使用

路径：`cx330/book/OrderBook.hpp:198-294`

```cpp
template <typename TypeDict, typename NODE_T, typename ALLOC_T>
class PriceLevel : private PriceLevelExtBase<TypeDict>,
                   public PriceLevelOrdersBase<TypeDict::kSummary, ...> {
    volume_t volume_ = 0;       // 价位总量（增量维护）
    int32_t  order_count_ = 0;  // 价位挂单数（增量维护）
    price_t  price_ = 0;        // 价格

    // Full mode 才有：
    queue_t orders_;            // ★ eureka::IntrusiveList<BookOrder>

    // API:
    queue_iterator EmplaceBack(Args&&... args); // 新 order 入队尾
    void PushBackPtr(node_t* node);             // 外部节点入队尾
    void MoveToEnd(queue_iterator qit);         // 重排到队尾（改单用）

    // 增量维护：
    void AddVolume(volume_t v)      { volume_ += v; }
    void SubVolume(volume_t v)      { volume_ -= v; }
    void AddOrderCount(int32_t c)   { order_count_ += c; }
    void SubOrderCount(int32_t c)   { order_count_ -= c; }
};
```

---

## 三、`Side`：价位索引 + `top-10` 缓存 + 墓碑机制

路径：`cx330/book/OrderBook.hpp:299-400`

```cpp
class Side {
    // 价位索引（排序容器，BranchlessAssocVector 或 std::map）
    level_ref_map_t level_index_;

    // ★ top-10 缓存
    static constexpr int kMaxTopLevels = 10;
    struct TopLevelEntry { price_t price; const price_level_t* level; };

    // 对象池（Full mode 拥有节点内存）
    ALLOC_T alloc_;                         // order 节点 allocator
    std::unique_ptr<level_pool_t> level_pool_; // PriceLevel 节点池
    level_allocator_type level_alloc_;      // PriceLevel allocator

    // 墓碑机制（惰性删除）
    static constexpr size_t kDefaultEagerEraseThreshold = 16;
    // 排序 vector 删除价位不真删，标记为 nullptr (tombstone)
    // 遍历时跳过 tombstone，累积超过阈值再真正 compact
};
```

- **`top-10` 缓存对比**：个人设计维护 `depth_1/5/10` 等 `int64` 聚合值更轻量；公司存 `TopLevelEntry` 指针数组在 `rebuild` 时可在 top-10 内做任意计算，更灵活。

---

## 四、`GetQueuePosition`：从 `intrusive list` 计算排队位置

```cpp
Status GetQueuePositionOfNode(const node_t* target, QueuePosition* out) const {
    static_assert(!TypeDict::kSummary,
        "GetQueuePosition is unavailable in summary mode (no intrusive list)");

    const auto* lv = static_cast<const price_level_t*>(target->level); // O(1) 拿到价位
    volume_t qty_ahead = 0;
    int32_t orders_ahead = 0;

    const node_t* cur = target->prev; // ★ 从目标 order 反向往前遍历
    while (cur != nullptr) {
        if (cur->prev) {
            __builtin_prefetch(cur->prev, 0, 0); // ★ 手动预取下一个节点
        }
        qty_ahead += cur->volume;
        ++orders_ahead;
        cur = cur->prev;
    }

    out->qty_ahead = qty_ahead;
    out->orders_ahead = orders_ahead;
    out->qty_behind = lv->GetVolume() - qty_ahead - target->volume; // ★ O(1) 减法
    out->orders_behind = lv->GetOrderCount() - orders_ahead - 1;
    return Status::OK;
}
```

**三个关键设计点**：
1. **反向 `prev` 链遍历**：复杂度 `O(orders_ahead)`，排在队首极快。
2. **`__builtin_prefetch` 手动预取**：在累加当前节点 `volume` 时后台预取 `cur->prev`，隐藏 cache miss 延迟（约省 30~50% 遍历时间）。
3. **`qty_behind` 用减法而非二次遍历**：得益于 `PriceLevel` 增量维护 `GetVolume()` 与 `GetOrderCount()`，后方信息 O(1) 免费获得。

---

## 五、对象池 `AffinityNodePool` 与 `BranchlessAssocVector`

### 5.1 `AffinityNodePool`
- `Order` 节点由 `AffinityNodePool` 分配（预分配连续内存块、Full mode 64B 对齐、线程绑定防 false sharing）。
- `PriceLevel` 节点由 `Side` 自己的 `level_pool_` 池化分配；析构时 `Clear()` 归还所有节点。
- **EventBus Mode 特殊**：`kOwnOrderNodes = !TypeDict::kEventBusMode = false`，节点由外部 `DecoderStage` 拥有，`Side` 只持有指针引用（零拷贝共享）。

### 5.2 `BranchlessAssocVector`（排序 vector 替代 `std::map`）
| 维度 | `std::map`（红黑树） | `BranchlessAssocVector` |
| :--- | :--- | :--- |
| 查找 | `O(log N)` + 指针跳转 | `O(log N)` + 线性内存（无分支二分） |
| 插入 / 删除 | `O(log N)` + 旋转 | `O(N)` 搬移，配合 tombstone 惰性删除均摊 `O(1)` |
| 内存与遍历 | 节点散落堆中，cache 不友好 | 连续内存，**极度 cache 友好**（契合每 3 秒遍历前 10 档） |

---

## 六、完整数据结构栈（终极知识图谱）

```text
OrderBook<TypeDict>
├── bid_side_: Side<..., std::greater>
│   ├── level_index_: BranchlessAssocVector<price, PriceLevel*>
│   ├── top_cache_[10]: TopLevelEntry
│   ├── alloc_: AffinityNodePool
│   └── level_pool_: level pool
├── ask_side_: Side<..., std::less>
├── order_index_: flat_hash_map<id, node*>
└── trade_index_: flat_hash_map<...>

PriceLevel
├── price_, volume_, order_count_
└── orders_: IntrusiveList<BookOrder> (Full mode)

BookOrder
├── order_id, price, volume, is_bid
├── prev*, next* (Full mode)
├── void* level (O(1) 回指)
└── payload (EBO 优化可零字节)
```

- **M1 架构**：`std::map + std::list + 线性扫描`（正确、偏慢）
- **M4 目标架构**：`flat_hash_map + AssocVector + IntrusiveList + pool + void* level 回指`（与公司 `pulsar OrderBook` 完全对齐）。
