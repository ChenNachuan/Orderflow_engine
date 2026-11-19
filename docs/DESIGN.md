# 架构与设计决策

本文档定义 `Orderflow_engine` 的核心分层结构、关键数据结构选型、事件驱动路径以及工程演进边界。

---

## 一、四层核心架构

引擎按职责由底向上划分为四个层次，严格隔离数据载体与状态管理逻辑：

| 层次 | 对应类型 | 职责边界 |
| :--- | :--- | :--- |
| **输入事件层** | `Event`（`EventType`, `Side`） | 只读的标准化行情输入契约，表达单条新增、撤单或成交动作；不包含任何订单簿内部状态 |
| **挂单节点层** | `Order` | 簿中单个活跃挂单的数据载体，保存订单标识、价格、剩余量、原始申报量、方向以及链表/回指指针 |
| **价位队列层** | `PriceLevel` | 维护单一价格档位上的 FIFO 挂单队列，负责保证链表的指针封闭性以及 `total_volume` / `order_count` 的原子同步更新；不感知订单簿全局索引与内存分配 |
| **订单簿控制层** | `OrderBook` | 唯一对外事件入口；负责全局 `order_id` 索引、买卖两侧有序价位表、乱序 `pending_trades` 兜底、竞价状态切换、异常检测与节点生命周期管理 |

---

## 二、核心设计决策

1. **定点整数表示价格（`int64_t`，放大 `10000` 倍）**
   - 直接承接原始数据万分之一元整数精度（A 股最小变动价位 `0.01` 元对应 `100` 单位），彻底避免浮点数舍入误差与价格键比对不稳定问题。
2. **有符号整数表示数量与计数（`int64_t`）**
   - `volume`、`original_volume` 与 `order_count` 均采用有符号 `int64_t`，防止异常扣减时发生无符号下溢回绕（静默变成极大正数），便于在 Debug 断言与运行期直接捕获负值异常。
3. **`PriceLevel` 采用侵入式双向链表**
   - 将 `prev` / `next` 指针直接嵌入 `Order` 节点，`PriceLevel` 仅持有 `head_` 和 `tail_`。相比 `std::list` 省去每次入队/出队的额外节点堆分配，同时保证队尾追加与任意位置摘除均为严格 $O(1)$。
4. **`Order` 内嵌 `void* level` 回指所属 `PriceLevel`**
   - 撤单或成交时通过 `order_id` 拿到 `Order*` 后，顺着 `level` 回指即可在 $O(1)$ 时间内直接定位到对应 `PriceLevel`，无需再到价位映射表中执行一次 $O(\log N)$ 的价格查找。
5. **买卖两侧价位表采用有序 `std::map`**
   - 买侧 `bids` 使用降序比较器（`std::greater<int64_t>`），卖侧 `asks` 使用升序比较器（`std::less<int64_t>`），使得两侧的 `begin()` 始终分别为 `best_bid` 和 `best_ask`。同时 `std::map` 的节点地址稳定性保证 `Order::level` 指针在其他价位增删时不会失效。
6. **全局订单索引采用 `std::unordered_map`**
   - 维护 `order_id -> Order*` 映射，确保每笔 `CANCEL` 与 `EXECUTE` 都能在平均 $O(1)$ 时间内定位目标订单。
7. **增量维护价位汇总量**
   - `PriceLevel` 在每次 `add_order` / `reduce_order` / `remove_order` 时同步加减 `tol_volume_` 与 `order_count_`，特征提取器读取任意价位深度只需 $O(1)$ 访问，无需遍历链表求和。
8. **`pending_trades` 乱序成交兜底**
   - 在上交所逐笔流中，成交事件偶发先于对应的委托事件到达。当 `EXECUTE` 在 `order_index` 中找不到目标订单时，先按 `order_id` 将成交量暂存到 `pending_trades`；待后续 `ADD` 到达时优先扣除提前成交量，仅将剩余有效量挂入订单簿。
9. **分阶段内存管理策略**
   - 早期阶段优先使用常规动态分配（`new` / `delete`）以验证逻辑正确性与测试覆盖度；逻辑完备后再无感切换为 `OrderPool` 对象池，避免过早引入复杂性。

---

## 三、事件处理三条路径

所有行情事件通过 `OrderBook::apply(Event)` 统一入口进入，先更新竞价状态机，再按事件类型分派：

