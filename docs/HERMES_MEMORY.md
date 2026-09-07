# Hermes native memory integration

SWEGCA+VRS is the memory backend of the existing agent, not an agent controller.
The host calls its native provider on each supported text turn; the LLM need not
choose an MCP tool for automatic recall/capture to happen.

```
Hermes CLI sessions
  -> native MemoryProvider (automatic prefetch / completed-turn sync)
  -> private Unix socket
  -> one SWEGCA core owner -> hot index / SQLite / VRS
```

## Scope and compatibility

- Linux/Python 3.11+ with a recent Hermes MemoryProvider API. Actual loader and
  MemoryManager tested against local 0.19.0 sources; exact file hashes accompany
  the validation artifacts because that checkout contains pre-existing changes.
- Standalone plugin: no Hermes source patch, model invocation, or extra LLM.
- Current [official Hermes provider documentation](https://hermes-agent.nousresearch.com/docs/developer-guide/memory-provider-plugin)
  exposes `prefetch`, `sync_turn`, and directory plugins. Use the directory
  route for compatibility with the tested local revision.
- CLI primary-agent profiles only. Gateway/Discord/Telegram initialization is
  rejected: one socket shares the whole store and is NOT a multi-user ACL.
  Separate security domains need separate services/stores/sockets.
- OpenClaw 2.0 is not supported by this adapter. Its plugin hooks need their own
  implementation and version-specific acceptance test.
- The original nine-tool MCP core mode is unchanged. Do not start that stdio
  owner on a store already owned by the agent service. This release does not
  yet proxy those nine MCP tools through the resident service.

## Install

Install the core service in this repository's isolated environment:

```sh
uv sync --locked --extra core
```

Install the package into the **existing Hermes Python environment** as well.
The Hermes adapter/client imports only the standard library; `--no-deps` avoids
bringing a second torch/MCP installation into the host runtime:

```sh
uv pip install --python /absolute/path/to/hermes/python --no-deps /absolute/path/to/SWEGCA-VRS-MCP
```

Create a NEW profile; existing profiles/configs are never overwritten:

```sh
.venv/bin/python tools/prepare_hermes_profile.py \
  --profile /absolute/private/hermes-swegca \
  --socket /run/user/1000/swegca-vrs-hermes/m.sock
```

Use your actual UID, not necessarily 1000. The socket path must be short enough
for Unix sockets. The installer writes a plugin directory, a non-secret `.env`
socket setting and this profile-scoped configuration:

```yaml
memory:
  provider: swegca-vrs
  memory_enabled: false
  user_profile_enabled: false
```

This replaces the two built-in long-term memory channels **for this profile**.
It does not delete old memory files or replace Hermes' transcript, system prompt,
SOUL, context compressor or task executor. Configure the new profile's model
and authentication using Hermes' normal setup; credentials are not copied.

Start the service before starting Hermes:

```sh
.venv/bin/swegca-vrs-memory \
  --state-dir /absolute/private/swegca-hermes-store \
  --socket /run/user/1000/swegca-vrs-hermes/m.sock \
  --vrs-interval 30
```

The socket parent/store must be owner-only (0700). The socket is 0600. No TCP
listener, cloud memory, arbitrary filesystem tool or action API is exposed.
Run the normal Hermes command with `HERMES_HOME` pointing to the new profile;
it loads that profile's `.env`. A user-level systemd service is suitable for
keeping the core resident; see the [unit template](../integrations/hermes/swegca-vrs-hermes.service.example).

## Runtime behavior

1. Before each supported text turn: hot Déjà vu -> Recall -> Replay ->
   Re-evidence. All related candidates are processed; returned pages bound
   context size only. Query text alone is not independent current evidence,
   so it never grants verified support or write/action authority.
2. After a completed turn: persist the original user/assistant text as an
   unsigned `pending` observation with session/turn provenance and integer
   nanosecond timestamp. A response is NOT proof of task success.
3. Save delivery envelopes to a private, fsynced outbox before returning from
   capture. A background sender retries failures with the same immutable ID and
   timestamp. It removes an envelope only after the core acknowledges commit.
4. Every configured interval, VRS runs only if experience is dirty. This is
   coalescing, NOT an incremental numerical VRS implementation: the current
   numerical kernel still evaluates the whole graph, includes every outcome,
   and does not prune before convergence. Large-store latency remains unmeasured.
5. `swegca_recall`, `swegca_episode`, `swegca_memory_status` are additional host
   tools for pagination/full records/diagnosis; automatic callbacks do not
   depend on voluntary tool use. Core memory survives host and service restarts.

## Honest limits and failure behavior

- Retrieval is lexical, not semantic embedding search. The conversation adapter
  does not invent propositions, outcomes, independent evidence, translations or
  cross-turn semantic links. VRS participation is not proof of learned meaning.
- Only completed user/assistant text is auto-captured. Raw tool payloads,
  credentials, screen/audio, interrupted turns and arbitrary transcript dumps
  are NOT auto-captured. No comprehensive all-event experience claim is made.
  User/assistant text itself may be sensitive: all state/outbox data stays local
  and private; operators still control what they tell the host.
- The host may skip recall for non-text/multimodal turns in the tested version.
  No API-v2 compression-checkpoint guarantee is claimed for that version.
- Outbox delivery is durable after `sync_turn` returns, not before the host
  invokes it. Hermes itself queues the callback asynchronously; a crash before
  that invocation can lose this capture (the host transcript remains separate).
- Offline recall emits an explicit unavailable notice; it does not fabricate
  memory. Hermes treats external providers as best effort: this plugin cannot
  guarantee that the host refuses all reasoning while the service is down.
- One automatic context page is 20 records with a text budget; more related
  episodes/full content remain addressable through tools. Large responses may
  require a smaller page. Very large individual records can hit wire limits.
- The service's single owner/locks serialize mutations. VRS can hold that core
  lock; a large global convergence may exceed client timeouts. Pending envelopes
  remain retryable. No sub-nanosecond or large-corpus latency claim is made.
- Socket ownership is separately leased. A dead same-owner Unix socket can be
  recovered after a crash; a live socket, regular file or symlink is not removed.

## Verification commands

```sh
uv sync --locked --extra core --extra test
.venv/bin/python -m pytest -q
.venv/bin/python tools/verify_port.py
.venv/bin/python tools/verify_hermes_integration.py \
  --hermes-source /absolute/path/to/hermes-agent \
  --output local-data/hermes-verification-new-run
```

The last test runs three real Hermes loader/MemoryManager subprocesses and two
service subprocesses with isolated synthetic data. It makes no LLM call and
does not prove live generated answer quality. Output directories must be new;
failed logs are retained, not overwritten.
