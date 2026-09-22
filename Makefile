CXX ?= g++
AR ?= ar
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -Wpedantic
NUMERIC_CXXFLAGS = -ffp-contract=off -fno-fast-math

CORE_OBJECTS = build/digest.o build/json.o build/unicode.o build/keys.o build/observation.o build/memory_episode.o build/memory_evidence.o build/memory_promotion.o build/vrs_state_update.o build/hot_index.o build/hot_index_pending.o build/hot_index_projection.o build/hot_index_projection_log.o build/native_cue_directory.o build/native_hot_index_rebuild.o build/native_published_hot_index.o build/main_operations.o build/native_operation_directory.o build/native_operation_rebuild.o build/memory_vrs_pair.o build/main_read_generation.o build/deja_vu.o build/memory_recall.o build/memory_receipt.o build/event_vrs_inputs.o build/endpoint_segments.o build/event_delta.o build/python_fsum.o build/event_vrs_kernel.o build/event_signal_strength.o build/event_signal.o build/graph_append.o build/graph_auxiliary_generation.o build/connectivity_regions.o build/graph_regions.o build/region_preactivation.o build/region_navigation.o build/coactivation.o build/coactivation_associations.o build/portal_lifecycle.o build/portal_navigation.o build/session_first_read.o build/four_stage_read.o build/journal_frame.o build/journal_files.o build/owner_lock.o build/native_journal.o build/native_journal_entry.o build/original_journal_replay.o build/exact_journal_directory.o build/exact_journal_replay.o build/exact_journal_rebuild.o

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

build/memory_evidence.o: cpp/memory_evidence.cpp cpp/memory_evidence.hpp cpp/memory_recall.hpp cpp/memory_episode.hpp cpp/json.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/memory_promotion.o: cpp/memory_promotion.cpp cpp/memory_promotion.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/vrs_state_update.o: cpp/vrs_state_update.cpp cpp/vrs_state_update.hpp cpp/memory_promotion.hpp cpp/memory_receipt.hpp cpp/json.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/hot_index.o: cpp/hot_index.cpp cpp/hot_index.hpp cpp/memory_episode.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/hot_index_pending.o: cpp/hot_index_pending.cpp cpp/hot_index_pending.hpp cpp/hot_index.hpp cpp/memory_episode.hpp cpp/json.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/hot_index_projection.o: cpp/hot_index_projection.cpp cpp/hot_index_projection.hpp cpp/hot_index.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/hot_index_projection_log.o: cpp/hot_index_projection_log.cpp cpp/hot_index_projection_log.hpp cpp/hot_index_projection.hpp cpp/journal_files.hpp cpp/owner_lock.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/native_cue_directory.o: cpp/native_cue_directory.cpp cpp/native_cue_directory.hpp cpp/owner_lock.hpp cpp/journal_files.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/native_hot_index_rebuild.o: cpp/native_hot_index_rebuild.cpp cpp/native_hot_index_rebuild.hpp cpp/hot_index.hpp cpp/native_cue_directory.hpp cpp/exact_journal_directory.hpp cpp/hot_index_projection_log.hpp cpp/native_journal.hpp cpp/native_journal_entry.hpp cpp/memory_episode.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/native_published_hot_index.o: cpp/native_published_hot_index.cpp cpp/native_published_hot_index.hpp cpp/hot_index.hpp cpp/hot_index_projection_log.hpp cpp/exact_journal_directory.hpp cpp/native_cue_directory.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/main_operations.o: cpp/main_operations.cpp cpp/main_operations.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/native_operation_directory.o: cpp/native_operation_directory.cpp cpp/native_operation_directory.hpp cpp/main_operations.hpp cpp/owner_lock.hpp cpp/journal_files.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/native_operation_rebuild.o: cpp/native_operation_rebuild.cpp cpp/native_operation_rebuild.hpp cpp/native_operation_directory.hpp cpp/native_journal.hpp cpp/native_journal_entry.hpp cpp/memory_episode.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/memory_vrs_pair.o: cpp/memory_vrs_pair.cpp cpp/memory_vrs_pair.hpp cpp/main_read_generation.hpp cpp/hot_index.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/main_read_generation.o: cpp/main_read_generation.cpp cpp/main_read_generation.hpp cpp/graph_auxiliary_generation.hpp cpp/native_published_hot_index.hpp cpp/native_operation_directory.hpp cpp/session_first_read.hpp cpp/exact_journal_replay.hpp cpp/exact_journal_directory.hpp cpp/native_cue_directory.hpp cpp/native_journal.hpp cpp/event_vrs_inputs.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/deja_vu.o: cpp/deja_vu.cpp cpp/deja_vu.hpp cpp/hot_index.hpp cpp/keys.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/memory_recall.o: cpp/memory_recall.cpp cpp/memory_recall.hpp cpp/deja_vu.hpp cpp/hot_index.hpp cpp/keys.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/memory_receipt.o: cpp/memory_receipt.cpp cpp/memory_receipt.hpp cpp/deja_vu.hpp cpp/memory_recall.hpp cpp/memory_evidence.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/event_vrs_inputs.o: cpp/event_vrs_inputs.cpp cpp/event_vrs_inputs.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/endpoint_segments.o: cpp/endpoint_segments.cpp cpp/endpoint_segments.hpp cpp/event_vrs_inputs.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/event_delta.o: cpp/event_delta.cpp cpp/event_delta.hpp cpp/endpoint_segments.hpp cpp/sparse_event_radix.hpp cpp/event_vrs_inputs.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/python_fsum.o: cpp/python_fsum.cpp cpp/python_fsum.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/event_vrs_kernel.o: cpp/event_vrs_kernel.cpp cpp/event_vrs_kernel.hpp cpp/event_vrs_inputs.hpp cpp/python_fsum.hpp cpp/json.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/event_signal_strength.o: cpp/event_signal_strength.cpp cpp/event_signal_strength.hpp cpp/event_vrs_inputs.hpp cpp/vrs_state_update.hpp cpp/memory_promotion.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/event_signal.o: cpp/event_signal.cpp cpp/event_signal.hpp cpp/event_signal_strength.hpp cpp/event_vrs_kernel.hpp cpp/event_vrs_inputs.hpp cpp/python_fsum.hpp cpp/json.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/graph_append.o: cpp/graph_append.cpp cpp/graph_append.hpp cpp/event_delta.hpp cpp/event_signal.hpp cpp/event_vrs_inputs.hpp cpp/hot_index.hpp cpp/vrs_state_update.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/graph_auxiliary_generation.o: cpp/graph_auxiliary_generation.cpp cpp/graph_auxiliary_generation.hpp cpp/graph_append.hpp cpp/event_delta.hpp cpp/native_journal_entry.hpp cpp/hot_index.hpp cpp/memory_vrs_pair.hpp cpp/unicode.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/connectivity_regions.o: cpp/connectivity_regions.cpp cpp/connectivity_regions.hpp cpp/graph_append.hpp cpp/python_fsum.hpp cpp/digest.hpp cpp/json.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/graph_regions.o: cpp/graph_regions.cpp cpp/graph_regions.hpp cpp/connectivity_regions.hpp cpp/graph_append.hpp cpp/memory_vrs_pair.hpp cpp/python_fsum.hpp cpp/json.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/region_preactivation.o: cpp/region_preactivation.cpp cpp/region_preactivation.hpp cpp/graph_regions.hpp cpp/deja_vu.hpp cpp/python_fsum.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/region_navigation.o: cpp/region_navigation.cpp cpp/region_navigation.hpp cpp/graph_regions.hpp cpp/connectivity_regions.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/coactivation.o: cpp/coactivation.cpp cpp/coactivation.hpp cpp/exact_journal_replay.hpp cpp/graph_regions.hpp cpp/memory_receipt.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/coactivation_associations.o: cpp/coactivation_associations.cpp cpp/coactivation_associations.hpp cpp/coactivation.hpp cpp/graph_regions.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/portal_lifecycle.o: cpp/portal_lifecycle.cpp cpp/portal_lifecycle.hpp cpp/coactivation_associations.hpp cpp/python_fsum.hpp cpp/unicode.hpp | build
	$(CXX) $(CXXFLAGS) $(NUMERIC_CXXFLAGS) -Icpp -c $< -o $@

