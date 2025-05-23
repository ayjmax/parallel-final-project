#!/bin/bash

# Check if all required arguments are provided
if [ $# -ne 3 ]; then
    echo "Error: Missing arguments"
    echo "Usage: $0 <model_name> <version> <dataset ('exchange' or 'drybean')>"
    exit 1
fi

TESTING_HW="kmeans"
TESTING_MODEL=$1
VERSION=$2
DATASET=$3
OUTPUT_CSV_FILE="results/$TESTING_HW/$TESTING_MODEL-$VERSION-$DATASET.csv" # Output CSV file
OUTPUT_TXT_FILE="results/$TESTING_HW/$TESTING_MODEL-$VERSION-$DATASET.txt" # Output TXT file

# Clear CSV, write header
echo "Implementation,Dataset,Time(μs)" > "$OUTPUT_CSV_FILE"
echo "testing $TESTING_MODEL with $DATASET"

# Clear TXT
echo "" > "$OUTPUT_TXT_FILE"

# Compile
LFLAGS="-L oneapi-tbb-2022.0.0/lib/intel64/gcc4.8 -ltbb -lpthread" # Library flags
IFLAGS="-I oneapi-tbb-2022.0.0/include"
g++ -std=c++17 -O3 -fopenmp $IFLAGS src/kmeans/${TESTING_MODEL}-${TESTING_HW}-${VERSION}.cpp -o bin/${TESTING_HW}/${TESTING_MODEL}-${VERSION} $LFLAGS


if [ $? -ne 0 ]; then
    echo "Compilation failed. Exiting..."
    exit 1
fi

if [ "$DATASET" == "exchange" ]; then
    # Loop over all .txt files in the specified directory
    for dataset in /proj/cse375-475/exchange/datasets/*.txt; do
        debug_message="[DEBUG] Running $TESTING_MODEL-$TESTING_HW-$VERSION with: $dataset"
        echo "$debug_message"

        # Run the program with the dataset as input
        OUTPUT=$(cat "$dataset" | ./bin/$TESTING_HW/$TESTING_MODEL-$VERSION)

        # Write to TXT
        echo "$debug_message" >> "$OUTPUT_TXT_FILE"
        echo "$OUTPUT" >> "$OUTPUT_TXT_FILE"

        # Extract the total time from the output and append to the CSV file
        total_time=$(echo "$OUTPUT" | grep 'Total time:'    | cut -d' ' -f3)
        echo "$TESTING_MODEL,$(basename "$dataset"),$total_time" >> "$OUTPUT_CSV_FILE"

        # Print to terminal
        echo "$OUTPUT"
    done
elif [ "$DATASET" == "drybean" ]; then
    # Run the program with the dataset as input
    OUTPUT=$(cat "/proj/cse375-475/exchange/drybean/drybean1.txt" | ./bin/$TESTING_HW/$TESTING_MODEL-$VERSION)

    # Write to CSV
    total_time=$(echo "$OUTPUT" | grep 'Total time:'    | cut -d' ' -f3)
    echo "$TESTING_MODEL,$DATASET,$total_time" >> "$OUTPUT_CSV_FILE"

    # Write to TXT
    echo "$debug_message" >> "$OUTPUT_TXT_FILE"
    echo "$OUTPUT" >> "$OUTPUT_TXT_FILE"
fi

echo "All tests completed. Results saved in $OUTPUT_CSV_FILE and $OUTPUT_TXT_FILE"