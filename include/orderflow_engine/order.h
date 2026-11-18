//
// Created by Chen Nachuan on 2026/11/19.
//

#pragma once
#include <cstdint>
#include "types.h"

namespace orderflow_engine {

    struct Order {
        uint64_t order_id;
        uint64_t timestamp;

        int64_t price;
        int64_t volume;
        int64_t original_volume;

        Order* prev = nullptr;
        Order* next = nullptr;
        void* level = nullptr;

        Side side = Side::UNKNOWN;
    };
}
