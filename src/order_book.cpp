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
        (void)event;
    }

    void OrderBook::on_cancel(const Event& event) {
        (void)event;
    }

    void OrderBook::on_execute(const Event& event) {
        (void)event;
    }

}