### 1. `ADD`（新增委托）
1. 校验委托量 `volume > 0`、方向有效且 `order_id` 未重复存在。
2. 检查 `pending_trades` 中是否存在该 `order_id` 的提前成交记录；若有则取出并扣减，若扣减后剩余量 `<= 0` 则直接结束（`< 0` 时标记异常），不入簿。
3. 分配并初始化 `Order` 节点（填入 `order_id`、`timestamp`、`price`、剩余 `volume`、`original_volume`、`side`）。
4. 根据 `side` 选择 `bids` 或 `asks`，查找或新建对应价格的 `PriceLevel`。
5. 调用 `PriceLevel::add_order` 将节点挂入队尾、绑定 `level` 回指并更新价位汇总量。
6. 将 `order_id -> Order*` 写入 `order_index`。

### 2. `CANCEL`（撤单）
1. 在 `order_index` 中查找 `order_id`；若不存在则标记订单簿异常状态（`is_good = false`）并返回。
2. 取出 `Order*` 及其 `level` 回指指向的 `PriceLevel*`。
3. 调用 `PriceLevel::remove_order` 将节点从侵入式链表摘除，并扣减 `tol_volume_` 与 `order_count_`。
4. 若该 `PriceLevel` 已空（`is_empty()`），将其从对应的 `bids` 或 `asks` 价位表中删除。
5. 从 `order_index` 移除该 `order_id`，并释放 `Order` 节点。

### 3. `EXECUTE`（成交）
1. 在 `order_index` 中查找 `order_id`；若不存在，视作乱序到达的提前成交，将本次成交量累加到 `pending_trades[order_id]` 后返回。
2. 校验订单剩余量是否足够（`order->volume >= e.volume`）；若不足则发生负值异常，标记 `is_good = false` 并返回。
3. 通过 `level` 回指找到 `PriceLevel*`，调用 `PriceLevel::reduce_order` 同步扣减订单剩余量与价位总量。
4. 若扣减后 `order->volume == 0`，调用 `PriceLevel::remove_order` 摘除节点；若价位变空则从 `bids`/`asks` 删除，随后清理 `order_index` 并释放节点。

---

## 四、集合竞价状态机

引擎依据事件时间戳（纳秒精度日内时间）维护三段交易状态：

| 时段 | 竞价状态 | 处理策略 |
| :--- | :--- | :--- |
| **09:15:00 – 09:30:00** | 开盘集合竞价（`in_batch_auction = true`） | 正常维护委托入簿与撤单，但不判定盘口交叉为异常 |
| **09:30:00 – 14:57:00** | 连续竞价（`in_batch_auction = false`） | 每次事件后校验 `best_bid < best_ask`；若出现买一大于等于卖一（crossed book），累加异常计数 `ob_state`；恢复正常后重置 `ob_state = 0` |
| **14:57:00 – 15:00:00** | 收盘集合竞价（`in_batch_auction = true`） | 简化处理：跳过委托入簿或仅保留连续竞价末态用于特征输出 |

---

## 五、工程边界与取舍

为控制复杂度并聚焦离线因子研究，当前版本明确界定如下边界：

- **当前实现范围**：
  - 支持限价委托（含清洗层已转为限价的本方最优委托）的增、删、减处理；
  - `void* level` 的 $O(1)$ 价位回指与 `PriceLevel` 增量统计；
  - `pending_trades` 乱序成交兜底与负值防御检测；
  - 三段式集合竞价状态机与交叉盘口异常计数。
- **刻意不做的特性**：
  - 不处理实盘市价单自动转限价或跨档扫单撮合（A 股逐笔成交已由交易所拆分完毕，市价单占比极低且已在清洗层处理）；
  - 不维护收盘集合竞价（14:57 后）的挂单队列；
  - 不强制做 64 字节 cache-line 对齐（当前面向离线单线程回放，不存在多线程伪共享）；
  - 不实现订单排队位置查询（`GetQueuePosition`），仅聚焦盘口微观结构特征提取。

---

## 六、后续性能优化方向

当基础功能与全量快照对拍验证通过后，可按以下优先级推进低开销优化（均不影响对外接口契约）：

1. **内存分配优化**：将 `Order` 的 `new`/`delete` 替换为基于连续内存块的 `free-list` 对象池（`OrderPool`），消除频繁系统堆分配开销。
2. **订单哈希索引优化**：将 `std::unordered_map` 替换为开放寻址哈希表（如 `flat_hash_map`），减少链表节点跳跃与缓存未命中。
3. **价位表容器优化**：将买卖两侧 `std::map` 替换为连续内存的有序数组（Sorted Vector）配合二分查找（或墓碑惰性清理），提升深度遍历的缓存局部性。
4. **快照懒更新（Lazy Snapshot）**：在高频逐笔事件流中仅标记脏状态，仅当到达固定采样间隔需要输出特征时才读取盘口前 $N$ 档。
