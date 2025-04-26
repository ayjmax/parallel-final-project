#include <iostream>
#include <vector>
#include <optional>
#include <functional> // for std::hash
#include <mutex>
#include <shared_mutex> // Could use for reads if resize wasn't lock-all
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <stdexcept> // For std::runtime_error
#include <numeric>   // For std::iota
#include <algorithm> // For std::shuffle, std::min, std::max

// Configuration Constants
const size_t NUM_TABLES = 2;
const size_t NUM_LOCKS = 32; // Fixed number of locks for striping
const int MAX_KICK_LIMIT = 100; // Max displacement attempts before resize

// Use the specified seed for reproducibility
std::mt19937 gen(714);
std::mutex gen_mutex; // Mutex to protect the shared generator in the test harness

template <typename T>
class StripedCuckooHashSet {
private:
    std::vector<std::vector<std::optional<T>>> table;
    std::atomic<size_t> capacity; // Capacity of EACH table
    std::atomic<size_t> current_size;
    std::vector<std::mutex> locks;

    // Hash functions
    size_t h1(const T& x, size_t current_capacity) const {
        // Ensure capacity is not zero to avoid modulo by zero
        if (current_capacity == 0) return 0;
        // Use std::hash and combine with a prime number for better distribution
        return (std::hash<T>{}(x)) % current_capacity;
    }

    size_t h2(const T& x, size_t current_capacity) const {
         // Ensure capacity is not zero to avoid modulo by zero
        if (current_capacity == 0) return 0;
        // Simple secondary hash: XOR with a constant and shift
        size_t hash_val = std::hash<T>{}(x);
        return ((hash_val ^ (hash_val >> 16)) * 0x85ebca6b) % current_capacity; // Magic number multiplier
    }

    // --- Locking ---
    // Gets the indices of the locks responsible for item x
    std::pair<size_t, size_t> get_lock_indices(const T& x, size_t current_capacity) const {
        if (current_capacity == 0) return {0, 0}; // Handle empty table case
        size_t pos1 = h1(x, current_capacity);
        size_t pos2 = h2(x, current_capacity);
        // Map hash positions to lock indices
        size_t lock1_idx = pos1 % NUM_LOCKS;
        size_t lock2_idx = pos2 % NUM_LOCKS;
        // Return in fixed order (min, max) to prevent deadlock
        return {std::min(lock1_idx, lock2_idx), std::max(lock1_idx, lock2_idx)};
    }

    // Acquires the necessary locks for item x
    std::unique_lock<std::mutex> acquire_lock1(size_t lock_idx) {
        return std::unique_lock<std::mutex>(locks[lock_idx]);
    }

    std::unique_lock<std::mutex> acquire_lock2(size_t lock_idx1, size_t lock_idx2) {
         if (lock_idx1 == lock_idx2) {
             // If indices are same, return an empty unique_lock (no second lock needed)
             return std::unique_lock<std::mutex>();
         }
         return std::unique_lock<std::mutex>(locks[lock_idx2]);
    }


    // --- Unsafe Operations (Assume locks are held or called non-concurrently) ---

    // Non-thread-safe check if element exists (used internally)
    bool contains_unsafe(const T& x, size_t current_capacity) const {
        if (current_capacity == 0) return false;
        size_t pos1 = h1(x, current_capacity);
        if (table[0][pos1].has_value() && table[0][pos1].value() == x) {
            return true;
        }
        size_t pos2 = h2(x, current_capacity);
        if (table[1][pos2].has_value() && table[1][pos2].value() == x) {
            return true;
        }
        return false;
    }

