# Lock-Free SPMC Queue for IPC

A high-performance, lock-free Single Producer Multiple Consumer (SPMC) queue implementation in C++ designed for ultra-low latency inter-process communication using shared memory.

Blog post that decribes this queue in detail is present at: https://medium.com/@manojddesilva/efficient-inter-process-pub-sub-in-c-a-lock-free-low-latency-spmc-queue-9ee06f916827


## Features

- **Lock-Free Operations**: Zero blocking operations using only atomic primitives
- **Shared Memory IPC**: Cross-process communication without kernel involvement
- **Publish-Subscribe Semantics**: Each consumer receives every message independently
- **Ultra-Low Latency**: Sub-microsecond message delivery with nanosecond precision timing
- **Zero Dynamic Allocation**: Real-time safe with no malloc/free operations
- **Cache-Optimized**: Aligned data structures prevent false sharing
- **Dynamic Consumer Management**: Add/remove consumers at runtime without disrupting message flow
- **Multi-Type Messages**: Support for different message types without virtual function overhead

##  Architecture

The queue uses a ring buffer design inspired by the LMAX Disruptor pattern:

```
Producer → [Ring Buffer] → Consumer 1
                      ↘ → Consumer 2
                      ↘ → Consumer N
```

### Key Components

- **Ring Buffer**: Fixed-size circular array with power-of-2 size for efficient indexing
- **Sequence Numbers**: Lock-free coordination using atomic sequence counters
- **Consumer Slots**: Independent consumer state with cache-line alignment
- **Message Types**: Discriminated union for type-safe message handling

## Quick Start

### Prerequisites

- Linux x86_64 system
- GCC 7+ or Clang 6+ with C++17 support
- CMake 3.10+
- POSIX shared memory support

### Building

```bash
git clone https://github.com/manojds/LockFreeQueueForIPC.git
cd LockFreeQueueForIPC
mkdir build && cd build
cmake ..
make
```

### Running the Demo

1. **Start Producer** (Terminal 1):
```bash
./lockfree_ipc_queue p
# Press Enter when consumers are ready
```

2. **Start Consumers** (Terminals 2-4):
```bash
./lockfree_ipc_queue c  # Consumer 1
./lockfree_ipc_queue c  # Consumer 2
./lockfree_ipc_queue c  # Consumer 3
```

3. **Cleanup** (when done):
```bash
./lockfree_ipc_queue x
```

## API Reference

### Core Queue Operations

```cpp
// Create/map shared memory queue
LockFreePubSubQueue<Message>* queue = map_shared_queue(true);

// Producer: Publish a message
bool success = queue->publish(AddOrder{1001, 100.5, 10});

// Consumer: Register and consume
auto consumer_id = queue->register_consumer(0);
Message msg;
bool got_message = queue->consume(consumer_id.value(), msg);

// Cleanup
queue->unregister_consumer(consumer_id.value());
```

## Configuration

Key constants in `LockFreePubSubQueue.h`:

```cpp
constexpr size_t kRingSize = 1024;      // Ring buffer size (power of 2)
constexpr int MaxConsumers = 64;        // Maximum concurrent consumers
constexpr size_t kCacheLineSize = 64;   // CPU cache line size
```

## Detailed Design & Implementation
### Core Architecture

The queue implements a sophisticated lock-free design using several key components:

```cpp
template <typename T>
class LockFreePubSubQueue {
private:
    alignas(64) std::array<Slot<T>, kRingSize> ring_;           // Ring buffer storage
    alignas(64) std::array<ConsumerSlot, MaxConsumers> consumers_; // Consumer state
    uint64_t next_{0};                                          // Next sequence to publish
    uint64_t min_consumer_seq_{0};                             // Cached minimum consumer sequence
};
```
### Ring Buffer Design

The heart of the queue is a **power-of-2 sized ring buffer** that enables efficient wraparound:

