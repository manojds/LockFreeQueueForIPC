#include "Utils.h"

// Message Types
// Message type: AddOrder - represents a new order in the book
struct AddOrder {
    uint64_t order_id;
    double price;
    uint32_t quantity;
};

// Message type: Trade - represents a matched trade
struct Trade {
    uint64_t trade_id;
    double price;
    uint32_t quantity;
};

// Message type: DeleteOrder - represents removal of an order
struct DeleteOrder {
    uint64_t order_id;
};

enum class MessageType : uint8_t {
    Add,
    Trade,
    Delete
};

struct Message {
    MessageType type;
    uint64_t timestamp_ns;

    union {
        AddOrder add;
        Trade trade;
        DeleteOrder del;
    };

    Message() = default;
    ~Message() = default;

    // Timestamp the message with CPU cycle counter at publish time
    Message(const AddOrder& a) : type(MessageType::Add), timestamp_ns(rdtsc()), add(a) {}
    // Timestamp the message with CPU cycle counter at publish time
    Message(const Trade& t) : type(MessageType::Trade), timestamp_ns(rdtsc()), trade(t) {}
    // Timestamp the message with CPU cycle counter at publish time
    Message(const DeleteOrder& d) : type(MessageType::Delete), timestamp_ns(rdtsc()), del(d) {}
};