#include <atomic>
#include <array>

constexpr size_t kRingSize = 1024;
constexpr size_t kRingMask = kRingSize - 1;
constexpr int MaxConsumers = 64;
constexpr size_t kCacheLineSize = 64;

template <typename T>
struct alignas(kCacheLineSize) Slot {
    std::atomic<uint64_t> sequence;
    T data;
};

class alignas(kCacheLineSize) Sequence {
public:
    explicit Sequence(uint64_t initial = 0) : value_(initial) {}
    uint64_t get() const { return value_.load(std::memory_order_acquire); }
    void set(uint64_t v) { value_.store(v, std::memory_order_release); }

private:
    std::atomic<uint64_t> value_;
};

struct alignas(kCacheLineSize) ConsumerSlot {
    std::atomic<bool> active{false};
    Sequence sequence{0};
};

template <typename T>
class LockFreePubSubQueue {
public:
    LockFreePubSubQueue() = default;


    // ---
    // register_consumer(start_sequence) - Dynamically registers a new consumer.
    //  - Scans for an inactive consumer slot.
    //  - Marks it active and sets its initial sequence number.
    //  - Returns consumer ID or nullopt if no slot is available.
    std::optional<int> register_consumer(uint64_t start_sequence) {
        for (int i = 0; i < MaxConsumers; ++i) {
            bool expected = false;
            if (consumers_[i].active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                consumers_[i].sequence.set(start_sequence);
                return i;
            }
        }
        return std::nullopt;
    }

    // ---
    // unregister_consumer(id) - Marks a consumer as inactive by ID.
    void unregister_consumer(int id) {
        if (id >= 0 && id < MaxConsumers) {
            consumers_[id].active.store(false, std::memory_order_release);
        }
    }

    // Called by the producer to publish a new message to the queue
    // ---
    // publish(item) - Producer attempts to publish a new message:
    // 1. Atomically get the next sequence number.
    // 2. If buffer is full (ring size limit), compute min consumer sequence.
    // 3. If still full, return false (client can retry).
    // 4. Otherwise, write message to ring and update slot sequence.
    bool publish(const T& item) {
        // Atomically reserve the next sequence number
        uint64_t next_seq = next_ + 1;

        // Check if ring buffer is full by comparing to cached minimum
        if ((next_seq - min_consumner_seq_) >= kRingSize) [[unlikely]] {
            uint64_t min_seq = std::numeric_limits<uint64_t>::max();
            // Recompute true minimum sequence among active consumers
            for (int i = 0; i < MaxConsumers; ++i) {
                if (consumers_[i].active.load(std::memory_order_acquire)) {
                    min_seq = std::min(min_seq, consumers_[i].sequence.get());
                }
            }
            min_consumner_seq_ = min_seq;

            if ((next_seq - min_seq) >= kRingSize) {
                return false;
            }
        }

        next_ = next_seq;
        Slot<T>& slot = ring_[next_seq & kRingMask];
        // Write message to slot
        slot.data = item;
        // Signal message is ready to consumers
        slot.sequence.store(next_seq + 1, std::memory_order_release);
        return true;
    }

    // Called by a consumer to attempt reading a new message
    // ---
    // consume(consumer_id, out) - Consumer attempts to read the next message:
    // 1. Check if consumer is active.
    // 2. Load current expected sequence and corresponding slot.
    // 3. If slot is not yet published, return false.
    // 4. Otherwise, consume message, update sequence, return true.
    bool consume(int consumer_id, T& out) {
        if (consumer_id < 0 || consumer_id >= MaxConsumers) [[unlikely]] return false;
        if (!consumers_[consumer_id].active.load(std::memory_order_acquire)) [[unlikely]] return false;

        Sequence& consumer_seq = consumers_[consumer_id].sequence;
        // Read expected sequence number for this consumer
        uint64_t seq = consumer_seq.get();
        Slot<T>& slot = ring_[seq & kRingMask];

        // Check if message is ready (slot.sequence > expected seq)
        if (slot.sequence.load(std::memory_order_acquire) <= seq) {
            return false;
        }

        // Copy out the message
        out = slot.data;
        // Advance consumer's sequence after consuming
        consumer_seq.set(seq + 1);
        return true;
    }

private:
    alignas(kCacheLineSize) std::array<Slot<T>, kRingSize> ring_;
    alignas(kCacheLineSize) std::array<ConsumerSlot, MaxConsumers> consumers_;
    //to be made atomic variables in case of multi-producer setup
    uint64_t next_{0};
    uint64_t min_consumner_seq_{0};    
};