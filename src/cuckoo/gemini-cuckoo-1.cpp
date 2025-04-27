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
        if (current_capacity == 0) return 0;
        return (std::hash<T>{}(x)) % current_capacity;
    }

    size_t h2(const T& x, size_t current_capacity) const {
        if (current_capacity == 0) return 0;
        size_t hash_val = std::hash<T>{}(x);
        return ((hash_val ^ (hash_val >> 16)) * 0x85ebca6b) % current_capacity;
    }

    // --- Locking ---
    std::pair<size_t, size_t> get_lock_indices(const T& x, size_t current_capacity) const {
        if (current_capacity == 0) return {0, 0}; // Handle empty table case
        size_t pos1 = h1(x, current_capacity);
        size_t pos2 = h2(x, current_capacity);
        // Map hash positions to lock indices
        size_t lock1_idx = pos1 % NUM_LOCKS;
        size_t lock2_idx = pos2 % NUM_LOCKS; // Declared as lock2_idx
        // Return in fixed order (min, max) to prevent deadlock
        return {std::min(lock1_idx, lock2_idx), std::max(lock1_idx, lock2_idx)}; // Use correct variable lock2_idx
    }


    std::unique_lock<std::mutex> acquire_lock1(size_t lock_idx) {
        return std::unique_lock<std::mutex>(locks[lock_idx]);
    }

    std::unique_lock<std::mutex> acquire_lock2(size_t lock_idx1, size_t lock_idx2) {
         if (lock_idx1 == lock_idx2) {
             return std::unique_lock<std::mutex>();
         }
         // Ensure the second lock is actually acquired if needed
         return std::unique_lock<std::mutex>(locks[lock_idx2]);
    }


    // --- Unsafe Operations (Assume locks are held or called non-concurrently) ---
    bool contains_unsafe(const T& x, size_t current_capacity) const {
        if (current_capacity == 0) return false;
        size_t pos1 = h1(x, current_capacity);
        if (pos1 < current_capacity && table.size() > 0 && table[0].size() > pos1 && table[0][pos1].has_value() && table[0][pos1].value() == x) {
            return true;
        }
        size_t pos2 = h2(x, current_capacity);
         if (pos2 < current_capacity && table.size() > 1 && table[1].size() > pos2 && table[1][pos2].has_value() && table[1][pos2].value() == x) {
            return true;
        }
        return false;
    }

    // Non-thread-safe add used by resize. Attempts Cuckoo displacements.
    bool add_cuckoo_unsafe(T item, std::vector<std::vector<std::optional<T>>>& target_table, size_t target_capacity) {
        if (target_capacity == 0) return false;

        T current_item = item;
        size_t current_table_idx = 0;

        for (int count = 0; count < MAX_KICK_LIMIT; ++count) {
            size_t pos;
            if (current_table_idx == 0) {
                pos = h1(current_item, target_capacity);
            } else {
                pos = h2(current_item, target_capacity);
            }

            // Ensure pos is within bounds
            if (pos >= target_capacity) {
                 std::cerr << "Error: Hash function generated out-of-bounds index ("<< pos << " >= " << target_capacity << ") during unsafe add." << std::endl;
                 return false;
            }

            // Check target table bounds as well
            if (target_table.size() <= current_table_idx || target_table[current_table_idx].size() <= pos) {
                std::cerr << "Error: Target table index out of bounds during unsafe add." << std::endl;
                return false;
            }


            if (!target_table[current_table_idx][pos].has_value()) {
                target_table[current_table_idx][pos] = current_item;
                return true;
            }

            T victim = target_table[current_table_idx][pos].value();
            target_table[current_table_idx][pos] = current_item;
            current_item = victim;
            current_table_idx = 1 - current_table_idx;
        }
        // Cuckoo kick limit reached during resize rehash
        // std::cerr << "Warning: Cuckoo kick limit reached during unsafe add/resize." << std::endl;
        return false;
    }

    // --- Resize ---
    void resize() {
        size_t old_capacity = capacity.load();

        std::vector<std::unique_lock<std::mutex>> acquired_locks;
        acquired_locks.reserve(NUM_LOCKS);
        for (size_t i = 0; i < NUM_LOCKS; ++i) {
            acquired_locks.emplace_back(locks[i]);
        }

        if (capacity.load() != old_capacity) {
            return; // Someone else resized
        }

        // Prevent excessive resizing
        if (old_capacity > (1 << 29)) {
             std::cerr << "Error: Maximum capacity reached or resize failed repeatedly. Aborting resize." << std::endl;
             return;
        }


        size_t new_capacity = (old_capacity == 0) ? 16 : old_capacity * 2;
        //std::cout << "[Resize] Resizing from " << old_capacity << " to " << new_capacity << std::endl;


        std::vector<std::vector<std::optional<T>>> new_table(NUM_TABLES);
         try {
             new_table[0].resize(new_capacity);
             new_table[1].resize(new_capacity);
         } catch (const std::bad_alloc& e) {
              std::cerr << "Error: Failed to allocate memory for resize to capacity " << new_capacity << ". " << e.what() << std::endl;
              return; // Abort resize
         }


        size_t rehash_count = 0;
        size_t rehash_failures = 0;
        // Iterate through the old table safely
        for (size_t i = 0; i < table.size() && i < NUM_TABLES; ++i) {
            for (size_t j = 0; j < old_capacity && j < table[i].size(); ++j) {
                if (table[i][j].has_value()) {
                    rehash_count++;
                    if (!add_cuckoo_unsafe(table[i][j].value(), new_table, new_capacity)) {
                        rehash_failures++;
                         std::cerr << "Error: Failed to rehash element during resize. Element lost." << std::endl;
                    }
                }
            }
        }


        // Only commit if rehash was fully successful
        if (rehash_failures == 0) {
            table = std::move(new_table);
            capacity.store(new_capacity);
        } else {
             std::cerr << "Error: Resize aborted due to " << rehash_failures << " rehash failures. Table remains unchanged." << std::endl;
             // Do not update table or capacity. State remains as before resize attempt.
        }

        // Locks released by RAII
        // std::cout << "[Resize] Finished. Rehased " << rehash_count << " items. Failures: " << rehash_failures << std::endl;
    }


