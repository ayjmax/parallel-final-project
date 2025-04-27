// in this code we will only recalculate centroids of clusters based on changed points, not all points
#include <iostream>
#include <vector>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <tbb/tbb.h> // for tbb
#include <mutex>
#include <atomic>
#include <memory>
#include <immintrin.h> // for SIMD optimization using AVX
#include <cfloat> // for max double


using namespace std;

class Point {
private:
	int id_point, id_cluster;
	vector<double> values;
	int total_values;
	string name;

public:
	Point(int id_point, vector<double>& values, string name = "")
	{
		this->id_point = id_point;
		total_values = values.size();

		for(int i = 0; i < total_values; i++)
			this->values.push_back(values[i]);

		this->name = name;
		id_cluster = -1;
	}

	int getID()
	{
		return id_point;
	}

	void setCluster(int id_cluster)
	{
		this->id_cluster = id_cluster;
	}

	int getCluster()
	{
		return id_cluster;
	}

	double getValue(int index)
	{
		return values[index];
	}

	int getTotalValues()
	{
		return total_values;
	}

	void addValue(double value)
	{
		values.push_back(value);
	}

	string getName()
	{
		return name;
	}

    const double* getValuesData() const { //for accessing several data points at a time using SIMD
        return values.data();
    }    
};

class Cluster {
    private:
        int id_cluster;
        int num_points;
        vector<double> central_values;
        vector<double> sums;
        vector<mutex> sum_mutexes; // mutex for each sum to synchronize it
        mutex count_mutex;         // new mutex to protect num_points
    
    public:
        Cluster(int id_cluster, Point point)
        {
            this->id_cluster = id_cluster;
            num_points = 1;
    
            int total_values = point.getTotalValues();
            sum_mutexes = vector<mutex>(total_values);
    
            for (int i = 0; i < total_values; i++) {
                double val = point.getValue(i);
                central_values.push_back(val);
                sums.push_back(val);
            }
        }
    
        void addPoint(Point point)
        {
            { // Protect num_points update
                lock_guard<mutex> lock(count_mutex);
                num_points += 1;
            }
    
            for (int i = 0; i < point.getTotalValues(); i++) {
                lock_guard<mutex> lock(sum_mutexes[i]);
                sums[i] += point.getValue(i);
            }
        }
    
        bool removePoint(Point point) {
            for (int j = 0; j < point.getTotalValues(); j++) {
                lock_guard<mutex> lock(sum_mutexes[j]);
                sums[j] -= point.getValue(j);
            }
            { // Protect num_points update
                lock_guard<mutex> lock(count_mutex);
                num_points--;
            }
            return true;
        }
    
        double getCentralValue(int index)
        {
            return central_values[index];
        }
    
        void setCentralValue(int index, double value)
        {
            central_values[index] = value;
        }

        const double* getCentralValuesData() const { // method for accessing data for SIMD 
            return central_values.data();
        }
    
        int getTotalPoints()
        {
            lock_guard<mutex> lock(count_mutex);
            return num_points;
        }
    
        int getID()
        {
            return id_cluster;
        }
    
        double getSum(int index) {
            lock_guard<mutex> lock(sum_mutexes[index]);
            return sums[index];
        }
    };
    

class KMeans {
private:
	int K; // number of clusters
	int total_values, total_points, max_iterations;
	// vector<Cluster> clusters;

    //store pointer to vectors due to immovable mutex objects
    vector<unique_ptr<Cluster>> clusters;

