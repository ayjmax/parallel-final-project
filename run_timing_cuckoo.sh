#!/bin/bash

# Check if all required arguments are provided
if [ $# -ne 2 ]; then
    echo "Error: Missing arguments"
    echo "Usage: $0 <model_name> <version>"
    exit 1
fi

TESTING_HW="cuckoo"
TESTING_MODEL=$1
VERSION=$2
OUTPUT_CSV_FILE="results/$TESTING_HW/$TESTING_MODEL-$VERSION.csv" # Output CSV file
OUTPUT_TXT_FILE="results/$TESTING_HW/$TESTING_MODEL-$VERSION.txt" # Output TXT file

OPERATIONS=(1000 10000 100000 1000000 10000000)
THREADS=(2 4 8 16)

# Clear CSV, write header
echo "Implementation,Operations,Threads,Time(μs)" > "$OUTPUT_CSV_FILE"
echo "testing $TESTING_MODEL"

# Clear TXT
echo "" > "$OUTPUT_TXT_FILE"

g++ -std=c++17 -O3 src/$TESTING_HW/$TESTING_MODEL-$TESTING_HW-$VERSION.cpp -o bin/$TESTING_HW/$TESTING_MODEL-$VERSION  -lpthread

if [ $? -ne 0 ]; then
    echo "Compilation failed"
    exit 1
fi

for op in "${OPERATIONS[@]}"; do
    for thread in "${THREADS[@]}"; do
        echo "[DEBUG] Running $TESTING_MODEL-$TESTING_HW-$VERSION with: $thread Threads, $op Operations"

        # Write full output to txt file
        echo "[DEBUG] Running $TESTING_MODEL-$TESTING_HW-$VERSION with: $thread Threads, $op Operations" >> "$OUTPUT_TXT_FILE"
        OUTPUT=$(./bin/$TESTING_HW/$TESTING_MODEL-$VERSION "$op" "$thread")
        echo "$OUTPUT" >> "$OUTPUT_TXT_FILE"

        # Strip first only slowest thread time from output and put into csv
        # assume $OUTPUT contains your program’s stdout
        total_time=$(echo "$OUTPUT" | grep 'Total time:'    | cut -d' ' -f3)
        echo "$TESTING_MODEL,$op,$thread,$total_time" >> "$OUTPUT_CSV_FILE"

        # Print to terminal
        echo "$OUTPUT"
    done
done

echo "All tests completed. Results saved in $OUTPUT_CSV_FILE and $OUTPUT_TXT_FILE"