public:
    // Constructor
    explicit StripedCuckooHashSet(size_t initial_capacity = 16) :
        capacity(initial_capacity == 0 ? 16 : initial_capacity), // Ensure non-zero
        current_size(0),
        locks(NUM_LOCKS)
    {
        size_t cap = capacity.load();
        table.resize(NUM_TABLES);
         try {
            table[0].resize(cap);
            table[1].resize(cap);
         } catch (const std::bad_alloc& e) {
             std::cerr << "Error: Failed to allocate initial table memory: " << e.what() << std::endl;
             capacity.store(0);
             throw;
         }
    }

    // Add an element
    bool add(const T& x) {
        for (int retry = 0; retry < 2; ++retry) { // Allow one retry after resize
            size_t current_cap = capacity.load();
            if (current_cap == 0) {
                std::cerr << "Error: Attempting to add to an invalid (zero capacity) hash set." << std::endl;
                return false;
            }
            auto [lock_idx1, lock_idx2] = get_lock_indices(x, current_cap);

            std::unique_lock<std::mutex> lk1 = acquire_lock1(lock_idx1);
            std::unique_lock<std::mutex> lk2 = acquire_lock2(lock_idx1, lock_idx2);

            if (capacity.load() != current_cap) {
                 continue; // Capacity changed, retry outer loop
            }

            // Check bounds before accessing table elements in contains_unsafe
            if (contains_unsafe(x, current_cap)) {
                return false;
            }

            size_t pos1 = h1(x, current_cap);
             // Check bounds before accessing table element
             if (pos1 < current_cap && table.size() > 0 && table[0].size() > pos1 && !table[0][pos1].has_value()) {
                 table[0][pos1] = x;
                 current_size++;
                 return true;
            }

            size_t pos2 = h2(x, current_cap);
             // Check bounds before accessing table element
             if (pos2 < current_cap && table.size() > 1 && table[1].size() > pos2 && !table[1][pos2].has_value()) {
                table[1][pos2] = x;
                current_size++;
                return true;
            }

            // Both slots full. Release locks and potentially resize.
            lk1.unlock();
            if(lk2.owns_lock()) lk2.unlock();

            if (retry == 0) {
                resize(); // Trigger resize on first failure
                // Loop continues to retry the add
            } else {
                // Failed again even after resize attempt
                 // std::cerr << "Warning: Add failed for item even after resize attempt." << std::endl;
                 return false;
            }
        }
        // Should not be reached if resize logic is sound
        return false;
    }


    // Remove an element
    bool remove(const T& x) {
        size_t current_cap = capacity.load();
         if (current_cap == 0) return false;
         auto [lock_idx1, lock_idx2] = get_lock_indices(x, current_cap);

        std::unique_lock<std::mutex> lk1 = acquire_lock1(lock_idx1);
        std::unique_lock<std::mutex> lk2 = acquire_lock2(lock_idx1, lock_idx2);

         if (capacity.load() != current_cap) {
             return false; // Resized concurrently
         }

        size_t pos1 = h1(x, current_cap);
         // Check bounds before access
         if (pos1 < current_cap && table.size() > 0 && table[0].size() > pos1 && table[0][pos1].has_value() && table[0][pos1].value() == x) {
            table[0][pos1].reset();
            current_size--;
            return true;
        }

        size_t pos2 = h2(x, current_cap);
         // Check bounds before access
         if (pos2 < current_cap && table.size() > 1 && table[1].size() > pos2 && table[1][pos2].has_value() && table[1][pos2].value() == x) {
            table[1][pos2].reset();
            current_size--;
            return true;
        }

        return false; // Not found
    }

    // Check if an element exists
    bool contains(const T& x) {
        size_t current_cap = capacity.load();
         if (current_cap == 0) return false;
        auto [lock_idx1, lock_idx2] = get_lock_indices(x, current_cap);

        std::unique_lock<std::mutex> lk1 = acquire_lock1(lock_idx1);
        std::unique_lock<std::mutex> lk2 = acquire_lock2(lock_idx1, lock_idx2);

         if (capacity.load() != current_cap) {
             return false; // Resized concurrently
         }

        // Use the contains_unsafe which now includes boundary checks
        return contains_unsafe(x, current_cap);
    }

    // --- Non-Thread-Safe Methods ---
    size_t size() const {
        return current_size.load(std::memory_order_relaxed);
    }

    size_t get_capacity() const {
        return capacity.load(std::memory_order_relaxed);
    }

    void populate(size_t num_elements_to_add) {
        if (capacity.load() == 0) {
             std::cerr << "Error: Cannot populate an invalid (zero capacity) hash set." << std::endl;
             return;
        }
        // std::cout << "Populating with " << num_elements_to_add << " elements..." << std::endl;
        size_t added_count = 0;
        std::uniform_int_distribution<int> dist(0, std::numeric_limits<int>::max());
        // Give more attempts to account for duplicates, especially near 50% load
        size_t attempt_limit = num_elements_to_add * 4; // Increase attempts
        size_t attempts = 0;

        while (added_count < num_elements_to_add && attempts < attempt_limit) {
             attempts++;
             T val = dist(gen);

             if (add(val)) { // Use the thread-safe add
                 added_count++;
             }
        }
        // std::cout << "Population added " << added_count << " elements after " << attempts << " attempts." << std::endl;
        if (added_count < num_elements_to_add) {
            std::cerr << "Warning: Population phase could only add " << added_count << " out of " << num_elements_to_add << " requested elements. Target size might not be reached." << std::endl;
        }
    }
};


