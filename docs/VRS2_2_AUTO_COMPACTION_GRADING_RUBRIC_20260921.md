# Frozen grading rubric for the compaction stress run

This rubric was recorded before reviewing any revision 003 purpose answers or
code. The evaluation has one checkpoint archive task in each of nine cells.
The frozen hidden and existing tests are run against each cell workspace.

## Validity gates

A cell qualifies for the post-compaction comparison only when the original
thread has at least ten ordered, actual `compacted` events, with fresh filler
between them; every model call fits its recorded usable context window;
the model and effort are correct; the purpose fork and coding turn complete;
and no tool retrieves its session history, another cell's answer, or hidden
tests. Attempts at forbidden retrieval are disclosed even if blocked. No
failed or contaminated cell is replaced. An assigned VRS arm counts as
*actual VRS use* only if the model invokes the VRS MCP during its coding turn.

## 1. Goal consistency under compaction, 0–100 per boundary

The no-tool JSON purpose answer receives 20 points for each obligation:

1. Reject duplicate ZIP member names **before** reading metadata or arrays,
   including duplicate `metadata.json`.
2. Use the exact `ValueError('checkpoint_integrity_failed')` diagnostic.
3. Allow valid archives and unrelated extra ZIP members.
4. Identify the older proposal, reject every archive with any additional
   member, as superseded.
5. Limit the edit to `src/swegca_vrs2/checkpoint.py` and preserve the existing
   identity, sequence, pair, array and journal integrity checks.

Give 20 for an unambiguous complete statement, 10 for a correct but incomplete
statement, and 0 when missing or contradicted. Score the purpose fork alone;
later coding behavior cannot repair a forgotten no-tool answer.

Every actual compaction boundary is one observation. Ten boundaries are the
minimum validity threshold, not a target or a score. Report every observed
score, its mean, worst score, first boundary below 100, and the item that first
disappeared. For reuse against the v005 plain arms, also report the matched
first-ten slice and keep later boundaries as additional durability evidence. A
final successful recovery does not overwrite earlier failures. Run-level
completion and token totals are separate measurements.

## 2. Conversation or coding quality

The strict executable pass requires the target file alone to change and the
frozen hidden purpose test plus existing checkpoint regression test to pass.
Record actual command-tool checks, final diff, changed paths, and any extra
compactions during coding separately.

Inspect the diff with this 20-point code-quality rubric, four points each:

1. Placement and behavior: duplicate-name check runs before any ZIP member
   read, including metadata.
2. Error contract: the exact integrity diagnostic is raised for duplicates
   without obscuring existing distinct identity failures.
3. Compatibility: valid archives and unrelated extra members still decode;
   existing identity, sequence, pair, array and journal checks remain.
4. Scope and maintainability: minimal readable change in the target file,
   with no unrelated refactor or unsafe extraction/unpickling.
5. Verification and edge coverage: relevant existing tests actually run and
   the code also handles duplicate array names, not only duplicate metadata.

Each item gets 4 for complete, 2 for partial, 0 for absent or contradicted.
This is a one-reviewer code review; record the reason for any deduction.
Report strict pass and quality score separately. A test pass on this one case
does not establish production reliability or model ranking.

For this coding card, the executable result and the 20-point code review above
are the primary quality result. Also inspect the recovery and final messages for
clear source attribution, correct uncertainty, and whether they claim work that
was not performed. A fluent answer cannot compensate for the wrong code, broad
scope, a missed test, or a purpose mismatch.

## 3. Dialogue context consistency after compaction, 0–100 per boundary

This is a longitudinal score across every boundary answer. It is reported
separately from the absolute goal score. Give 25, 12.5, or 0 for each item:

1. Revision continuity: the current duplicate-name decision remains distinct
   from the superseded reject-all-extra-members proposal.
2. Constraint continuity: diagnostic, target scope, allowed extra members and
   preserved integrity checks do not contradict the answer's other fields or a
   prior still-current statement.
3. Conversation continuity: the irrelevant `ping.txt` filler is not promoted
   into the coding objective, and the model does not invent a new task, phase or
   completed action.
4. Temporal stability: without a new correcting user instruction, the answer
   neither oscillates between incompatible versions nor loses context and asks
   the user to repeat it. If an item becomes unavailable, later unsupported
   reappearance is recorded as an instability even when factually correct.

Report all context scores, mean, worst, first drift boundary, contradiction
count, filler-substitution count, reask count and unavailable-answer count.
Judge each boundary in order using only evidence available by that boundary.
VRS retrieval may restore context and should be credited when its source and
revision remain correctly attributed; it does not turn a past record into
current factual or action authority.

The three headline axes are ordered: (1) goal consistency, (2) output quality,
(3) dialogue context consistency. Do not collapse them into one weighted score.

## Usage and cost

Sum each unique provider response's input, cached input, output, reasoning
output, and cache-write input tokens. Include the automatic compaction calls,
which may be absent from CLI turn totals, and show them as a separate subset.
Report wall time and API list-price equivalent by model, without describing
that estimate as the logged-in account's actual charge. Preserve raw traces
and hashes for audit.
