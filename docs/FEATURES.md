# 微观结构特征定义

本文档定义 `Orderflow_engine` 从重建后的限价订单簿（LOB）中提取的 **19 个盘口微观结构特征**、计算逻辑、采样策略与值域不变量约束。

---

## 一、19 个微观结构特征总览

| # | 特征名 | 类别 | 类型 | 计算公式 / 定义 | 经济含义 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 1 | `best_bid` | 盘口价格 | `int64_t` | 买侧最高挂单价（`bids` 第 1 档价格） | 当前市场买方最高出价（万分之一元） |
| 2 | `best_ask` | 盘口价格 | `int64_t` | 卖侧最低挂单价（`asks` 第 1 档价格） | 当前市场卖方最低要价（万分之一元） |
| 3 | `mid_price` | 盘口价格 | `double` | `(best_bid + best_ask) / 2.0` | 买卖最优中间价，反映瞬时公允价值基准 |
| 4 | `spread` | 盘口价格 | `int64_t` | `best_ask - best_bid` | 买卖价差，衡量即时流动性成本与逆向选择风险 |
| 5 | `bid_depth_1` | 盘口深度 | `int64_t` | 买 1 档累计挂单量 | 买一价位即时承接流动性 |
| 6 | `ask_depth_1` | 盘口深度 | `int64_t` | 卖 1 档累计挂单量 | 卖一价位即时供给阻力 |
| 7 | `bid_depth_5` | 盘口深度 | `int64_t` | 买 1–5 档累计挂单总量 | 浅层买盘支撑厚度 |
| 8 | `ask_depth_5` | 盘口深度 | `int64_t` | 卖 1–5 档累计挂单总量 | 浅层卖盘抛压厚度 |
| 9 | `bid_depth_10` | 盘口深度 | `int64_t` | 买 1–10 档累计挂单总量 | 十档买盘深度承接能力 |
| 10 | `ask_depth_10` | 盘口深度 | `int64_t` | 卖 1–10 档累计挂单总量 | 十档卖盘深度供给压力 |
| 11 | `imbalance_1` | 盘口不平衡 | `double` | `(bid_depth_1 - ask_depth_1) / (bid_depth_1 + ask_depth_1)` | 一档买卖失衡度，刻画短期微观买卖压差 |
| 12 | `imbalance_5` | 盘口不平衡 | `double` | `(bid_depth_5 - ask_depth_5) / (bid_depth_5 + ask_depth_5)` | 前五档买卖失衡度，过滤单档噪点后的供需倾斜 |
| 13 | `imbalance_10` | 盘口不平衡 | `double` | `(bid_depth_10 - ask_depth_10) / (bid_depth_10 + ask_depth_10)` | 前十档买卖失衡度，刻画宽尺度盘口压力 |
| 14 | `bid_levels` | 盘口形态 | `int32_t` | 当前买侧非空价位档位总数 | 买盘报价分散程度与深度覆盖广度 |
| 15 | `ask_levels` | 盘口形态 | `int32_t` | 当前卖侧非空价位档位总数 | 卖盘报价分散程度与深度覆盖广度 |
| 16 | `bid_vwap_5` | 盘口形态 | `double` | 前 5 档买单挂单量加权均价 | 买盘前五档有效承接价格重心 |
| 17 | `ask_vwap_5` | 盘口形态 | `double` | 前 5 档卖单挂单量加权均价 | 卖盘前五档有效卖出成本重心 |
| 18 | `slope_bid_5` | 盘口形态 | `double` | 买盘前 5 档挂单量随档位深度 `i = 1..5` 的 OLS 斜率 | 买盘厚度沿价格深度的衰减或堆积速度 |
| 19 | `slope_ask_5` | 盘口形态 | `double` | 卖盘前 5 档挂单量随档位深度 `i = 1..5` 的 OLS 斜率 | 卖盘厚度沿价格深度的衰减或堆积速度 |

---

## 二、计算细节与边界处理

1. **深度累计（`depth_N`）**
   - 从 `bids.begin()` 或 `asks.begin()` 起顺序遍历前 $\min(N, \text{levels})$ 个非空价位，直接累加每个 `PriceLevel` 的增量汇总字段 `total_volume()`。
   - 若某侧盘口为空，对应深度返回 `0`。

2. **买卖失衡度（`imbalance_N`）**
   - 当分母 `bid_depth_N + ask_depth_N == 0`（双边无深度）时，约定返回 `0.0`，避免除零产生 `NaN`。

3. **前五档加权均价（`vwap_5`）**
   - 定义为前 $k = \min(5, \text{levels})$ 档的量价加权平均：
     $$\text{vwap}_5 = \frac{\sum_{i=1}^{k} \text{price}_i \times \text{volume}_i}{\sum_{i=1}^{k} \text{volume}_i}$$
   - 若某侧不足 5 档，则按实际可用档位计算；若该侧为空（总量为 0），返回 `0.0` 或 `NaN` 并标记无效快照。

4. **前五档深度斜率（`slope_5`）**
   - 以档位序号 $x_i = i \in \{1, 2, \dots, k\}$ 为自变量、对应档位挂单量 $y_i = \text{volume}_i$ 为因变量，计算普通最小二乘（OLS）线性回归斜率：
     $$\text{slope} = \frac{\sum_{i=1}^{k} (x_i - \bar{x})(y_i - \bar{y})}{\sum_{i=1}^{k} (x_i - \bar{x})^2}$$
   - 当可用档位数 $k < 2$ 时，斜率约定返回 `0.0`。

5. **单边缺档处理**
   - 涨跌停封板等场景下，买侧或卖侧可能为空。此时 `best_bid` / `best_ask` 为空的一侧记为占位值（如 `0`），对应的 `spread` 与 `mid_price` 在上层因子中结合涨跌停状态识别并过滤。

---

## 三、采样与聚合策略

1. **引擎侧快照采样**：
   - 在逐笔驱动 `OrderBook::apply()` 的过程中，按固定时间步长（默认 `snapshot_interval_ms = 3000`，即每 **3 秒**）抽取一次 `BookSnapshot`。
   - 仅在采样时点跨越时间窗边界时触发一次前 10 档遍历，避免每笔事件都扫描全盘口。
2. **分钟频聚合（因子层）**：
   - 上层 Python 因子库将 3 秒级快照流按 `1min` 时间桶向下取整分组；
   - 根据因子研究需求提取分钟末切片状态（`last`）或分钟内统计聚合（`mean`、`std`、`max`），最终落盘为按月分区的 Parquet 因子表。

---

## 四、值域与结构约束

在连续竞价且盘口双边非空、状态正常（`is_good == true && ob_state == 0`）的前提下，输出特征必须满足以下数学不变量：

- $\text{imbalance}_1,\ \text{imbalance}_5,\ \text{imbalance}_{10} \in [-1.0,\ 1.0]$
- $\text{spread} \ge 0$（出现负值说明发生交叉盘口，需核查异常）
- $\text{bid\_depth}_N \ge 0,\quad \text{ask\_depth}_N \ge 0$
- $0 \le \text{bid\_depth}_1 \le \text{bid\_depth}_5 \le \text{bid\_depth}_{10}$
- $0 \le \text{ask\_depth}_1 \le \text{ask\_depth}_5 \le \text{ask\_depth}_{10}$
- $\text{bid\_levels} \ge 0,\quad \text{ask\_levels} \ge 0$
- $\text{bid\_vwap}_5 \le \text{best\_bid} < \text{best\_ask} \le \text{ask\_vwap}_5$