build/portal_navigation.o: cpp/portal_navigation.cpp cpp/portal_navigation.hpp cpp/portal_lifecycle.hpp cpp/region_navigation.hpp cpp/region_preactivation.hpp cpp/memory_recall.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/session_first_read.o: cpp/session_first_read.cpp cpp/session_first_read.hpp cpp/exact_journal_replay.hpp cpp/portal_navigation.hpp cpp/keys.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/four_stage_read.o: cpp/four_stage_read.cpp cpp/four_stage_read.hpp cpp/session_first_read.hpp cpp/exact_journal_replay.hpp cpp/portal_navigation.hpp cpp/memory_receipt.hpp cpp/memory_recall.hpp cpp/memory_evidence.hpp cpp/graph_regions.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/journal_frame.o: cpp/journal_frame.cpp cpp/journal_frame.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/journal_files.o: cpp/journal_files.cpp cpp/journal_files.hpp cpp/journal_frame.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/owner_lock.o: cpp/owner_lock.cpp cpp/owner_lock.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/native_journal.o: cpp/native_journal.cpp cpp/native_journal.hpp cpp/journal_files.hpp cpp/owner_lock.hpp cpp/json.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/native_journal_entry.o: cpp/native_journal_entry.cpp cpp/native_journal_entry.hpp cpp/observation.hpp cpp/json.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/original_journal_replay.o: cpp/original_journal_replay.cpp cpp/original_journal_replay.hpp cpp/native_journal.hpp cpp/memory_episode.hpp cpp/observation.hpp cpp/digest.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/exact_journal_directory.o: cpp/exact_journal_directory.cpp cpp/exact_journal_directory.hpp cpp/hot_index_projection_log.hpp cpp/original_journal_replay.hpp cpp/owner_lock.hpp cpp/journal_files.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/exact_journal_replay.o: cpp/exact_journal_replay.cpp cpp/exact_journal_replay.hpp cpp/exact_journal_directory.hpp cpp/original_journal_replay.hpp cpp/native_journal.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/exact_journal_rebuild.o: cpp/exact_journal_rebuild.cpp cpp/exact_journal_rebuild.hpp cpp/exact_journal_directory.hpp cpp/hot_index_projection_log.hpp cpp/native_journal.hpp cpp/native_journal_entry.hpp cpp/memory_episode.hpp | build
	$(CXX) $(CXXFLAGS) -Icpp -c $< -o $@

build/libswegca-vrs.a: $(CORE_OBJECTS)
	$(AR) rcs $@ $(CORE_OBJECTS)

clean:
	rm -rf build
