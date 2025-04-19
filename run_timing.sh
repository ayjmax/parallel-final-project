#!/bin/bash

VERSION="2"
TESTING_HW="bank"
TESTING_MODEL="o3"
OUTPUT_CSV_FILE="results/$TESTING_HW/$TESTING_MODEL-$VERSION.csv" # Output CSV file
OUTPUT_TXT_FILE="results/$TESTING_HW/$TESTING_MODEL-$VERSION.txt" # Output TXT file

MODELS=("o3" "gemini" "claude" "deepseek")
N=(1000 10000 100000 1000000 10000000 100000000 1000000000)
numKeys=(1000000)
threads=(1 2 4 8 12 16 24)

# Write CSV header
echo "File,N,Threads,Time" > "$OUTPUT_CSV_FILE"

for model in "${MODELS[@]}"; do
    echo "testing $model"

    g++ -std=c++17 -O3 src/${TESTING_HW}/${model}-${TESTING_HW}-${VERSION}.cpp -o ${model}-${TESTING_HW}-${VERSION}  -lpthread

    if [ $? -ne 0 ]; then
        echo "Compilation failed"
        exit 1
    fi
    for n in "${N[@]}"; do
        for numKey in "${numKeys[@]}"; do
            for thread in "${threads[@]}"; do
                echo "[DEBUG] Running ${model}-${TESTING_HW}-${VERSION} with: $thread Threads, $numKey Accounts, $n Iterations"
                OUTPUT=$(./${model}-${TESTING_HW}-${VERSION} "$n" "$numKey" "$thread")
                echo "$OUTPUT"
                read -r time <<< "$OUTPUT"
                echo "${model},${n},${thread},${time}" >> "$OUTPUT_CSV_FILE"
                echo "${model},${n},${thread},${time}" >> "$OUTPUT_TXT_FILE"
            done
        done
    done
done

echo "All tests completed. Results saved in $OUTPUT_CSV_FILE and $OUTPUT_TXT_FILE"
