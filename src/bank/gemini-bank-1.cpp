#include <iostream>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <random>
#include <chrono>
#include <numeric> // For std::accumulate
#include <future>  // For std::future and std::packaged_task
#include <stdexcept> // For argument error handling
#include <cstdlib>   // For std::atoi, std::atof (though not needed here)
#include <iomanip> // For std::fixed, std::setprecision
#include <string> // For std::stoi

// --- Global Shared Data ---
// Step 1: Define the map of bank accounts <int ID, float Balance>
std::map<int, float> accountMap;
// Mutex to protect access to the accountMap for atomic operations
std::mutex mapMutex;

// --- Random Number Generation ---
// Use thread_local for random number generators to avoid contention and ensure diversity
thread_local std::mt19937 rng; // Mersenne Twister engine

// Function to seed the thread-local RNG uniquely
void seed_rng(int threadId) {
    // Seed with a combination of time and thread ID for better randomness across threads
    unsigned int seed = static_cast<unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^ (threadId << 16);
    rng.seed(seed);
}

// --- Helper Function ---
// Initializes the map with a specified number of accounts and total balance.
void initializeAccounts(int numAccounts, float totalBalance = 100000.0f) {
    // Ensure the map is empty before population
    accountMap.clear();
    if (numAccounts <= 0) {
        // Handle the case of zero or negative accounts gracefully
         accountMap.clear(); // Ensure it's cleared
         if (totalBalance != 0.0f) {
              std::cerr << "Warning: Cannot distribute non-zero balance (" << totalBalance
                        << ") among zero accounts." << std::endl;
         }
        return; // Nothing to populate
    }

    // Step 2: Populate the map ensuring the total sum is totalBalance
    float balancePerAccount = totalBalance / static_cast<float>(numAccounts);
    for (int i = 0; i < numAccounts; ++i) {
        // Use insert for clarity as requested, although operator[] would also work
        accountMap.insert({i, balancePerAccount});
    }

    // Verification (optional, good for debugging initial state)
    float current_total = 0.0f;
    for(const auto& pair : accountMap) {
        current_total += pair.second;
    }
     // Use a small epsilon for floating point comparison
     if (std::abs(current_total - totalBalance) > 1e-3 * std::abs(totalBalance)) {
          std::cerr << "[WARN] Initial balance verification failed! Sum: " << current_total
                    << ", Expected: " << totalBalance << std::endl;
     } else {
        // std::cout << "[DEBUG] Initial balance verified: " << current_total << std::endl;
     }
}


// --- Core Functions ---

// Step 3: Define the 'deposit' function (atomic transfer)
// Selects two random accounts and transfers a random amount between them.
void deposit(int numAccounts) {
    // Ensure there are at least two accounts to perform a transfer
    if (numAccounts < 2) return;

    // Use thread-local rng seeded previously
    std::uniform_int_distribution<int> account_dist(0, numAccounts - 1);
    std::uniform_real_distribution<float> amount_dist(1.0f, 100.0f); // Example random amount range

    int id1 = account_dist(rng);
    int id2 = account_dist(rng);
    // Ensure the two selected accounts are distinct
    while (id1 == id2) {
        id2 = account_dist(rng);
    }

    float amount = amount_dist(rng);

    // --- Critical Section Start ---
    // Lock the mutex using std::lock_guard for exception safety.
    // This ensures the read-modify-write operations on both accounts are atomic
    // relative to other deposit or balance operations.
    std::lock_guard<std::mutex> lock(mapMutex);

    // Check if account IDs are valid (they should be if generated correctly)
    // map::count is safer than operator[] if ID might not exist, though here it should.
    if (accountMap.count(id1) && accountMap.count(id2)) {
        // Perform the transfer as specified: B1 -= V, B2 += V
        // No check for sufficient funds is specified, so we allow potential negative balances
        // as per the problem description's implicit requirement.
        accountMap[id1] -= amount;
        accountMap[id2] += amount;
    } else {
        // This state should ideally not be reached if numAccounts is correct
        std::cerr << "[ERROR] Invalid account ID (" << id1 << " or " << id2
                  << ") encountered during deposit! NumAccounts: " << numAccounts << std::endl;
    }
    // --- Critical Section End ---
    // Mutex is automatically released when 'lock' goes out of scope.
}

// Step 4: Define the 'balance' function (atomic sum)
// Calculates the sum of all account balances atomically.
float balance() {
    float total_balance = 0.0f;

    // --- Critical Section Start ---
    // Lock the mutex to ensure no deposit operations interleave during summation,
    // guaranteeing a consistent snapshot of the total balance.
    std::lock_guard<std::mutex> lock(mapMutex);

    // Iterate through the map and sum the balances
    // std::accumulate is a concise way to do this
    total_balance = std::accumulate(accountMap.begin(), accountMap.end(), 0.0f,
                                    [](float sum, const auto& pair) {
                                        return sum + pair.second;
                                    });

    // --- Critical Section End ---
    // Mutex is automatically released when 'lock' goes out of scope.

    // Verification as requested: ensure balance always returns 100000 (within float precision)
    if (std::abs(total_balance - 100000.0f) > 1e-2) { // Use a slightly larger epsilon due to repeated operations
         std::cerr << "[ALERT] Balance check returned: " << std::fixed << std::setprecision(4)
                   << total_balance << " (Deviation detected!)" << std::endl;
    }


    return total_balance;
}

