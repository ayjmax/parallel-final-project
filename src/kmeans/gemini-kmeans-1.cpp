// Parallel Implementation of the KMeans Algorithm using OpenMP
// Based on: https://github.com/marcoscastro/kmeans
// Optimized for parallel execution

#include <iostream>
#include <vector>
#include <cmath> // Use cmath for std::sqrt, std::fabs
#include <cstdlib> // Use cstdlib for std::stod, std::stoi
#include <ctime>
#include <algorithm>
#include <chrono>
#include <limits>   // Required for numeric_limits
#include <random>   // Required for mt19937
#include <omp.h>    // Include OpenMP
#include <sstream>  // For parsing comma-separated values

using namespace std;

// --- Point Class (Mostly Unchanged) ---
class Point
{
private:
	int id_point;
	// Make id_cluster volatile as it can be read/written by different threads,
	// although the chosen strategy minimizes direct concurrent access issues.
	// Alternatively, ensure proper synchronization if needed, but current
	// strategy uses it mainly for read access in getIDNearestCenter and
	// write access protected by omp for loop scheduling.
	// Using atomic might be safer but potentially slower if heavily contended.
	// Sticking with plain int for now based on the accumulator strategy.
	int id_cluster;
	vector<double> values;
	int total_values;
	// string name; // Removed name as dataset description doesn't include it and `has_name` is 0

public:
	Point(int id_point, vector<double>& values) // Removed name param
	{
		this->id_point = id_point;
		this->total_values = values.size();
		this->values = values; // Direct assignment might be faster for large vectors
		// for(int i = 0; i < total_values; i++)
		// 	this->values.push_back(values[i]); // Original copy loop
		this->id_cluster = -1; // Initialize cluster id
	}

	int getID() const // Add const for read-only methods
	{
		return id_point;
	}

	void setCluster(int id_cluster)
	{
		this->id_cluster = id_cluster;
	}

	int getCluster() const // Add const
	{
		return id_cluster;
	}

	double getValue(int index) const // Add const
	{
		// Add bounds checking for safety if needed, but assume valid index for performance
		return values[index];
	}

	int getTotalValues() const // Add const
	{
		return total_values;
	}

    // Add direct access to values vector for SIMD efficiency if needed elsewhere
    const vector<double>& getValues() const {
        return values;
    }

	// addValue and getName removed as they are not used in the core parallel logic
    // or input processing based on the sample.
};


// --- Cluster Class (Simplified for Parallel Accumulator Approach) ---
class Cluster
{
private:
	int id_cluster;
	vector<double> central_values;
	int total_values; // Store total_values for convenience

	// The vector<Point> points is removed from the cluster itself
	// as the main iteration loop will use accumulators.
	// It complicates parallel updates significantly.
	// Points are managed in the main vector<Point> in KMeans.

public:
    // Constructor now takes initial centroid values directly
	Cluster(int id_cluster, const vector<double>& initial_centroid_values)
	{
		this->id_cluster = id_cluster;
		this->total_values = initial_centroid_values.size();
		this->central_values = initial_centroid_values;
	}

	double getCentralValue(int index) const // Add const
	{
		return central_values[index];
	}

	void setCentralValue(int index, double value)
	{
		central_values[index] = value;
	}

    // Provides direct access to central values vector if needed
    const vector<double>& getCentralValues() const {
        return central_values;
    }

	int getTotalValues() const // Add const
	{
		return total_values;
	}

	int getID() const // Add const
	{
		return id_cluster;
	}

    // Methods related to adding/removing/getting points from the cluster's
    // internal list are removed (addPoint, removePoint, getPoint, getTotalPoints).
};


// --- KMeans Class (Parallel Implementation) ---
class KMeans
{
private:
	int K; // number of clusters
	int total_values, total_points, max_iterations;
	vector<Cluster> clusters;

