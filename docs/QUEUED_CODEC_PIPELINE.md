# Queued codec → SWEGCA → VRS application

Status update 2026-09-28: native `whole-file-ingress` is now connected to
`BlockIngress` and block-local durable collectors. See BLOCK_INGRESS.md for the
current execution and evidence boundary. The historical single-applier fixture
below remains a supported default mode; routed mode uses independent collectors.
No corpus service was restarted, and raw media semantic inference is not claimed.

## Implemented execution contract

`work_pipeline.hpp` owns pre-created bounded queues for files, decoded inputs,
and verified results. Codec workers and registered verification endpoints pull
ready work. No file-to-worker assignment is fixed. Each endpoint processes a
batch of logical SWEGCA operations, then immediately pulls the next available
batch. Batch size adapts to measured service time, within its registered memory
capacity; logical core operations are not equated with host thread count.

Only the applier thread invokes the application callback. It drains at the
configured batch threshold; finite latency flush and final drain prevent small
remaining batches from being stranded. Producer and verification work continue
while application executes. Backpressure bounds queue growth. Exceptions stop
new work, wake blocked workers and propagate to the caller. Application is not
retried automatically: its durable transaction/recovery must be supplied by the
VRS owner to avoid counting a partly applied batch twice.

`codec_input.cpp` uses installed codecs and volatile buffers, not a tar archive,
SessionRuntime, retain_input or pre-verification experience storage. A sealed
RAM descriptor permits codecs that require seeking. It is not durable storage.
Original content and decoded views are retained until application; codec errors
remain explicit and do not turn into fabricated observations. Text is unchanged.
Audio/video use rawvideo/PCM views, text subtitle streams are extracted without
language filtering. Bitmap subtitle decode errors are explicit; original bytes
remain available. Unsupported formats retain original bytes. Sidecar alignment
is NOT inferred. Raw decoded videos may exceed memory; allocation failure is
reported, not silently truncated or spilled into intermediate disk storage.
RAM accounting currently covers vectors, not the temporary kernel memfd copy or
external codec RSS: production process limits must cover those as well.

`device_evidence.cpp` invokes the existing CPU `judge_evidence_batch` and compiles
the shared SWEGCA scalar source on CUDA. Runtime ABI checks and strict arithmetic
flags preserve the existing numeric contract. CUDA endpoints accept bulk arrays;
there is no promise/future per logical judgment. Other execution devices can be
registered through the same backend contract; NPU execution has NOT been tested
or implemented in this checkout.

## Production connection

`BlockIngress` replaces the old retain-before-judge call. Original bytes and
codec views remain volatile through core execution. Explicit incoming relation
observations also reach the association core on CPU/CUDA. Block collectors then
persist payload batches and atomically publish address/count journal entries.
The old SessionRuntime path is not used by this executable. Automatic semantic
observation extraction from arbitrary media remains outside this wiring change.

## Executed validation

- work-pipeline-tests: 1,048,576 real core judgments, CPU and both GPUs, ordered
  per-result ownership, exactly-once application in the test, stage overlap,
  one applier, codec/verification/application exception propagation.
- Each available GPU additionally checks 65,536 judgments against CPU fields.
- codec-input-tests: installed image codec, unchanged multilingual text, volatile
  output, no intermediate disk artifacts, missing input failure.
- codec-pipeline-tests: real image → codec → core → one application callback;
  fixture uses explicitly unknown evidence and asserts no storage before judgment.
  This is transport/order evidence, not semantic synapse validation.
