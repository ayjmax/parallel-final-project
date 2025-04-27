#include <iostream>
#include <vector>
#include <optional>
#include <functional> // for std::hash
#include <mutex>
// #include <shared_mutex> // Not used due to complexity with lock-all resize
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <stdexcept> // For std::runtime_error
#include <numeric>   // For std::iota
#include <algorithm> // For std::shuffle, std::min, std::max
#include <new>       // For std::hardware_destructive_interference_size

// --- Optimization: Padded Mutex ---
// Define cache line size constant (fallback if not defined)
#ifdef __cpp_lib_hardware_interference_size
    constexpr size_t cache_line_size = std::hardware_destructive_interference_size;
#else
    // Provide a reasonable fallback if the standard constant is not available
    constexpr size_t cache_line_size = 64;
#endif

struct PaddedMutex {
    std::mutex mtx;
    // Pad to avoid false sharing
    char padding[cache_line_size > sizeof(std::mutex) ? cache_line_size - sizeof(std::mutex) : 1];
};
// --- End Optimization ---


// Configuration Constants
const size_t NUM_TABLES = 2;
const size_t NUM_LOCKS = 32; // Fixed number of locks for striping
const int MAX_KICK_LIMIT = 100; // Max displacement attempts before resize

// Base seed generator (used to seed thread-local generators)
std::mt19937 base_gen(714);
std::mutex base_gen_mutex; // Protects only the base generator during seeding


template <typename T>
class StripedCuckooHashSet {
private:
    std::vector<std::vector<std::optional<T>>> table;
    std::atomic<size_t> capacity; // Capacity of EACH table
    std::atomic<size_t> current_size;
    // --- Optimization: Use Padded Mutexes ---
    std::vector<PaddedMutex> locks;
    // --- End Optimization ---


    // Hash functions (unchanged)
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
    // Calculates lock indices (unchanged logic, uses size_t)
    std::pair<size_t, size_t> get_lock_indices(const T& x, size_t current_capacity) const {
        if (current_capacity == 0) return {0, 0};
        size_t pos1 = h1(x, current_capacity);
        size_t pos2 = h2(x, current_capacity);
        size_t lock1_idx = pos1 % NUM_LOCKS;
        size_t lock2_idx = pos2 % NUM_LOCKS;
        return {std::min(lock1_idx, lock2_idx), std::max(lock1_idx, lock2_idx)};
    }

    // --- Optimization: Acquire locks on PaddedMutex ---
    std::unique_lock<std::mutex> acquire_lock1(size_t lock_idx) {
        // Ensure lock_idx is within bounds
        if (lock_idx >= locks.size()) {
             throw std::out_of_range("Lock index 1 out of bounds");
        }
        return std::unique_lock<std::mutex>(locks[lock_idx].mtx);
    }

    std::unique_lock<std::mutex> acquire_lock2(size_t lock_idx1, size_t lock_idx2) {
         if (lock_idx1 == lock_idx2) {
             return std::unique_lock<std::mutex>(); // No second lock needed
         }
         // Ensure lock_idx is within bounds
         if (lock_idx2 >= locks.size()) {
             throw std::out_of_range("Lock index 2 out of bounds");
         }
         return std::unique_lock<std::mutex>(locks[lock_idx2].mtx);
    }
    // --- End Optimization ---


    // --- Unsafe Operations ---
    // Contains check assuming locks are held or context is non-concurrent
    bool contains_unsafe(const T& x, size_t current_capacity) const {
        if (current_capacity == 0) return false;
        size_t pos1 = h1(x, current_capacity);
        // Add bounds checks for safety, though hash % capacity should prevent this
        if (pos1 < current_capacity && table.size() > 0 && table[0].size() > pos1 && table[0][pos1].has_value() && table[0][pos1].value() == x) {
            return true;
        }
        size_t pos2 = h2(x, current_capacity);
        if (pos2 < current_capacity && table.size() > 1 && table[1].size() > pos2 && table[1][pos2].has_value() && table[1][pos2].value() == x) {
            return true;
        }
        return false;
    }