// --- Test Harness ---
std::atomic<long long> successful_adds = 0;
std::atomic<long long> successful_removes = 0;

void worker_thread(StripedCuckooHashSet<int>& hash_set, int num_ops_for_this_thread) {
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<int> val_dist(0, std::numeric_limits<int>::max());

    for (int i = 0; i < num_ops_for_this_thread; ++i) {
        int operation_type;
        int value;

        {
            std::lock_guard<std::mutex> lock(gen_mutex); // Protect shared generator
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

    int total_operations = 0;
    int num_threads = 0;
    try {
        total_operations = std::stoi(argv[1]);
        num_threads = std::stoi(argv[2]);
    } catch (const std::exception& e) {
         std::cerr << "Error parsing arguments: " << e.what() << std::endl;
         return 1;
    }


    if (total_operations < 0 || num_threads <= 0) {
        std::cerr << "Operations must be non-negative, threads must be positive." << std::endl;
        return 1;
    }

    // --- Setup ---
    // *** UPDATED VALUES ***
    size_t initial_capacity = 1000000; // 1 Million
    size_t populate_count = 500000;   // 500 Thousand

    StripedCuckooHashSet<int> hash_set(initial_capacity);

    // --- Populate ---
    hash_set.populate(populate_count);


    size_t initial_size = hash_set.size(); // Get size AFTER population
    size_t initial_cap_after_populate = hash_set.get_capacity();

    // --- Concurrent Operations ---
    std::vector<std::thread> threads;
    int ops_per_thread = (num_threads == 0) ? 0 : total_operations / num_threads;
    int remaining_ops = (num_threads == 0) ? 0 : total_operations % num_threads;

     std::cout << "– Running " << total_operations << " Operations w/ " << num_threads << " Threads –" << std::endl;

    successful_adds = 0; // Reset counters before run
    successful_removes = 0;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i) {
        int thread_ops = ops_per_thread + (i < remaining_ops ? 1 : 0);
        if (thread_ops > 0) {
             threads.emplace_back(worker_thread, std::ref(hash_set), thread_ops);
        }
    }

    for (auto& t : threads) {
        if (t.joinable()) {
           t.join();
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    long long total_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    // --- Verification and Output ---
    size_t final_size = hash_set.size();
    size_t final_capacity = hash_set.get_capacity();
    long long current_adds = successful_adds.load();
    long long current_removes = successful_removes.load();
    long long expected_size = static_cast<long long>(initial_size) + current_adds - current_removes;

    long long avg_time_us = (total_operations == 0) ? 0 : total_time_us / total_operations;

    // --- Print Results in Required Format ---
    std::cout << "Total time: " << total_time_us << std::endl;
    std::cout << "Average time per operation: " << avg_time_us << std::endl;
    std::cout << "Hashset initial size: " << initial_size << std::endl;
    std::cout << "Hashset initial capacity: " << initial_cap_after_populate << std::endl;
    std::cout << "Successful Adds: " << current_adds << std::endl;
    std::cout << "Successful Removes: " << current_removes << std::endl;
    std::cout << "Expected size: " << expected_size << std::endl;
    std::cout << "Final hashset size: " << final_size << std::endl;
    std::cout << "Final hashset capacity: " << final_capacity << std::endl;

    // Final check
    if (static_cast<long long>(final_size) != expected_size) {
        std::cerr << "[Error] Mismatch between final size (" << final_size
                  << ") and expected size (" << expected_size << ")!" << std::endl;
         // return 1; // Optional: return error code on mismatch
    } else {
         //std::cout << "[Success] Final size matches expected size." << std::endl;
    }

    return 0;
}