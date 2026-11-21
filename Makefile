CXX ?= c++
ARCH ?= -march=native
CPPFLAGS += -Isrc
CXXFLAGS ?= -O3 -DNDEBUG

pipeline: src/main.cpp src/output.cpp $(wildcard src/*.hpp)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ARCH) -std=c++20 -pthread src/main.cpp src/output.cpp $(LDFLAGS) -o $@
