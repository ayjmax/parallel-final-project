// Implementation of the KMeans Algorithm
// reference: https://github.com/marcoscastro/kmeans

#include <iostream>
#include <vector>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <algorithm>
#include <chrono>

using namespace std;

//class for a ponit
class Point
{
private:
	int id_point, id_cluster;//each point has an id and its cluster
	vector<double> values;//feature values of a point
	int total_values;//# of features
	string name;//name of point -- shouldnt be needed really?

public:
	//constructor to initialize points with properties
	Point(int id_point, vector<double>& values, string name = "")
	{
		this->id_point = id_point;
		total_values = values.size();

		for(int i = 0; i < total_values; i++)
			this->values.push_back(values[i]);

		this->name = name;
		id_cluster = -1;//initially point is homeless (no cluster ;))
	}

	//bunch of getters and setter blah blah blah the boring reuglar
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
};

//class for a Cluster
class Cluster
{
private:
	int id_cluster;//id for a cluster, should be unique
	vector<double> central_values;// centroid (cneter) of cluster
	vector<Point> points; //points of cluster

public:
	//constructor to give id and centroid 
	Cluster(int id_cluster, Point point)
	{
		this->id_cluster = id_cluster;

		int total_values = point.getTotalValues();
		//give centroid feature vals of first point
		for(int i = 0; i < total_values; i++)
			central_values.push_back(point.getValue(i));

		points.push_back(point);//add the point to cluster
	}

	//add point
	void addPoint(Point point)
	{
		points.push_back(point);
	}

	//removes point by id
	bool removePoint(int id_point)
	{
		int total_points = points.size();

		for(int i = 0; i < total_points; i++)
		{
			if(points[i].getID() == id_point)
			{
				points.erase(points.begin() + i);
				return true;
			}
		}
		return false;
	}

	void clearPoints(){
		points.clear();
	}

	double getCentralValue(int index)
	{
		return central_values[index];
	}

	void setCentralValue(int index, double value)
	{
		central_values[index] = value;
	}

	Point getPoint(int index)
	{
		return points[index];
	}

	int getTotalPoints()
	{
		return points.size();
	}

	int getID()
	{
		return id_cluster;
	}
};

//class for actually executing the algorithm
class KMeans
{
private:
	int K; // number of clusters
	int total_values, total_points, max_iterations; //# of features points iterations
	vector<Cluster> clusters;//list of each K

	// return ID of nearest center (uses euclidean distance)
	int getIDNearestCenter(Point point)
	{
		double sum = 0.0, min_dist;
		int id_cluster_center = 0;
		double diff=0;//to store diff between dist and cluster

		//calc dist to first cluster center
		for(int i = 0; i < total_values; i++)
		{
			diff = clusters[0].getCentralValue(i) - point.getValue(i);
			// sum += pow(clusters[0].getCentralValue(i) -
			// 		   point.getValue(i), 2.0); removign to remove a little overhead from math call. 
			sum += diff * diff;
		}

		// min_dist = sqrt(sum);
		min_dist = sum; //dont need the exact distance from sqrt so can reduce that call

		//compare dist with other cluster centers
		for(int i = 1; i < K; i++)
		{
			double dist;
			sum = 0.0;

			for(int j = 0; j < total_values; j++)
			{
				dist = clusters[i].getCentralValue(j) - point.getValue(j);
				// sum += pow(clusters[i].getCentralValue(j) -
				// 		   point.getValue(j), 2.0);removign to remove a little overhead from math call. 
				sum += dist* dist;
			}

			// dist = sqrt(sum);
			dist = sum;
			//update nearest cluster center if better one is found
			if(dist < min_dist)
			{
				min_dist = dist;
				id_cluster_center = i;
			}
		}

		return id_cluster_center;
	}

public:
	//constructor to initialize algortim
	KMeans(int K, int total_points, int total_values, int max_iterations)
	{
		this->K = K;
		this->total_points = total_points;
		this->total_values = total_values;
		this->max_iterations = max_iterations;
	}

