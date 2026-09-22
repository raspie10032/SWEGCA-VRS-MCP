# SWEGCA VRS-MCP logic order — approved 2026-09-22

The user approved the flow with `순서맞음 ㄱㄱ` on 2026-09-22. Claude's
16:33 KST review records the three N/C/E corrections that accompanied the
reviewed flow: receipt cardinality, complete pre-Replay timing, and Déjà vu
miss detection. This revision writes those corrections into the diagram. It
is not an implementation claim or a test result.

## User input and memory read

```mermaid
flowchart TD
    A[User input arrives at resident hook] --> B[Déjà vu: use input as the key on pinned session VRS]
    B --> E{Session matched cues empty?}
    E -- Yes --> F[Déjà vu on pinned main VRS with the same input]
    E -- No --> C[Preactivate weighted regions from matched cues]
    F --> C2[Preactivate weighted main regions from matched cues]
    C --> D[Query coactivation witnesses, plan eligible portal, navigate one cue page]
    C2 --> D2[Query main coactivation witnesses, plan portal, navigate one cue page]
    D --> R[Recall complete candidate address set]
    D2 --> R2[Main Recall complete candidate address set]
    R --> S[Keep all addresses in memory_selection]
    R2 --> S
    S --> H[Select first current original from author Recall order]
    H --> I[Replay only selected original: exact content, source, revision, outcome, uncertainty]
    I --> J[Main-owned Re-evidence against current generation]
    J --> K{Relevant opposing evidence or conflict?}
    K -- Yes --> L[Replay only relevant opposing originals]
    L --> M[Re-evidence with both sides and preserve unresolved conflict]
    K -- No --> N[Issue four-stage receipt for opened originals]
    M --> N
    N --> O[Page full memory_selection separately; no action or truth authority]
```

The input is not subjected to a model decision, old capsule lookup, main
fallback probe, transcript scan, or cold index build before Déjà vu. Author
region preactivation, coactivation query, portal planning, and one bounded
local-navigation page sit between Déjà vu and Recall. The <1 ms gate covers
Déjà vu, navigation, and completion of Recall before Replay. First Recall
entry, Replay, and MCP completion are separate diagnostics. Main fallback
latency is reported separately and is not hidden by session timing.
The target begins when the host receives the user input, including dispatch
before the hook. Hook entry is an internal diagnostic start only. Without a
host input timestamp, the full target remains unverified.

The session-to-main miss is `matched_cues == ()` at Déjà vu. A zero-candidate
Recall records an honest miss and no Replay. Navigation follows the author's
single eligible portal and one cue page; deferred regions remain explicit, so
the result does not claim a complete transitive graph search. A transport page
limit never removes a Recall address. The receipt's Recall, Replay, and
Re-evidence rows bind one-to-one for the opened originals; the full Recall
address set remains separately in `memory_selection`. A historical result or
retrieval rank cannot grant authority.

## Experience admission and SessionEnd

```mermaid
flowchart TD
    A[Host appends a visible conversation record] --> B[Session capture reads the complete record]
    B --> C[Preserve role, exact source, revision, uncertainty and original content]
    C --> D[Admit observation through SWEGCA VRS into session-local native journal]
    D --> E[Publish session VRS generation and derived cue, region and portal addresses]
    E --> F{Real SessionEnd received?}
    F -- No --> G[Continue session-local admission and session-first reads]
    G --> A
    F -- Yes --> H[Capture final stable transcript tail and close session writers]
    H --> I[Validate all complete session VRS shards and their source lineage]
    I --> J[Atomically link complete native session journals to main ownership]
    J --> K[In background, replay VRS observations through main SWEGCA append and graph update]
    K --> L[Publish main generation and derived read projections]
```

The transcript cursor keeps positions and exact VRS addresses, not recalled
content. A record is not promoted to semantic truth simply because it was
admitted. `Interrupt`, silence, and elapsed time are not SessionEnd. The link
transfers ownership durably after SessionEnd; main generation integration
then replays already admitted VRS observations through the author's append
path in the background. Original session journals remain available. Neither
step reads the transcript as a recall source or runs before SessionEnd.
Visible tool results are admitted as ordered source-bound VRS observations as
well. A result longer than one author observation field is split while
preserving its byte/line spans, digest, role, revision, and part order.

## Authority and background work

Only main-owned SWEGCA evidence accumulation and judgment can change a
persistent belief. A read receipt and an observation admission grant no World,
action, semantic promotion, model-update, distribution, or P3 authority.
Consolidation and derived-index publication are background generation work;
independent preparation can use the 16-thread CPU. They cannot stand in front of
the input → Déjà vu → navigation → Recall path. Memory and disk budgets remain hard
limits without deleting experiences, regions, portals, or evidence logic.

## Approved read-route points

1. Déjà vu reads the active session VRS first. Empty matched cues trigger
   Déjà vu, navigation, and Recall on main with the same input.
2. Region preactivation, membership lookup, coactivation witness query,
   portal planning, and one local-navigation page follow Déjà vu and feed
   navigation cues into Recall. They precede its candidate selection.
3. A Replay opens the first selected current original; only conflict-related
   opposing originals are added before the final Re-evidence receipt.
4. The current input is a recall key immediately; it becomes an admitted
   observation when the host-visible transcript record is captured, without
   holding up the pre-Replay <1 ms boundary.