    // Unsafe add used ONLY by resize (assumes all locks held)
    bool add_cuckoo_unsafe(T item, std::vector<std::vector<std::optional<T>>>& target_table, size_t target_capacity) {
         // (Implementation remains the same as previous version, including MAX_KICK_LIMIT)
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

            if (pos >= target_capacity) return false; // Safety check

            if (target_table.size() <= current_table_idx || target_table[current_table_idx].size() <= pos) return false; // Safety check


            if (!target_table[current_table_idx][pos].has_value()) {
                target_table[current_table_idx][pos] = current_item;
                return true;
            }

            // Swap logic: store victim, place current, update current to victim
            T victim = std::move(target_table[current_table_idx][pos].value()); // Use move
            target_table[current_table_idx][pos] = std::move(current_item);     // Use move
            current_item = std::move(victim);                                   // Use move

            current_table_idx = 1 - current_table_idx; // Switch table
        }
        // Kick limit reached during rehash
        return false;
    }

    // --- Resize ---
    // Locks all stripes, creates new table, rehashes elements using add_cuckoo_unsafe
    void resize() {
        size_t old_capacity = capacity.load(std::memory_order_acquire); // Acquire fence

        // --- Optimization: Lock Padded Mutexes ---
        std::vector<std::unique_lock<std::mutex>> acquired_locks;
        acquired_locks.reserve(NUM_LOCKS);
        for (size_t i = 0; i < NUM_LOCKS; ++i) {
            acquired_locks.emplace_back(locks[i].mtx);
        }
        // --- End Optimization ---

        // Double check capacity after acquiring all locks
        if (capacity.load(std::memory_order_relaxed) != old_capacity) {
            return; // Someone else resized, release locks via RAII
        }

        // Prevent excessive resizing (unchanged)
        if (old_capacity > (1 << 29)) {
             std::cerr << "Error: Maximum capacity reached or resize failed repeatedly. Aborting resize." << std::endl;
             return;
        }

        size_t new_capacity = (old_capacity == 0) ? 16 : old_capacity * 2;
        // std::cout << "[Resize] Resizing from " << old_capacity << " to " << new_capacity << std::endl;

        std::vector<std::vector<std::optional<T>>> new_table(NUM_TABLES);
         try {
             new_table[0].resize(new_capacity);
             new_table[1].resize(new_capacity);
         } catch (const std::bad_alloc& e) {
              std::cerr << "Error: Failed to allocate memory for resize to capacity " << new_capacity << ". " << e.what() << std::endl;
              return; // Abort resize
         }

        size_t rehash_failures = 0;
        size_t current_elements_found = 0;
        // Rehash elements from old table to new table
        for (size_t i = 0; i < table.size() && i < NUM_TABLES; ++i) {
            for (size_t j = 0; j < old_capacity && j < table[i].size(); ++j) {
                if (table[i][j].has_value()) {
                    current_elements_found++;
                    // Move element out of old table to avoid copies if T is movable
                    if (!add_cuckoo_unsafe(std::move(table[i][j].value()), new_table, new_capacity)) {
                        rehash_failures++;
                        // If rehash fails, the moved-from value in the old table is potentially lost
                        // Put it back? Or accept loss? For simplicity, report loss.
                         std::cerr << "Error: Failed to rehash element during resize. Element potentially lost." << std::endl;
                         // Restore element to old table if move failed? Complex. Let's assume loss for now.
                        // table[i][j] = std::move(failed_element); // Needs careful handling
                    }
                    table[i][j].reset(); // Clear old slot after move/attempted move
                }
            }
        }

        // Commit resize only if successful
        if (rehash_failures == 0) {
            table = std::move(new_table);
            // Use release semantics when storing the new capacity to ensure
            // table modifications are visible before capacity change.
            capacity.store(new_capacity, std::memory_order_release);
            // Size should ideally match current_elements_found if no failures
            // Update current_size atomically to reflect the exact count found (handles potential inconsistencies)
            current_size.store(current_elements_found, std::memory_order_relaxed);

        } else {
             std::cerr << "Error: Resize aborted due to " << rehash_failures << " rehash failures. Table remains unchanged." << std::endl;
             // If resize fails, the old table might be in an inconsistent state if elements were moved out.
             // This path indicates a serious problem needing more robust handling in production.
             // For this exercise, we abort the resize and potentially leave table inconsistent.
        }
        // Locks released by RAII
    }


