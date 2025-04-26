#include <iostream>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <climits>  // For INT_MAX

// Thread-safe implementation of open-addressed hash set with fixed number of locks
template<typename T>
class PhasedCuckooHashSet {
private:
    struct Probe {  // From Figure 13.24
        T x;
        int h0, h1;
        
        Probe(T x, int h0, int h1) : x(x), h0(h0), h1(h1) {}
    };

    std::atomic<T>* table;
    std::vector<std::recursive_mutex> locks;
    int capacity;
    int numLocks;
    std::hash<T> hashFunction;
    const T EMPTY = 0;  // Assuming T=int for simplicity

    // Hash functions
    int hash0(T x) const {
        return hashFunction(x) % capacity;
    }

    int hash1(T x) const {
        return (hashFunction(x) / capacity) % capacity;
    }

    void resize() {
        int oldCapacity = capacity;
        std::atomic<T>* oldTable = new std::atomic<T>[capacity];
        
        // Copy existing elements to temp storage
        for (int i = 0; i < capacity; i++) {
            oldTable[i].store(table[i].load());
        }
        
        // Save old table pointer
        auto oldTablePtr = table;
        
        // Double the capacity
        capacity *= 2;
        table = new std::atomic<T>[capacity];
        
        // Initialize new table
        for (int i = 0; i < capacity; i++) {
            table[i].store(EMPTY);
        }
        
        // Rehash all elements
        for (int i = 0; i < oldCapacity; i++) {
            T val = oldTable[i].load();
            if (val != EMPTY) {
                add(val);
            }
        }
        
        // Clean up old table
        delete[] oldTable;
    }

    void acquire(T x) {  // From Figure 13.32
        locks[x % numLocks].lock();
    }

    void release(T x) {  // From Figure 13.33
        locks[x % numLocks].unlock();
    }

    bool relocate(int i, int hi) {  // From Figure 13.27
        int hj = 0;
        int j = 1 - i;
        const int LIMIT = 32;  // Relocation limit
        
        for (int round = 0; round < LIMIT; round++) {
            T y = table[hi].load();
            
            if (y == EMPTY) {
                return false;
            }
            
            switch (i) {
                case 0: hj = hash1(y) % capacity; break;
                case 1: hj = hash0(y) % capacity; break;
            }
            
            // Lock and relocate
            acquire(y);
            
            T temp = table[hj].load();
            if (temp == EMPTY) {
                table[hj].store(y);
                table[hi].store(EMPTY);
                release(y);
                return true;
            }
            
            if (temp == y) {
                release(y);
                return false;
            }
            
            table[hj].store(y);
            table[hi].store(temp);
            release(y);
            
            hi = hj;
            i = 1 - i;
        }
        
        return false;
    }

public:
    PhasedCuckooHashSet(int initialCapacity, int numLocks = 32) 
        : capacity(initialCapacity), numLocks(numLocks), locks(numLocks) {
        table = new std::atomic<T>[capacity];
        for (int i = 0; i < capacity; i++) {
            table[i].store(EMPTY);
        }
    }
    
    ~PhasedCuckooHashSet() {
        delete[] table;
    }

    bool add(T x) {  // From Figure 13.26
        if (contains(x)) {
            return false;
        }

        int h0 = hash0(x) % capacity;
        int h1 = hash1(x) % capacity;
        bool mustResize = false;

        try {
            if (x != EMPTY) {
                acquire(x);
                
                if (table[h0].load() == EMPTY) {
                    table[h0].store(x);
                    release(x);
                    return true;
                } else if (table[h1].load() == EMPTY) {
                    table[h1].store(x);
                    release(x);
                    return true;
                } else if (table[h0].load() == x || table[h1].load() == x) {
                    release(x);
                    return false;
                } else {
                    mustResize = true;
                }
                
                if (mustResize) {
                    release(x);
                    resize();
                    add(x);
                    return true;
                } else if (!relocate(0, h0)) {
                    release(x);
                    resize();
                    add(x);
                    return true;
                }
            }
        } catch (...) {
            release(x);
            throw;
        }
        
        release(x);
        return true;
    }

