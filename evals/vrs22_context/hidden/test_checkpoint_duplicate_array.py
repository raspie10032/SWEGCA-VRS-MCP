"""Supplemental frozen acceptance: duplicate array checkpoint member names."""
from io import BytesIO
import warnings
import zipfile

import pytest

from swegca_vrs2.checkpoint import decode, encode
from swegca_vrs2.store import Main


def test_duplicate_array_member_fails_before_array_decode(tmp_path):
    main = Main(tmp_path / "state", allow_ingest=True)
    try:
        main.ingest(dict(request_id="array", text="array duplicate", source="test:array",
                         revision="1"))
        view = main.checkpoint_view()
        original = encode(view)
        arguments = dict(identity=view.identity, seq=view.sequence, pair=view.pair.snapshot_id)
        with zipfile.ZipFile(BytesIO(original)) as archive:
            array_name = next(name for name in archive.namelist() if name.endswith(".npy"))
            array_body = archive.read(array_name)
        duplicate = BytesIO(original)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(duplicate, "a") as archive:
                archive.writestr(array_name, array_body)
        with pytest.raises(ValueError, match="checkpoint_integrity_failed"):
            decode(duplicate.getvalue(), **arguments)
    finally:
        main.close()


def test_duplicate_guard_runs_before_any_member_read(tmp_path, monkeypatch):
    main = Main(tmp_path / "state", allow_ingest=True)
    try:
        main.ingest(dict(request_id="order", text="order", source="test:order", revision="1"))
        view = main.checkpoint_view()
        original = encode(view)
        arguments = dict(identity=view.identity, seq=view.sequence, pair=view.pair.snapshot_id)
        with zipfile.ZipFile(BytesIO(original)) as archive:
            metadata = archive.read("metadata.json")
        duplicate = BytesIO(original)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(duplicate, "a") as archive:
                archive.writestr("metadata.json", metadata)
        def forbidden_read(*_args, **_kwargs):
            raise AssertionError("archive member read occurred before duplicate rejection")
        monkeypatch.setattr(zipfile.ZipFile, "read", forbidden_read)
        with pytest.raises(ValueError, match="checkpoint_integrity_failed"):
            decode(duplicate.getvalue(), **arguments)
    finally:
        main.close()
