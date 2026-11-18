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

    struct Event {
        uint64_t timestamp; // 纳秒精度时间戳
        uint64_t seq; //ApplSeqNum（交易所委托号/成交编号，全局单调）
        uint64_t order_id; // 委托的订单 id

        int64_t price; // *10000，转化成整数避免精度损失
        int64_t volume;

        EventType type;
        Side side;
    };
}
