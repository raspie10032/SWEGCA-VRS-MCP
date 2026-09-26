CXX ?= c++
CPPFLAGS ?=
CXXFLAGS ?= -O3
BUILD := build
INCLUDES := -Icpp
CORE_FLAGS := -pthread -std=c++20 -ffp-contract=off -Wall -Wextra -Wpedantic
CORE_SOURCES := cpp/swegca_architecture/evidence_rules.cpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/strong_types.cpp
CORE_HEADERS := $(wildcard cpp/swegca_architecture/*.hpp)
VRS_HEADERS := $(wildcard cpp/vrs/*.hpp)
VRS_SOURCES := cpp/vrs/runtime.cpp cpp/vrs/main_sources.cpp cpp/vrs/main_graph.cpp cpp/vrs/persistent_main_graph.cpp cpp/vrs/experience_block.cpp cpp/vrs/evidence_experience.cpp cpp/vrs/connection.cpp cpp/vrs/session_store.cpp cpp/vrs/persistent_connection.cpp cpp/vrs/connection_catalog.cpp cpp/vrs/session_runtime.cpp

.PHONY: all check check-stdio bench clean
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
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pwrite -o $@

$(BUILD)/transfer-budget-tests: tests/transfer_budget_tests.cpp cpp/vrs/transfer_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/memory-budget-tests: tests/memory_budget_tests.cpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -pthread $(INCLUDES) $< -o $@

$(BUILD)/connection-tests: tests/connection_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -pthread $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/session-tests: tests/session_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -pthread $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/persistent-connection-tests: tests/persistent_connection_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pwrite -o $@

$(BUILD)/catalog-tests: tests/catalog_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=rename -Wl,--wrap=fsync -o $@

$(BUILD)/session-runtime-tests: tests/session_runtime_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/main-graph-tests: tests/main_graph_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/persistent-main-tests: tests/persistent_main_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fdatasync -o $@

$(BUILD)/main-sources-tests: tests/main_sources_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -o $@

$(BUILD)/runtime-tests: tests/runtime_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/swegca-vrs-mcp: cpp/transport/stdio_main.cpp cpp/transport/json.cpp cpp/transport/json.hpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/parallel-main-tests: tests/parallel_main_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -ldl -o $@

check-stdio: $(BUILD)/swegca-vrs-mcp
	python3 tests/stdio_tests.py $(BUILD)/swegca-vrs-mcp

check: $(BUILD)/transfer-budget-tests $(BUILD)/parallel-main-tests $(BUILD)/runtime-tests $(BUILD)/main-sources-tests $(BUILD)/core-tests $(BUILD)/vrs-tests $(BUILD)/experience-block-tests $(BUILD)/memory-budget-tests $(BUILD)/connection-tests $(BUILD)/session-tests $(BUILD)/persistent-connection-tests $(BUILD)/catalog-tests $(BUILD)/session-runtime-tests $(BUILD)/main-graph-tests $(BUILD)/persistent-main-tests
	./$(BUILD)/transfer-budget-tests
	./$(BUILD)/parallel-main-tests
	./$(BUILD)/runtime-tests
	./$(BUILD)/main-sources-tests
	./$(BUILD)/core-tests
	./$(BUILD)/vrs-tests
	./$(BUILD)/experience-block-tests
	./$(BUILD)/memory-budget-tests
	./$(BUILD)/connection-tests
	./$(BUILD)/session-tests
	./$(BUILD)/persistent-connection-tests
	./$(BUILD)/catalog-tests
	./$(BUILD)/session-runtime-tests
	./$(BUILD)/main-graph-tests
	./$(BUILD)/persistent-main-tests
	CXX="$(CXX)" python3 tests/compile_contract.py

bench: $(BUILD)/core-bench
	./$(BUILD)/core-bench

clean:
	rm -f $(BUILD)/transfer-budget-tests $(BUILD)/parallel-main-tests $(BUILD)/swegca-vrs-mcp $(BUILD)/runtime-tests $(BUILD)/main-sources-tests $(BUILD)/core-tests $(BUILD)/core-bench $(BUILD)/vrs-tests $(BUILD)/experience-block-tests $(BUILD)/memory-budget-tests $(BUILD)/connection-tests $(BUILD)/session-tests $(BUILD)/persistent-connection-tests $(BUILD)/catalog-tests $(BUILD)/session-runtime-tests $(BUILD)/main-graph-tests $(BUILD)/persistent-main-tests
