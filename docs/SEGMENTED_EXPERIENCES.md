# Segmented connection metadata

Connection retains sealed ExperienceEvidence values in segments of 8, 16, 32,
64, 128, then 256 entries each. Growing a connection never relocates earlier
experience objects. The PMR directory of segment descriptors can still move.
All allocations remain charged to the caller's VRS MemoryBudget.

Logical indices and iteration order are unchanged. Random shuffle access uses
an arithmetic segment lookup, without walking preceding segments. SWEGCA
admission, shuffled aggregation, verdicts and strength updates are unchanged.

The returned View captures a size and resolves entries through its owner; it
remains usable after append but expires when the Connection is destroyed.
Connections still require serialized ownership. This does not introduce safe
concurrent mutation/read access.

Append first reserves a segment and directory space, then commits the sealed
value without allocation. Allocation failure frees the candidate segment and
preserves existing values, revision, strength and allocation accounting.
Persistent append continues to reserve before writing its durable event.

Verification:

- 1,030 distinct stored originals cross all initial and fixed segment boundaries.
- Existing addresses, indexed and iterated values, snapshot sizes and suffix
  views remain consistent across growth.
- Injected segment/directory allocation failures preserve state and reservations.
- During these appends no individual allocation exceeds 256 sealed values.
  This bound is specific to the tested directory size: the descriptor directory
  itself continues to grow with segment count.
- 9,756 connection checks and 81 runtime lifecycle checks pass.
- The same 9,756 checks pass with undefined-behavior sanitization in trap mode.
  The diagnostic-runtime build could not link because this host lacks libubsan;
  trap mode instruments checks without that runtime dependency.
- 15 shuffled batches / 2,038 observations agree with the user's original
  SWEGCA accumulator, including the new segmented case.

This removes contiguous whole-connection metadata reallocation. It does not
implement region/portal/shared-experience graph partitioning, bound total RSS,
stream giant originals, or eliminate the refinement report's linear storage.