	// return ID of nearest center (uses euclidean distance)
    // Made const as it doesn't modify KMeans state
	int getIDNearestCenter(const Point& point) const
	{
		double min_dist = numeric_limits<double>::max(); // Initialize with max value
		int id_cluster_center = 0;

		for (int i = 0; i < K; i++)
		{
			double dist;
			double sum = 0.0;
            const vector<double>& central_vals = clusters[i].getCentralValues(); // Get reference

            // ** SIMD Optimization Candidate **
            // Ensure total_values is accessible here (it's a member of KMeans)
            // Use (a-b)*(a-b) instead of pow(a-b, 2.0) for performance and SIMD friendliness
            #pragma omp simd reduction(+:sum) // Request SIMD vectorization
			for (int j = 0; j < total_values; j++)
			{
                double diff = central_vals[j] - point.getValue(j);
				sum += diff * diff;
			}

			dist = sqrt(sum); // Calculate sqrt *after* the loop

			if (dist < min_dist)
			{
				min_dist = dist;
				id_cluster_center = i;
			}
		}

		return id_cluster_center;
	}

public:
	KMeans(int K, int total_points, int total_values, int max_iterations)
	{
		this->K = K;
		this->total_points = total_points;
		this->total_values = total_values;
		this->max_iterations = max_iterations;
	}

    // Main parallel execution function
	void run(vector<Point>& points) // Pass points by non-const reference
	{
		if (K > total_points) {
            cerr << "Error: Number of clusters K cannot exceed total points." << endl;
            return;
        }

        auto total_start_time = chrono::high_resolution_clock::now();

		// --- Phase 1: Initialization (Serial) ---
        // Use the required random seed and generator
        std::mt19937 gen(714);
        std::uniform_int_distribution<> distrib(0, total_points - 1);

		vector<int> used_point_indexes;
		for (int i = 0; i < K; i++)
		{
            int index_point;
            while(true) {
                index_point = distrib(gen); // Use the seeded generator
                // Check if index has already been used
                bool found = false;
                for(int used_idx : used_point_indexes) {
                    if (used_idx == index_point) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    used_point_indexes.push_back(index_point);
                    break;
                }
            }
            // Initialize cluster with the values of the chosen point
			points[index_point].setCluster(i); // Assign point to its initial cluster
            Cluster cluster(i, points[index_point].getValues()); // Pass values vector
			clusters.push_back(cluster);
		}
        cout << "Initialized " << K << " clusters." << endl;

		// --- Phase 2: Iteration Loop (Parallel) ---
		int iter = 1;
		bool done = false;
		while (!done && iter <= max_iterations)
		{
            done = true; // Assume done until a point changes cluster

            // Global accumulators for centroid updates
            vector<vector<double>> global_sums(K, vector<double>(total_values, 0.0));
            vector<int> global_counts(K, 0);

			#pragma omp parallel // Start parallel region
			{
                // Thread-local accumulators
                vector<vector<double>> local_sums(K, vector<double>(total_values, 0.0));
                vector<int> local_counts(K, 0);
                bool thread_done = true; // Thread-local done flag

                #pragma omp for // Distribute points loop across threads
				for (int i = 0; i < total_points; i++)
				{
					int id_old_cluster = points[i].getCluster();
					int id_nearest_center = getIDNearestCenter(points[i]);

					if (id_old_cluster != id_nearest_center)
					{
						points[i].setCluster(id_nearest_center);
                        thread_done = false; // This thread found a change
                        // No need to remove/add from cluster point lists here
					}
                    // Accumulate for the *new* cluster assignment
                    int current_cluster_id = points[i].getCluster(); // Get potentially updated cluster ID
                    if (current_cluster_id != -1) { // Ensure point is assigned
                        for(int j=0; j<total_values; ++j) {
                            local_sums[current_cluster_id][j] += points[i].getValue(j);
                        }
                        local_counts[current_cluster_id]++;
                    }
				} // End of parallel for loop

                // Combine thread-local accumulators into global ones safely
                #pragma omp critical
                {
                    for(int k=0; k<K; ++k) {
                        for(int j=0; j<total_values; ++j) {
                            global_sums[k][j] += local_sums[k][j];
                        }
                        global_counts[k] += local_counts[k];
                    }
                    // Combine the 'done' status: if any thread found a change (!thread_done),
                    // the overall iteration is not done.
                    if (!thread_done) {
                        done = false;
                    }
                } // End of critical section

            } // End of parallel region

			// --- Update Centroids (Serial Part) ---
            // This part is relatively fast (loops K times) compared to the points loop
			for (int i = 0; i < K; i++)
			{
				if (global_counts[i] > 0)
				{
					for (int j = 0; j < total_values; j++)
					{
						clusters[i].setCentralValue(j, global_sums[i][j] / global_counts[i]);
					}
				}
                // Optional: Handle empty clusters? (e.g., re-initialize randomly)
                // Current code keeps the old centroid if a cluster becomes empty.
			}

            if (done) {
                 cout << "Converged in iteration " << iter << endl;
            } else if (iter == max_iterations) {
                 cout << "Reached max iterations (" << max_iterations << ")" << endl;
            }


			iter++;
		} // End of while loop

        auto total_end_time = chrono::high_resolution_clock::now();
        auto duration_ms = chrono::duration_cast<chrono::milliseconds>(total_end_time - total_start_time).count();

        // --- Final Output ---
        cout << "Total time: " << duration_ms << endl; // Required output format in ms


        // --- Optional: Print cluster info (can be slow for large datasets) ---
        /*
        cout << "\nFinal Cluster Information:\n";
		for (int i = 0; i < K; i++)
		{
            // To print points per cluster, we'd need to iterate through all points again
            int points_in_cluster = 0;
            #pragma omp parallel for reduction(+:points_in_cluster)
            for(int p=0; p<total_points; ++p) {
                if(points[p].getCluster() == i) {
                    points_in_cluster++;
                }
            }

			cout << "Cluster " << clusters[i].getID() + 1 << " (" << points_in_cluster << " points)" << endl;
			cout << "  Centroid: ";
			for (int j = 0; j < total_values; j++) {
				cout << clusters[i].getCentralValue(j) << " ";
            }
			cout << "\n" << endl;
		}
        */
	} // End of run()
};


