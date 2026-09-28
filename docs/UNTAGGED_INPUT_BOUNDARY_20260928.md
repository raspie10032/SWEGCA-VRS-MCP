# Untagged input boundary, 2026-09-28

A missing membership observation is not an observed empty membership set.
`CollisionInput::membership_observed` now defaults to false. The existing membership-only engine returns `needs_observation` without changing counts or emitting a fabricated verdict in that case. This is a pending work state, not a SWEGCA abstention, content rejection, or completed VRS processing. The image adapter explicitly marks its recorded tag observation present. An explicitly observed empty set still reaches the membership predicate and core judgment.

Regression checks cover absent observations with unchanged states and zero callbacks, explicitly observed empty sets with core rejection, and previous dirty-input, recovery, repeat, and invalid-binding cases. The runner and test build succeeded; input-collision-tests passed.

This repair does not implement a general raw-input common-denominator observer. The present raw-content observer explicitly implements byte equality; it must not be substituted for the user's broader common-denominator proposition. Runtime retention without an incoming observation also does not supply that missing implementation. Full-PC ingestion/collision has not begun.

Local progress terminal is launched separately with discovered-file counts, zero ingestion/verification counts, and the missing implementation. Inventory categories are extension-based estimates, not deduplicated or MIME-verified. No file contents or private inventory paths are committed.