	//runs the K-means alg
	void run(vector<Point> & points)
	{
        auto begin = chrono::high_resolution_clock::now();//times the prog

		if(K > total_points) //validate # of valid clsuters
			return;

		vector<int> prohibited_indexes;

		// choose K distinct values for the centers of the clusters
		for(int i = 0; i < K; i++)
		{
			while(true)
			{
				int index_point = rand() % total_points;
				//makes sure same point is not selected 2x
				if(find(prohibited_indexes.begin(), prohibited_indexes.end(),
						index_point) == prohibited_indexes.end())
				{
					prohibited_indexes.push_back(index_point);
					points[index_point].setCluster(i);
					Cluster cluster(i, points[index_point]);
					clusters.push_back(cluster);
					break;
				}
			}
		}
        auto end_phase1 = chrono::high_resolution_clock::now();//enc of lcock

		int iter = 1;
		//main loop 0_
		while(true)
		{
			bool done = true;
			int changed = 0; //counts how many points change cluster assignment

			// associates each point to the nearest center
			#pragma omp parallel for reduction(+:changed) //nearest cluster is indepednet of location of others, so its ok to do so here. 
			for(int i = 0; i < total_points; i++)
			{
				int id_old_cluster = points[i].getCluster();
				int id_nearest_center = getIDNearestCenter(points[i]);

				if(id_old_cluster != id_nearest_center)//if point changes, update assignment
				{
					points[i].setCluster(id_nearest_center);
					changed++;
				}
			}
			if(changed > 0)// no changed == done hell yeah
                done = false;
            else done = true;

			for(int i = 0; i < K; i++){//clear cluster's point vecotrs
				clusters[i].clearPoints();
			}
		
			//repopulate clusters with points based on updated assignments.
			for(int i = 0; i < total_points; i++){
				int cluster_id = points[i].getCluster();
				clusters[cluster_id].addPoint(points[i]);
			}

			// recalculating the center of each cluster
			#pragma omp parallel for //each cluster center is independent of others, so their calc can be done in parallel
			for(int i = 0; i < K; i++)
			{
				int total_points_cluster = clusters[i].getTotalPoints();//this check was inside the loop below before... not sure why? should be calced once 4 each cluster
				if(total_points_cluster == 0) continue; //trying to avoid branch misprediction penalty wt skipping empty cluster

				for(int j = 0; j < total_values; j++){
					double sum = 0.0;
					#pragma omp simd reduction(+:sum)
					for(int p = 0; p < total_points_cluster; p++) //sum features of vals of all points in calc
						sum += clusters[i].getPoint(p).getValue(j);
					clusters[i].setCentralValue(j, sum / total_points_cluster);///updates center
				}
			}

			if(done == true || iter >= max_iterations)//when nothing chagnes, or max iterations are reached
			{
				cout << "Break in iteration " << iter << "\n\n";
				break;
			}

			iter++;
		}
        auto end = chrono::high_resolution_clock::now();//ends lop

		// shows elements of clusters
		for(int i = 0; i < K; i++)
		{
			cout << "Cluster " << i+1 << " values: ";
			for(int j = 0; j < total_values; j++)
				cout << clusters[i].getCentralValue(j) << " ";
			cout << "\n";
		}

		cout << "\n\n";
		cout << "Total time: "<<std::chrono::duration_cast<std::chrono::microseconds>(end-begin).count()<<"\n";
		cout << "TIME PHASE 1 = "<<std::chrono::duration_cast<std::chrono::microseconds>(end_phase1-begin).count()<<"\n";
		cout << "TIME PHASE 2 = "<<std::chrono::duration_cast<std::chrono::microseconds>(end-end_phase1).count()<<"\n";
	}
};

//main exec of kmeans alg
int main(int argc, char *argv[])
{
	srand (42);//setting reandom generator

	int total_points, total_values, K, max_iterations, has_name;

	//read input
	cin >> total_points >> total_values >> K >> max_iterations >> has_name;

	vector<Point> points;
	string point_name;
	//datapoints read in
	for(int i = 0; i < total_points; i++)
	{
		vector<double> values;

		for(int j = 0; j < total_values; j++)
		{
			double value;
			cin >> value;
			values.push_back(value);
		}

		//if has name GRAB IT
		if(has_name)
		{
			cin >> point_name;
			Point p(i, values, point_name);
			points.push_back(p);
		}
		else
		{
			Point p(i, values);
			points.push_back(p);
		}
	}
	// instance of kmeans to run the algorithm fr
	KMeans kmeans(K, total_points, total_values, max_iterations);
	kmeans.run(points);

	return 0;
}