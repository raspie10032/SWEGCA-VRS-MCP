# Unicode data

`CaseFolding-16.0.0.txt` is the Unicode Character Database file
https://www.unicode.org/Public/16.0.0/ucd/CaseFolding.txt, downloaded
2026-09-23 with the user's approval, unchanged. SHA-256:
6f1f9c588eb4a5c718d9e8f93b782685e5c7fec872cf05e8e6878053599e09bb.
Its own header carries the Unicode copyright and terms of use.

16.0.0 is the Unicode version of the host python3 (`unicodedata`), which
runs the user's `_cue` (tinylm-slicer-sanabi-bazzite@3bddcb7
src/tinylm_slicer/mosaic_memory_activation.py:34-35, `str.casefold`).

`cpp/swegca_vrs/unicode_casefold.hpp` is generated from it:

    awk -f tools/generate_casefold_table.awk third_party/unicode/CaseFolding-16.0.0.txt \
        > cpp/swegca_vrs/unicode_casefold.hpp

The generator and this file are not part of the running store; only the
generated header is compiled.
