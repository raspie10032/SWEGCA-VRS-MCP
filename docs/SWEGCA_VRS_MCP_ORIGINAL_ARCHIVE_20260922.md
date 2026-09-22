# Existing original-experience archive — 2026-09-22

This is an offline preservation receipt, not a rebuilt Main generation or a
runtime acceptance result. The original SQLite stores were opened read-only
in separate transactions; no owner rows were combined. An ephemeral standard
library exporter wrote the existing five-field rows into the canonical
`VRS2JNL1` frame format and left no migration program in this repository.
It reread every compressed frame, checked each frame checksum and sequence,
and compared the ordered five-field row SHA-256 with the source transaction.
The hash input for each row was its canonical UTF-8 JSON five-element array,
preceded by the byte length as unsigned little-endian 64-bit.

The private archives and complete per-owner receipts are under
`~/.local/share/swegca-vrs2-rebuild-archives-20260922/`. Directories are
owner-only; journal and receipt files are mode `0600`. Original files remain
at their prior addresses. The two source identities differ. No SessionEnd,
session-to-main link, certificate regeneration or C++ generation publication
was performed.

| Separate owner | Rows | Ordered source/archive row SHA-256 | Archive head SHA-256 | Export receipt SHA-256 |
| --- | ---: | --- | --- | --- |
| `owner-1` session original | 44,270 | `fe8da1c492b6ab17d1787a220fff38db86bbad589b040073c233859c1d4a99da` | `d36da9a56f88c4912002a6f4a6f9d7bc57b1c758c4d623e0d9d6efd368140c29` | `990e301f6e9e944a23fd8ece0b2b38b76c393e4478fc866312f0547969baf66b` |
| `owner-2` session shard original | 3,296 | `7b01afa9a44f051ca8b97fd69dcc359a1c248f4707a48b00cdff84ec75ca6a7c` | `35d3c22a04c5130b163fa81feb846a530626307f0ecd25edf4718c307f232e5f` | `8bc015057a0bdba3685aa0852cfe66ed48eebcd0bade78f6c9fb7afd6af4b6d7` |

The archives occupy 41,754,624 allocated bytes together. This receipt proves
row-preserving export at the observed source snapshot only. The next product
step is rebuilding one active SWEGCA C++ generation **per owner** from its
native archive, keeping the old pair IDs as historical certificates. Any
re-derived active pair IDs require a complete author-rule rebuild and a
separate lineage check. The archive itself does not authorize a main merge.
