//
// Created by Chen Nachuan on 2026/11/19.
//

#include "orderflow_engine/price_level.h"
#include "orderflow_engine/order.h"
#include <cassert>

namespace orderflow_engine {
    //price level 的挂单函数
    void PriceLevel::add_order(Order* order) {

        assert(order != nullptr);
        assert(order->volume > 0);
        assert(order->price == price_);
        assert(order->level == nullptr && order->prev == nullptr && order->next == nullptr);

        order->next = nullptr;
        //如果订单为空，将头指针指向这笔订单
        if (head_ == nullptr) {
            order->prev = nullptr;

            head_ = order;
            tail_ = order;

        } else {
            // 挂载这笔订单
            order->prev = tail_;
            tail_->next = order;

            tail_ = order;

        }
        order->level = this;

        order_count_++;
        tol_volume_ += order->volume;

    }

    //price level 的撤单函数
    void PriceLevel::remove_order(Order* order) {
        assert(order != nullptr);
        assert(order->level == this);

        if (order->prev != nullptr) { // 处理订单左侧
            order->prev->next = order->next;

        }else {

            head_ = order->next;
        }

        if (order->next != nullptr) { // 处理订单右侧

            order->next->prev = order->prev;

        }else {

            tail_ = order->prev;
        }
        order->prev = nullptr;
        order->next = nullptr;
        order->level = nullptr;


        order_count_--;
        tol_volume_ -= order->volume;

    }

    void PriceLevel::reduce_order(Order* order, int64_t volume) {
        assert(order != nullptr);
        assert(order->level == this);
        assert(volume > 0);
        assert(volume <= order->volume);
        assert(tol_volume_ >= order->volume);

        order->volume -= volume;
        tol_volume_ -= volume;

        if (order->volume == volume) {
            remove_order(order);
        }
    }
}