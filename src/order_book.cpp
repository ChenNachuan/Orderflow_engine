//
// Created by Chen Nachuan on 2026/11/20.
//

#include "orderflow_engine/order_book.h"
#include "orderflow_engine/order.h"
#include <cassert>

namespace orderflow_engine{

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
};