```cpp
template <typename T>
struct alignas(64) Slot {
    std::atomic<uint64_t> sequence;  // Version/readiness indicator
    T data;                          // Actual message payload
};
```

**Key Design Features:**
- **Power-of-2 Size**: Enables fast modulo using bitwise AND (`seq & kRingMask`)
- **Cache-Line Alignment**: Each slot is 64-byte aligned to prevent false sharing
- **Atomic Sequence**: Acts as both version counter and readiness flag
- **In-Place Storage**: Messages stored directly, no pointer indirection

### Lock-Free Coordination Protocol

#### Producer Algorithm:

```cpp
bool publish(const T& item) {
    uint64_t next_seq = next_ + 1;
    
    // Fast path: check cached minimum to avoid scanning consumers
    if ((next_seq - min_consumer_seq_) >= kRingSize) {
        // Slow path: recalculate true minimum across all consumers
        uint64_t min_seq = scan_active_consumers();
        if ((next_seq - min_seq) >= kRingSize) {
            return false;  // Ring buffer full
        }
    }
    
    // Safe to write - update data and signal completion
    Slot<T>& slot = ring_[next_seq & kRingMask];
    slot.data = item;
    slot.sequence.store(next_seq + 1, std::memory_order_release);
    next_ = next_seq;
    return true;
}
```
**Producer Guarantees:**
- **Single Writer**: Only one producer can publish at a time
- **No Overwrites**: Never overwrites unread data
- **Atomic Commitment**: Message becomes visible atomically via sequence update

#### Consumer Algorithm:

```cpp
bool consume(int consumer_id, T& out) {
    uint64_t seq = consumers_[consumer_id].sequence.get();
    Slot<T>& slot = ring_[seq & kRingMask];
    
    // Check if message is ready
    if (slot.sequence.load(std::memory_order_acquire) <= seq) {
        return false;  // Not ready yet
    }
    
    // Safe to read - copy data and advance
    out = slot.data;
    consumers_[consumer_id].sequence.set(seq + 1);
    return true;
}
```
**Consumer Guarantees:**
- **Independent Progress**: Each consumer advances at its own pace
- **No Coordination**: Consumers don't block or interfere with each other
- **Consistent Reads**: Memory ordering ensures complete message visibility

### Cache-Friendly Memory Layout
#### False Sharing Prevention:

```cpp
struct alignas(64) ConsumerSlot {
    std::atomic<bool> active{false};
    Sequence sequence{0};
    // Padding to 64 bytes prevents false sharing
};
```

**Cache Optimization Techniques:**
- **64-Byte Alignment**: Aligns with typical CPU cache line size
- **Separated Hot Data**: Producer and consumer data on different cache lines
- **Minimal Shared State**: Only sequence numbers are truly shared

#### Memory Layout Diagram:

```
Cache Line 0: [Slot 0: seq + data]
Cache Line 1: [Slot 1: seq + data]
...
Cache Line N: [Consumer 0 state]
Cache Line N+1: [Consumer 1 state]
```

### Message Type System

Multiple message types are supported using a Discriminated Union.

```cpp
enum class MessageType : uint8_t { Add, Trade, Delete };

struct Message {
    MessageType type;           // 1-byte type discriminator
    uint64_t timestamp_ns;      // High-resolution timestamp
    union {                     // Space-efficient payload
        AddOrder add;
        Trade trade;
        DeleteOrder del;
    };
};
```

### Dynamic Consumer Management
#### Registration Protocol:

```cpp
std::optional<int> register_consumer(uint64_t start_sequence) {
    for (int i = 0; i < MaxConsumers; ++i) {
        bool expected = false;
        if (consumers_[i].active.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
            consumers_[i].sequence.set(start_sequence);
            return i;  // Return consumer ID
        }
    }
    return std::nullopt;  // No slots available
}
```

