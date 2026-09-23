# SWEGCA C++ Stage 6 write-receipt codec conditions

Status: static-review draft for the architecture inventory §10 step 9. No
test code, build, syntax compilation or benchmark is authorized by this list.
The codec, its placement and Main's publication route are incomplete. These
conditions become executable only after the architecture and the complete
test-condition list pass static review.

Sources: `SWEGCA_CPP_ARCHITECTURE_MODULE_INVENTORY_20260923.md` §2.9-10,
§3E-G, §4, §9-10; `SWEGCA_CPP_MAIN_STATE_STORAGE_REVIEW.md` §Required
contract, §Candidate representation; the user's
`mosaic_bounded_world_write.py@3bddcb7:216-306,433-475`.

| ID | Setup | Required result | Rule |
|---|---|---|---|
| R1 | Encode one committed C++ `BoundedWriteReceipt` twice with different sink buffering, then hash each sink's bytes or feed each stream to the parser | Identical canonical byte streams and SHA-256 digests; every field of the C++ receipt appears in one fixed order | Inventory §2.9, I07; writer receipt :458-465 |
| R2 | Use evidence references in proposal order with repeats, a present prior write head, then an absent one | Decode keeps order, repeats and the exact optional value; no evidence is silently dropped or inferred | Writer source :216-230, :433-475 |
| R3 | Give a before-slot larger than one 16 MiB journal record; feed its logical stream at field boundaries and several positions within the slot | Codec accepts the valid stream without a whole-receipt allocation; each split gives the same decoded value and digest | Storage review: bounded receipt parts; inventory §9 |
| R4 | Decode a truncated stream, extra trailing byte, unknown version, invalid scalar type, zero slot width, malformed text, multiplication overflow, a slot byte count beyond the host container's `size_type`/`max_size`, or a slot length different from Main's expected shape | Fail closed before a truncated length is cast or reserved; malformed text may report the typed identity's own error; never return a partial stored receipt or Main publication identity | Inventory §2.9, §4; writer source :243-306; native tensor `validate_shape` rejects zero width |
| R5 | Alter `before_slot` bytes or `before_slot_hash`, or alter one receipt-ID seed field while leaving the ID unchanged | Recomputed slot hash or receipt ID rejects the data | Writer source :433-445, :458-465; I07 |
| R6 | Supply a valid stream with a different expected scalar type or width | Reject before allocating the slot byte buffer; charged allocations remain within the caller's supplied account | Storage review §Required contract; writer source :424-428 |
| R7 | Change the raw `before_publication` locator to an all-zero field or to another structurally valid record | The zero form is rejected; a structurally valid locator remains **data only** until Main compares it with its selected marker, journal root and predecessor | Inventory §4, §9; I01, I10 |
| R8 | Supply an authority domain other than cognitive-state commit, or feed codec output to an unrelated authority gate | Wrong domain fails decode; a correctly decoded value still grants no capability or write authority | Inventory §2.8-10; I10 |
| R9 (`route pending`) | Change a field outside the slot-hash and receipt-ID preimages, such as the after-state/slot hashes, target role, prior write head, structurally valid predecessor publication, or decision/binding/preview digests; make the stream internally well formed | The detached codec may decode it as data. Main must compare the complete receipt with its selected predecessor, successor, journal and decision inputs before publication, rollback, retraction or linked promotion; the prior head's ID cannot be recomputed from this receipt alone | Inventory §2.9, §4, §9; I07, I10 |

Later tests must also cover the kind-7 publication body's receipt placement,
bounded parts, exact predecessor, selected Main marker and crash boundaries.
Those are route conditions, not properties of a detached codec.
