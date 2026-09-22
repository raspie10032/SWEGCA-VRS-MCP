CXX ?= g++
AR ?= ar
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wpedantic

CORE_OBJECTS = build/digest.o build/json.o build/unicode.o build/keys.o build/observation.o build/journal_frame.o build/journal_files.o

.PHONY: all clean
all: build/libswegca-vrs.a

build:
	mkdir -p build

build/digest.o: cpp/digest.cpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/json.o: cpp/json.cpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/unicode.o: cpp/unicode.cpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/keys.o: cpp/keys.cpp cpp/keys.hpp cpp/unicode.hpp cpp/unicode_tables.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/observation.o: cpp/observation.cpp cpp/observation.hpp cpp/json.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/journal_frame.o: cpp/journal_frame.cpp cpp/journal_frame.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/journal_files.o: cpp/journal_files.cpp cpp/journal_files.hpp cpp/journal_frame.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/libswegca-vrs.a: $(CORE_OBJECTS)
	$(AR) rcs $@ $(CORE_OBJECTS)

clean:
	rm -rf build
