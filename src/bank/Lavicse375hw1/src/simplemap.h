// CSE 375/475 Assignment #1
// Spring 2024
//
// Description: This file specifies a custom map implemented using two vectors.
// << use templates for creating a map of generic types >>
// One vector is used for storing keys. One vector is used for storing values.
// The expectation is that items in equal positions in the two vectors correlate.
// That is, for all values of i, keys[i] is the key for the item in values[i].
// Keys in the map are not necessarily ordered.
//
// The specification for each function appears as comments.
// Students are responsible for implementing the simple map as specified.

#include <iostream>
#include <utility>
#include <vector>
#include <cassert>
#include <emmintrin.h>
#include <smmintrin.h>
#include <immintrin.h>

template <int MAX_SIZE>
class simplemap_t {
    int size;
    alignas(16) int32_t values[MAX_SIZE];
  public:

    // The constructor should just initialize the vectors to be empty
    simplemap_t(int size) {
        this->size = size;
    }

    // Insert (key, val) if and only if the key is not currently present in
    // the map.  Returns true on success, false if the key was
    // already present.
    bool insert(int key, int val) {
        if (key >= size){
            std::cout << "Keys out of range" << std::endl;
            return false;
        }
        values[key] = val;
    	return true;
    }

    // If key is present in the data structure, replace its value with val
    // and return true; if key is not present in the data structure, return
    // false.
    bool update(int key, int val) {
        if (size <= key || key < 0) return false;
        values[key] = val;
        return true;
    }

    // Remove the (key, val) pair if it is present in the data structure.
    // Returns true on success, false if the key was not already present.
    void clear(){
        for(int i = 0; i < size; i++){
            values[i] = 0;
        }
    }

    // If key is present in the map, return a pair consisting of
    // the corresponding value and true. Otherwise, return a pair with the
    // boolean entry set to false.
    // Be careful not to share the memory of the map with application threads, you might
    // get unexpected race conditions
    std::pair<int, bool> lookup(int key) {
        if (size <= key || key < 0){ 
            int def;
            return std::make_pair(def, false);
        }
        return std::make_pair(values[key], true);
    }

    int parallel_sum(){
        // Loop through the 32-bit int array 8 at a time
        __m256i vec256 = _mm256_load_si256(reinterpret_cast<__m256i*>(values));
        if (size % 8 != 0) {
            std::cout << "Only doing multiples of 8 for size" << std::endl;
            return -1;
        }
        for (int n = 8; n < size; n += 8) {
            // Load the next 4 ints
            __m256i to_add = _mm256_load_si256(reinterpret_cast<__m256i*>(values + n));
            // And add together
            vec256 = _mm256_add_epi32(vec256, to_add);
        }

        alignas(16) int32_t ints[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(ints), vec256);
        // combine into result
        return ints[0] + ints[1] + ints[2] + ints[3] + ints[4] + ints[5] + ints[6] + ints[7];
    }

    // Apply a function to each key in the map
    void apply(void (*f)(int, int)) {
    	for (int i = 0; i < size; i++) {
    		f(i, values[i]);
    	}
    }
};