    bool remove(T x) {  // From Figure 13.25
        if (x == EMPTY) {
            return false;
        }

        acquire(x);
        
        try {
            int h0 = hash0(x) % capacity;
            int h1 = hash1(x) % capacity;
            
            if (table[h0].load() == x) {
                table[h0].store(EMPTY);
                release(x);
                return true;
            } else if (table[h1].load() == x) {
                table[h1].store(EMPTY);
                release(x);
                return true;
            }
        } catch (...) {
            release(x);
            throw;
        }
        
        release(x);
        return false;
    }

    bool contains(T x) {  // From Figure 13.32
        if (x == EMPTY) {
            return false;
        }

        int h0 = hash0(x) % capacity;
        int h1 = hash1(x) % capacity;
        
        // No locking needed for contains
        return (table[h0].load() == x || table[h1].load() == x);
    }

    int size() const {  // Non-thread safe
        int count = 0;
        for (int i = 0; i < capacity; i++) {
            if (table[i].load() != EMPTY) {
                count++;
            }
        }
        return count;
    }

    void populate(int count) {  // Non-thread safe
        std::mt19937 gen(714);  // Fixed seed as required
        std::uniform_int_distribution<> dis(1, INT_MAX);
        
        for (int i = 0; i < count; i++) {
            int val = dis(gen);
            add(val);
        }
    }

    int getCapacity() const {
        return capacity;
    }
};

// Main function following the test script requirements
int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <operations> <threads>" << std::endl;
        return 1;
    }

    int numOperations = std::atoi(argv[1]);
    int numThreads = std::atoi(argv[2]);
    
    // Initialize hash set with capacity 10 million as required
    PhasedCuckooHashSet<int> hashSet(10000000);
    
    // Populate with 5 million elements as required
    int initialPopulation = 5000000;
    hashSet.populate(initialPopulation);
    
    int initialSize = hashSet.size();
    int initialCapacity = hashSet.getCapacity();
    
    std::vector<std::thread> threads;
    std::atomic<int> successfulAdds(0);
    std::atomic<int> successfulRemoves(0);
    
    // Start timing
    auto start = std::chrono::high_resolution_clock::now();
    
    // Launch threads
    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back([&, i]() {
            std::mt19937 gen(714 + i);  // Fixed seed with thread offset
            std::uniform_int_distribution<> valueDis(1, INT_MAX);
            std::uniform_int_distribution<> opDis(1, 100);
            
            int localAdds = 0;
            int localRemoves = 0;
            
            for (int j = 0; j < numOperations / numThreads; j++) {
                int operation = opDis(gen);
                int value = valueDis(gen);
                
                if (operation <= 80) {  // 80% contains
                    hashSet.contains(value);
                } else if (operation <= 90) {  // 10% insert
                    if (hashSet.add(value)) {
                        localAdds++;
                    }
                } else {  // 10% remove
                    if (hashSet.remove(value)) {
                        localRemoves++;
                    }
                }
            }
            
            successfulAdds.fetch_add(localAdds);
            successfulRemoves.fetch_add(localRemoves);
        });
    }
    
    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }
    
    // End timing
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    // Calculate results
    int finalSize = hashSet.size();
    int finalCapacity = hashSet.getCapacity();
    int expectedSize = initialSize + successfulAdds.load() - successfulRemoves.load();
    
    // Print results in the required format
    std::cout << "Total time: " << duration.count() << std::endl;
    std::cout << "Average time per operation: " << duration.count() / numOperations << std::endl;
    std::cout << "Hashset initial size: " << initialSize << std::endl;
    std::cout << "Hashset initial capacity: " << initialCapacity << std::endl;
    std::cout << "Expected size: " << expectedSize << std::endl;
    std::cout << "Final hashset size: " << finalSize << std::endl;
    std::cout << "Final hashset capacity: " << finalCapacity << std::endl;
    
    return 0;
}