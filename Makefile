CXX ?= c++
ARCH ?= -march=native
CPPFLAGS += -Isrc
CXXFLAGS ?= -O3 -DNDEBUG
ARROW_PREFIX ?= /usr/local
ARROW_INCLUDE ?= $(ARROW_PREFIX)/include
ARROW_LIB ?= $(ARROW_PREFIX)/lib
ARROW_LINK ?= -lparquet -larrow

pipeline: src/main.cpp src/output.cpp $(wildcard src/*.hpp)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ARCH) -std=c++20 -pthread -I$(ARROW_INCLUDE) src/main.cpp src/output.cpp -L$(ARROW_LIB) -Wl,-rpath,$(ARROW_LIB) $(ARROW_LINK) -o $@