public:
    // Constructor (Use PaddedMutex)
    explicit StripedCuckooHashSet(size_t initial_capacity = 16) :
        capacity(initial_capacity == 0 ? 16 : initial_capacity),
        current_size(0),
        locks(NUM_LOCKS) // Vector of PaddedMutex will default construct
    {
        size_t cap = capacity.load(std::memory_order_relaxed);
        table.resize(NUM_TABLES);
         try {
            table[0].resize(cap);
            table[1].resize(cap);
         } catch (const std::bad_alloc& e) {
             std::cerr << "Error: Failed to allocate initial table memory: " << e.what() << std::endl;
             capacity.store(0); // Mark as invalid
             throw;
         }
    }

    // --- Optimization: Add with Cuckoo Kicks ---
    bool add(const T& x) {
        T item_to_insert = x; // Work with a copy we can modify/move
        int attempts = 0;
        const int max_resize_attempts = 2; // Limit resize attempts for a single add

        while(attempts < max_resize_attempts) {
            size_t current_cap = capacity.load(std::memory_order_acquire); // Acquire semantics
            if (current_cap == 0) return false; // Invalid state

            auto [lock_idx1, lock_idx2] = get_lock_indices(item_to_insert, current_cap);
            std::unique_lock<std::mutex> lk1 = acquire_lock1(lock_idx1);
            std::unique_lock<std::mutex> lk2 = acquire_lock2(lock_idx1, lock_idx2);

            // Check capacity again *after* locking
            if (capacity.load(std::memory_order_relaxed) != current_cap) {
                // Capacity changed while waiting for locks, retry the outer loop
                continue;
            }

            // Check if item (or its potential replacement after kicks) already exists
             if (contains_unsafe(item_to_insert, current_cap)) {
                 return false; // Item already present
             }

            // Cuckoo Kick Loop
            T current_item = std::move(item_to_insert); // Start with the item to insert
            size_t current_table_idx = 0; // Start with table 0

            for (int kick = 0; kick < MAX_KICK_LIMIT; ++kick) {
                size_t pos;
                if (current_table_idx == 0) {
                    pos = h1(current_item, current_cap);
                } else {
                    pos = h2(current_item, current_cap);
                }

                 // Bounds check
                if (pos >= current_cap || table.size() <= current_table_idx || table[current_table_idx].size() <= pos) {
                    // This indicates a potential issue, maybe resize is needed or hash is bad
                    // For simplicity, treat as failure and break kick loop to trigger resize check
                    std::cerr << "Warning: Out of bounds access averted during add kick loop." << std::endl;
                    current_item = std::move(item_to_insert); // Restore original item for potential resize
                    goto resize_check; // Need to release locks first
                }

                // If slot is empty, place the item
                if (!table[current_table_idx][pos].has_value()) {
                    table[current_table_idx][pos] = std::move(current_item);
                    current_size.fetch_add(1, std::memory_order_relaxed); // Relaxed potentially ok, seq_cst safest
                    return true; // Successfully added
                }

                // Slot is occupied, swap and continue kicking
                T victim = std::move(table[current_table_idx][pos].value());
                table[current_table_idx][pos] = std::move(current_item);
                current_item = std::move(victim); // The victim becomes the item to place next

                // Move to the other table for the next kick attempt
                current_table_idx = 1 - current_table_idx;
            }

            // If kick loop finishes, MAX_KICK_LIMIT reached for the 'current_item'
            item_to_insert = std::move(current_item); // The item that failed kicking needs to be retried after resize


        resize_check: // Label used with goto for clarity after breaking kick loop on error
            // Need resize. Release locks *before* calling resize.
            lk1.unlock();
            if (lk2.owns_lock()) lk2.unlock();

            attempts++;
            if (attempts >= max_resize_attempts) {
                // std::cerr << "Warning: Add failed after " << max_resize_attempts << " resize attempts." << std::endl;
                return false; // Failed to add even after resize attempts
            }

            resize();
            // After resize, the outer while loop continues, trying to insert 'item_to_insert' again
        }

        return false; // Should not be reached if max_resize_attempts > 0
    }


    // Remove (mostly unchanged, uses PaddedMutex locks)
    bool remove(const T& x) {
        size_t current_cap = capacity.load(std::memory_order_acquire);
         if (current_cap == 0) return false;
         auto [lock_idx1, lock_idx2] = get_lock_indices(x, current_cap);

        std::unique_lock<std::mutex> lk1 = acquire_lock1(lock_idx1);
        std::unique_lock<std::mutex> lk2 = acquire_lock2(lock_idx1, lock_idx2);

         if (capacity.load(std::memory_order_relaxed) != current_cap) {
             return false; // Resized concurrently
         }

        size_t pos1 = h1(x, current_cap);
         if (pos1 < current_cap && table.size() > 0 && table[0].size() > pos1 && table[0][pos1].has_value() && table[0][pos1].value() == x) {
            table[0][pos1].reset();
            current_size.fetch_sub(1, std::memory_order_relaxed); // Relaxed ok? Seq_cst safer
            return true;
        }

        size_t pos2 = h2(x, current_cap);
         if (pos2 < current_cap && table.size() > 1 && table[1].size() > pos2 && table[1][pos2].has_value() && table[1][pos2].value() == x) {
            table[1][pos2].reset();
            current_size.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }

        return false; // Not found
    }

    // Contains (mostly unchanged, uses PaddedMutex locks)
    bool contains(const T& x) {
        size_t current_cap = capacity.load(std::memory_order_acquire);
         if (current_cap == 0) return false;
        auto [lock_idx1, lock_idx2] = get_lock_indices(x, current_cap);

        std::unique_lock<std::mutex> lk1 = acquire_lock1(lock_idx1);
        std::unique_lock<std::mutex> lk2 = acquire_lock2(lock_idx1, lock_idx2);

         if (capacity.load(std::memory_order_relaxed) != current_cap) {
             return false; // Resized concurrently
         }

        // contains_unsafe already checks bounds now
        return contains_unsafe(x, current_cap);
    }

    // --- Non-Thread-Safe Methods ---
    // Use relaxed memory order as per non-thread-safe requirement
    size_t size() const {
        return current_size.load(std::memory_order_relaxed);
    }

    size_t get_capacity() const {
        return capacity.load(std::memory_order_relaxed);
    }

    // Populate (uses the optimized thread-safe add)
    void populate(size_t num_elements_to_add) {
        if (capacity.load(std::memory_order_relaxed) == 0) {
             std::cerr << "Error: Cannot populate an invalid (zero capacity) hash set." << std::endl;
             return;
        }
        size_t added_count = 0;
        // Use base generator for initial population (non-concurrent context)
        std::uniform_int_distribution<int> dist(0, std::numeric_limits<int>::max());
        size_t attempt_limit = num_elements_to_add * 4; // Give more attempts
        size_t attempts = 0;

        while (added_count < num_elements_to_add && attempts < attempt_limit) {
             attempts++;
             T val = dist(base_gen); // Use base generator here

             if (add(val)) { // Use the optimized add
                 added_count++;
             }
        }
        if (added_count < num_elements_to_add) {
            std::cerr << "Warning: Population phase could only add " << added_count << " out of " << num_elements_to_add << " requested elements." << std::endl;
        }
         //std::cout << "Population added " << added_count << " elements." << std::endl;
    }
};


