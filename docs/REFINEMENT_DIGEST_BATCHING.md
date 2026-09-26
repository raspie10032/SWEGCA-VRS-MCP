# Batch the same refinement digest bytes

Refinement hashing previously called SHA-256 update twice per sample, once for
each little-endian uint64 index/admission field. It now fills a fixed 1024-byte
stack buffer (64 samples), flushes full buffers and hashes only initialized bytes
in the final partial buffer. Field width, order, domain, sample traversal,
float-bit encoding and all other report fields are unchanged. At 8192 samples,
this reduces sample-array update calls from 16384 to 128; it does not remove
samples, core calls, history replay or checksum verification. No heap scratch or
persistent tally is introduced.

Before changing the serializer, the native 45af310 implementation emitted 15
report digests from the existing connection fixtures. Those exact vectors are
retained in tests/refinement_digest_vectors.hpp and checked during normal tests.
Cases cover 0,48,96,144 and 1030 samples, including accept/reject/abstain, expiry,
repeated/correlated evidence, mixed source cases and multiple buffer flushes.
These are current native format regression vectors, not discarded implementation
code or an old-format migration path.

Connection tests passed 15011 checks, and persistent connection tests passed 69.
The permitted original accumulator oracle still matched 15 shuffled batches /
2038 observations. Its source SHA-256 was
21ea34ed0cbff51f91f1463e5429419d4ac34b4afc664e52091cb4ca26a68ab6.

With the same 8192-event single-connection workload, one worker on CPU 6 and no
concurrent compilation, full reopening measured 3145.047 ms, compared with
3614.046 ms immediately before this change. This approximately 13% single-run
reduction is not a stable speedup guarantee. Raw output is retained in
benchmarks/results/recall-digest-repeated-8192.jsonl. All 150 input-to-Recall
samples stayed below 1 ms (maximum 770 ns) for the fixture's 128-byte input.
Selected temporary/Main originals and final zero-allocation accounting passed.

The sum of all historical prefixes is still processed, so long-history recovery
still takes seconds in this workload. Neither asynchronous scheduling nor the
large-graph architecture is completed by this serialization optimization.
