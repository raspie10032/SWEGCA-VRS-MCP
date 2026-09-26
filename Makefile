CXX ?= c++
CPPFLAGS ?=
CXXFLAGS ?= -O3
BUILD := build
INCLUDES := -Icpp
CORE_FLAGS := -std=c++20 -ffp-contract=off -Wall -Wextra -Wpedantic
CORE_SOURCES := cpp/swegca_architecture/evidence_rules.cpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/strong_types.cpp
CORE_HEADERS := $(wildcard cpp/swegca_architecture/*.hpp)
VRS_HEADERS := $(wildcard cpp/vrs/*.hpp)
VRS_SOURCES := cpp/vrs/experience_block.cpp cpp/vrs/evidence_experience.cpp cpp/vrs/connection.cpp cpp/vrs/session_store.cpp cpp/vrs/persistent_connection.cpp

.PHONY: all check bench clean
all: $(BUILD)/core-tests

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/core-tests: tests/core_tests.cpp $(CORE_SOURCES) $(CORE_HEADERS) cpp/vrs/verification.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(CORE_SOURCES) -o $@

$(BUILD)/core-bench: benchmarks/core_bench.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(CORE_SOURCES) -o $@

$(BUILD)/vrs-tests: tests/vrs_tests.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(CORE_SOURCES) -o $@

$(BUILD)/experience-block-tests: tests/experience_block_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/memory-budget-tests: tests/memory_budget_tests.cpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -pthread $(INCLUDES) $< -o $@

$(BUILD)/connection-tests: tests/connection_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -pthread $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/session-tests: tests/session_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -pthread $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/persistent-connection-tests: tests/persistent_connection_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pwrite -o $@

check: $(BUILD)/core-tests $(BUILD)/vrs-tests $(BUILD)/experience-block-tests $(BUILD)/memory-budget-tests $(BUILD)/connection-tests $(BUILD)/session-tests $(BUILD)/persistent-connection-tests
	./$(BUILD)/core-tests
	./$(BUILD)/vrs-tests
	./$(BUILD)/experience-block-tests
	./$(BUILD)/memory-budget-tests
	./$(BUILD)/connection-tests
	./$(BUILD)/session-tests
	./$(BUILD)/persistent-connection-tests
	CXX="$(CXX)" python3 tests/compile_contract.py

bench: $(BUILD)/core-bench
	./$(BUILD)/core-bench

clean:
	rm -f $(BUILD)/core-tests $(BUILD)/core-bench $(BUILD)/vrs-tests $(BUILD)/experience-block-tests $(BUILD)/memory-budget-tests $(BUILD)/connection-tests $(BUILD)/session-tests $(BUILD)/persistent-connection-tests
