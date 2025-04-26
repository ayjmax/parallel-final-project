#!/bin/bash

# Check if all required arguments are provided
if [ $# -ne 2 ]; then
    echo "Error: Missing arguments"
    echo "Usage: $0 <homework_name> <model_name> <version>"
    exit 1
fi

TESTING_HW="bank" # Get first argument as homework name
TESTING_MODEL=$1 # Get second argument as model name
VERSION=$2
OUTPUT_CSV_FILE="results/$TESTING_HW/$TESTING_MODEL-$VERSION.csv" # Output CSV file
OUTPUT_TXT_FILE="results/$TESTING_HW/$TESTING_MODEL-$VERSION.txt" # Output TXT file

N=(1000 10000 100000 1000000 10000000)
NUM_ACCOUNTS=(250 1000 10000)
THREADS=(2 4 8 16)

# Clear CSV, write header
echo "Implementation,Iterations,Threads,Accounts,Time(ms),FinalBalance" > "$OUTPUT_CSV_FILE"
echo "testing $TESTING_MODEL"

# Clear TXT
echo "" > "$OUTPUT_TXT_FILE"

g++ -std=c++17 -O3 src/$TESTING_HW/$TESTING_MODEL-$TESTING_HW-$VERSION.cpp -o bin/$TESTING_HW/$TESTING_MODEL-$VERSION  -lpthread

if [ $? -ne 0 ]; then
    echo "Compilation failed"
    exit 1
fi
for n in "${N[@]}"; do
    for numAccounts in "${NUM_ACCOUNTS[@]}"; do
        for thread in "${THREADS[@]}"; do
            # Write full output to txt file
            echo "[DEBUG] Running $TESTING_MODEL-$TESTING_HW-$VERSION with: $thread Threads, $numAccounts Accounts, $n Iterations" >> "$OUTPUT_TXT_FILE"
            OUTPUT=$(./bin/$TESTING_HW/$TESTING_MODEL-$VERSION "$n" "$numAccounts" "$thread")
            echo "$OUTPUT" >> "$OUTPUT_TXT_FILE"

            # Strip first only slowest thread time from output and put into csv
            # assume $OUTPUT contains your program’s stdout
            total_time=$(echo "$OUTPUT" | grep 'Total time:'    | cut -d' ' -f3)
            final_bal=$(echo "$OUTPUT" | grep 'Final balance:' | cut -d' ' -f3)
            echo "$TESTING_MODEL,$n,$thread,$numAccounts,$total_time,$final_bal" >> "$OUTPUT_CSV_FILE"

            echo "[DEBUG] Running $TESTING_MODEL-$TESTING_HW-$VERSION with: $thread Threads, $numAccounts Accounts, $n Iterations"
            echo "$OUTPUT"
        done
    done
done

echo "All tests completed. Results saved in $OUTPUT_CSV_FILE and $OUTPUT_TXT_FILE"
