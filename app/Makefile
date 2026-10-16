CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS += -I../include
LDLIBS += -lsqlite3 -lcrypto -pthread

COMMON = allocator.cpp database.cpp

.PHONY: all clean

all: emergency_app ea_tests

emergency_app: main.cpp $(COMMON) allocator.hpp database.hpp ../include/ea_ioctl.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) main.cpp $(COMMON) -o $@ $(LDLIBS)

ea_tests: tests.cpp $(COMMON) allocator.hpp database.hpp ../include/ea_ioctl.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests.cpp $(COMMON) -o $@ $(LDLIBS)

clean:
	rm -f emergency_app ea_tests
