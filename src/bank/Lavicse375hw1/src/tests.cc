// CSE 375/475 Assignment #1
// Spring 2024
//
// Description: This file implements a function 'run_custom_tests' that should be able to use
// the configuration information to drive tests that evaluate the correctness
// and performance of the map_t object.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <ios>
#include <iostream>
#include <mutex>
#include <fstream>
#include <random>
#include <thread>
#include <utility>
#include "config_t.h"
#include "tests.h"

#include "simplemap.h"
#include "doubly_shared_lock.h"

void do_assert(bool test, const char* reason){
	if (!test) {
		std::cout << "Failed assert " << reason << std::endl;
		exit(1);
	}
}

void printer(int k, int v) {
	std::cout<<"<"<<k<<","<<v<<">"<< std::endl;
}

void run_custom_tests(config_t& cfg) {
	const int NUM_ACCOUNT = cfg.key_max;
	const int TOTAL_BAL = 10000 * 100; // $10,000 as pennies
	const int THREAD_COUNT = cfg.threads;
	const int MUTEX_COUNT = NUM_ACCOUNT; // todo: THREAD_COUNT * 12;
	const int PROB_BAL_OP = 5; // probability of a balance operation

	// Step 1: Define a map
	simplemap_t<1000000> map = simplemap_t<1000000>(NUM_ACCOUNT);
	std::mutex* locks[MUTEX_COUNT];
	double_shared_lock shared_lock;
	for(int i = 0; i < MUTEX_COUNT; i++){
		locks[i] = new std::mutex();
	}

	// Step 2: Populate
	for(int i = 0; i < NUM_ACCOUNT; i++){
		map.insert(i, TOTAL_BAL / NUM_ACCOUNT);
	}

	std::function<bool(int, int, int)> deposit = [&](int b1, int b2, int value){
		// Step 3: Deposit operation parallel defintion
		bool status = true;
		if (b1 == b2) return true; // a deposit on two of the same account has no effect
		int l1 = b1 % MUTEX_COUNT;
		int l2 = b2 % MUTEX_COUNT;
		if (l1 > l2){
			// make sure we are locking the lower one first
			std::swap(l1, l2);
		}
		// get the locks
		locks[l1]->lock();
		// avoid locking the same mutex (deadlock)
		if (l1 != l2)
			locks[l2]->lock();
		shared_lock.lock_deposit();

		// make the transaction
		auto acc1 = map.lookup(b1);
		auto acc2 = map.lookup(b2);
		if (!acc1.second || !acc2.second) {
			status = false;
			goto unlock; // cannot find b1 or b2 (err)
		}
		// guard to make sure that the value being subtracted will never cause a debt
		if (value > acc1.first)
			value = acc1.first;
		map.update(b1, acc1.first - value);
		map.update(b2, acc2.first + value);

		// unlock
		unlock:
		shared_lock.unlock_deposit();
		locks[l1]->unlock();
		// avoid unlocking the same mutex (double unlock)
		if (l1 != l2)
			locks[l2]->unlock();
		return status;
	};

	std::function<int()> balance = [&](){
		// Step 4: Balance operation parallel defintion
		int total = 0;
		// locks (whole data structure)
		if (shared_lock.register_balance(&total)){
			// compute the sum
			total = map.parallel_sum();
			shared_lock.complete_balance(total);
		}
		return total;
	};

	// Step 5: Function to time operations
	std::function<std::chrono::milliseconds(int)> do_work = [&](int op_count){
		std::random_device rd;  // a seed source for the random number engine
		std::mt19937 gen(rd()); // mersenne_twister_engine seeded with rd()
		std::uniform_int_distribution<> acc_dist(0, NUM_ACCOUNT - 1);
		// the value distribution is 0 to half the original value of each bank account (half the average bank account size)
		std::uniform_int_distribution<> val_dist(0, TOTAL_BAL / (NUM_ACCOUNT * 2));
		// distribution for deciding on the operations
		std::uniform_int_distribution<> op_dist(1, 100);

		std::chrono::milliseconds start = std::chrono::duration_cast<std::chrono::milliseconds>
			(std::chrono::system_clock::now().time_since_epoch());
		for(int it = 0; it < op_count; it++){
			int prob = op_dist(gen);
			if (prob <= PROB_BAL_OP){
				// 5% chance of a balance operation
				int total = balance();
				if (total != TOTAL_BAL){ 
					std::cout << total << "!=" << TOTAL_BAL << std::endl;
					do_assert(false, "Balance is incorrect in multithreaded");
				}
			} else {
				// 95% chance of a random transaction
				bool success = deposit(acc_dist(gen), acc_dist(gen), val_dist(gen));
				do_assert(success, "Deposit failed in multithreaded");
			}
		}
		std::chrono::milliseconds end = std::chrono::duration_cast<std::chrono::milliseconds>
			(std::chrono::system_clock::now().time_since_epoch());
		return end - start;
	};

	// Step 6: Parallel benchmark
	std::thread threads[THREAD_COUNT];
	std::chrono::milliseconds result[THREAD_COUNT];
	// start all threads
	for(int i = 0; i < THREAD_COUNT; i++){
		threads[i] = std::thread([&](int index){
			result[index] = do_work(cfg.iters / THREAD_COUNT);
		}, i);
	}
	// join all threads
	for(int i = 0; i < THREAD_COUNT; i++){
		threads[i].join();
	}
	// reduce to the longest running thread's time
	long parallel_max = result[0].count();
	for(int i = 1; i < THREAD_COUNT; i++){
		parallel_max = std::max(parallel_max, result[i].count());
	}
	// check balance is valid
	do_assert(balance() == TOTAL_BAL, "End balance failed");

	// Step 7: Serial benchmark
	std::random_device rd;  // a seed source for the random number engine
    std::mt19937 gen(rd()); // mersenne_twister_engine seeded with rd()
	std::uniform_int_distribution<> acc_dist(0, NUM_ACCOUNT - 1);
	// the value distribution is 0 to half the original value of each bank account (half the average bank account size) 
	std::uniform_int_distribution<> val_dist(0, (100 * TOTAL_BAL) / (NUM_ACCOUNT * 2));
	// distribution for deciding on the operations
	std::uniform_int_distribution<> op_dist(1, 100);
	
	std::chrono::milliseconds start = std::chrono::duration_cast<std::chrono::milliseconds>
			(std::chrono::system_clock::now().time_since_epoch());
	for(int i = 0; i < cfg.iters; i++){
		int prob = op_dist(gen);
		if (prob <= PROB_BAL_OP){
			// 5% chance of a balance operation
			int total = 0;
			// acquire all locks and add to the sum
			for(int i = 0; i < NUM_ACCOUNT; i++){
				auto acc = map.lookup(i);
				do_assert(acc.second, "Can't find account");
				total += acc.first;
			}
			do_assert(total == TOTAL_BAL, "Balance is incorrect serial");
		} else {
			// 95% chance of a random transaction
			// generate two random numbers for accounts (based on account distribution)
			int b1 = acc_dist(gen);
			int b2 = acc_dist(gen);
			if (b1 == b2) continue; // a deposit on two of the same account has no effect
			// generate a value to shift among accounts
			int v = val_dist(gen);

			// make the transaction
			auto acc1 = map.lookup(b1);
			auto acc2 = map.lookup(b2);
			do_assert(acc1.second && acc2.second, "Can't find accounts in serial");
			// guard to make sure that the value being subtracted will never cause a debt
			if (v > acc1.first)
				v = acc1.first;
			map.update(b1, acc1.first - v);
			map.update(b2, acc2.first + v);
		}
	}
	std::chrono::milliseconds end = std::chrono::duration_cast<std::chrono::milliseconds>
		(std::chrono::system_clock::now().time_since_epoch());
	std::chrono::milliseconds serial_result = end - start;

	// Step 8: Clear the map
	map.clear();
	for(int i = 0; i < MUTEX_COUNT; i++){
		free(locks[i]);
	}

	// Step 9: Save data to graph later
	std::string filename = cfg.name + ".txt";
	std::ifstream in(filename);
	bool add_header = true;
	if (in.is_open()){
		in.bad();
		int fstart = in.tellg();
		in.seekg(0, std::ios_base::end);
		// if the file is empty, then we add the header
		add_header = fstart == in.tellg();
	}
	in.close();

	std::ofstream out(filename, std::ios_base::app);
	if (add_header){
		out << "threads,key_max,iters,time(ms),time_serial(ms)" << std::endl;
	}
	// experimental configuration
    out << THREAD_COUNT << "," << NUM_ACCOUNT << "," << cfg.iters << ",";
	// timing results
	out << parallel_max << "," << serial_result.count() << std::endl;
    out.close();
}

void test_driver(config_t &cfg) {
	run_custom_tests(cfg);
}
