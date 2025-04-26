// Parallel Implementation of the KMeans Algorithm
// Based on: https://github.com/marcoscastro/kmeans
// Parallelized using OpenMP

#include <iostream>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <string>
#include <omp.h>
#include <random>
#include <sstream>

using namespace std;

class Point
{
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
        this->values = values;
        this->name = name;
        id_cluster = -1;
    }

    int getID() const { return id_point; }
    void setCluster(int id_cluster) { this->id_cluster = id_cluster; }
    int getCluster() const { return id_cluster; }
    double getValue(int index) const { return values[index]; }
    int getTotalValues() const { return total_values; }
    const vector<double>& getValues() const { return values; }
    string getName() const { return name; }
};

class Cluster
{
private:
    int id_cluster;
    vector<double> central_values;
    vector<int> point_ids; // Store only point IDs instead of entire Points

public:
    Cluster(int id_cluster, const Point& point)
    {
        this->id_cluster = id_cluster;
        central_values = point.getValues();
        point_ids.push_back(point.getID());
    }

    void addPoint(int point_id) { point_ids.push_back(point_id); }

    bool removePoint(int id_point)
    {
        auto it = find(point_ids.begin(), point_ids.end(), id_point);
        if (it != point_ids.end()) {
            point_ids.erase(it);
            return true;
        }
        return false;
    }

    double getCentralValue(int index) const { return central_values[index]; }
    void setCentralValue(int index, double value) { central_values[index] = value; }
    const vector<double>& getCentralValues() const { return central_values; }
    int getPointID(int index) const { return point_ids[index]; }
    int getTotalPoints() const { return point_ids.size(); }
    int getID() const { return id_cluster; }
    void clearPoints() { point_ids.clear(); }
};

class KMeans
{
private:
    int K; // number of clusters
    int total_values, total_points, max_iterations;
    vector<Cluster> clusters;
    std::mt19937 gen; // Random number generator

    // Calculate squared Euclidean distance between a point and cluster center
    double calculateSquaredDistance(const Point& point, const vector<double>& center)
    {
        double sum = 0.0;
        #pragma omp simd reduction(+:sum)
        for(int i = 0; i < total_values; i++)
        {
            double diff = center[i] - point.getValue(i);
            sum += diff * diff;
        }
        return sum;
    }