// --- Main Function (Reads Input, Runs KMeans) ---
int main(int argc, char *argv[])
{
	// srand (time(NULL)); // Remove this, using mt19937 with fixed seed

	int total_points, total_values, K, max_iterations, has_name;

    // Read the header line
	if (!(cin >> total_points >> total_values >> K >> max_iterations >> has_name)) {
        cerr << "Error reading header line from input." << endl;
        return 1;
    }

	vector<Point> points;
    points.reserve(total_points); // Pre-allocate memory
	string line;
    // Consume the rest of the header line (including potential newline)
    getline(cin >> ws, line); // ws consumes leading whitespace

	for (int i = 0; i < total_points; ++i)
	{
        if (!getline(cin, line)) {
             cerr << "Error reading data line " << i+1 << endl;
             // Handle incomplete input - maybe proceed with points read so far?
             total_points = i; // Adjust total_points
             break;
        }

        vector<double> values;
        values.reserve(total_values);
        stringstream ss(line);
        string value_str;

        for (int j = 0; j < total_values; ++j) {
            if (!getline(ss, value_str, ',')) { // Read values separated by comma
                 cerr << "Error reading value " << j+1 << " on data line " << i+1 << endl;
                 // Handle error - skip point? Abort?
                 goto read_error; // Using goto for simplicity in nested loops
            }
            try {
                 values.push_back(stod(value_str));
            } catch (const std::invalid_argument& ia) {
                cerr << "Invalid double format '" << value_str << "' on line " << i+1 << endl;
                goto read_error;
            } catch (const std::out_of_range& oor) {
                cerr << "Double value out of range '" << value_str << "' on line " << i+1 << endl;
                goto read_error;
            }
        }

        // If has_name were 1, we would read the name here from the end of the line
        // string point_name = ""; if(has_name) { /* read name */ } Point p(i, values, point_name);
        // Since has_name=0 for the sample data and it's not used, simplify:
        Point p(i, values);
        points.push_back(p);

        continue; // Continue to next point loop iteration

        read_error: // Label for error handling
             cerr << "Skipping point " << i+1 << endl;
             // Optionally adjust total_points if skipping, though it might skew results
             continue; // Skip this point and try reading the next line

	}
    // Adjust total_points if any were skipped due to errors
    if (points.size() != total_points) {
        cout << "Warning: Read " << points.size() << " points, expected " << total_points << "." << endl;
        total_points = points.size();
        if (total_points < K) {
            cerr << "Error: Not enough valid points (" << total_points << ") for K=" << K << " clusters." << endl;
            return 1;
        }
    }

    if (total_points > 0) {
	    cout << "Read " << total_points << " points with " << total_values << " values each." << endl;
	    KMeans kmeans(K, total_points, total_values, max_iterations);
	    kmeans.run(points);
    } else {
        cerr << "Error: No valid data points read." << endl;
        return 1;
    }

	return 0;
}