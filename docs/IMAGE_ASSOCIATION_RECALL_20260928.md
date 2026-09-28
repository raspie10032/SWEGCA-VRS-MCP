# Image association consumer

This is a read-only consumer of the existing VRS graph, not a new VRS stage.
No changes were made to the SWEGCA core, its verdicts or stored strengths.

1. `tools/image_cue.py` uses the already installed local DINO model to make a
   descriptor. It preserves the original image as input.bin. WD14 comparison
   labels are written separately and never supplied to the consumer. Python
   remains solely the adapter to existing model runtimes; traversal is C++.
2. `tools/image_associations.cpp` rejects a query already in the dataset and
   incompatible/nonfinite descriptors. It matches the DINO model digest and
   finds the highest cosine cue (SHA breaks exact ties). This measurement routes
   to a remembered source; it is not SWEGCA approval or a semantic truth claim.
3. The selected source and feature payload are read through ExperienceBlock's
   authenticated native addresses. The selected descriptor is checked against
   the sealed payload. Its recorded tags supply activation seeds.
4. Native link records are read, and one-hop neighbors outside the seed set are
   returned. SWEGCA `prefer_replay` selects the strongest persisted route to each
   endpoint and orders results, preserving its canonical address tie-break.
   All outcomes remain accessible, as specified by that core function. Labels
   are read from sealed endpoint records; links.csv is not used for recall.
5. Each additional tag includes its via tag, stored strength and native record
   block/offset/digest. No new-image tags are used, no verdict is manufactured,
   and neither the experience graph nor Main is changed.

## Invocation

```
make build/swegca-image-associations
build/swegca-image-associations DATASET GRAPH CUE_JSON OUTPUT_JSON
```

`CUE_JSON` has image_sha256, dino_sha256 and 384-element dino. Its directory
contains input.bin. Run `tools/image_cue.py IMAGE OUTPUT_DIRECTORY` using the
existing local model environment to produce it. Paths to those existing models
are currently local-machine specific. No package installation is performed.

## Actual held-out image test

Artifacts: `/var/home/raspie/Documents/Codex/vrs-unseen-image-20260928`.
Query SHA: `45b177d6ee53b9dfb57829371709ce54e5eb70325c9405073379d613bd78444c`.
It is absent from all 6,274 experience image hashes.

- Recalled source: 688; DINO cosine 0.718539, not a probability.
- Remembered seed tags: 49. Additional associated tags: 5,392.
- Traversed paths: 121,501. All selected stored strengths: 1.01.
- Disconnect control: same source and 49 seed tags, zero additional tags.
- Rejected: query tag leakage, zero descriptor, incompatible model digest.
- WD14 comparison has 59 labels: 13 overlap with seed tags and 45 with the
  additional set. Additional top 20 overlap: **0**. WD14 is not ground truth.
- Initial C++ full scan: 1.3466 seconds, excluding model extraction. This is
  not a resident indexed lookup and does not meet the 5ms Recall target.

The graph is connected enough that nearly its entire vocabulary is returned.
Uniform 1.01 strengths cannot discriminate relevance; canonical ties are not
semantic ranking. The test establishes stored-link-dependent traversal, but
**useful selective image-to-tag association has not been achieved**. No quality
thresholds, reranking model, or strengthening changes were added to mask this.

`tests/image_associations_integration.py EXECUTABLE DATASET GRAPH QUERY_DIR`
reproduces the disconnected control and malformed/leaking query checks after
an ordinary run has written recall.json. The current selection scans all stored
DINO observations and native link chunks; a selected query hash and selected
sealed descriptor are checked, but full descriptor-index freshness is not
independently audited for every nonselected source in this experimental CLI.