    // Non-thread-safe add used by resize and populate. Attempts Cuckoo displacements.
    // Returns true if added successfully, false otherwise (needs resize).
    bool add_cuckoo_unsafe(T item, std::vector<std::vector<std::optional<T>>>& target_table, size_t target_capacity) {
        if (target_capacity == 0) return false; // Cannot add to zero-capacity table

        T current_item = item;
        size_t current_table_idx = 0; // Start with table 0

        for (int count = 0; count < MAX_KICK_LIMIT; ++count) {
            size_t pos;
            if (current_table_idx == 0) {
                pos = h1(current_item, target_capacity);
            } else {
                pos = h2(current_item, target_capacity);
            }

            // Try to place the item
            if (!target_table[current_table_idx][pos].has_value()) {
                target_table[current_table_idx][pos] = current_item;
                return true; // Successfully placed
            }

            // If slot occupied, swap and continue displacing the old item
            T victim = target_table[current_table_idx][pos].value();
            target_table[current_table_idx][pos] = current_item;
            current_item = victim;

            // Move to the other table for the next attempt
            current_table_idx = 1 - current_table_idx;
        }

        // If loop finishes, we failed to place the item after MAX_KICK_LIMIT attempts
        // The 'current_item' that failed is effectively lost in this unsafe context.
        // The caller (resize) needs to handle this, typically by resizing again.
        // For populate, we might just skip the element if it causes too many kicks.
        // In our resize, we'll print a warning and potentially try resizing again,
        // but for simplicity here, we just return false.
        std::cerr << "Warning: Cuckoo kick limit reached during unsafe add/resize for item. Resize might be needed again." << std::endl;
        return false;
    }

    // --- Resize ---
    // Resize the hash table. Locks ALL stripes.
    void resize() {
        size_t old_capacity = capacity.load();

        // Acquire all locks in fixed order to prevent deadlock
        std::vector<std::unique_lock<std::mutex>> acquired_locks;
        for (size_t i = 0; i < NUM_LOCKS; ++i) {
            acquired_locks.emplace_back(locks[i]);
        }

        // Double-check capacity: another thread might have resized already
        if (capacity.load() != old_capacity) {
            return; // Release locks via unique_lock RAII
        }

        // Prevent resizing infinitely if add_cuckoo_unsafe consistently fails
        if (old_capacity > (1 << 28)) { // Set a reasonable upper limit
             std::cerr << "Error: Maximum capacity reached or resize failed repeatedly." << std::endl;
             // Optionally throw an exception or handle differently
             return;
        }

        size_t new_capacity = (old_capacity == 0) ? 16 : old_capacity * 2; // Start small or double
        //std::cout << "[Resize] Resizing from " << old_capacity << " to " << new_capacity << std::endl;


        std::vector<std::vector<std::optional<T>>> new_table(NUM_TABLES);
        new_table[0].resize(new_capacity);
        new_table[1].resize(new_capacity);

        size_t rehash_count = 0;
        // Rehash existing elements into the new table
        for (size_t i = 0; i < NUM_TABLES; ++i) {
            for (size_t j = 0; j < old_capacity; ++j) {
                if (table[i][j].has_value()) {
                    rehash_count++;
                    // Use the unsafe cuckoo add logic for re-inserting
                    if (!add_cuckoo_unsafe(table[i][j].value(), new_table, new_capacity)) {
                        // This indicates the new table couldn't accommodate the elements
                        // even after kicking. This shouldn't happen often if the hash
                        // functions are good and the load factor isn't pathologically high.
                        // We might need to resize again immediately with a larger factor,
                        // or throw an error. For simplicity, print error and potentially lose element.
                         std::cerr << "Error: Failed to rehash element during resize. Table might be full or hash collision issue." << std::endl;
                         // To try again, we could loop the resize here, but let's avoid infinite loops
                         // For now, just proceed, potentially losing the element that failed rehash
                    }
                }
            }
        }

        // Swap the tables and update capacity
        table = std::move(new_table);
        capacity.store(new_capacity); // Update atomic capacity *after* table swap

        // Locks are released automatically when unique_locks go out of scope
         //std::cout << "[Resize] Finished. Rehased " << rehash_count << " items." << std::endl;
    }


public:
    // Constructor
    explicit StripedCuckooHashSet(size_t initial_capacity = 16) :
        capacity(initial_capacity),
        current_size(0),
        locks(NUM_LOCKS) // Initialize NUM_LOCKS mutexes
    {
        if (initial_capacity == 0) initial_capacity = 16; // Ensure non-zero capacity
        capacity.store(initial_capacity); // Store initial capacity
        table.resize(NUM_TABLES);
        table[0].resize(capacity);
        table[1].resize(capacity);
    }

