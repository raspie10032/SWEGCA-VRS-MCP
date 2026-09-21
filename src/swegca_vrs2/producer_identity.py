# -*- coding: utf-8 -*-
"""Signed producers and users — the sixth step toward 3.0 (2026-09-19).

The evidence accumulator counts *producers*: the paper's "distinct source" proxy. In vrs2 a producer has been
a string in ``metadata.producer`` — anything could claim to be ``asm-agent`` or ``pytest-runner``, and once
several users or machines feed one store, a claimed producer id is worth nothing. This module binds a producer
id to a key pair (Ed25519, via ``cryptography``; the private key stays with the producer, the registry holds
public keys only), lets ``produce()`` sign what it sends, and lets the daemon verify on ingest:

* registered producer + valid signature   -> ``metadata.verified = True``
* registered producer, no/invalid signature -> ``metadata.verified = False``
* unregistered producer                    -> no ``verified`` key (a string proxy, as before)

The evidence layer then folds no ``verified: False`` row into any hypothesis (it stays recallable, weight 0):
a registered id cannot be borrowed for source diversity, and a row that says nothing about *who* observed is
not counted as a producer of its own either. Everything else is as it was: rows ingested before this step
carry no ``verified`` key and count as they always did. Signing is identity binding, not truth: a signed row
is a producer's *claim*, verified to be that producer's; the accumulator decides as before.

``user`` is provenance beside the producer: the configured identity of the session that produced a row
(``~/.claude/vrs2.json`` ``user``, default the OS login), carried in ``metadata.user`` and in the registry
entry of each producer. Nothing here grants authority; ``Memory != Truth`` holds for signed rows too.

Signed payload (canonical JSON, sorted keys): producer, hypothesis, outcome, axes (sorted), context, source,
revision, text_sha256. The row's text is bound by its digest, so a signature does not cover 60 KB of text.
"""
import hashlib
import io
import json
import os
import time

import getpass

def _setting(name, default):
    return os.environ.get("VRS2_" + name, default)

_HOME = os.path.expanduser("~")
PRODUCERS = _setting("PRODUCERS", os.path.join(_HOME, ".config", "swegca-vrs2", "producers.json"))
KEYS = _setting("KEYS", os.path.join(_HOME, ".local", "share", "swegca-vrs2", "producer-keys"))
try:
    _LOGIN = getpass.getuser()
except Exception:
    _LOGIN = "unknown"
USER = _setting("USER", _LOGIN)

SIGNED_FIELDS = ("producer", "hypothesis", "outcome", "axes", "context", "source", "revision", "text_sha256")


def _crypto():
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
        from cryptography.hazmat.primitives import serialization
        from cryptography.exceptions import InvalidSignature
        return Ed25519PrivateKey, Ed25519PublicKey, serialization, InvalidSignature
    except ImportError:
        return None


def load_registry(path=None):
    path = path or PRODUCERS                     # resolved at call time (tests point PRODUCERS elsewhere)
    try:
        data = json.load(io.open(path, encoding="utf-8")) if os.path.isfile(path) else {}
    except ValueError:
        data = {}
    producers = data.get("producers") if isinstance(data, dict) else None
    return dict(producers=dict(producers or {}), path=path)


def save_registry(registry, path=None):
    path = path or registry.get("path") or PRODUCERS
    os.makedirs(os.path.dirname(path), exist_ok=True)
    io.open(path, "w", encoding="utf-8", newline="\n").write(
        json.dumps(dict(producers=registry["producers"]), ensure_ascii=False, indent=1))


def registry_digest(registry):
    body = json.dumps({k: v.get("public_key") for k, v in sorted(registry["producers"].items())}, sort_keys=True)
    return hashlib.sha256(body.encode("utf-8")).hexdigest()[:12]


def key_path(producer, keys=None):
    keys = keys or KEYS
    safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in producer)
    return os.path.join(keys, safe + ".key")


