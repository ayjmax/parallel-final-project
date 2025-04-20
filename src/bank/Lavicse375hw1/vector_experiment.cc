#include <chrono>
#include <iostream>
#include <emmintrin.h>
#include <smmintrin.h>
#include <immintrin.h>

#define SIZE 1000000

int main (){
    alignas(16) int32_t data[SIZE];
    // vector to sum to 100_000
    for(int i = 0; i < SIZE; i++){
        data[i] = 1;
    }

    // baseline
    std::chrono::microseconds start = std::chrono::duration_cast<std::chrono::microseconds>
    (std::chrono::system_clock::now().time_since_epoch());
    int total = 0;
    for (int n = 0; n < SIZE; n += 1) {
        total += data[n];
    }
    std::chrono::microseconds end = std::chrono::duration_cast<std::chrono::microseconds>
    (std::chrono::system_clock::now().time_since_epoch());
    std::cout << "Serial: " << (end - start).count() << std::endl;
    std::cout << '[' << total << "]" << std::endl;
    
    // vector 128 byte
    start = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
    // Loop through the 32-bit int array 4 at a time
    __m128i vec = _mm_load_si128(reinterpret_cast<__m128i*>(data));
    for (int n = 4; n < SIZE; n += 4) {
        // Load the next 4 ints
        __m128i to_add = _mm_load_si128(reinterpret_cast<__m128i*>(data + n));
        // And add together
        vec = _mm_add_epi32(vec, to_add);
    }
    end = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());

    alignas(16) int32_t ints[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(ints), vec);
    std::cout << "128 vector instructions: " << (end - start).count() << std::endl;
    std::cout << '[';
    for(int i = 0; i < 4; i++){
        std::cout << ints[i] << ", ";
    }
    std::cout << ']' << std::endl;

    // trying out 256byte
    start = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());
    // Loop through the 32-bit int array 8 at a time
    __m256i vec256 = _mm256_load_si256(reinterpret_cast<__m256i*>(data));
    for (int n = 8; n < SIZE; n += 8) {
        // Load the next 4 ints
        __m256i to_add = _mm256_load_si256(reinterpret_cast<__m256i*>(data + n));
        // And add together
        vec256 = _mm256_add_epi32(vec256, to_add);
    }
    end = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch());

    alignas(16) int32_t ints2[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(ints2), vec256);
    std::cout << "256 vector instructions: " << (end - start).count() << std::endl;
    std::cout << '[';
    for(int i = 0; i < 8; i++){
        std::cout << ints2[i] << ", ";
    }
    std::cout << ']' << std::endl;
}