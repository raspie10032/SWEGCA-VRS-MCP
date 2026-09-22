CXX ?= g++
AR ?= ar
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wpedantic

CORE_OBJECTS = build/digest.o build/json.o build/unicode.o build/keys.o build/observation.o build/memory_episode.o build/memory_evidence.o build/hot_index.o build/hot_index_pending.o build/memory_vrs_pair.o build/deja_vu.o build/journal_frame.o build/journal_files.o build/owner_lock.o build/native_journal.o

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

build/memory_episode.o: cpp/memory_episode.cpp cpp/memory_episode.hpp cpp/keys.hpp cpp/unicode.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/memory_evidence.o: cpp/memory_evidence.cpp cpp/memory_evidence.hpp cpp/memory_episode.hpp cpp/json.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/hot_index.o: cpp/hot_index.cpp cpp/hot_index.hpp cpp/memory_episode.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/hot_index_pending.o: cpp/hot_index_pending.cpp cpp/hot_index_pending.hpp cpp/hot_index.hpp cpp/memory_episode.hpp cpp/json.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/memory_vrs_pair.o: cpp/memory_vrs_pair.cpp cpp/memory_vrs_pair.hpp cpp/hot_index.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/deja_vu.o: cpp/deja_vu.cpp cpp/deja_vu.hpp cpp/hot_index.hpp cpp/keys.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/journal_frame.o: cpp/journal_frame.cpp cpp/journal_frame.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/journal_files.o: cpp/journal_files.cpp cpp/journal_files.hpp cpp/journal_frame.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/owner_lock.o: cpp/owner_lock.cpp cpp/owner_lock.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/native_journal.o: cpp/native_journal.cpp cpp/native_journal.hpp cpp/journal_files.hpp cpp/owner_lock.hpp cpp/json.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/libswegca-vrs.a: $(CORE_OBJECTS)
	$(AR) rcs $@ $(CORE_OBJECTS)

clean:
	rm -rf build
