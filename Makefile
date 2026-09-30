CXX ?= c++
CPPFLAGS ?=
CXXFLAGS ?= -O3
BUILD := build
OPENBLAS_SO ?= $(firstword $(wildcard /usr/lib64/libopenblaso.so.0 /usr/lib/x86_64-linux-gnu/libopenblas.so.0))
.DEFAULT_GOAL := all
INCLUDES := -Icpp
CORE_FLAGS := -pthread -std=c++20 -ffp-contract=off -Wall -Wextra -Wpedantic
CORE_SOURCES := cpp/swegca_architecture/evidence_rules.cpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/strong_types.cpp
CORE_HEADERS := $(wildcard cpp/swegca_architecture/*.hpp)
VRS_HEADERS := $(wildcard cpp/vrs/*.hpp)
VRS_SOURCES := cpp/vrs/portal_page.cpp cpp/vrs/experience_page.cpp cpp/vrs/runtime.cpp cpp/vrs/main_sources.cpp cpp/vrs/main_graph.cpp cpp/vrs/persistent_main_graph.cpp cpp/vrs/experience_block.cpp cpp/vrs/evidence_experience.cpp cpp/vrs/connection.cpp cpp/vrs/session_store.cpp cpp/vrs/persistent_connection.cpp cpp/vrs/connection_catalog.cpp cpp/vrs/session_runtime.cpp
CHECKPOINT_HEADERS := $(wildcard cpp/checkpoint/*.hpp)
CHECKPOINT_SOURCES := cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_pickle.cpp \
	cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/checkpoint_profile.cpp \
	cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/materialized_tensor.cpp \
	cpp/checkpoint/prototype_checkpoint.cpp cpp/checkpoint/prototype_materialized_tensor.cpp
WORLD_HEADERS := $(wildcard cpp/world/*.hpp)
WORLD_SOURCES := cpp/world/cognitive_state.cpp cpp/world/cognitive_event.cpp \
	cpp/world/world_state.cpp cpp/world/definition_contract.cpp \
	cpp/world/evidence_accumulator.cpp cpp/world/evidence_revision.cpp \
	cpp/world/image_tag_experience.cpp \
	cpp/world/detached_vrs_state_update.cpp \
	cpp/world/vrs_event_signal.cpp \
	cpp/world/vrs_array_blocks.cpp \
	cpp/world/vrs_event_storage.cpp \
	cpp/world/vrs_event_delta.cpp \
	cpp/world/vrs_edge_address_index.cpp \
	cpp/world/vrs_canonicalization.cpp \
	cpp/world/vrs_sparse_lineage.cpp \
	cpp/world/term_address_index.cpp \
	cpp/world/proposition_directory.cpp \
	cpp/world/semantic_family_directory.cpp \
	cpp/world/snapshot_digest.cpp \
	cpp/world/memory_activation.cpp \
	cpp/world/hot_memory_step.cpp \
	cpp/world/hot_review_throughput.cpp \
	cpp/world/packed_memberships.cpp \
	cpp/world/semantic_speech_value.cpp \
	cpp/world/semantic_table_value.cpp \
	cpp/world/semantic_source_context.cpp \
	cpp/world/provider_cancellation.cpp \
	cpp/world/provider_transport.cpp \
	cpp/world/parallel_experience_transport.cpp \
	cpp/world/expression_wire.cpp \
	cpp/world/semantic_response_error.cpp \
	cpp/world/offline_semantic_batch.cpp \
	cpp/world/semantic_local_provider.cpp \
	cpp/world/configured_semantic_ingress.cpp \
	cpp/world/syllogism.cpp \
	cpp/world/syllogism_sources.cpp \
	cpp/world/utterance_receipt.cpp \
	cpp/world/semantic_encoding.cpp \
	cpp/world/semantic_input_scope.cpp \
	cpp/world/semantic_vrs_ingress.cpp \
	cpp/world/stage_timing.cpp \
	cpp/world/vrs_kernel_timing.cpp \
	cpp/world/vrs_cue_cost.cpp \
	cpp/world/vrs_region_arrays.cpp \
	cpp/world/transport_plain.cpp \
	cpp/world/session_semantic_binding.cpp \
	cpp/world/session_message_content.cpp \
	cpp/world/session_call_content.cpp \
	cpp/world/session_result_content.cpp \
	cpp/world/session_operation_content.cpp \
	cpp/world/session_event_index.cpp \
	cpp/world/session_document.cpp \
	cpp/world/session_occurrences.cpp \
	cpp/world/session_document_directory.cpp \
	cpp/world/prepared_session_cache.cpp \
	cpp/world/session_result_collection.cpp \
	cpp/world/session_speech_ingress.cpp \
	cpp/world/session_speech_segments.cpp \
	cpp/world/session_content_encoding.cpp \
	cpp/world/session_report_scope.cpp \
	cpp/world/semantic_event_append.cpp \
	cpp/world/counterfactual_replay.cpp \
	cpp/world/hypothesis_proposer.cpp cpp/world/sensor_definition.cpp \
	cpp/world/sensor_counterfactual.cpp \
	cpp/world/sensor_term_index.cpp cpp/world/unicode_nfkc.cpp \
	cpp/world/synapse_arbiter.cpp cpp/world/dynamic_cognition.cpp \
	cpp/world/bounded_world_write.cpp cpp/world/re_evidence_receipt.cpp \
	cpp/world/re_evidence_transaction.cpp cpp/world/re_evidence_arbitration.cpp \
	cpp/world/modal_to_world.cpp cpp/world/recurrent_cognition.cpp \
	cpp/world/prototype_recurrent_cognition.cpp

SESSION_BINDING_SUPPORT_SOURCES := cpp/world/session_speech_ingress.cpp \
	cpp/world/session_speech_segments.cpp \
	cpp/world/prepared_session_cache.cpp cpp/world/session_occurrences.cpp \
	cpp/world/session_document_directory.cpp cpp/world/session_document.cpp \
	cpp/world/session_event_index.cpp cpp/world/session_message_content.cpp \
	cpp/world/session_call_content.cpp cpp/world/session_result_content.cpp \
	cpp/world/session_operation_content.cpp cpp/world/unicode_nfkc.cpp cpp/transport/json.cpp

PRODUCTION_BINARIES := $(BUILD)/swegca-vrs-mcp $(BUILD)/swegca-content-observer $(BUILD)/swegca-codex-wrapper $(BUILD)/swegca-desktop-host $(BUILD)/swegca-app-server-proxy
MCP_PRODUCTION_CLOSURE_SOURCES := \
	cpp/swegca_architecture/agent_delivery_identity.hpp cpp/swegca_architecture/agent_event_kernel.hpp \
	cpp/swegca_architecture/connection_strength_kernel.hpp cpp/swegca_architecture/content_observation_kernel.hpp \
	cpp/swegca_architecture/core_platform.hpp cpp/swegca_architecture/digest_bytes.hpp \
	cpp/swegca_architecture/evidence_kernel.hpp cpp/swegca_architecture/evidence_observation_kernel.hpp \
	cpp/swegca_architecture/evidence_rules.cpp cpp/swegca_architecture/evidence_rules.hpp \
	cpp/swegca_architecture/evidence_scalar.hpp cpp/swegca_architecture/head_publication_kernel.hpp \
	cpp/swegca_architecture/input_cue.hpp cpp/swegca_architecture/input_span_kernel.hpp \
	cpp/swegca_architecture/metadata_residency_kernel.hpp cpp/swegca_architecture/numeric_contract.hpp \
	cpp/swegca_architecture/portal_range_kernel.hpp cpp/swegca_architecture/quoted_revision_kernel.hpp \
	cpp/swegca_architecture/recall_route_kernel.hpp cpp/swegca_architecture/record_address.hpp \
	cpp/swegca_architecture/region_partition_kernel.hpp cpp/swegca_architecture/replay_evidence_kernel.hpp \
	cpp/swegca_architecture/session_kernel.hpp cpp/swegca_architecture/sha256.cpp \
	cpp/swegca_architecture/sha256.hpp cpp/swegca_architecture/strong_types.cpp \
	cpp/swegca_architecture/strong_types.hpp cpp/transport/agent_event.hpp \
	cpp/transport/agent_query_socket.hpp cpp/transport/app_server_requests.hpp \
	cpp/transport/ingress_probe.hpp cpp/transport/input_candidates.hpp cpp/transport/json.cpp \
	cpp/transport/json.hpp cpp/transport/measurement_observation.hpp cpp/transport/requirement_anchor.hpp \
	cpp/transport/resource_profile.hpp cpp/transport/revision_references.hpp cpp/transport/socket_frames.hpp \
	cpp/transport/stdio_frames.hpp cpp/transport/stdio_main.cpp cpp/vrs/connection.cpp cpp/vrs/connection.hpp \
	cpp/vrs/connection_catalog.cpp cpp/vrs/connection_catalog.hpp cpp/vrs/connection_regions.hpp \
	cpp/vrs/evidence_executor.hpp cpp/vrs/evidence_experience.cpp cpp/vrs/evidence_experience.hpp \
	cpp/vrs/experience_block.cpp cpp/vrs/experience_block.hpp cpp/vrs/experience_page.cpp \
	cpp/vrs/experience_page.hpp cpp/vrs/experience_sequence.hpp cpp/vrs/main_graph.cpp cpp/vrs/main_graph.hpp \
	cpp/vrs/main_sources.cpp cpp/vrs/main_sources.hpp cpp/vrs/memory_budget.hpp \
	cpp/vrs/persistent_connection.cpp cpp/vrs/persistent_connection.hpp cpp/vrs/persistent_main_graph.cpp \
	cpp/vrs/persistent_main_graph.hpp cpp/vrs/portal_page.cpp cpp/vrs/portal_page.hpp cpp/vrs/replay_position.hpp \
	cpp/vrs/runtime.cpp cpp/vrs/runtime.hpp cpp/vrs/session_runtime.cpp cpp/vrs/session_runtime.hpp \
	cpp/vrs/session_store.cpp cpp/vrs/session_store.hpp cpp/vrs/shared_transfer_state.hpp \
	cpp/vrs/storage_budget.hpp cpp/vrs/storage_inventory.hpp cpp/vrs/transfer_budget.hpp cpp/vrs/verification.hpp
PRODUCTION_SUPPORT_CLOSURE_SOURCES := \
	cpp/transport/stdio_main.cpp cpp/transport/content_observer_main.cpp cpp/transport/codex_wrapper_main.cpp \
	cpp/transport/desktop_host_main.cpp cpp/transport/proxy_main.cpp cpp/transport/json.cpp \
	$(wildcard cpp/transport/*.hpp) cpp/vrs/file_observation.cpp cpp/vrs/file_observation.hpp \
	$(CHECKPOINT_HEADERS) $(CHECKPOINT_SOURCES) $(WORLD_HEADERS) $(WORLD_SOURCES)
PRODUCTION_GATE_CLOSURE_SOURCES := Makefile AGENTS.md tools/stage0_gate.py docs/stage0-external-p0.json \
	$(wildcard tests/*.cpp) $(wildcard tests/*.hpp) $(wildcard tests/*.py)
EXPERIMENTAL_CLOSURE_SOURCES := \
	cpp/swegca_architecture/association_scalar.hpp cpp/swegca_architecture/connection_growth_kernel.hpp \
	cpp/swegca_architecture/core_platform.hpp cpp/swegca_architecture/evidence_scalar.hpp \
	cpp/vrs/block_collectors.hpp cpp/vrs/block_ingress.cpp cpp/vrs/block_ingress.hpp \
	cpp/vrs/block_store.cpp cpp/vrs/block_store.hpp cpp/vrs/codec_input.cpp cpp/vrs/codec_input.hpp \
	cpp/vrs/codec_stream.cpp cpp/vrs/codec_stream.hpp cpp/vrs/collision_dispatch.cpp cpp/vrs/collision_dispatch.hpp \
	cpp/vrs/concept_growth.hpp cpp/vrs/device_evidence.cpp cpp/vrs/device_evidence.hpp \
	cpp/vrs/evidence_executor.hpp cpp/vrs/gpu_evidence.cpp cpp/vrs/gpu_evidence.hpp \
	cpp/vrs/input_collision.hpp cpp/vrs/octahedral_blocks.hpp cpp/vrs/parallel_ingress.hpp cpp/vrs/work_pipeline.hpp \
	tests/cooccurrence_tests.cpp tests/input_collision_tests.cpp tests/ternary_count_tests.cpp \
	tests/parallel_ingress_tests.cpp tests/gpu_evidence_tests.cpp tests/collision_dispatch_tests.cpp \
	tests/work_pipeline_tests.cpp tests/codec_input_tests.cpp tests/codec_pipeline_tests.cpp \
	tests/codec_stream_tests.cpp tests/concept_growth_tests.cpp tests/block_store_tests.cpp \
	tests/block_ingress_tests.cpp tests/block_session_tests.cpp tools/embed_gpu_core.py tools/whole_file_ingress.cpp
EXPERIMENTAL_CPU_TESTS := $(BUILD)/cooccurrence-tests $(BUILD)/input-collision-tests $(BUILD)/ternary-count-tests \
	$(BUILD)/parallel-ingress-tests $(BUILD)/codec-input-tests $(BUILD)/codec-stream-tests \
	$(BUILD)/concept-growth-tests $(BUILD)/block-store-tests
EXPERIMENTAL_GPU_TESTS := $(BUILD)/gpu-evidence-tests $(BUILD)/collision-dispatch-tests \
	$(BUILD)/work-pipeline-tests $(BUILD)/codec-pipeline-tests $(BUILD)/block-ingress-tests $(BUILD)/block-session-tests

# VRS experience/synapse maintenance builds without Runtime, SessionRuntime,
# transport, or the four-stage memory activation path.
SYNAPSE_SOURCES := cpp/vrs/block_store.cpp cpp/vrs/synapse.cpp cpp/vrs/experience_page.cpp cpp/vrs/experience_block.cpp cpp/vrs/evidence_experience.cpp cpp/vrs/connection.cpp cpp/vrs/session_store.cpp cpp/vrs/persistent_connection.cpp
SYNAPSE_OBJECTS := $(patsubst %.cpp,$(BUILD)/synapse/%.o,$(SYNAPSE_SOURCES) $(CORE_SOURCES))

.PHONY: check-synapse vrs-synapse
.PHONY: check-association
$(BUILD)/association-tests: tests/association_tests.cpp $(CORE_HEADERS) $(CORE_SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(CORE_SOURCES) -o $@

check-association: $(BUILD)/association-tests
	./$(BUILD)/association-tests

$(BUILD)/swegca-tag-associations: tools/tag_associations.cpp cpp/transport/json.cpp cpp/transport/json.hpp $(BUILD)/libswegca-vrs.a $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a -o $@

vrs-synapse: $(BUILD)/libswegca-vrs.a

$(BUILD)/synapse/%.o: %.cpp $(CORE_HEADERS) $(VRS_HEADERS)
	mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libswegca-vrs.a: $(SYNAPSE_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD)/synapse-tests: tests/synapse_tests.cpp $(BUILD)/libswegca-vrs.a $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(BUILD)/libswegca-vrs.a -Wl,--wrap=pwrite -o $@

check-synapse: $(BUILD)/synapse-tests
	./$(BUILD)/synapse-tests

.PHONY: check-raw-observation
$(BUILD)/raw-content-observation-tests: tests/raw_content_observation_tests.cpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/original-observation-tests: tests/original_observation_tests.cpp $(BUILD)/libswegca-vrs.a cpp/vrs/original_observation.hpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(BUILD)/libswegca-vrs.a -o $@

check-raw-observation: $(BUILD)/raw-content-observation-tests $(BUILD)/original-observation-tests
	./$(BUILD)/raw-content-observation-tests
	./$(BUILD)/original-observation-tests

.PHONY: check-agent-event all production check check-production check-checkpoint check-world check-prototype-recurrent-artifacts check-prototype-recurrent-activation check-experimental-cpu check-experimental-gpu stage0-gate check-stdio check-sha256 check-resource-profile check-oom-recovery bench clean
all: production

production: $(PRODUCTION_BINARIES)

$(BUILD)/swegca-content-observer: cpp/transport/content_observer_main.cpp cpp/transport/agent_query_client.hpp cpp/transport/socket_frames.hpp cpp/transport/requirement_anchor.hpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/transport/stdio_frames.hpp cpp/vrs/file_observation.cpp cpp/vrs/file_observation.hpp cpp/vrs/memory_budget.hpp cpp/vrs/transfer_budget.hpp cpp/vrs/shared_transfer_state.hpp $(CORE_HEADERS) cpp/swegca_architecture/sha256.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp cpp/vrs/file_observation.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/file-observation-tests: tests/file_observation_tests.cpp cpp/vrs/file_observation.cpp cpp/vrs/file_observation.hpp cpp/vrs/transfer_budget.hpp cpp/vrs/shared_transfer_state.hpp cpp/vrs/memory_budget.hpp $(CORE_HEADERS) cpp/swegca_architecture/sha256.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/file_observation.cpp cpp/swegca_architecture/sha256.cpp -Wl,--wrap=pread -o $@

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

$(BUILD)/transfer-budget-tests: tests/transfer_budget_tests.cpp cpp/vrs/transfer_budget.hpp cpp/vrs/shared_transfer_state.hpp | $(BUILD)
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

$(BUILD)/swegca-vrs-mcp: cpp/transport/stdio_main.cpp cpp/transport/input_candidates.hpp cpp/transport/measurement_observation.hpp cpp/transport/revision_references.hpp cpp/transport/agent_query_socket.hpp cpp/transport/socket_frames.hpp cpp/transport/requirement_anchor.hpp cpp/transport/ingress_probe.hpp cpp/transport/stdio_frames.hpp cpp/transport/app_server_requests.hpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/transport/resource_profile.hpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/parallel-main-tests: tests/parallel_main_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -ldl -o $@

$(BUILD)/parallel-recovery-tests: tests/parallel_recovery_tests.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -ldl -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

check-oom-recovery: $(BUILD)/swegca-vrs-mcp
	python3 tests/check_oom_recovery.py $(BUILD)/swegca-vrs-mcp

check-resource-profile: $(BUILD)/swegca-content-observer $(BUILD)/resource-profile-probe $(BUILD)/swegca-vrs-mcp $(BUILD)/swegca-codex-wrapper $(BUILD)/swegca-desktop-host $(BUILD)/app-server-pump-tests $(BUILD)/swegca-app-server-proxy
	python3 tests/check_resource_profile.py $(BUILD)/resource-profile-probe
	python3 tests/stdio_tests.py $(BUILD)/swegca-vrs-mcp --limited

check-stdio: $(BUILD)/swegca-content-observer $(BUILD)/json-stream-tests-scalar $(BUILD)/stdio-frame-tests $(BUILD)/json-stream-tests $(BUILD)/swegca-codex-wrapper $(BUILD)/swegca-desktop-host $(BUILD)/swegca-vrs-mcp $(BUILD)/app-server-pump-tests $(BUILD)/swegca-app-server-proxy
	./$(BUILD)/stdio-frame-tests
	./$(BUILD)/json-stream-tests
	./$(BUILD)/json-stream-tests-scalar
	python3 tests/codex_wrapper_tests.py $(BUILD)/swegca-codex-wrapper
	python3 tests/content_observer_tests.py $(BUILD)/swegca-content-observer
	python3 tests/stdio_tests.py $(BUILD)/swegca-vrs-mcp

check: $(BUILD)/parallel-recovery-tests $(BUILD)/transfer-budget-tests $(BUILD)/parallel-main-tests $(BUILD)/runtime-tests $(BUILD)/main-sources-tests $(BUILD)/core-tests $(BUILD)/vrs-tests $(BUILD)/experience-block-tests $(BUILD)/memory-budget-tests $(BUILD)/connection-tests $(BUILD)/session-tests $(BUILD)/persistent-connection-tests $(BUILD)/catalog-tests $(BUILD)/session-runtime-tests $(BUILD)/main-graph-tests $(BUILD)/persistent-main-tests
	./$(BUILD)/experience-page-tests
	./$(BUILD)/portal-page-tests
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
	rm -rf -- $(BUILD)

$(BUILD)/agent-event-tests: tests/agent_event_tests.cpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(CORE_SOURCES) -o $@

check-agent-event: $(BUILD)/agent-event-tests
	./$(BUILD)/agent-event-tests

$(BUILD)/multi-session-runtime-tests: tests/multi_session_runtime_tests.cpp cpp/transport/agent_event.hpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -Wl,--wrap=pread -Wl,--wrap=pwrite -o $@

$(BUILD)/app-server-requests-tests: tests/app_server_requests_tests.cpp cpp/transport/app_server_requests.hpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp $(CORE_HEADERS) $(CORE_SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(CORE_SOURCES) -o $@

check: $(BUILD)/app-server-requests-tests

$(BUILD)/app-server-wire-tests: tests/app_server_wire_tests.cpp cpp/transport/requirement_coverage.hpp cpp/transport/revision_references.hpp cpp/transport/input_candidates.hpp cpp/transport/requirement_anchor.hpp cpp/transport/replay_context.hpp cpp/transport/socket_frames.hpp cpp/transport/app_server_wire.hpp cpp/transport/app_server_requests.hpp cpp/transport/agent_event.hpp cpp/transport/json.cpp cpp/transport/json.hpp $(CORE_HEADERS) $(CORE_SOURCES) | $(BUILD)
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

$(BUILD)/swegca-desktop-host: cpp/transport/desktop_host_main.cpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/transport/resource_profile.hpp cpp/vrs/memory_budget.hpp cpp/vrs/shared_transfer_state.hpp | $(BUILD)
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

$(BUILD)/portal-page-tests: tests/portal_page_tests.cpp cpp/vrs/portal_page.cpp cpp/vrs/portal_page.hpp cpp/vrs/experience_block.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/portal_page.cpp cpp/vrs/experience_block.cpp $(CORE_SOURCES) -Wl,--wrap=pwrite -o $@

check: $(BUILD)/portal-page-tests

$(BUILD)/continuation-recovery-probe: benchmarks/continuation_recovery_probe.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/related-pages-bench: benchmarks/related_pages_bench.cpp $(VRS_SOURCES) $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -o $@

$(BUILD)/swegca-image-associations: tools/image_associations.cpp cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a -o $@

$(BUILD)/experience-pairs-tests: tests/experience_pairs_tests.cpp cpp/vrs/experience_pairs.hpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

.PHONY: check-experience-pairs
check-experience-pairs: $(BUILD)/experience-pairs-tests
	./$(BUILD)/experience-pairs-tests

$(BUILD)/swegca-tag-image-refinement: tools/tag_image_refinement.cpp cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a -o $@

$(BUILD)/swegca-tag-tag-refinement: tools/tag_tag_refinement.cpp cpp/transport/json.cpp cpp/transport/json.hpp $(BUILD)/libswegca-vrs.a $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a -o $@

$(BUILD)/cooccurrence-tests: tests/cooccurrence_tests.cpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/input-collision-tests: tests/input_collision_tests.cpp cpp/vrs/input_collision.hpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/swegca-input-collision: tools/input_collision.cpp cpp/vrs/input_collision.hpp cpp/transport/json.cpp cpp/transport/json.hpp $(BUILD)/libswegca-vrs.a $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a -o $@

$(BUILD)/ternary-count-tests: tests/ternary_count_tests.cpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/parallel-ingress-tests: tests/parallel_ingress_tests.cpp cpp/vrs/parallel_ingress.hpp $(VRS_SOURCES) $(CORE_SOURCES) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(VRS_SOURCES) $(CORE_SOURCES) -o $@


CUDA_ROOT ?=
CUDA_LIBDIR ?= $(CUDA_ROOT)/lib64
GPU_FLAGS = -I$(CUDA_ROOT)/include -I$(BUILD) -L$(CUDA_LIBDIR) -Wl,-rpath,$(CUDA_LIBDIR) -lnvrtc -lcuda

.PHONY: require-cuda-toolkit
require-cuda-toolkit:
	@test -n "$(CUDA_ROOT)" || { echo "CUDA_ROOT must name a system or source toolkit; venv/site-packages toolkits are forbidden" >&2; exit 2; }
	@case "$(CUDA_ROOT)" in *venv*|*site-packages*|*.whl*) echo "forbidden CUDA dependency origin: $(CUDA_ROOT)" >&2; exit 2;; esac
	@test -f "$(CUDA_ROOT)/include/nvrtc.h" || { echo "missing $(CUDA_ROOT)/include/nvrtc.h" >&2; exit 2; }
	@test -e "$(CUDA_LIBDIR)/libnvrtc.so" || { echo "missing $(CUDA_LIBDIR)/libnvrtc.so" >&2; exit 2; }
$(BUILD)/gpu_core_source.hpp: cpp/swegca_architecture/core_platform.hpp cpp/swegca_architecture/evidence_scalar.hpp cpp/swegca_architecture/association_scalar.hpp tools/embed_gpu_core.py | $(BUILD)
	python3 tools/embed_gpu_core.py

$(BUILD)/gpu-evidence-tests: tests/gpu_evidence_tests.cpp cpp/vrs/gpu_evidence.cpp $(BUILD)/gpu_core_source.hpp $(CORE_SOURCES) $(VRS_HEADERS) $(CORE_HEADERS) | $(BUILD) require-cuda-toolkit
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/gpu_evidence.cpp $(CORE_SOURCES) $(GPU_FLAGS) -o $@


$(BUILD)/collision-dispatch-tests: tests/collision_dispatch_tests.cpp cpp/vrs/collision_dispatch.cpp $(BUILD)/gpu_core_source.hpp $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD) require-cuda-toolkit
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/collision_dispatch.cpp $(GPU_FLAGS) -o $@

$(BUILD)/swegca-input-collision-parallel: tools/input_collision.cpp cpp/vrs/collision_dispatch.cpp $(BUILD)/gpu_core_source.hpp cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD) require-cuda-toolkit
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -fopenmp -DSWEGCA_PARALLEL_COLLISION $(INCLUDES) $< cpp/vrs/collision_dispatch.cpp cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a $(GPU_FLAGS) -o $@

$(BUILD)/work-pipeline-tests: tests/work_pipeline_tests.cpp cpp/vrs/work_pipeline.hpp cpp/vrs/device_evidence.cpp cpp/vrs/device_evidence.hpp $(BUILD)/gpu_core_source.hpp $(CORE_HEADERS) $(CORE_SOURCES) | $(BUILD) require-cuda-toolkit
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/device_evidence.cpp $(CORE_SOURCES) $(GPU_FLAGS) -o $@

$(BUILD)/codec-input-tests: tests/codec_input_tests.cpp cpp/vrs/codec_input.cpp cpp/vrs/codec_input.hpp cpp/transport/json.cpp $(CORE_SOURCES) $(CORE_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/codec_input.cpp cpp/transport/json.cpp $(CORE_SOURCES) -o $@

$(BUILD)/codec-pipeline-tests: tests/codec_pipeline_tests.cpp cpp/vrs/work_pipeline.hpp cpp/vrs/codec_input.cpp cpp/vrs/device_evidence.cpp cpp/transport/json.cpp $(BUILD)/gpu_core_source.hpp $(CORE_SOURCES) $(CORE_HEADERS) | $(BUILD) require-cuda-toolkit
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/codec_input.cpp cpp/vrs/device_evidence.cpp cpp/transport/json.cpp $(CORE_SOURCES) $(GPU_FLAGS) -o $@

$(BUILD)/compare-count-checkpoints: tools/compare_count_checkpoints.cpp cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a $(CORE_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/transport/json.cpp $(BUILD)/libswegca-vrs.a -o $@

$(BUILD)/concept-growth-tests: tests/concept_growth_tests.cpp cpp/vrs/concept_growth.hpp $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< -o $@

$(BUILD)/block-store-tests: tests/block_store_tests.cpp cpp/vrs/block_store.cpp cpp/vrs/experience_block.cpp $(CORE_SOURCES) $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/block_store.cpp cpp/vrs/experience_block.cpp $(CORE_SOURCES) -o $@

BLOCK_INGRESS_SOURCES := cpp/vrs/block_ingress.cpp cpp/vrs/block_store.cpp cpp/vrs/codec_input.cpp cpp/vrs/device_evidence.cpp cpp/vrs/experience_block.cpp cpp/transport/json.cpp $(CORE_SOURCES)
$(BUILD)/codec-stream-tests: tests/codec_stream_tests.cpp cpp/vrs/codec_stream.cpp cpp/vrs/codec_stream.hpp cpp/transport/json.cpp $(CORE_SOURCES) $(CORE_HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/vrs/codec_stream.cpp cpp/transport/json.cpp $(CORE_SOURCES) -o $@

$(BUILD)/whole-file-ingress $(BUILD)/whole-file-ingress-gpu: tools/whole_file_ingress.cpp $(BLOCK_INGRESS_SOURCES) $(BUILD)/gpu_core_source.hpp $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD) require-cuda-toolkit
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(if $(filter %gpu,$@),-DSWEGCA_GPU_CORE) $(INCLUDES) $< $(BLOCK_INGRESS_SOURCES) $(GPU_FLAGS) -o $@

$(BUILD)/block-ingress-tests: tests/block_ingress_tests.cpp $(BLOCK_INGRESS_SOURCES) $(BUILD)/gpu_core_source.hpp $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD) require-cuda-toolkit
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(BLOCK_INGRESS_SOURCES) $(GPU_FLAGS) -o $@

$(BUILD)/block-session-tests: tests/block_session_tests.cpp $(BLOCK_INGRESS_SOURCES) $(BUILD)/gpu_core_source.hpp $(CORE_HEADERS) $(VRS_HEADERS) | $(BUILD) require-cuda-toolkit
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(BLOCK_INGRESS_SOURCES) $(GPU_FLAGS) -o $@

$(BUILD)/restricted-zip-tests: tests/restricted_zip_tests.cpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_zip.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/checkpoint/restricted_zip.cpp -o $@

$(BUILD)/restricted-pickle-tests: tests/restricted_pickle_tests.cpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/restricted_pickle.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/checkpoint/restricted_pickle.cpp -o $@

$(BUILD)/canonical-symbolic-json-tests: tests/canonical_symbolic_json_tests.cpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/canonical_symbolic_json.hpp cpp/checkpoint/restricted_pickle.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/checkpoint/canonical_symbolic_json.cpp -o $@

$(BUILD)/checkpoint-profile-tests: tests/checkpoint_profile_tests.cpp cpp/checkpoint/checkpoint_profile.cpp cpp/checkpoint/checkpoint_profile.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/checkpoint/checkpoint_profile.cpp -o $@

$(BUILD)/restricted-checkpoint-tests: tests/restricted_checkpoint_tests.cpp $(CHECKPOINT_HEADERS) $(CHECKPOINT_SOURCES) cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< $(CHECKPOINT_SOURCES) cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/prototype-checkpoint-tests: tests/prototype_checkpoint_tests.cpp cpp/checkpoint/prototype_checkpoint.cpp cpp/checkpoint/prototype_checkpoint.hpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_checkpoint.hpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_zip.hpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/restricted_pickle.hpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/canonical_symbolic_json.hpp cpp/checkpoint/checkpoint_profile.cpp cpp/checkpoint/checkpoint_profile.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/checkpoint/prototype_checkpoint.cpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/checkpoint_profile.cpp cpp/swegca_architecture/sha256.cpp -o $@

check-checkpoint: $(BUILD)/restricted-zip-tests $(BUILD)/restricted-pickle-tests \
	$(BUILD)/canonical-symbolic-json-tests $(BUILD)/checkpoint-profile-tests \
	$(BUILD)/restricted-checkpoint-tests $(BUILD)/materialized-tensor-tests
	./$(BUILD)/restricted-zip-tests
	./$(BUILD)/restricted-pickle-tests
	./$(BUILD)/canonical-symbolic-json-tests
	./$(BUILD)/checkpoint-profile-tests
	./$(BUILD)/restricted-checkpoint-tests
	./$(BUILD)/materialized-tensor-tests

$(BUILD)/materialized-tensor-tests: tests/materialized_tensor_tests.cpp cpp/checkpoint/materialized_tensor.cpp cpp/checkpoint/materialized_tensor.hpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_checkpoint.hpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_zip.hpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/restricted_pickle.hpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/canonical_symbolic_json.hpp cpp/checkpoint/checkpoint_profile.cpp cpp/checkpoint/checkpoint_profile.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/checkpoint/materialized_tensor.cpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/checkpoint_profile.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/cognitive-state-tests: tests/cognitive_state_tests.cpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/cognitive_state.cpp -o $@

$(BUILD)/definition-contract-tests: tests/definition_contract_tests.cpp cpp/world/definition_contract.cpp cpp/world/definition_contract.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/definition_contract.cpp -o $@

$(BUILD)/hypothesis-proposer-tests: tests/hypothesis_proposer_tests.cpp cpp/world/hypothesis_proposer.cpp cpp/world/hypothesis_proposer.hpp cpp/world/cognitive_event.cpp cpp/world/cognitive_event.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/hypothesis_proposer.cpp cpp/world/cognitive_event.cpp cpp/world/cognitive_state.cpp cpp/world/evidence_accumulator.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/image-tag-experience-tests: tests/image_tag_experience_tests.cpp cpp/world/image_tag_experience.cpp cpp/world/image_tag_experience.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/world/unicode_word_data.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/image_tag_experience.cpp cpp/world/cognitive_state.cpp cpp/world/unicode_nfkc.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/detached-vrs-state-update-tests: tests/detached_vrs_state_update_tests.cpp cpp/world/detached_vrs_state_update.cpp cpp/world/detached_vrs_state_update.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/detached_vrs_state_update.cpp -o $@

$(BUILD)/vrs-event-signal-tests: tests/vrs_event_signal_tests.cpp cpp/world/vrs_event_signal.cpp cpp/world/vrs_event_signal.hpp cpp/world/detached_vrs_state_update.cpp cpp/world/detached_vrs_state_update.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/vrs_event_signal.cpp cpp/world/detached_vrs_state_update.cpp -o $@

$(BUILD)/vrs-array-blocks-tests: tests/vrs_array_blocks_tests.cpp cpp/world/vrs_array_blocks.cpp cpp/world/vrs_array_blocks.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp cpp/transport/json.cpp cpp/transport/json.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/vrs_array_blocks.cpp cpp/swegca_architecture/sha256.cpp cpp/transport/json.cpp -lz -o $@

$(BUILD)/vrs-event-storage-tests: tests/vrs_event_storage_tests.cpp cpp/world/vrs_event_storage.cpp cpp/world/vrs_event_storage.hpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_delta.hpp cpp/world/persistent_event_vector.hpp cpp/world/vrs_array_blocks.cpp cpp/world/vrs_array_blocks.hpp cpp/world/vrs_event_signal.cpp cpp/world/vrs_event_signal.hpp cpp/world/detached_vrs_state_update.cpp cpp/world/detached_vrs_state_update.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp cpp/transport/json.cpp cpp/transport/json.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/vrs_event_storage.cpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_array_blocks.cpp cpp/world/vrs_event_signal.cpp cpp/world/detached_vrs_state_update.cpp cpp/swegca_architecture/sha256.cpp cpp/transport/json.cpp -lz -o $@

$(BUILD)/vrs-event-delta-tests: tests/vrs_event_delta_tests.cpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_delta.hpp cpp/world/persistent_event_vector.hpp cpp/world/vrs_event_signal.cpp cpp/world/vrs_event_signal.hpp cpp/world/detached_vrs_state_update.cpp cpp/world/detached_vrs_state_update.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_signal.cpp cpp/world/detached_vrs_state_update.cpp -o $@

$(BUILD)/vrs-edge-address-index-tests: tests/vrs_edge_address_index_tests.cpp cpp/world/vrs_edge_address_index.cpp cpp/world/vrs_edge_address_index.hpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_delta.hpp cpp/world/persistent_event_vector.hpp cpp/world/vrs_event_signal.cpp cpp/world/vrs_event_signal.hpp cpp/world/detached_vrs_state_update.cpp cpp/world/detached_vrs_state_update.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/vrs_edge_address_index.cpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_signal.cpp cpp/world/detached_vrs_state_update.cpp -o $@

$(BUILD)/vrs-canonicalization-tests: tests/vrs_canonicalization_tests.cpp cpp/world/vrs_canonicalization.cpp cpp/world/vrs_canonicalization.hpp cpp/world/vrs_sparse_lineage.cpp cpp/world/vrs_sparse_lineage.hpp cpp/world/vrs_edge_address_index.cpp cpp/world/vrs_edge_address_index.hpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_delta.hpp cpp/world/persistent_event_vector.hpp cpp/world/vrs_event_signal.cpp cpp/world/vrs_event_signal.hpp cpp/world/detached_vrs_state_update.cpp cpp/world/detached_vrs_state_update.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/vrs_canonicalization.cpp cpp/world/vrs_sparse_lineage.cpp cpp/world/vrs_edge_address_index.cpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_signal.cpp cpp/world/detached_vrs_state_update.cpp -o $@

$(BUILD)/vrs-sparse-lineage-tests: tests/vrs_sparse_lineage_tests.cpp cpp/world/vrs_sparse_lineage.cpp cpp/world/vrs_sparse_lineage.hpp cpp/world/vrs_canonicalization.cpp cpp/world/vrs_canonicalization.hpp cpp/world/vrs_edge_address_index.cpp cpp/world/vrs_edge_address_index.hpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_delta.hpp cpp/world/persistent_event_vector.hpp cpp/world/vrs_event_signal.cpp cpp/world/vrs_event_signal.hpp cpp/world/detached_vrs_state_update.cpp cpp/world/detached_vrs_state_update.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/vrs_sparse_lineage.cpp cpp/world/vrs_canonicalization.cpp cpp/world/vrs_edge_address_index.cpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_signal.cpp cpp/world/detached_vrs_state_update.cpp -o $@

$(BUILD)/term-address-index-tests: tests/term_address_index_tests.cpp cpp/world/term_address_index.cpp cpp/world/term_address_index.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/term_address_index.cpp -o $@

$(BUILD)/semantic-vrs-ingress-tests: tests/semantic_vrs_ingress_tests.cpp cpp/world/semantic_vrs_ingress.cpp cpp/world/semantic_vrs_ingress.hpp cpp/world/term_address_index.cpp cpp/world/term_address_index.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/vrs_event_signal.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/semantic_vrs_ingress.cpp cpp/world/term_address_index.cpp cpp/world/cognitive_state.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/session-semantic-binding-tests: tests/session_semantic_binding_tests.cpp cpp/world/session_semantic_binding.cpp cpp/world/session_semantic_binding.hpp cpp/world/semantic_vrs_ingress.cpp cpp/world/semantic_vrs_ingress.hpp cpp/world/term_address_index.cpp cpp/world/term_address_index.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/vrs_event_signal.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp $(SESSION_BINDING_SUPPORT_SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/session_semantic_binding.cpp cpp/world/semantic_vrs_ingress.cpp cpp/world/term_address_index.cpp cpp/world/cognitive_state.cpp cpp/swegca_architecture/sha256.cpp $(SESSION_BINDING_SUPPORT_SOURCES) -o $@

$(BUILD)/session-message-content-tests: tests/session_message_content_tests.cpp cpp/world/session_message_content.cpp cpp/world/session_message_content.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/transport/json.cpp cpp/transport/json.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/session_message_content.cpp cpp/world/unicode_nfkc.cpp cpp/transport/json.cpp -o $@

$(BUILD)/session-call-content-tests: tests/session_call_content_tests.cpp cpp/world/session_call_content.cpp cpp/world/session_call_content.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/transport/json.cpp cpp/transport/json.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/session_call_content.cpp cpp/world/unicode_nfkc.cpp cpp/transport/json.cpp -o $@

$(BUILD)/session-result-content-tests: tests/session_result_content_tests.cpp cpp/world/session_result_content.cpp cpp/world/session_result_content.hpp cpp/world/session_call_content.cpp cpp/world/session_call_content.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/transport/json.cpp cpp/transport/json.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/session_result_content.cpp cpp/world/session_call_content.cpp cpp/world/unicode_nfkc.cpp cpp/transport/json.cpp -o $@

$(BUILD)/session-operation-content-tests: tests/session_operation_content_tests.cpp cpp/world/session_operation_content.cpp cpp/world/session_operation_content.hpp cpp/world/session_call_content.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/transport/json.cpp cpp/transport/json.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/session_operation_content.cpp cpp/world/unicode_nfkc.cpp cpp/transport/json.cpp -o $@

$(BUILD)/session-archive-pipeline-tests: tests/session_archive_pipeline_tests.cpp \
	cpp/world/session_event_index.cpp cpp/world/session_document.cpp \
	cpp/world/session_occurrences.cpp cpp/world/session_document_directory.cpp \
	cpp/world/prepared_session_cache.cpp cpp/world/session_result_collection.cpp \
	cpp/world/session_speech_ingress.cpp cpp/world/session_speech_segments.cpp \
	cpp/world/session_semantic_binding.cpp cpp/world/session_content_encoding.cpp \
	cpp/world/semantic_vrs_ingress.cpp \
	cpp/world/term_address_index.cpp cpp/world/cognitive_state.cpp \
	cpp/world/session_message_content.cpp cpp/world/session_call_content.cpp \
	cpp/world/session_result_content.cpp cpp/world/session_operation_content.cpp \
	cpp/world/unicode_nfkc.cpp cpp/transport/json.cpp cpp/swegca_architecture/sha256.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $^ -o $@

$(BUILD)/session-report-scope-tests: tests/session_report_scope_tests.cpp \
	cpp/world/session_report_scope.cpp cpp/world/session_report_scope.hpp \
	cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp \
	cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< \
		cpp/world/session_report_scope.cpp cpp/world/cognitive_state.cpp \
		cpp/world/unicode_nfkc.cpp -o $@

$(BUILD)/semantic-event-append-tests: tests/semantic_event_append_tests.cpp cpp/world/semantic_event_append.cpp cpp/world/semantic_event_append.hpp cpp/world/session_semantic_binding.cpp cpp/world/session_semantic_binding.hpp cpp/world/semantic_vrs_ingress.cpp cpp/world/semantic_vrs_ingress.hpp cpp/world/term_address_index.cpp cpp/world/term_address_index.hpp cpp/world/vrs_canonicalization.cpp cpp/world/vrs_canonicalization.hpp cpp/world/vrs_sparse_lineage.cpp cpp/world/vrs_sparse_lineage.hpp cpp/world/vrs_edge_address_index.cpp cpp/world/vrs_edge_address_index.hpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_delta.hpp cpp/world/vrs_event_signal.cpp cpp/world/vrs_event_signal.hpp cpp/world/detached_vrs_state_update.cpp cpp/world/detached_vrs_state_update.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp $(SESSION_BINDING_SUPPORT_SOURCES) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/semantic_event_append.cpp cpp/world/session_semantic_binding.cpp cpp/world/semantic_vrs_ingress.cpp cpp/world/term_address_index.cpp cpp/world/vrs_canonicalization.cpp cpp/world/vrs_sparse_lineage.cpp cpp/world/vrs_edge_address_index.cpp cpp/world/vrs_event_delta.cpp cpp/world/vrs_event_signal.cpp cpp/world/detached_vrs_state_update.cpp cpp/world/cognitive_state.cpp cpp/swegca_architecture/sha256.cpp $(SESSION_BINDING_SUPPORT_SOURCES) -o $@

$(BUILD)/sensor-definition-tests: tests/sensor_definition_tests.cpp cpp/world/sensor_definition.cpp cpp/world/sensor_definition.hpp cpp/world/sensor_term_index.cpp cpp/world/sensor_term_index.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/world/definition_contract.cpp cpp/world/definition_contract.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/sensor_definition.cpp cpp/world/sensor_term_index.cpp cpp/world/unicode_nfkc.cpp cpp/world/definition_contract.cpp cpp/world/evidence_accumulator.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/sensor-term-index-tests: tests/sensor_term_index_tests.cpp cpp/world/sensor_term_index.cpp cpp/world/sensor_term_index.hpp cpp/world/sensor_definition.cpp cpp/world/sensor_definition.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/world/definition_contract.cpp cpp/world/definition_contract.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/sensor_term_index.cpp cpp/world/sensor_definition.cpp cpp/world/unicode_nfkc.cpp cpp/world/definition_contract.cpp cpp/world/evidence_accumulator.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/sensor-counterfactual-tests: tests/sensor_counterfactual_tests.cpp cpp/world/sensor_counterfactual.cpp cpp/world/sensor_counterfactual.hpp cpp/world/sensor_term_index.cpp cpp/world/sensor_term_index.hpp cpp/world/sensor_definition.cpp cpp/world/sensor_definition.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/world/definition_contract.cpp cpp/world/definition_contract.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/sensor_counterfactual.cpp cpp/world/sensor_term_index.cpp cpp/world/sensor_definition.cpp cpp/world/unicode_nfkc.cpp cpp/world/definition_contract.cpp cpp/world/evidence_accumulator.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/counterfactual-replay-tests: tests/counterfactual_replay_tests.cpp cpp/world/counterfactual_replay.cpp cpp/world/counterfactual_replay.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/counterfactual_replay.cpp cpp/world/evidence_accumulator.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/autonomous-cognition-tests: tests/autonomous_cognition_tests.cpp cpp/world/autonomous_cognition.cpp cpp/world/autonomous_cognition.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/autonomous_cognition.cpp cpp/world/cognitive_state.cpp cpp/world/evidence_accumulator.cpp cpp/world/unicode_nfkc.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/accelerated-verification-tests: tests/accelerated_verification_tests.cpp cpp/world/accelerated_verification.cpp cpp/world/accelerated_verification.hpp cpp/world/autonomous_cognition.cpp cpp/world/autonomous_cognition.hpp cpp/world/counterfactual_replay.cpp cpp/world/counterfactual_replay.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/accelerated_verification.cpp cpp/world/autonomous_cognition.cpp cpp/world/counterfactual_replay.cpp cpp/world/cognitive_state.cpp cpp/world/evidence_accumulator.cpp cpp/world/unicode_nfkc.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/hybrid-verification-tests: tests/hybrid_verification_tests.cpp cpp/world/hybrid_verification.cpp cpp/world/hybrid_verification.hpp cpp/world/accelerated_verification.cpp cpp/world/accelerated_verification.hpp cpp/world/autonomous_cognition.cpp cpp/world/autonomous_cognition.hpp cpp/world/counterfactual_replay.cpp cpp/world/counterfactual_replay.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/hybrid_verification.cpp cpp/world/accelerated_verification.cpp cpp/world/autonomous_cognition.cpp cpp/world/counterfactual_replay.cpp cpp/world/cognitive_state.cpp cpp/world/evidence_accumulator.cpp cpp/world/unicode_nfkc.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/incremental-definition-loop-tests: tests/incremental_definition_loop_tests.cpp cpp/world/hybrid_verification.cpp cpp/world/hybrid_verification.hpp cpp/world/accelerated_verification.cpp cpp/world/accelerated_verification.hpp cpp/world/autonomous_cognition.cpp cpp/world/autonomous_cognition.hpp cpp/world/counterfactual_replay.cpp cpp/world/counterfactual_replay.hpp cpp/world/sensor_counterfactual.cpp cpp/world/sensor_counterfactual.hpp cpp/world/sensor_term_index.cpp cpp/world/sensor_term_index.hpp cpp/world/sensor_definition.cpp cpp/world/sensor_definition.hpp cpp/world/definition_contract.cpp cpp/world/definition_contract.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/world/unicode_nfkc.cpp cpp/world/unicode_nfkc.hpp cpp/world/unicode_nfkc_data.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/hybrid_verification.cpp cpp/world/accelerated_verification.cpp cpp/world/autonomous_cognition.cpp cpp/world/counterfactual_replay.cpp cpp/world/sensor_counterfactual.cpp cpp/world/sensor_term_index.cpp cpp/world/sensor_definition.cpp cpp/world/definition_contract.cpp cpp/world/cognitive_state.cpp cpp/world/evidence_accumulator.cpp cpp/world/unicode_nfkc.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/cognitive-event-tests: tests/cognitive_event_tests.cpp cpp/world/cognitive_event.cpp cpp/world/cognitive_event.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/cognitive_event.cpp cpp/world/cognitive_state.cpp -o $@

$(BUILD)/world-state-tests: tests/world_state_tests.cpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/world_state.cpp cpp/world/cognitive_state.cpp -o $@

$(BUILD)/evidence-accumulator-tests: tests/evidence_accumulator_tests.cpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/evidence_accumulator.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/evidence-revision-tests: tests/evidence_revision_tests.cpp cpp/world/evidence_revision.cpp cpp/world/evidence_revision.hpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/evidence_revision.cpp cpp/transport/json.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/synapse-arbiter-world-tests: tests/synapse_arbiter_world_tests.cpp cpp/world/synapse_arbiter.cpp cpp/world/synapse_arbiter.hpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/synapse_arbiter.cpp cpp/world/world_state.cpp cpp/world/cognitive_state.cpp -o $@

$(BUILD)/dynamic-cognition-tests: tests/dynamic_cognition_tests.cpp cpp/world/dynamic_cognition.cpp cpp/world/dynamic_cognition.hpp cpp/world/synapse_arbiter.cpp cpp/world/synapse_arbiter.hpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/dynamic_cognition.cpp cpp/world/synapse_arbiter.cpp cpp/world/world_state.cpp cpp/world/cognitive_state.cpp -o $@

$(BUILD)/bounded-world-write-tests: tests/bounded_world_write_tests.cpp cpp/world/bounded_world_write.cpp cpp/world/bounded_world_write.hpp cpp/world/synapse_arbiter.cpp cpp/world/synapse_arbiter.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/world/evidence_revision.cpp cpp/world/evidence_revision.hpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/bounded_world_write.cpp cpp/world/synapse_arbiter.cpp cpp/world/cognitive_state.cpp cpp/world/world_state.cpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_revision.cpp cpp/transport/json.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/re-evidence-receipt-tests: tests/re_evidence_receipt_tests.cpp cpp/world/re_evidence_receipt.cpp cpp/world/re_evidence_receipt.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/re_evidence_receipt.cpp -o $@

$(BUILD)/re-evidence-transaction-tests: tests/re_evidence_transaction_tests.cpp cpp/world/re_evidence_transaction.cpp cpp/world/re_evidence_transaction.hpp cpp/world/re_evidence_receipt.cpp cpp/world/re_evidence_receipt.hpp cpp/world/cognitive_event.cpp cpp/world/cognitive_event.hpp cpp/world/evidence_accumulator.cpp cpp/world/evidence_accumulator.hpp cpp/world/synapse_arbiter.cpp cpp/world/synapse_arbiter.hpp cpp/world/bounded_world_write.cpp cpp/world/bounded_world_write.hpp cpp/world/evidence_revision.cpp cpp/world/evidence_revision.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/re_evidence_transaction.cpp cpp/world/re_evidence_receipt.cpp cpp/world/cognitive_event.cpp cpp/world/evidence_accumulator.cpp cpp/world/synapse_arbiter.cpp cpp/world/bounded_world_write.cpp cpp/world/evidence_revision.cpp cpp/world/cognitive_state.cpp cpp/world/world_state.cpp cpp/transport/json.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/re-evidence-arbitration-tests: tests/re_evidence_arbitration_tests.cpp cpp/world/re_evidence_arbitration.cpp cpp/world/re_evidence_arbitration.hpp cpp/world/re_evidence_receipt.cpp cpp/world/re_evidence_receipt.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/transport/json.cpp cpp/transport/json.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/re_evidence_arbitration.cpp cpp/world/re_evidence_receipt.cpp cpp/world/cognitive_state.cpp cpp/world/world_state.cpp cpp/transport/json.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/modal-to-world-tests: tests/modal_to_world_tests.cpp tests/modal_to_world_python214_fixture.hpp cpp/world/modal_to_world.cpp cpp/world/modal_to_world.hpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/checkpoint/materialized_tensor.cpp cpp/checkpoint/materialized_tensor.hpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_checkpoint.hpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_zip.hpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/restricted_pickle.hpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/canonical_symbolic_json.hpp cpp/checkpoint/checkpoint_profile.cpp cpp/checkpoint/checkpoint_profile.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/modal_to_world.cpp cpp/world/world_state.cpp cpp/world/cognitive_state.cpp cpp/checkpoint/materialized_tensor.cpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/checkpoint_profile.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/recurrent-cognition-tests: tests/recurrent_cognition_tests.cpp tests/recurrent_cognition_python214_fixture.hpp cpp/world/recurrent_cognition.cpp cpp/world/recurrent_cognition.hpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) -Itests $< cpp/world/recurrent_cognition.cpp cpp/world/world_state.cpp cpp/world/cognitive_state.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/prototype-recurrent-cognition-tests: tests/prototype_recurrent_cognition_tests.cpp cpp/world/prototype_recurrent_cognition.cpp cpp/world/prototype_recurrent_cognition.hpp cpp/world/recurrent_cognition.cpp cpp/world/recurrent_cognition.hpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/checkpoint/prototype_materialized_tensor.cpp cpp/checkpoint/prototype_materialized_tensor.hpp cpp/checkpoint/materialized_tensor.cpp cpp/checkpoint/materialized_tensor.hpp cpp/checkpoint/prototype_checkpoint.cpp cpp/checkpoint/prototype_checkpoint.hpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_checkpoint.hpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_zip.hpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/restricted_pickle.hpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/canonical_symbolic_json.hpp cpp/checkpoint/checkpoint_profile.cpp cpp/checkpoint/checkpoint_profile.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) $(INCLUDES) $< cpp/world/prototype_recurrent_cognition.cpp cpp/world/recurrent_cognition.cpp cpp/world/world_state.cpp cpp/world/cognitive_state.cpp cpp/checkpoint/prototype_materialized_tensor.cpp cpp/checkpoint/materialized_tensor.cpp cpp/checkpoint/prototype_checkpoint.cpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/checkpoint_profile.cpp cpp/swegca_architecture/sha256.cpp -o $@

$(BUILD)/prototype-recurrent-activation-tests: tests/prototype_recurrent_activation_tests.cpp tests/generate_prototype_recurrent_fixture.py tests/fixtures/prototype_recurrent_seed631_python214_output.f32le tests/fixtures/prototype_recurrent_seed631_python214_output.json cpp/world/prototype_recurrent_cognition.cpp cpp/world/prototype_recurrent_cognition.hpp cpp/world/recurrent_cognition.cpp cpp/world/recurrent_cognition.hpp cpp/world/world_state.cpp cpp/world/world_state.hpp cpp/world/cognitive_state.cpp cpp/world/cognitive_state.hpp cpp/checkpoint/prototype_materialized_tensor.cpp cpp/checkpoint/prototype_materialized_tensor.hpp cpp/checkpoint/materialized_tensor.cpp cpp/checkpoint/materialized_tensor.hpp cpp/checkpoint/prototype_checkpoint.cpp cpp/checkpoint/prototype_checkpoint.hpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_checkpoint.hpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_zip.hpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/restricted_pickle.hpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/canonical_symbolic_json.hpp cpp/checkpoint/checkpoint_profile.cpp cpp/checkpoint/checkpoint_profile.hpp cpp/swegca_architecture/sha256.cpp cpp/swegca_architecture/sha256.hpp | $(BUILD)
	test -n "$(OPENBLAS_SO)" || (echo "OpenBLAS shared library not found" >&2; exit 1)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CORE_FLAGS) -DSWEGCA_RECURRENT_USE_CBLAS $(INCLUDES) $< cpp/world/prototype_recurrent_cognition.cpp cpp/world/recurrent_cognition.cpp cpp/world/world_state.cpp cpp/world/cognitive_state.cpp cpp/checkpoint/prototype_materialized_tensor.cpp cpp/checkpoint/materialized_tensor.cpp cpp/checkpoint/prototype_checkpoint.cpp cpp/checkpoint/restricted_checkpoint.cpp cpp/checkpoint/restricted_zip.cpp cpp/checkpoint/restricted_pickle.cpp cpp/checkpoint/canonical_symbolic_json.cpp cpp/checkpoint/checkpoint_profile.cpp cpp/swegca_architecture/sha256.cpp $(OPENBLAS_SO) -o $@

check-prototype-recurrent-activation: $(BUILD)/prototype-recurrent-activation-tests
	OPENBLAS_NUM_THREADS=10 ./$(BUILD)/prototype-recurrent-activation-tests

# Exact local artifact integration. This deliberately remains outside the
# hermetic production gate because it requires the two audited 529 MB
# Prototype0 checkpoints and the pinned source/config snapshot.
check-prototype-recurrent-artifacts: $(BUILD)/prototype-checkpoint-tests \
	$(BUILD)/prototype-recurrent-cognition-tests \
	$(BUILD)/prototype-recurrent-activation-tests
	./$(BUILD)/prototype-checkpoint-tests
	./$(BUILD)/prototype-recurrent-cognition-tests
	OPENBLAS_NUM_THREADS=10 ./$(BUILD)/prototype-recurrent-activation-tests

check-world: $(BUILD)/cognitive-state-tests $(BUILD)/definition-contract-tests \
	$(BUILD)/cognitive-event-tests $(BUILD)/hypothesis-proposer-tests $(BUILD)/image-tag-experience-tests \
	$(BUILD)/detached-vrs-state-update-tests \
	$(BUILD)/vrs-event-signal-tests \
	$(BUILD)/vrs-array-blocks-tests \
	$(BUILD)/vrs-event-storage-tests \
	$(BUILD)/vrs-event-delta-tests \
	$(BUILD)/vrs-edge-address-index-tests \
	$(BUILD)/vrs-canonicalization-tests \
	$(BUILD)/vrs-sparse-lineage-tests \
	$(BUILD)/term-address-index-tests \
	$(BUILD)/semantic-vrs-ingress-tests \
	$(BUILD)/session-semantic-binding-tests \
	$(BUILD)/session-message-content-tests \
	$(BUILD)/session-call-content-tests \
	$(BUILD)/session-result-content-tests \
	$(BUILD)/session-operation-content-tests \
	$(BUILD)/session-archive-pipeline-tests \
	$(BUILD)/session-report-scope-tests \
	$(BUILD)/semantic-event-append-tests \
	$(BUILD)/sensor-definition-tests $(BUILD)/sensor-term-index-tests \
	$(BUILD)/sensor-counterfactual-tests $(BUILD)/counterfactual-replay-tests \
	$(BUILD)/autonomous-cognition-tests $(BUILD)/accelerated-verification-tests \
	$(BUILD)/hybrid-verification-tests $(BUILD)/incremental-definition-loop-tests \
	$(BUILD)/world-state-tests $(BUILD)/evidence-accumulator-tests $(BUILD)/evidence-revision-tests \
	$(BUILD)/synapse-arbiter-world-tests $(BUILD)/dynamic-cognition-tests \
		$(BUILD)/bounded-world-write-tests $(BUILD)/re-evidence-receipt-tests \
		$(BUILD)/re-evidence-transaction-tests $(BUILD)/re-evidence-arbitration-tests \
		$(BUILD)/modal-to-world-tests $(BUILD)/recurrent-cognition-tests
	./$(BUILD)/cognitive-state-tests
	./$(BUILD)/definition-contract-tests
	./$(BUILD)/cognitive-event-tests
	./$(BUILD)/hypothesis-proposer-tests
	./$(BUILD)/image-tag-experience-tests
	./$(BUILD)/detached-vrs-state-update-tests
	./$(BUILD)/vrs-event-signal-tests
	./$(BUILD)/vrs-array-blocks-tests
	./$(BUILD)/vrs-event-storage-tests
	./$(BUILD)/vrs-event-delta-tests
	./$(BUILD)/vrs-edge-address-index-tests
	./$(BUILD)/vrs-canonicalization-tests
	./$(BUILD)/vrs-sparse-lineage-tests
	./$(BUILD)/term-address-index-tests
	./$(BUILD)/semantic-vrs-ingress-tests
	./$(BUILD)/session-semantic-binding-tests
	./$(BUILD)/session-message-content-tests
	./$(BUILD)/session-call-content-tests
	./$(BUILD)/session-result-content-tests
	./$(BUILD)/session-operation-content-tests
	./$(BUILD)/session-archive-pipeline-tests
	./$(BUILD)/session-report-scope-tests
	./$(BUILD)/semantic-event-append-tests
	./$(BUILD)/sensor-definition-tests
	./$(BUILD)/sensor-term-index-tests
	./$(BUILD)/sensor-counterfactual-tests
	./$(BUILD)/counterfactual-replay-tests
	./$(BUILD)/autonomous-cognition-tests
	./$(BUILD)/accelerated-verification-tests
	./$(BUILD)/hybrid-verification-tests
	./$(BUILD)/incremental-definition-loop-tests
	./$(BUILD)/world-state-tests
	./$(BUILD)/evidence-accumulator-tests
	./$(BUILD)/evidence-revision-tests
	./$(BUILD)/synapse-arbiter-world-tests
	./$(BUILD)/dynamic-cognition-tests
	./$(BUILD)/bounded-world-write-tests
	./$(BUILD)/re-evidence-receipt-tests
	./$(BUILD)/re-evidence-transaction-tests
	./$(BUILD)/re-evidence-arbitration-tests
	./$(BUILD)/modal-to-world-tests
	./$(BUILD)/recurrent-cognition-tests

check-production: production
	$(MAKE) --no-print-directory check
	$(MAKE) --no-print-directory check-stdio
	$(MAKE) --no-print-directory check-resource-profile
	$(MAKE) --no-print-directory check-oom-recovery
	$(MAKE) --no-print-directory check-sha256
	$(MAKE) --no-print-directory check-synapse
	$(MAKE) --no-print-directory check-association
	$(MAKE) --no-print-directory check-raw-observation
	$(MAKE) --no-print-directory check-experience-pairs
	$(MAKE) --no-print-directory check-checkpoint
	$(MAKE) --no-print-directory check-world
	$(MAKE) --no-print-directory $(BUILD)/file-observation-tests
	./$(BUILD)/file-observation-tests

check-experimental-cpu: $(EXPERIMENTAL_CPU_TESTS)
	@for test in $(EXPERIMENTAL_CPU_TESTS); do $$test || exit $$?; done

check-experimental-gpu: require-cuda-toolkit $(EXPERIMENTAL_GPU_TESTS)
	@for test in $(EXPERIMENTAL_GPU_TESTS); do $$test $(BUILD)/gpu_core_source.hpp || exit $$?; done

STAGE0_BUILD ?= build/stage0-clean
STAGE0_RECEIPT := $(STAGE0_BUILD)/stage0-receipt.json
stage0-gate:
	@test -z "$(filter /%,$(STAGE0_BUILD))" || { echo "STAGE0_BUILD must be repository-relative" >&2; exit 2; }
	rm -rf -- $(STAGE0_BUILD)
	mkdir -p $(STAGE0_BUILD)
	$(MAKE) --no-print-directory -j$${SWEGCA_BUILD_JOBS:-10} BUILD=$(STAGE0_BUILD) check-production >$(STAGE0_BUILD)/build.log 2>&1
	python3 tools/stage0_gate.py --root . --build-dir $(STAGE0_BUILD) --output $(STAGE0_RECEIPT) \
		--build-log $(STAGE0_BUILD)/build.log --compiler "$(CXX)" --external-manifest docs/stage0-external-p0.json \
		$(foreach path,$(sort $(MCP_PRODUCTION_CLOSURE_SOURCES)),--mcp-source $(path)) \
		$(foreach path,$(sort $(PRODUCTION_SUPPORT_CLOSURE_SOURCES)),--support-source $(path)) \
		$(foreach path,$(sort $(PRODUCTION_GATE_CLOSURE_SOURCES)),--gate-source $(path)) \
		$(foreach path,$(sort $(EXPERIMENTAL_CLOSURE_SOURCES)),--experimental-source $(path)) \
		$(foreach path,$(PRODUCTION_BINARIES),--production-binary $(patsubst $(BUILD)/%,$(STAGE0_BUILD)/%,$(path)))