def keygen(producer, *, user=None, note="", registry_path=None, keys=None, replace=False):
    """A new Ed25519 pair for ``producer``: private seed to ``<keys>/<producer>.key`` (hex), public key into the
    registry with the user and time. Refuses to replace an existing registration unless ``replace``."""
    crypto = _crypto()
    if crypto is None:
        raise RuntimeError("cryptography is not installed: cannot generate keys here")
    Ed25519PrivateKey, _, serialization, _ = crypto
    registry_path = registry_path or PRODUCERS
    keys = keys or KEYS
    registry = load_registry(registry_path)
    if producer in registry["producers"] and not replace:
        raise ValueError(f"producer {producer!r} is already registered (use replace)")
    private = Ed25519PrivateKey.generate()
    seed = private.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    public = private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    path = key_path(producer, keys)
    os.makedirs(keys, exist_ok=True)
    with io.open(path, "w", encoding="ascii", newline="\n") as out:
        out.write(seed.hex() + "\n")
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    entry = dict(public_key=public.hex(), key_id=hashlib.sha256(public).hexdigest()[:12], user=user or USER,
                 since=time.strftime("%Y-%m-%dT%H:%M:%S"), note=note)
    registry["producers"][producer] = entry
    save_registry(registry, registry_path)
    return dict(producer=producer, key_id=entry["key_id"], public_key=entry["public_key"], private_key_path=path, user=entry["user"])


def private_key_for(producer, keys=None):
    """The producer's private key when this machine holds it, else None."""
    crypto = _crypto()
    path = key_path(producer, keys)
    if crypto is None or not os.path.isfile(path):
        return None
    Ed25519PrivateKey = crypto[0]
    try:
        seed = bytes.fromhex(io.open(path, encoding="ascii").read().strip())
        return Ed25519PrivateKey.from_private_bytes(seed)
    except (ValueError, OSError):
        return None


def canonical(fields):
    body = {k: fields.get(k) for k in SIGNED_FIELDS}
    body["axes"] = sorted(str(a) for a in (body.get("axes") or ()))
    return json.dumps(body, sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def text_digest(text):
    return hashlib.sha256((text or "").encode("utf-8")).hexdigest()


def sign(fields, producer=None, keys=None):
    """Signature (hex) over the canonical payload with the producer's private key, or None when this machine
    holds no key for it (the row then goes unsigned, as before)."""
    producer = producer or fields.get("producer")
    private = private_key_for(producer, keys)
    if private is None:
        return None
    return private.sign(canonical(fields)).hex()


def signature_fields(args):
    """The signed payload of an ingest request (``produce()`` argument shape or a daemon ``ingest`` row)."""
    meta = args.get("metadata") or {}
    return dict(producer=meta.get("producer"), hypothesis=args.get("proposition") or args.get("hypothesis"),
                outcome=args.get("outcome"), axes=meta.get("axes") or (), context=meta.get("project") or args.get("context"),
                source=args.get("source"), revision=args.get("revision"), text_sha256=meta.get("text_sha256") or text_digest(args.get("text")))


def verify(producer, fields, signature, registry):
    """True / False for a registered producer, None when the producer is not registered (or no crypto here)."""
    entry = registry["producers"].get(producer)
    if entry is None:
        return None
    crypto = _crypto()
    if crypto is None or not signature:
        return False
    _, Ed25519PublicKey, _, InvalidSignature = crypto
    try:
        public = Ed25519PublicKey.from_public_bytes(bytes.fromhex(entry["public_key"]))
        public.verify(bytes.fromhex(signature), canonical(fields))
        return True
    except (ValueError, InvalidSignature):
        return False


def verify_row(args, registry=None):
    """Daemon side: stamp ``metadata.verified`` (True / False) on a row whose producer is registered; leave an
    unregistered producer's row untouched. Returns the verdict (True / False / None). Never raises."""
    try:
        registry = registry or load_registry()
        meta = args.get("metadata")
        if not isinstance(meta, dict) or not meta.get("producer"):
            return None
        fields = signature_fields(args)
        result = verify(meta["producer"], fields, meta.get("signature"), registry)
        if result is None:
            return None
        meta["verified"] = bool(result)
        meta.setdefault("text_sha256", fields["text_sha256"])
        if result:
            meta["key_id"] = registry["producers"][meta["producer"]].get("key_id")
        return result
    except Exception:
        return None


def producer_for_evidence(metadata):
    """The producer id the evidence layer counts, or None when the row is not evidence (a registered producer
    whose row failed verification); an unregistered id is the string as stored (a proxy)."""
    if metadata is not None and metadata.get("verified") is False:
        return None
    return str(metadata.get("producer") or "main") if metadata else "main"