**Key Features:**
- **Atomic Registration**: Uses CAS for race-free slot acquisition
- **Flexible Start Position**: Can join at any sequence number
- **No Hot Path Impact**: Registration doesn't block message flow
- **Runtime Scalability**: Add/remove consumers without restart

### Shared Memory Implementation

#### POSIX Shared Memory Setup:

```cpp
LockFreePubSubQueue<Message>* map_shared_queue(bool create) {
    int fd = shm_open("/lockfree_pubsub_queue", 
                      create ? (O_CREAT | O_RDWR) : O_RDWR, 0666);
    
    size_t size = sizeof(LockFreePubSubQueue<Message>);
    if (create) ftruncate(fd, size);
    
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    return reinterpret_cast<LockFreePubSubQueue<Message>*>(ptr);
}
```

## Design Principles

### Lock-Free Coordination
- Uses atomic operations with carefully chosen memory orderings
- No mutexes, condition variables, or blocking primitives
- Wait-free for consumers, lock-free for producers

### Memory Management
- Fixed-size data structures using `std::array`
- Cache-line aligned structures prevent false sharing
- Shared memory compatible with trivially copyable types

### Performance Optimization
- Branchless operations where possible
- Efficient power-of-2 ring buffer indexing
- Minimal memory barriers and atomic operations


## Implementation Deep Dive
### Sequence Number Protocol

The queue uses a sophisticated sequence numbering scheme:

```cpp
Sequence Relationships:
- Producer Next:     N
- Slot Sequence:     N+1 (when message ready)
- Consumer Sequence: C (next expected message)

Reading Logic:
if (slot.sequence > consumer.sequence) {
    // Message is ready and newer than what consumer expects
    consume_message();
}
```

**Why This Works:**
- **Monotonic Increase**: Sequences only increment, never wrap
- **Readiness Signal**: Slot sequence > consumer sequence means data ready
- **Race-Free**: Atomic sequence update provides synchronization point

### Producer Backpressure Handling

When the ring buffer approaches capacity:

```cpp
Fast Path Check:
if ((next_seq - cached_min) >= kRingSize) {
    // Potentially full - need precise check
    Slow Path: Scan all active consumers
    true_min = min(all_active_consumer_sequences)
    if ((next_seq - true_min) >= kRingSize) {
        return false;  // Definitely full
    }
}
```
**Optimization Strategy:**
- **Cached Minimum**: Avoids expensive consumer scan on every publish
- **Lazy Updates**: Only recalculate when necessary
- **Conservative Estimation**: Better to occasionally scan than miss full condition

## Current Limitations & Trade-offs

### Technical Constraints

- **Single Producer**: Currently supports only one producer process
  - *Rationale*: Simplifies coordination and maximizes single-producer performance
  - *Future*: Multi-producer support planned using ticket-based allocation

- **Fixed Ring Size**: Buffer size is compile-time constant (1024 slots)
  - *Rationale*: Enables power-of-2 optimizations and predictable memory usage
  - *Alternative*: Could be template parameter for different size variants

- **Platform Specific**: Uses POSIX shared memory and x86_64 `rdtsc`
  - *Rationale*: Targets high-performance Linux trading systems
  - *Portability*: Windows and ARM64 ports possible with platform abstraction

## Future Enhancements

- **Multi-Producer Support**: Extend to multiple producers
- **Branchless Optimizations**: Reduce branch mispredictions
- **Event-Driven Consumers**: Reduce CPU usage with notifications from producer to consumer

## References

- [LMAX Disruptor Pattern](https://lmax-exchange.github.io/disruptor/)
- [Building Low Latency Applications with C++](https://www.packtpub.com/en-us/product/building-low-latency-applications-with-c-9781837634477)
- [Blog post: Efficient Inter-Process Pub-Sub in C++: A Lock-Free, Low-Latency SPMC Queue](https://medium.com/@manojddesilva/efficient-inter-process-pub-sub-in-c-a-lock-free-low-latency-spmc-queue-9ee06f916827)

