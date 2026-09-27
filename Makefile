CXX ?= c++
CPPFLAGS ?=
CXXFLAGS ?= -O3
BUILD := build
INCLUDES := -Icpp
CORE_FLAGS := -pthread -std=c++20 -ffp-contract=off -Wall -Wextra -Wpedantic
CORE_SOURCES := cpp/swegca_architecture/evidence_rules.cpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/strong_types.cpp
CORE_HEADERS := $(wildcard cpp/swegca_architecture/*.hpp)
VRS_HEADERS := $(wildcard cpp/vrs/*.hpp)
VRS_SOURCES := cpp/vrs/experience_page.cpp cpp/vrs/runtime.cpp cpp/vrs/main_sources.cpp cpp/vrs/main_graph.cpp cpp/vrs/persistent_main_graph.cpp cpp/vrs/experience_block.cpp cpp/vrs/evidence_experience.cpp cpp/vrs/connection.cpp cpp/vrs/session_store.cpp cpp/vrs/persistent_connection.cpp cpp/vrs/connection_catalog.cpp cpp/vrs/session_runtime.cpp

.PHONY: check-agent-event all check check-stdio check-sha256 check-resource-profile check-oom-recovery bench clean
all: $(BUILD)/core-tests

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/core-tests: tests/core_tests.cpp $(CORE_SOURCES) $(CORE_HEADERS) cpp/vrs/verification.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(CORE_SOURCES) -o $@

$(BUILD)/core-bench: benchmarks/core_bench.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(CORE_SOURCES) -o $@

$(BUILD)/input-cue-bench: benchmarks/input_cue_bench.cpp cpp/swegca_architecture/sha256.cpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/resource-profile-probe: tests/resource_profile_probe.cpp cpp/transport/resource_profile.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/sha256-vectors: tests/sha256_vectors.cpp cpp/swegca_architecture/sha256.cpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/sha256-vectors-scalar: tests/sha256_vectors.cpp cpp/swegca_architecture/sha256.cpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -DSWEGCA_SHA256_SCALAR_ONLY $(INCLUDES) $< cpp/swegca_architecture/sha256.cpp -o $@

check-sha256: $(BUILD)/sha256-vectors $(BUILD)/sha256-vectors-scalar
	python3 tests/check_sha256_vectors.py $(BUILD)/sha256-vectors $(BUILD)/sha256-vectors-scalar

$(BUILD)/input-recall-bench: benchmarks/input_recall_bench.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -DSWEGCA_RECALL_ENTRY_PROBE $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/recall-scale-bench: benchmarks/recall_scale_bench.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -DSWEGCA_RECALL_ENTRY_PROBE $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/vrs-tests: tests/vrs_tests.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(CORE_SOURCES) -o $@

$(BUILD)/experience-block-tests: tests/experience_block_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pwrite -Wl,--wrap=pread -o $@

$(BUILD)/transfer-budget-tests: tests/transfer_budget_tests.cpp cpp/vrs/transfer_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/memory-budget-tests: tests/memory_budget_tests.cpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -pthread $(INCLUDES) $< -o $@

$(BUILD)/background-recall-bench: benchmarks/background_recall_bench.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -DSWEGCA_RECALL_ENTRY_PROBE -DSWEGCA_BACKGROUND_WORK_PROBE $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/connection-tests: tests/connection_tests.cpp tests/refinement_digest_vectors.hpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -pthread $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/session-tests: tests/session_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -pthread $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=rename -Wl,--wrap=fsync -o $@

$(BUILD)/persistent-connection-tests: tests/persistent_connection_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pwrite -Wl,--wrap=open -o $@

$(BUILD)/catalog-tests: tests/catalog_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=rename -Wl,--wrap=fsync -o $@

$(BUILD)/session-runtime-tests: tests/session_runtime_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/main-graph-tests: tests/main_graph_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/persistent-main-tests: tests/persistent_main_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fdatasync -Wl,--wrap=fsync -o $@

$(BUILD)/main-sources-tests: tests/main_sources_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -o $@

$(BUILD)/runtime-tests: tests/runtime_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/async-runtime-tests: tests/async_runtime_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -ldl -o $@

check: $(BUILD)/multi-session-runtime-tests $(BUILD)/async-runtime-tests $(BUILD)/agent-event-tests

$(BUILD)/swegca-vrs-mcp: cpp/transport/stdio_main.cpp cpp/transport/ingress_probe.hpp cpp/transport/stdio_frames.hpp cpp/transport/app_server_requests.hpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/transport/resource_profile.hpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/parallel-main-tests: tests/parallel_main_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -ldl -o $@

$(BUILD)/parallel-recovery-tests: tests/parallel_recovery_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -ldl -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

check-oom-recovery: $(BUILD)/swegca-vrs-mcp
	python3 tests/check_oom_recovery.py $(BUILD)/swegca-vrs-mcp

check-resource-profile: $(BUILD)/resource-profile-probe $(BUILD)/swegca-vrs-mcp $(BUILD)/swegca-codex-wrapper $(BUILD)/swegca-desktop-host $(BUILD)/app-server-pump-tests $(BUILD)/swegca-app-server-proxy
	python3 tests/check_resource_profile.py $(BUILD)/resource-profile-probe
	python3 tests/stdio_tests.py $(BUILD)/swegca-vrs-mcp --limited

check-stdio: $(BUILD)/json-stream-tests-scalar $(BUILD)/stdio-frame-tests $(BUILD)/json-stream-tests $(BUILD)/swegca-codex-wrapper $(BUILD)/swegca-desktop-host $(BUILD)/swegca-vrs-mcp $(BUILD)/app-server-pump-tests $(BUILD)/swegca-app-server-proxy
	./$(BUILD)/stdio-frame-tests
	./$(BUILD)/json-stream-tests
	./$(BUILD)/json-stream-tests-scalar
	python3 tests/codex_wrapper_tests.py $(BUILD)/swegca-codex-wrapper
	python3 tests/stdio_tests.py $(BUILD)/swegca-vrs-mcp

check: $(BUILD)/parallel-recovery-tests $(BUILD)/transfer-budget-tests $(BUILD)/parallel-main-tests $(BUILD)/runtime-tests $(BUILD)/main-sources-tests $(BUILD)/core-tests $(BUILD)/vrs-tests $(BUILD)/experience-block-tests $(BUILD)/memory-budget-tests $(BUILD)/connection-tests $(BUILD)/session-tests $(BUILD)/persistent-connection-tests $(BUILD)/catalog-tests $(BUILD)/session-runtime-tests $(BUILD)/main-graph-tests $(BUILD)/persistent-main-tests
	./$(BUILD)/experience-page-tests
	./$(BUILD)/connection-regions-tests
	./$(BUILD)/multi-session-runtime-tests
	./$(BUILD)/app-server-pump-tests
	./$(BUILD)/socket-frames-tests
	./$(BUILD)/app-server-socket-tests
	./$(BUILD)/app-server-wire-tests
	./$(BUILD)/app-server-requests-tests
	./$(BUILD)/agent-event-tests
	./$(BUILD)/async-runtime-tests
	./$(BUILD)/transfer-budget-tests
	./$(BUILD)/parallel-recovery-tests
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
	rm -f $(BUILD)/background-recall-bench
	rm -f $(BUILD)/async-runtime-tests
	rm -f $(BUILD)/input-cue-bench $(BUILD)/json-quote-bench $(BUILD)/experience-page-tests $(BUILD)/json-parse-memory-bench $(BUILD)/wire-memory-bench $(BUILD)/commit-memory-bench $(BUILD)/json-stream-tests-scalar $(BUILD)/swegca-proxy-stages-probe $(BUILD)/swegca-vrs-stages-probe $(BUILD)/stdio-frame-tests $(BUILD)/swegca-vrs-ingress-probe $(BUILD)/json-stream-tests $(BUILD)/swegca-codex-wrapper $(BUILD)/swegca-desktop-host $(BUILD)/parallel-recovery-tests $(BUILD)/recall-scale-bench $(BUILD)/resource-profile-probe $(BUILD)/sha256-vectors $(BUILD)/sha256-vectors-scalar $(BUILD)/input-recall-bench $(BUILD)/transfer-budget-tests $(BUILD)/parallel-main-tests $(BUILD)/swegca-vrs-mcp $(BUILD)/runtime-tests $(BUILD)/main-sources-tests $(BUILD)/core-tests $(BUILD)/core-bench $(BUILD)/vrs-tests $(BUILD)/experience-block-tests $(BUILD)/memory-budget-tests $(BUILD)/connection-tests $(BUILD)/session-tests $(BUILD)/persistent-connection-tests $(BUILD)/catalog-tests $(BUILD)/session-runtime-tests $(BUILD)/main-graph-tests $(BUILD)/persistent-main-tests

$(BUILD)/agent-event-tests: tests/agent_event_tests.cpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(CORE_SOURCES) -o $@

check-agent-event: $(BUILD)/agent-event-tests
	./$(BUILD)/agent-event-tests

$(BUILD)/multi-session-runtime-tests: tests/multi_session_runtime_tests.cpp cpp/transport/agent_event.hpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/app-server-requests-tests: tests/app_server_requests_tests.cpp cpp/transport/app_server_requests.hpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp $(CORE_HEADERS) $(CORE_SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(CORE_SOURCES) -o $@

check: $(BUILD)/app-server-requests-tests

$(BUILD)/app-server-wire-tests: tests/app_server_wire_tests.cpp cpp/transport/replay_context.hpp cpp/transport/socket_frames.hpp cpp/transport/app_server_wire.hpp cpp/transport/app_server_requests.hpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp $(CORE_HEADERS) $(CORE_SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(CORE_SOURCES) -o $@

check: $(BUILD)/app-server-wire-tests

$(BUILD)/app-server-socket-tests: tests/app_server_socket_tests.cpp cpp/transport/app_server_wire.hpp cpp/transport/app_server_requests.hpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp $(CORE_HEADERS) $(CORE_SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(CORE_SOURCES) -Wl,--wrap=send -o $@

check: $(BUILD)/app-server-socket-tests

$(BUILD)/socket-frames-tests: tests/socket_frames_tests.cpp cpp/transport/socket_frames.hpp $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -Wl,--wrap=recv -o $@

check: $(BUILD)/socket-frames-tests

$(BUILD)/app-server-pump-tests: tests/app_server_pump_tests.cpp cpp/transport/ingress_probe.hpp cpp/transport/agent_event_commit.hpp cpp/transport/app_server_pump.hpp cpp/transport/socket_frames.hpp cpp/transport/app_server_wire.hpp cpp/transport/app_server_requests.hpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp $(CORE_HEADERS) $(CORE_SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(CORE_SOURCES) -o $@

check: $(BUILD)/app-server-pump-tests

$(BUILD)/swegca-app-server-proxy: cpp/transport/proxy_main.cpp $(wildcard cpp/transport/*.hpp) cpp/transport/json.cpp $(CORE_HEADERS) $(CORE_SOURCES) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(CORE_SOURCES) -o $@

$(BUILD)/connection-regions-tests: tests/connection_regions_tests.cpp cpp/vrs/connection_regions.hpp $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

check: $(BUILD)/connection-regions-tests

$(BUILD)/region-partition-bench: benchmarks/region_partition_bench.cpp cpp/swegca_architecture/region_partition_kernel.hpp cpp/swegca_architecture/recall_route_kernel.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/swegca-desktop-host: cpp/transport/desktop_host_main.cpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/transport/resource_profile.hpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp -o $@

$(BUILD)/swegca-codex-wrapper: cpp/transport/codex_wrapper_main.cpp cpp/transport/json.cpp cpp/transport/json.hpp $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp -o $@

$(BUILD)/json-stream-tests: tests/json_stream_tests.cpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp -o $@

$(BUILD)/swegca-vrs-ingress-probe: cpp/transport/stdio_main.cpp benchmarks/recall_entry_marker.cpp $(wildcard cpp/transport/*.hpp) cpp/transport/json.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -DSWEGCA_RECALL_ENTRY_PROBE $(INCLUDES) $< benchmarks/recall_entry_marker.cpp cpp/transport/json.cpp $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/stdio-frame-tests: tests/stdio_frames_tests.cpp cpp/transport/stdio_frames.hpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/swegca-vrs-stages-probe: cpp/transport/stdio_main.cpp benchmarks/recall_entry_marker.cpp $(wildcard cpp/transport/*.hpp) cpp/transport/json.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -DSWEGCA_RECALL_ENTRY_PROBE -DSWEGCA_INGRESS_STAGE_PROBE $(INCLUDES) $< benchmarks/recall_entry_marker.cpp cpp/transport/json.cpp $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/swegca-proxy-stages-probe: cpp/transport/proxy_main.cpp benchmarks/recall_entry_marker.cpp $(wildcard cpp/transport/*.hpp) cpp/transport/json.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -DSWEGCA_INGRESS_STAGE_PROBE $(INCLUDES) $< benchmarks/recall_entry_marker.cpp cpp/transport/json.cpp $(CORE_SOURCES) -o $@

$(BUILD)/json-stream-tests-scalar: tests/json_stream_tests.cpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -DSWEGCA_JSON_SCALAR_ONLY $(INCLUDES) $< cpp/transport/json.cpp -o $@

$(BUILD)/commit-memory-bench: benchmarks/commit_memory.cpp cpp/transport/agent_event_commit.hpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp -o $@

$(BUILD)/wire-memory-bench: benchmarks/wire_memory.cpp $(wildcard cpp/transport/*.hpp) cpp/transport/json.cpp $(CORE_SOURCES) $(CORE_HEADERS) cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(CORE_SOURCES) -o $@

$(BUILD)/json-parse-memory-bench: benchmarks/json_parse_memory.cpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp -o $@

$(BUILD)/experience-page-tests: tests/experience_page_tests.cpp cpp/vrs/experience_page.cpp cpp/vrs/experience_page.hpp cpp/vrs/evidence_experience.cpp cpp/vrs/experience_block.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/experience_page.cpp cpp/vrs/evidence_experience.cpp cpp/vrs/experience_block.cpp $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -Wl,--wrap=fdatasync -Wl,--wrap=fsync -o $@

check: $(BUILD)/experience-page-tests

$(BUILD)/json-quote-bench: benchmarks/json_quote_bench.cpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/vrs/memory_budget.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp -o $@