	// return ID of nearest center (uses euclidean distance)
	int getIDNearestCenter(Point point)
	{
		double min_dist = DBL_MAX;
		int id_cluster_center = 0;
        
        const double *pt_ptr = point.getValuesData();
        
		for(int i = 0; i < K; i++) {
            // initialize SIMD
            __m256d sum_vec = _mm256_setzero_pd();

            const double *central_ptr = clusters[i]->getCentralValuesData();
            
            for(int j = 0; j < total_values; j+=4){
                __m256d center_val = _mm256_loadu_pd(central_ptr + j);
                __m256d point_val = _mm256_loadu_pd(pt_ptr + j);
                __m256d diff = _mm256_sub_pd(center_val, point_val);
                __m256d sq = _mm256_mul_pd(diff, diff);
                sum_vec = _mm256_add_pd(sum_vec, sq);
            }

            double temp[4];  //for horizontal addition
            _mm256_storeu_pd(temp, sum_vec);
            double dist = temp[0] + temp[1] + temp[2] + temp[3];

			if(dist < min_dist) {
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

	void run(vector<Point> & points) {
		if(K > total_points)
			return;

		vector<int> prohibited_indexes;

		// choose K distinct values for the centers of the clusters
		for(int i = 0; i < K; i++)
		{
			while(true)
			{
				int index_point = rand() % total_points;

				if(find(prohibited_indexes.begin(), prohibited_indexes.end(),
						index_point) == prohibited_indexes.end())
				{
					prohibited_indexes.push_back(index_point);
					points[index_point].setCluster(i);
                    clusters.emplace_back(new Cluster(i, points[index_point]));
					// Cluster cluster(i, points[index_point]);
					// clusters.push_back(cluster);
					break;
				}
			}
		}
		int iter = 1;

		while(true)
		{
			atomic<bool> done{true};

			// associates each point to the nearest center
            tbb::parallel_for(tbb::blocked_range<int> (0, total_points),
                [this, &points, &done](const tbb::blocked_range<int> &range) {
                    for (int i = range.begin(); i < range.end(); ++i) {
                        int id_old_cluster = points[i].getCluster();
                        int id_nearest_center = getIDNearestCenter(points[i]);

                        if(id_old_cluster != id_nearest_center)
                        {
                            if(id_old_cluster != -1)
                                this->clusters[id_old_cluster]->removePoint(points[i]);

                            points[i].setCluster(id_nearest_center);
                            this->clusters[id_nearest_center]->addPoint(points[i]);
                            done = false;
				        }
                    }
                }
            );

            // wait for all threads (TBB takes care of this)

			// recalculate central values
            tbb::parallel_for(tbb::blocked_range<int>(0, K),
                [this](const tbb::blocked_range<int> &range) {
                    for (ssize_t i = range.begin(); i != range.end(); ++i) {
                        for(int j = 0; j < total_values; j++)
                        {
                            int cluster_size = this->clusters[i]->getTotalPoints();
                            if(cluster_size > 0)
                            {
                                double dim_sum = this->clusters[i]->getSum(j); // dimensions sum
                                this->clusters[i]->setCentralValue(j, dim_sum / cluster_size);
                            }
                        }
                    }
                }
            );

			if(done == true || iter >= max_iterations)
			{
				// cout << "Break in iteration " << iter << "\n\n";
				break;
			}

			iter++;
		}
	}
};

int main(int argc, char *argv[])
{
	srand (time(NULL));

	vector<Point> points;
    int id_counter = 0; 

	string line;
	while (true) {
		if (!getline(cin, line)) {
			break;
		}

		if(line.empty()) {
			continue;
		}

		char c = line[0];
        if(c == '@' || c == '%' || c == ' ')
            continue;

		vector<string> tokens;
		{
			// Minimal parsing: assume comma-separated
			istringstream iss(line);
			string token;
			while (getline(iss, token, ',')) {
				tokens.push_back(token);
			}
		}

		if (tokens.size() < 17) {
            // Not enough columns; skip or break
            continue;
        }

		// Convert first 16 tokens to double
		vector<double> features(16);
		for(int i = 0; i < 16; i++) {
			features[i] = stod(tokens[i]);
		}
		// 17th token is the class label
		string label = tokens[16];

		Point p(id_counter, features, label);
		points.push_back(p);
		id_counter++;
	}

	int total_points = points.size();
    int total_values = 16;       
    int K = 27;                   
    int max_iterations = 1000000; 
	
	auto begin = chrono::high_resolution_clock::now();

	// for (int i=0; i<10; i++) {
	// 	auto begin = chrono::high_resolution_clock::now();
	// 	KMeans kmeans(K, total_points, total_values, max_iterations);
	// 	kmeans.run(points);
	// 	auto end = chrono::high_resolution_clock::now();
	// 	cout << "Time taken for iteration " << i+1 << " = " << chrono::duration_cast<chrono::microseconds>(end-begin).count() << " microseconds" << endl;
	// 	total_time += std::chrono::duration_cast<std::chrono::microseconds>(end-begin);
	// }


	KMeans kmeans(K, total_points, total_values, max_iterations);
	kmeans.run(points);

	auto end = chrono::high_resolution_clock::now();

	cout << "Total time: "<< chrono::duration_cast<chrono::microseconds>(end-begin).count() <<"\n";
	return 0;
}
