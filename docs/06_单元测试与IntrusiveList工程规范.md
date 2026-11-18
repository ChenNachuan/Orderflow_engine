# 06. 单元测试与 IntrusiveList 工程规范

> 本文档整理自「单元测试」目录下的 4 份分析记录，覆盖 `QueuePositionTest.cpp`（383 行，10 个用例）与 `eureka/IntrusiveList.hpp`（532 行，完整实现）。

---

## 一、`QueuePositionTest` —— 公司 LOB 单元测试范式

测试分为两套，分别验证 `pulsar book`（直接操作 `pulsar::OrderBook`）和 `neolob V2 accessor`（通过 `NAS OrderBook` 完整管线）：
- `ob_.Init(ob_t::Config::ForPeakOrders(1024, 64));` —— 预分配 1024 个 `BookOrder` 节点 + 64 个 `PriceLevel`（测试用小规模，生产用百万级/千级）。

### 10 个核心测试用例矩阵
| 编号 | 用例名 | 覆盖场景 | M1 覆盖状态 |
| :--- | :--- | :--- | :--- |
| #1 | `SingleOrder` | 唯一一笔，前后都是 0 | 已覆盖 |
| #2 | `OrderNotFound` | 查不存在的 id → 返回 `NOT_FOUND` | 需补充 |
| #3 | `FrontOfQueue` | 队首：`ahead=0, behind=后面所有` | 已覆盖 |
| #4 | `MiddleOfQueue` | 队中：`ahead=前面的和, behind=后面的和` | 已覆盖 |
| #5 | `BackOfQueue` | 队尾：`ahead=前面所有, behind=0` | 需补充 |
| #6 | `AskSide` | **卖侧**（确认 `less` 排序未搞反） | ★ 需补充 |
| #7 | `MultiplePriceLevels` | **跨价位不串扰**（最易出 bug） | ★ 需补充 |
| #8 | `AfterCancel` | 全撤后：前面的消失，后面的前移 | 已覆盖 |
| #9 | `AfterPartialCancel` | **部分撤单**：`ahead` 减少相应量 | ★ 需补充 |
| #10 | `MoveToEndOnPriorityLoss` | 改单失去优先级：移到队尾 | 未来扩展 |

---

## 二、`eureka::IntrusiveList` —— 生产级双向链表实现

### 2.1 核心设计约束
- **Value 类型自带 `prev/next`**（轻量 compile-time duck typing，无需显式 `static_assert`）。
- **`TrackSize` EBO 优化**：
  ```cpp
  template <bool TrackSize> struct IntrusiveListSizeBase { size_t size_ = 0; };
  template <> struct IntrusiveListSizeBase<false> {}; // 空！EBO 优化掉
  ```
  - LOB 场景中 `PriceLevel` 已维护 `order_count_`，故配置 `TrackSize = false`，每条链表省 8 字节及每次 `push/pop` 的 `++/--`。
- **可插拔 `allocator`**：配合 `AffinityNodePool`，`allocate/deallocate` 约 `~1ns`，比 `std::allocator` 的 `new/delete`（`~80ns`）快 **80 倍**。

### 2.2 四个核心 `O(1)` 指针操作 API
1. **`push_back_ptr(pointer node)`**：已分配节点入队尾（4 次指针赋值 + 1 次分支，不分配内存）。
2. **`erase(iterator it)`**：摘除节点并 `destroy_and_free(node)` 归还 allocator，返回下一迭代器（6 次指针赋值 + 2 次分支）。
3. **`extract(iterator it)`**：`unlink_node(node)` 摘出节点但**不释放内存**，返回裸指针（用于“从一个 `PriceLevel` 零分配转移到另一个 `PriceLevel`”）。
4. **`move_to_back(iterator qit)`**：从当前位置摘出直接接到尾部，不分配/释放（用于改单失去优先级）。

---

## 三、提炼给 M4 的 5 条必用设计模式

1. **`BookOrder` 嵌 `void* level` 回指 `PriceLevel`**：`order_index` 只需存 `Order*`，直接 `node->level` O(1) 定位。
2. **`IntrusiveList<BookOrder, PoolAlloc, false>`**：`TrackSize=false` + 对象池分配器组合。
3. **`extract() + push_back_ptr()`**：零分配跨价位转移。
4. **`move_to_back()`**：O(1) 失去优先级移到队尾。
5. **单测必补场景**：`MultiplePriceLevels`（#7）+ `PartialCancel`（#9）+ `AskSide`（#6）。

---

## 四、M1 测试用例补充代码（建议直接加入 `test_order_book.cpp`）

```cpp
TEST(OrderBook, MultiplePriceLevels) {
    // ADD buy 10.00 x 100
    // ADD buy 10.00 x 200
    // ADD buy 9.99 x 300 (不同价位!)
    // 断言: best_bid=10.00, bid_depth_at(10.00)=300, bid_depth_at(9.99)=300
    // CANCEL order at 10.00 x 100
    // 断言: bid_depth_at(10.00)=200, bid_depth_at(9.99)=300 (9.99 不受影响)
}

TEST(OrderBook, AskSide) {
    // ADD sell 10.02 x 100
    // ADD sell 10.02 x 200
    // 断言: best_ask=10.02, ask_depth=300
    // 确认 asks 用 less 排序 (begin()=最低价=最优卖价)
}

TEST(OrderBook, PartialCancel) {
    // ADD buy 10.00 x 500
    // EXECUTE(部分成交) 200 股
    // 断言: 该 order 剩余 300, price_level 总量 300
    // EXECUTE 剩余 300 股
    // 断言: order 删除, price_level 删除 (空了)
}
```
