//
// Created by Chen Nachuan on 2026/11/15.
//

#pragma once
#include <cstdint>

namespace orderflow_engine {

    enum class EventType : uint8_t {
        ADD = 1,
        CANCEL = 2,
        EXECUTE = 3,
        MODIFY = 4
    };

    enum class Side : int8_t {
        BUY = 1,
        SELL = -1,
        UNKNOWN = 0
    };

    // 强制编译器按 1 字节对齐，禁用内存填充，保证内存布局与磁盘二进制文件完全一致
    #pragma pack(push, 1)
    struct Event {
        uint64_t timestamp;
        uint64_t order_id;

        int64_t price;
        int64_t volume;

        EventType type;
        Side side;
    };
    #pragma pack(pop)
}
