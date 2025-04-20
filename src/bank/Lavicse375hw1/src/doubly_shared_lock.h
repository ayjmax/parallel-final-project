#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

class double_shared_lock {
private:
    // Deposit sync
    std::atomic<int> deposit_ops;

    // condition variables
    std::condition_variable done_deposits;
    std::condition_variable done_balance;
    std::condition_variable waiting_for_notif;

    // Notifications array
    std::vector<int*> notifications;

    // global lock on the data
    std::mutex notifier_lock;

    // The overall state of the lock
    bool is_balanced_locked;
    bool is_deposit_locked;
public:
    double_shared_lock(){
        deposit_ops = 0;
        is_deposit_locked = false;
        is_balanced_locked = false;
    }

    void lock_deposit(){
        // use a increment if deposit isn't 0, avoiding the lock :)
        while(true){
            int before = deposit_ops;
            if (before == 0) break;
            // make the exchange and exit if successful
            bool success_increment = deposit_ops.compare_exchange_weak(before, before + 1);
            if (success_increment) return;
        }
        // synchronize
        std::unique_lock<std::mutex> notifier_lock_unique(notifier_lock);
        // add to count if already locked
        if (is_deposit_locked){
            deposit_ops++;
            return;
        }
        // wait until balance is done
        while (is_balanced_locked){
            done_balance.wait(notifier_lock_unique);
        }
        // increment deposit operations and "acquire" the doubly_shared_lock in deposit mode
        deposit_ops++;
        is_deposit_locked = true;
    }

    void unlock_deposit(){
        // use a decrement if deposit isn't 1, avoiding the lock :)
        while(true){
            int before = deposit_ops;
            if (before == 1) break;
            // make the exchange and exit if successful
            bool success_decrement = deposit_ops.compare_exchange_weak(before, before - 1);
            if (success_decrement) return;
        }
        // synchronize
        std::unique_lock<std::mutex> notifier_lock_unique(notifier_lock);
        // do the decrement
        deposit_ops--;
        if (deposit_ops == 0){
            // if we are 0, we must release the lock
            is_deposit_locked = false;
            done_deposits.notify_all();
        }
    }

    /// Use the result of the other balance operation if the balance lock is held
    /// Will return (true, 0) if the balance lock was acquired
    /// Otherwise will return (false, X) to be the result of the other balance operation
    bool register_balance(int* result){
        *result = -1;
        // Get notifier lock first
        std::unique_lock<std::mutex> notifier_lock_unique(notifier_lock);
        // wait until deposits are done
        while (is_deposit_locked){
            done_deposits.wait(notifier_lock_unique);
        }
        // then try to get balance lock to hold permanently
        if (is_balanced_locked){
            notifications.push_back(result);
            // wait for the result
            while(*result == -1){
                waiting_for_notif.wait(notifier_lock_unique);
            }
            // return false to indicate someone else did the operation for us
            return false;
        }
        // no mode is defined, so we can move us into balance mode
        is_balanced_locked = true;
        return true;
    }

    /// Release the balance lock
    void complete_balance(int value){
        // complete all the notifications
        std::unique_lock<std::mutex> notifier_lock_unique(notifier_lock);
        while(notifications.size() != 0){
            *notifications[notifications.size() - 1] = value;
            notifications.pop_back();
        }
        // unlock the balance lock
        is_balanced_locked = false;
        // wake everybody else up
        waiting_for_notif.notify_all();
        done_balance.notify_all();
    }
};