    // Add an element
    bool add(const T& x) {
        for (int attempt = 0; attempt < 2; ++attempt) { // Try twice (once before resize, once after potential resize)
            size_t current_cap = capacity.load();
            auto [lock_idx1, lock_idx2] = get_lock_indices(x, current_cap);

            std::unique_lock<std::mutex> lk1 = acquire_lock1(lock_idx1);
            std::unique_lock<std::mutex> lk2 = acquire_lock2(lock_idx1, lock_idx2);

            // Check capacity again after acquiring locks, might have changed
            if (capacity.load() != current_cap) {
                 // Capacity changed, release locks and retry the outer loop
                 lk1.unlock(); // Manually unlock before continuing
                 if(lk2.owns_lock()) lk2.unlock();
                 continue;
            }

            // Check if element already exists
            if (contains_unsafe(x, current_cap)) {
                return false; // Already present
            }

            // Check load factor - If too high, trigger resize proactively?
            // For Cuckoo, primary trigger is insertion failure.
            // size_t size_now = current_size.load();
            // if (size_now >= current_cap * NUM_TABLES * 0.5) { // Example threshold
            //     // Release locks before calling resize
            //     lk1.unlock();
            //     if(lk2.owns_lock()) lk2.unlock();
            //     resize();
            //     continue; // Retry add after resize
            // }

            // Try direct insertion into table 0
            size_t pos1 = h1(x, current_cap);
            if (!table[0][pos1].has_value()) {
                table[0][pos1] = x;
                current_size++;
                return true;
            }

            // Try direct insertion into table 1
            size_t pos2 = h2(x, current_cap);
            if (!table[1][pos2].has_value()) {
                table[1][pos2] = x;
                current_size++;
                return true;
            }

            // If both slots are full, release locks and trigger resize (if first attempt)
            if (attempt == 0) {
                 lk1.unlock();
                 if(lk2.owns_lock()) lk2.unlock();
                resize();
                // After resize, the outer loop retries the add.
            } else {
                // If still fails after resize attempt, report failure
                // This might indicate the resize failed or the element couldn't be placed.
                 //std::cerr << "Warning: Add failed even after resize attempt for item." << std::endl;
                 return false;
            }
        }
        // Should not be reached if resize logic is sound and capacity increases
        return false;
    }


    // Remove an element
    bool remove(const T& x) {
        size_t current_cap = capacity.load();
         auto [lock_idx1, lock_idx2] = get_lock_indices(x, current_cap);

        std::unique_lock<std::mutex> lk1 = acquire_lock1(lock_idx1);
        std::unique_lock<std::mutex> lk2 = acquire_lock2(lock_idx1, lock_idx2);

        // Check capacity again after acquiring locks
         if (capacity.load() != current_cap) {
             // Capacity changed, indicates resize happened concurrently.
             // The element *might* be at a different location now.
             // Simplest is to release and let caller retry if needed, or just return false.
             return false; // Indicate potential inconsistency or element moved
         }

        size_t pos1 = h1(x, current_cap);
        if (table[0][pos1].has_value() && table[0][pos1].value() == x) {
            table[0][pos1].reset();
            current_size--;
            return true;
        }

        size_t pos2 = h2(x, current_cap);
        if (table[1][pos2].has_value() && table[1][pos2].value() == x) {
            table[1][pos2].reset();
            current_size--;
            return true;
        }

        return false; // Not found
    }