    // Return ID of nearest center (uses euclidean distance)
    int getIDNearestCenter(const Point& point)
    {
        double min_dist = calculateSquaredDistance(point, clusters[0].getCentralValues());
        int id_cluster_center = 0;

        for(int i = 1; i < K; i++)
        {
            double dist = calculateSquaredDistance(point, clusters[i].getCentralValues());
            if(dist < min_dist)
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
        this->gen = std::mt19937(714); // Set the specified seed for reproducibility
    }

    void run(vector<Point>& points)
    {
        auto begin = chrono::high_resolution_clock::now();

        if(K > total_points) {
            cout << "Error: K cannot be greater than total points" << endl;
            return;
        }

        // Initialize cluster centers
        vector<int> prohibited_indexes;
        std::uniform_int_distribution<> distrib(0, total_points - 1);

        // Choose K distinct values for the centers of the clusters
        for(int i = 0; i < K; i++)
        {
            while(true)
            {
                int index_point = distrib(gen);

                if(find(prohibited_indexes.begin(), prohibited_indexes.end(), index_point) == prohibited_indexes.end())
                {
                    prohibited_indexes.push_back(index_point);
                    points[index_point].setCluster(i);
                    Cluster cluster(i, points[index_point]);
                    clusters.push_back(cluster);
                    break;
                }
            }
        }

        auto end_phase1 = chrono::high_resolution_clock::now();

        int iter = 1;
        
        // Allocate space for sums and counts for parallel reduction
        vector<vector<double>> new_centroids(K, vector<double>(total_values, 0.0));
        vector<int> cluster_sizes(K, 0);

        while(true)
        {
            bool done = true;

            // Reset cluster points and centroids for next iteration
            for (int i = 0; i < K; i++) {
                clusters[i].clearPoints();
                fill(new_centroids[i].begin(), new_centroids[i].end(), 0.0);
            }
            
            fill(cluster_sizes.begin(), cluster_sizes.end(), 0);

            // Associates each point to the nearest center in parallel
            #pragma omp parallel
            {
                vector<vector<double>> local_sums(K, vector<double>(total_values, 0.0));
                vector<int> local_counts(K, 0);

                #pragma omp for reduction(&&:done) schedule(dynamic, 64)
                for(int i = 0; i < total_points; i++)
                {
                    int id_old_cluster = points[i].getCluster();
                    int id_nearest_center = getIDNearestCenter(points[i]);

                    if(id_old_cluster != id_nearest_center)
                    {
                        points[i].setCluster(id_nearest_center);
                        done = false;
                    }
                    
                    // Add this point's values to the local sums
                    int cluster_id = points[i].getCluster();
                    local_counts[cluster_id]++;
                    
                    for(int j = 0; j < total_values; j++) {
                        local_sums[cluster_id][j] += points[i].getValue(j);
                    }
                }

                // Critical section to update cluster centers safely
                #pragma omp critical
                {
                    for(int i = 0; i < K; i++) {
                        for(int j = 0; j < total_values; j++) {
                            new_centroids[i][j] += local_sums[i][j];
                        }
                        cluster_sizes[i] += local_counts[i];
                    }
                }
            }
            
            // Update cluster centers
            for(int i = 0; i < K; i++) {
                if(cluster_sizes[i] > 0) {
                    for(int j = 0; j < total_values; j++) {
                        clusters[i].setCentralValue(j, new_centroids[i][j] / cluster_sizes[i]);
                    }
                }
            }

            // Add points to their final clusters for this iteration
            for(int i = 0; i < total_points; i++) {
                int cluster_id = points[i].getCluster();
                clusters[cluster_id].addPoint(i);
            }

            // Check termination criteria
            if(done || iter >= max_iterations)
            {
                cout << "Break in iteration " << iter << "\n\n";
                break;
            }

            iter++;
        }
        
        auto end = chrono::high_resolution_clock::now();

        // Shows elements of clusters
        for(int i = 0; i < K; i++)
        {
            int total_points_cluster = clusters[i].getTotalPoints();

            cout << "Cluster " << clusters[i].getID() + 1 << endl;
            for(int j = 0; j < total_points_cluster && j < 10; j++) // Limit output to avoid overwhelming stdout
            {
                int point_id = clusters[i].getPointID(j);
                cout << "Point " << point_id + 1 << ": ";
                for(int p = 0; p < total_values; p++)
                    cout << points[point_id].getValue(p) << " ";

                string point_name = points[point_id].getName();

                if(point_name != "")
                    cout << "- " << point_name;

                cout << endl;
            }
            
            if(total_points_cluster > 10) {
                cout << "... and " << (total_points_cluster - 10) << " more points" << endl;
            }

            cout << "Cluster values: ";

            for(int j = 0; j < total_values; j++)
                cout << clusters[i].getCentralValue(j) << " ";

            cout << "\n\n";
        }

        // Calculate total time in milliseconds
        auto time_ms = chrono::duration_cast<chrono::milliseconds>(end - begin).count();
        cout << "Total time: " << time_ms << endl;

        // Output additional timing information in microseconds for analysis
        cout << "TIME PHASE 1 = " << std::chrono::duration_cast<std::chrono::microseconds>(end_phase1 - begin).count() << "\n";
        cout << "TIME PHASE 2 = " << std::chrono::duration_cast<std::chrono::microseconds>(end - end_phase1).count() << "\n";
    }
};

int main(int argc, char *argv[])
{
    int total_points, total_values, K, max_iterations, has_name;

    // Parse first line with metadata
    string first_line;
    getline(cin, first_line);
    istringstream iss(first_line);
    iss >> total_points >> total_values >> K >> max_iterations >> has_name;

    vector<Point> points;
    points.reserve(total_points);  // Pre-allocate memory
    string point_name;

    // Parse data points
    for(int i = 0; i < total_points; i++)
    {
        string line;
        getline(cin, line);
        istringstream line_stream(line);
        
        vector<double> values;
        values.reserve(total_values);  // Pre-allocate memory
        
        double value;
        char comma;
        
        for(int j = 0; j < total_values; j++)
        {
            if(j > 0) line_stream >> comma;  // Skip comma
            if(!(line_stream >> value)) {
                cout << "Error reading value at point " << i << ", dimension " << j << endl;
                return 1;
            }
            values.push_back(value);
        }

        if(has_name)
        {
            line_stream >> point_name;
            Point p(i, values, point_name);
            points.push_back(p);
        }
        else
        {
            Point p(i, values);
            points.push_back(p);
        }
    }

    KMeans kmeans(K, total_points, total_values, max_iterations);
    kmeans.run(points);

    return 0;
}