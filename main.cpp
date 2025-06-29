#include <atomic>
#include <array>
#include <thread>
#include <iostream>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <x86intrin.h>
#include <optional>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "LockFreePubSubQueue.h"
#include "Messages.h"

constexpr const char* kShmName = "/lockfree_pubsub_queue";
constexpr int kTotalMessages = 1'000'000;

// ---
// map_shared_queue(create) - Maps shared memory segment to local address space:
//  - Creates or opens POSIX SHM object and memory maps it.
//  - Returns pointer to shared queue structure.
LockFreePubSubQueue<Message>* map_shared_queue(bool create = false) {
    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    // Create or open a POSIX shared memory object
    int fd = shm_open(kShmName, flags, 0666);
    if (fd == -1) {
        // Create or open a POSIX shared memory object
        perror("shm_open");
        return nullptr;
    }

    size_t size = sizeof(LockFreePubSubQueue<Message>);
    if (create && ftruncate(fd, size) != 0) {
        perror("ftruncate");
        return nullptr;
    }

    // Memory map the shared memory object to access shared queue and state
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        // Memory map the shared memory object to access shared queue and state
        perror("mmap");
        return nullptr;
    }

    return reinterpret_cast<LockFreePubSubQueue<Message>*>(ptr);
}

void run_producer(LockFreePubSubQueue<Message>* queue) {
    std::cout << "Press Enter to start publishing messages..." << std::endl;
    std::cin.get();

    uint64_t add_count = 0, trade_count = 0, delete_count = 0;
    MessageType types[3] = { MessageType::Add, MessageType::Trade, MessageType::Delete };
    int index = 0;

    auto start = std::chrono::high_resolution_clock::now();
    for (unsigned int i = 1; i <= kTotalMessages; ++i) {
        // publish(item) - Producer attempts to publish a new message:
        // 1. Atomically get the next sequence number.
        // 2. If buffer is full (ring size limit), compute min consumer sequence.
        // 3. If still full, return false (client can retry).
        // 4. Otherwise, write message to ring and update slot sequence.
        bool published = false;
        while (!published) {
            MessageType type = types[index];
            index = (index + 1 < 3) ? index + 1 : 0;

            switch (type) {
                case MessageType::Add:
                    published = queue->publish(AddOrder{1000 + i, 101.5 + i, 10 + i});
                    if (published) add_count++;
                    break;
                case MessageType::Trade:
                    published = queue->publish(Trade{2000 + i, 100.1 + i, 5 + i});
                    if (published) trade_count++;
                    break;
                case MessageType::Delete:
                    published = queue->publish(DeleteOrder{3000 + i});
                    if (published) delete_count++;
                    break;
            }
            // Yield execution briefly to avoid busy-waiting
            if (!published) [[unlikely]] _mm_pause();
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    std::cout << "Published " << kTotalMessages << " messages in " << elapsed.count() << " seconds"
              << " | Add: " << add_count << ", Trade: " << trade_count << ", Delete: " << delete_count << std::endl;
}

void run_consumer(LockFreePubSubQueue<Message>* queue, double cycles_per_ns) {
    //Register consumer with start sequence of 1
    auto cid_opt = queue->register_consumer(1);
    if (!cid_opt.has_value()) {
        std::cerr << "Failed to register consumer" << std::endl;
        return;
    }
    int cid = cid_opt.value();
    uint64_t total_latency = 0;
    uint64_t count = 0, add_count = 0, trade_count = 0, delete_count = 0;

    while (count < kTotalMessages) {
        Message msg;
        if (!queue->consume(cid, msg)) {
          // Yield execution briefly to avoid busy-waiting
            _mm_pause();
            continue;
        }

        uint64_t now = rdtsc();
        total_latency += (now - msg.timestamp_ns);
        count++;

        switch (msg.type) {
            case MessageType::Add: add_count++; break;
            case MessageType::Trade: trade_count++; break;
            case MessageType::Delete: delete_count++; break;
        }
    }

    double avg_latency_us = ((double)total_latency / cycles_per_ns) / count / 1000.0;
    std::cout << "Consumer " << cid << " avg latency: " << avg_latency_us << " us | "
              << "Add: " << add_count << ", Trade: " << trade_count << ", Delete: " << delete_count << std::endl;

    queue->unregister_consumer(cid);
}


// ---
// main(argc, argv) - Entry point to run producer (p), consumer (c), or cleanup (x):
//  - In 'p' mode: waits for keypress, publishes 1M messages with cycling types.
//  - In 'c' mode: registers a consumer, receives messages, calculates latency.
//  - In 'x' mode: unlinks the shared memory segment.
int main(int argc, char* argv[]) {
    if (argc != 2 || (argv[1][0] != 'p' && argv[1][0] != 'c' && argv[1][0] != 'x')) {
        std::cerr << "Usage: " << argv[0] << " [p|c|x]" << std::endl;
        return 1;
    }

    double cycles_per_ns = calibrate_cycles_per_ns();

    if (argv[1][0] == 'x') {
        if (shm_unlink(kShmName) == 0) {
            std::cout << "Shared memory cleaned up successfully." << std::endl;
        } else {
            perror("shm_unlink");
        }
        return 0;
    }

    bool is_producer = (argv[1][0] == 'p');
    LockFreePubSubQueue<Message>* queue = map_shared_queue(is_producer);
    if (!queue) return 1;

    if (is_producer) {
        run_producer(queue);
    } else {
        run_consumer(queue, cycles_per_ns);
    }

    return 0;
}