    // Check if an element exists
    bool contains(const T& x) {
        size_t current_cap = capacity.load();
        auto [lock_idx1, lock_idx2] = get_lock_indices(x, current_cap);

        std::unique_lock<std::mutex> lk1 = acquire_lock1(lock_idx1);
        std::unique_lock<std::mutex> lk2 = acquire_lock2(lock_idx1, lock_idx2);

        // Check capacity again after acquiring locks
         if (capacity.load() != current_cap) {
              // Capacity changed, element might have moved.
              // For contains, returning false is safer than potentially wrong true.
              // Or retry? Let's return false for simplicity.
             return false;
         }

        size_t pos1 = h1(x, current_cap);
        if (table[0][pos1].has_value() && table[0][pos1].value() == x) {
            return true;
        }

        size_t pos2 = h2(x, current_cap);
        if (table[1][pos2].has_value() && table[1][pos2].value() == x) {
            return true;
        }

        return false;
    }

    // --- Non-Thread-Safe Methods ---

    // Get current size (non-thread-safe as per requirement)
    // Provides a snapshot, might be slightly stale in concurrent scenarios.
    size_t size() const {
        return current_size.load(std::memory_order_relaxed); // Relaxed is fine for non-thread-safe
    }

    // Get current capacity (non-thread-safe)
    size_t get_capacity() const {
        return capacity.load(std::memory_order_relaxed);
    }

     // Populate the set with random elements (non-thread-safe)
    void populate(size_t num_elements) {
        // Ensure called non-concurrently. Lock all perhaps? No, spec says non-thread-safe.
        if (current_size.load() != 0) {
             std::cerr << "Warning: Populating a non-empty hash set." << std::endl;
             // Clear existing elements? Or just add? Let's just add.
        }

        std::uniform_int_distribution<int> dist(0, std::numeric_limits<int>::max());
        size_t added_count = 0;
        size_t initial_cap = capacity.load();

        // Use the shared generator directly as this method is non-thread-safe
        for (size_t i = 0; i < num_elements; ++i) {
            T val = dist(gen);

            // Use a simpler internal add logic since we are non-concurrent
            // Or just call the thread-safe add? Let's use a direct unsafe add
            // to avoid potential resize loops if populate hits limits early.

            // Need to ensure capacity is sufficient *before* unsafe add attempts.
            // Resize if needed based on current size and target population size.
            // Simplification: Assume initial capacity is large enough for populate.
            // If add_cuckoo_unsafe fails, we might skip elements.

            if(contains_unsafe(val, initial_cap)) continue; // Avoid duplicates during populate

             if (add_cuckoo_unsafe(val, table, initial_cap)) {
                 added_count++;
             } else {
                 // If unsafe add fails, maybe try resize and add again?
                 // For populate simplicity, let's just report and potentially skip.
                 // std::cerr << "Warning: Populate failed to add element due to kick limit." << std::endl;

                 // Let's try calling the regular 'add' which handles resize
                 // Need to be careful about concurrent access if populate isn't truly isolated
                 // Assuming populate runs *before* any concurrent threads:
                 if (add(val)) { // This 'add' might trigger resize
                     added_count++;
                     // Update initial_cap if resize happened, though add handles it internally
                     initial_cap = capacity.load();
                 } else {
                     std::cerr << "Warning: Populate failed to add element even with resize attempts." << std::endl;
                 }
             }
        }
        // Update size atomically just once at the end if using unsafe add
        // current_size.fetch_add(added_count); // If using add_cuckoo_unsafe
        // If using regular add(), current_size is updated internally.
        // Let's rely on regular add() for robustness including resize.
        // Note: We called add() inside the loop, so current_size is already updated.
    }
};


// --- Test Harness ---

// Shared counters for tracking operations
std::atomic<long long> successful_adds = 0;
std::atomic<long long> successful_removes = 0;