// Step 5: Define the 'do_work' function executed by each thread
// Returns the execution time in milliseconds.
long long do_work(int iterations, int numAccounts, int threadId) {
    // Seed this thread's random number generator
    seed_rng(threadId);

    // Define the probability distribution for actions
    std::uniform_real_distribution<float> prob_dist(0.0f, 1.0f);
    const float deposit_probability = 0.95f;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        if (prob_dist(rng) < deposit_probability) {
            // 95% chance to call deposit
            deposit(numAccounts);
        } else {
            // 5% chance to call balance
            float current_balance = balance();
            // Optional: Print intermediate balance checks for debugging
            // std::cout << "Thread " << threadId << " balance check: " << current_balance << std::endl;
             // We already have verification inside balance()
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Return execution time for this thread
    return duration.count();
}

// --- Main Execution Logic ---
int main(int argc, char* argv[]) {
    // Step 6: Parse command-line arguments
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <iterations> <numAccounts> <numThreads>" << std::endl;
        return 1;
    }

    int n; // Number of iterations per thread
    int numAccounts; // Number of bank accounts
    int numThreads; // Number of threads to create

    try {
        n = std::stoi(argv[1]);
        numAccounts = std::stoi(argv[2]);
        numThreads = std::stoi(argv[3]);

        if (n <= 0 || numAccounts < 0 || numThreads <= 0) {
             throw std::invalid_argument("Arguments must be positive integers (numAccounts can be 0).");
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing arguments: " << e.what() << std::endl;
         std::cerr << "Usage: " << argv[0] << " <iterations> <numAccounts> <numThreads>" << std::endl;
        return 1;
    }


    // Step 2 (cont.): Initialize the bank accounts
    const float initialTotalBalance = 100000.0f;
    initializeAccounts(numAccounts, initialTotalBalance);

    // Step 6 (cont.): Create threads and manage futures
    std::vector<std::thread> threads;
    std::vector<std::future<long long>> futures; // To store results (exec_time_i)

    auto overall_start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numThreads; ++i) {
        // Create a packaged_task to wrap the do_work function.
        // This allows getting a future associated with the task's result.
        std::packaged_task<long long(int, int, int)> task(do_work);

        // Get the future from the packaged_task
        futures.push_back(task.get_future());

        // Create and launch the thread, moving the task into it.
        // Pass arguments needed by do_work: iterations, numAccounts, threadId.
        threads.emplace_back(std::move(task), n, numAccounts, i);
    }

    // Step 6 (cont.): Wait for all threads to complete
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto overall_end_time = std::chrono::high_resolution_clock::now();
    auto overall_duration = std::chrono::duration_cast<std::chrono::milliseconds>(overall_end_time - overall_start_time);

    // Step 6 (cont.): Collect execution times from futures
    std::vector<long long> execution_times;
    for (auto& f : futures) {
        try {
            execution_times.push_back(f.get()); // Retrieve the result from each future
        } catch (const std::future_error& e) {
            std::cerr << "Error retrieving future result: " << e.what() << " (" << e.code() << ")" << std::endl;
            execution_times.push_back(-1); // Indicate an error
        }
    }

    // Step 6 (cont.): Perform the final balance check after all threads joined
    float final_balance = balance();

    // --- Output Results ---
    // Print results in the EXACT format required by the script

    // Total time (usually the time until the last thread finishes, which is approximated by overall_duration here)
    // The script seems to extract the *longest* thread time or the overall wall clock time.
    // Let's print the overall wall-clock time.
    std::cout << "Total time: " << overall_duration.count() << std::endl;

    // Final balance (print with fixed point notation for consistency if needed)
    std::cout << "Final balance: " << std::fixed << std::setprecision(0) << final_balance << std::endl;

    // Individual thread execution times
    for (int i = 0; i < numThreads; ++i) {
        if (i < execution_times.size()) {
            std::cout << "Thread " << i << " execution time (ms): " << execution_times[i] << std::endl;
        } else {
             std::cout << "Thread " << i << " execution time (ms): [Error retrieving]" << std::endl;
        }
    }


    // Step 8: Clean up resources
    // The map holds simple types (int, float), so clearing it is sufficient.
    // There are no other manually allocated resources in this example.
    // The prompt asks to use 'remove', but std::map uses 'erase' or 'clear'.
    // We'll use 'clear' to remove all items.
    accountMap.clear();
    // std::cout << "[DEBUG] Account map cleared." << std::endl; // Optional debug message


    // Step 8 (cont.): Execution terminates after main returns
    return 0;
}