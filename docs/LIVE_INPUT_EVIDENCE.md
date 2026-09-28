# Live input evidence

`Runtime::receive` and `receive_envelope` accept an optional `EvidenceObservation`
with the incoming original. `swegca/receive` exposes it as `observation`.

Order: input → Déjà vu/Recall → validate observation → seal original and observation
in one record → shuffle current connection experiences → SWEGCA judgment →
strength update. The initial Recall still precedes storage and evidence work.
No old processed experience, separate `observe` call, session end or Main merge
is required to admit a live observation.

The host supplies recorded observational support/refutation/insufficiency, never
an accept/reject/abstain verdict. The input determines the hypothesis/cue. Source,
producer, actual observation context, axis, confidence and expiry are preserved.
The original payload is retained alongside those values. Invalid input is checked
before creating a durable connection. Missing observations remain insufficient.

Host field example (alongside the existing receive envelope):

```json
{
  "observation": {
    "source": "<64 lowercase hex digits>",
    "producer": "<64 lowercase hex digits>",
    "context": "<64 lowercase hex digits identifying the actual observation context>",
    "axis": "0",
    "confidence": 1.0,
    "hasExpiry": false,
    "expiresAt": "0",
    "outcome": "support"
  }
}
```

`outcome` accepts `support`, `refute`, `insufficient`. `hypothesis`, `address`,
`verdict` and `status` are rejected for incoming observations. The observation's
time is the input time. Same source/context/axis repetition remains the same
evidence group; callers must not fabricate contexts or axes to force decisions.
Existing SWEGCA sample, diversity, expiry and regime checks still apply.

This is a host evidence ingress contract. It does not infer measurements from
arbitrary natural language or prove a producer honest. Native event callers can
pass measured observations to `receive_envelope`; this change does not invent
observations for ordinary user text, or replace the existing address-bound tool
observation path. Deployment and native adapter wiring must be verified separately.
