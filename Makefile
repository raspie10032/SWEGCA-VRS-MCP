CXX ?= g++
AR ?= ar
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wpedantic

CORE_OBJECTS = build/digest.o build/json.o build/observation.o

.PHONY: all clean
all: build/libswegca-vrs.a

build:
	mkdir -p build

build/digest.o: cpp/digest.cpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/json.o: cpp/json.cpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/observation.o: cpp/observation.cpp cpp/observation.hpp cpp/json.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/libswegca-vrs.a: $(CORE_OBJECTS)
	$(AR) rcs $@ $(CORE_OBJECTS)

clean:
	rm -rf build
