CXXFLAGS = -std=c++17 -Wall -O3

all: test-sequential test-parallel

test-sequential:
	g++ $(CXXFLAGS) test/test-sequential.cpp -o bin/test-sequential

test-parallel:
	g++ $(CXXFLAGS) test/test-parallel.cpp -o bin/test-parallel

clean:
	rm bin/*
