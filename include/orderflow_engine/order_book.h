//
// Created by Chen Nachuan on 2026/11/19.
//

#pragma once
#include <cstdint>
#include <map>
#include <unordered_map>
#include <optional>
#include <cassert>
#include "types.h"
#include "orderflow_engine/price_level.h"

namespace orderflow_engine {
    class OrderBook {
    public:
        //构造和析构函数
        OrderBook() = default;
        ~OrderBook() = default;

        // 唯一的操作入口 apply 函数
        void apply(const Event& event);

        // 查询最优买卖价
        [[nodiscard]] std::optional<int64_t> best_ask() const;
        [[nodiscard]] std::optional<int64_t> best_bid() const;

        // 查询对应价位的 volume
        [[nodiscard]] std::optional<int64_t> ask_volume_at(int64_t price) const;
        [[nodiscard]] std::optional<int64_t> bid_volume_at(int64_t price) const;

        //返回对应 ask 和 bid 的对应指针
        const std::map<int64_t, PriceLevel>* ask(){return &ask_;}
        const std::map<int64_t, PriceLevel,std::greater<>>* bid(){return &bid_;}

        [[nodiscard]] const Order* get_order(uint64_t order_id) const;

        order_count();
        bid_level_count();
        ask_level_count();

        is_good();
        ob_state();
        n_batch_auction();

    private:

        // 两个 PriceLevel 队列，红黑树形式
        std::map<int64_t, PriceLevel> ask_;
        std::map<int64_t, PriceLevel,std::greater<>> bid_;

        // 全局订单索引
        std::unordered_map<uint64_t, Order*> order_index_;

        // 判定池 记录“先于委托到达的成交量”
        std::unordered_map<uint64_t, int64_t> pending_trade_;

        // 判断订单薄状态是否健康
        bool is_good_ = true;
        // 记录两个的最优价是否交叉
        int64_t ob_state_ = 0;

        // 集合竞价标志
        bool in_batch_auction_ = true;
        // 最后一个时间的时间戳
        uint64_t last_timestamp_ = 0;
    };
}