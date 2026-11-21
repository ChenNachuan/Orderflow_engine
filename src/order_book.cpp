//
// Created by Chen Nachuan on 2026/11/20.
//

#include "orderflow_engine/order_book.h"
#include "orderflow_engine/order.h"
#include "orderflow_engine/types.h"

namespace orderflow_engine {

    namespace {
        constexpr uint64_t kOpenAuctionEndNs = 93'000'000ULL * 1'000'000ULL;    // 09:30:00.000
        constexpr uint64_t kCloseAuctionStartNs = 145'700'000ULL * 1'000'000ULL; // 14:57:00.000
    }

    std::optional<int64_t> OrderBook::best_ask() const {
        if (ask_.empty()) {
            return std::nullopt;
        }
        return ask_.begin()->first;
    }

    std::optional<int64_t> OrderBook::best_bid() const {
        if (bid_.empty()) {
            return std::nullopt;
        }
        return bid_.begin()->first;
    }

    std::optional<int64_t> OrderBook::ask_volume_at(int64_t price) const {
        auto it = ask_.find(price);
        if (it == ask_.end()) {
            return std::nullopt;
        }
        return it->second.total_volume();
    }

    std::optional<int64_t> OrderBook::bid_volume_at(int64_t price) const {
        auto it = bid_.find(price);
        if (it == bid_.end()) {
            return std::nullopt;
        }
        return it->second.total_volume();
    }

    const Order* OrderBook::get_order(uint64_t order_id) const {
        auto it = order_index_.find(order_id);
        if (it == order_index_.end()) {
            return nullptr;
        }
        return it->second;
    }

    void OrderBook::apply(const Event& event) {
        // 更新时间戳（单调向前推进）
        if (event.timestamp > last_timestamp_) {
            last_timestamp_ = event.timestamp;
        }

        // 更新集合竞价状态：收盘集合竞价（>= 14:57）直接跳过，不再更新订单簿
        if (event.timestamp >= kCloseAuctionStartNs) {
            in_batch_auction_ = true;
            return;
        }
        if (event.timestamp >= kOpenAuctionEndNs) {
            in_batch_auction_ = false;
        } else {
            in_batch_auction_ = true;
        }

        // 按照 EventType 分派处理
        switch (event.type) {
            case EventType::ADD:
                on_add(event);
                break;
            case EventType::CANCEL:
                on_cancel(event);
                break;
            case EventType::EXECUTE:
                on_execute(event);
                break;
            default:
                is_good_ = false;
                break;
        }

        // 连续竞价期间检查交叉盘口与未消化 pending trade
        if (!in_batch_auction_) {
            const std::optional<int64_t> best_ask_price = best_ask();
            const std::optional<int64_t> best_bid_price = best_bid();

            const bool is_crossed = best_ask_price.has_value()
                && best_bid_price.has_value()
                && *best_ask_price <= *best_bid_price;

            if (is_crossed || !pending_trade_.empty()) {
                ob_state_++;
            } else {
                ob_state_ = 0;
            }
        }
    }

    void OrderBook::on_add(const Event& event) {
        // Event 检查是否干净，并且是否已经在 OrderBook 中
        if (event.volume <= 0 ||
            (event.side != Side::BUY && event.side != Side::SELL) ||
            order_index_.contains(event.order_id)) {

            is_good_ = false;
            return;
        }

        int64_t remaining_volume = event.volume;
        // 如果在 pending_trade_里面有对应的 id，减去对应量，将剩余量挂进 OrderBook
        if (auto ptr = pending_trade_.find(event.order_id);
            ptr != pending_trade_.end()) {

            remaining_volume = event.volume - ptr->second;
            pending_trade_.erase(ptr);
        }

        // 判定剩余的 volume 是否需要挂进订单
        if (remaining_volume < 0) {
            is_good_ = false;
            return;
        }
        if (remaining_volume == 0){return;}

        // 将剩余部分初始化成一笔新订单
        auto* order = new Order{
            .order_id = event.order_id,
            .timestamp = event.timestamp,
            .price = event.price,
            .volume = remaining_volume,
            .original_volume = event.volume,
            .side = event.side
        };

        // 根据ask/bid 挂进对应的订单簿
        if (order->side == Side::BUY) {
            auto it = bid_.try_emplace(event.price, event.price).first;
            it->second.add_order(order);
            order_index_.emplace(order->order_id, order);
        } else {
            auto it = ask_.try_emplace(event.price, event.price).first;
            it->second.add_order(order);
            order_index_.emplace(order->order_id, order);
        }


    }

    void OrderBook::on_cancel(const Event& event) {
        const auto it = order_index_.find(event.order_id);
        // 判断订单是否正常，并且是否在 order_index_内
        if (it == order_index_.end()) {
            is_good_ = false;
            return;
        }

        Order* order = it->second;

        // 查找对应的订单，将其 level 转化为 PriceLevel* 然后移除对应的 Order
        auto* level = static_cast<PriceLevel*>(order->level);
        level->remove_order(order);

        // 将空 PriceLevel 删除
        if (level->total_volume() == 0) {
            // 从对应的订单簿中移除空 PriceLevel
            if (order->side == Side::SELL) {
                ask_.erase(order->price);
            }else {
                bid_.erase(order->price);
            }
        }

        // 释放对应订单的内存
        delete order;
        order_index_.erase(it);

    }

    void OrderBook::on_execute(const Event& event) {
        
    }

    OrderBook::~OrderBook() {
        for (const auto& [order_id, order] : order_index_) {
            delete order;
        }
    }

}
