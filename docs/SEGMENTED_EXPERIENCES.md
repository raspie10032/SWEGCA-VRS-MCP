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

## Main candidate prefix sharing

Main merge candidates now retain references to immutable full segments of the
previous Main connection. Their incomplete last segment is copied eagerly;
new appended observations never mutate any shared segment. This preserves
logical indices and existing object addresses in each sequence. Segment data
and shared ownership control storage both use the same VRS MemoryBudget, which
must outlive all holders. Sharing across different budget owners is rejected.

All segments are prepared in a temporary directory before publishing the new
sequence. Failed directory/control/data allocation releases the candidate's
references and leaves the previous Main state and accounting unchanged. The
last holder releases a segment, so destroying a previous sequence does not
invalidate its successor. Main still publishes its candidate graph atomically
only after successful preparation and persistence.

Connection inheritance preserves per-experience core admission checks and sets
the same initial candidate revision as the former repeated-append path. The
candidate still shuffles every actual old and new experience and reruns the
same core reduction and verdict. It does not inherit an accumulated tally or
skip older observations. Origin-store ranges and refinement reports
are still separately allocated; this does not make the whole merge constant
space or implement concurrent reader snapshots/region graph partitioning.

Regression coverage uses a 1030-value prefix: 1016 values in full immutable
segments are shared, the 14-value partial tail has separate storage, and both
owners can append independently. The successor survives destruction of the
previous owner. Failure injection covers prefix directory/control/data setup.
The real Main merge also checks that an original in a full segment retains its
object address after publication, while its exact serial/core result is checked
against freshly appended observations.

Observed allocation in the 1030-value regression: sealed values total 313120
bytes; creating the shared successor required 78024 additional tracked bytes,
including its directory, ownership control and independently allocated tail.
This is a prefix-metadata allocation result, not a whole-merge/RSS saving claim.
Normal/UBSan connection checks: 12907. Main graph: 3746 (742 allocation failure
points); parallel Main: 1272 (223 failure points); persistent Main: 174. The
original SWEGCA oracle still matches 15 shuffled batches / 2038 observations.


## Main original-source ranges

Each connection now retains one exclusive-end range per nonempty incoming
source, rather than repeating the source store pointer and read limit for each
experience. Appending experiences keeps their exact ordering and locations.
Replay checks the connection's observation count, then binary-searches the first
range ending after the selected index. The existing SWEGCA source-readability
check and exact original-location verification still apply before returning it.

Ranges are candidate-owned and published with the same atomic Main merge. A
failed candidate cannot change prior ranges. Recovery reconstructs the ranges
from existing source merges; no journal or original format changes. Source stores
must still outlive Main reads. This reduces provenance-reference metadata, not
the observation sequence or the full shuffle/core reduction work.

Regression coverage replays every index across two source ranges (16 and 64
observations), also checking a separate connection and both out-of-range bounds.
Existing allocation-failure, parallel merge and persistent recovery tests cover
the same publication paths.