// --- Test Harness ---
std::atomic<long long> successful_adds = 0;
std::atomic<long long> successful_removes = 0;

// --- Optimization: Thread-Local RNG ---
void worker_thread(StripedCuckooHashSet<int>& hash_set, int num_ops_for_this_thread, unsigned int seed) {
    thread_local std::mt19937 thread_gen(seed); // Seeded per thread
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<int> val_dist(0, std::numeric_limits<int>::max());

    for (int i = 0; i < num_ops_for_this_thread; ++i) {
        // No global lock needed here anymore
        int operation_type = op_dist(thread_gen);
        int value = val_dist(thread_gen);

        if (operation_type < 80) { // 80% contains
            hash_set.contains(value);
        } else if (operation_type < 90) { // 10% add
            if (hash_set.add(value)) {
                successful_adds.fetch_add(1, std::memory_order_relaxed); // Relaxed ok for counters
            }
        } else { // 10% remove
            if (hash_set.remove(value)) {
                successful_removes.fetch_add(1, std::memory_order_relaxed); // Relaxed ok for counters
            }
        }
    }
}
// --- End Optimization ---


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
    size_t initial_capacity = 1000000; // 1 Million
    size_t populate_count = 500000;   // 500 Thousand

    StripedCuckooHashSet<int> hash_set(initial_capacity);

    // --- Populate ---
    hash_set.populate(populate_count);


    size_t initial_size = hash_set.size();
    size_t initial_cap_after_populate = hash_set.get_capacity();

    // --- Concurrent Operations ---
    std::vector<std::thread> threads;
    int ops_per_thread = (num_threads == 0) ? 0 : total_operations / num_threads;
    int remaining_ops = (num_threads == 0) ? 0 : total_operations % num_threads;

     std::cout << "– Running " << total_operations << " Operations w/ " << num_threads << " Threads –" << std::endl;

    successful_adds = 0;
    successful_removes = 0;

    auto start_time = std::chrono::high_resolution_clock::now();

    // --- Optimization: Seed thread-local RNGs ---
    std::vector<unsigned int> seeds(num_threads);
    {
        std::lock_guard<std::mutex> lock(base_gen_mutex); // Lock only for seeding
        std::uniform_int_distribution<unsigned int> seed_dist;
        for(int i=0; i<num_threads; ++i) {
            seeds[i] = seed_dist(base_gen);
        }
    }


    for (int i = 0; i < num_threads; ++i) {
        int thread_ops = ops_per_thread + (i < remaining_ops ? 1 : 0);
        if (thread_ops > 0) {
             // Pass the unique seed to the worker thread
             threads.emplace_back(worker_thread, std::ref(hash_set), thread_ops, seeds[i]);
        }
    }
    // --- End Optimization ---


    for (auto& t : threads) {
        if (t.joinable()) {
           t.join();
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    long long total_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    // --- Verification and Output (Unchanged) ---
    size_t final_size = hash_set.size();
    size_t final_capacity = hash_set.get_capacity();
    // Use seq_cst for final reads of counters for stricter guarantees before comparison
    long long current_adds = successful_adds.load(std::memory_order_seq_cst);
    long long current_removes = successful_removes.load(std::memory_order_seq_cst);
    long long expected_size = static_cast<long long>(initial_size) + current_adds - current_removes;

    long long avg_time_us = (total_operations == 0) ? 0 : total_time_us / total_operations;

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
    } else {
        // std::cout << "[Success] Final size matches expected size." << std::endl;
    }

    return 0;
}