//
// Created by Chen Nachuan on 2026/11/19.
//

#pragma once
#include <cstdint>
#include "order.h"

namespace orderflow_engine {

    class PriceLevel {
    public:
        // 初始化函数，explicit 禁止隐式类型转换 PriceLevel = price
        explicit PriceLevel(int64_t price = 0) : price_(price) {}

        void add_order(Order*);
        void reduce_order(Order* ,int64_t);
        void remove_order(Order*);

        // 内部变量访问函数
        [[nodiscard]] int64_t price() const { return price_; }
        [[nodiscard]] int64_t total_volume() const { return tol_volume_; }
        [[nodiscard]] int64_t order_count() const { return order_count_; }
        [[nodiscard]] bool is_empty() const { return order_count_ == 0; }

        [[nodiscard]] Order* head() const { return head_; }
        [[nodiscard]] Order* tail() const { return tail_; }

    private:
        int64_t price_ = 0;
        int64_t tol_volume_ = 0;
        int64_t order_count_ = 0;

        Order* head_ = nullptr;
        Order* tail_ = nullptr;

    };
}
