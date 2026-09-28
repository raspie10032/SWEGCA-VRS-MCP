# Whole-file codec ingress

Replaces the invalidated 256 KiB RPC-part experiment. The old run is stopped and is not imported into this run.

`tools/codec_file_ingest.py` snapshots the source bytes, uses installed ImageMagick/FFmpeg/pdftotext, and places the source, decoded views, codec metadata, and manifest into one whole-file bundle. No language/content exclusion. Same-stem transcript/subtitle sidecars are explicitly candidates, not asserted alignment. Unsupported subtitle conversion errors remain recorded; original embedded streams remain in the source. No fabricated tags or semantic verdicts.

`tools/whole_file_ingress.cpp` maps the whole bundle and invokes retain exactly once. ParallelIngress composes the existing core-backed retain path across at most ten different attached temporary sessions. One Runtime owns Main and shared memory/storage budgets. Duplicate session writers are rejected before mutation. Main merging is not performed during the active sessions. CPU code performs current judgments; GPU core execution is NOT implemented yet. This path does not perform new cross-experience semantic collision verification. Codec observations do not by themselves prove semantic association.

Basic regression: ten independent native writers, exact binary payload readback, and rejection of two writers to one session passed. First live checkpoint: 200 whole-source bundles, 200 originals, 320,912,913 original bytes, no ingestion exceptions; 200 core abstentions. That is input-path evidence, not validation of corpus relationships. Service RAM at that checkpoint: 1,219,710,976 bytes.

The bundle is one experience, not one experience per frame/chunk. Its media type declares the bundle encoding. Large mappings rely on the process cgroup resident-memory cap, not an address-space cap. Full-source duplicate hashes are recorded by the orchestration layer; native bundle hashes additionally guard identical bundle delivery. A changed snapshot is not marked complete.

Live root: `/var/mnt/storage-cold/vrs-pc-whole-20260928`; source contents, receipts, and private corpus manifests are NOT published to GitHub. No package install or prebuilt wheel. GPU core wiring, full corpus completion, and broad performance claims remain outstanding.