// Worker function for each thread
void worker_thread(StripedCuckooHashSet<int>& hash_set, int ops_per_thread) {
    // Each thread needs its own range of operations
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<int> val_dist(0, std::numeric_limits<int>::max()); // Range of values

    for (int i = 0; i < ops_per_thread; ++i) {
        int operation_type;
        int value;

        // Lock the shared generator to get random numbers
        {
            std::lock_guard<std::mutex> lock(gen_mutex);
            operation_type = op_dist(gen);
            value = val_dist(gen);
        }

        if (operation_type < 80) { // 80% contains
            hash_set.contains(value);
        } else if (operation_type < 90) { // 10% add
            if (hash_set.add(value)) {
                successful_adds++;
            }
        } else { // 10% remove
            if (hash_set.remove(value)) {
                successful_removes++;
            }
        }
    }
}


int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <total_operations> <num_threads>" << std::endl;
        return 1;
    }

    int total_operations = std::stoi(argv[1]);
    int num_threads = std::stoi(argv[2]);

    if (total_operations <= 0 || num_threads <= 0) {
        std::cerr << "Operations and threads must be positive integers." << std::endl;
        return 1;
    }

    // --- Setup ---
    // Per requirements: Initial capacity 10 million, populate to 1/2 capacity (5 million)
    size_t initial_capacity = 10000000;
    size_t populate_count = initial_capacity / 2;

    StripedCuckooHashSet<int> hash_set(initial_capacity);

    // --- Populate ---
    // std::cout << "Populating hash set with " << populate_count << " elements..." << std::endl;
    auto populate_start = std::chrono::high_resolution_clock::now();
    hash_set.populate(populate_count);
    auto populate_end = std::chrono::high_resolution_clock::now();
    long long populate_time = std::chrono::duration_cast<std::chrono::microseconds>(populate_end - populate_start).count();
    // std::cout << "Population finished in " << populate_time << " us." << std::endl;

    size_t initial_size = hash_set.size();
    size_t initial_cap_after_populate = hash_set.get_capacity(); // Capacity might change if populate caused resize

    // --- Concurrent Operations ---
    std::vector<std::thread> threads;
    int ops_per_thread = total_operations / num_threads;
    int remaining_ops = total_operations % num_threads; // Distribute remainder ops

     std::cout << "– Running " << total_operations << " Operations w/ " << num_threads << " Threads –" << std::endl;

    successful_adds = 0;
    successful_removes = 0;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i) {
        int thread_ops = ops_per_thread + (i < remaining_ops ? 1 : 0);
        if (thread_ops > 0) {
             threads.emplace_back(worker_thread, std::ref(hash_set), thread_ops);
        }
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    long long total_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    // --- Verification and Output ---
    size_t final_size = hash_set.size();
    size_t final_capacity = hash_set.get_capacity();
    long long expected_size = static_cast<long long>(initial_size) + successful_adds.load() - successful_removes.load();

    long long avg_time_us = (total_operations == 0) ? 0 : total_time_us / total_operations;

    std::cout << "Total time: " << total_time_us << std::endl;
    std::cout << "Average time per operation: " << avg_time_us << std::endl; // Might be 0 if total_time_us < total_operations
    std::cout << "Hashset initial size: " << initial_size << std::endl;
    std::cout << "Hashset initial capacity: " << initial_cap_after_populate << std::endl; // Report capacity *after* populate
    std::cout << "Successful Adds: " << successful_adds.load() << std::endl;
    std::cout << "Successful Removes: " << successful_removes.load() << std::endl;
    std::cout << "Expected size: " << expected_size << std::endl;
    std::cout << "Final hashset size: " << final_size << std::endl;
    std::cout << "Final hashset capacity: " << final_capacity << std::endl;

    // Final check
    if (static_cast<long long>(final_size) != expected_size) {
        std::cerr << "[Error] Mismatch between final size and expected size!" << std::endl;
         return 1; // Indicate failure
    } else {
         //std::cout << "[Success] Final size matches expected size." << std::endl;
    }

    return 0;
}