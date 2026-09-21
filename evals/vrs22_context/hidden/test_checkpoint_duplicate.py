"""Frozen confirmation acceptance: duplicate checkpoint archive names."""
from io import BytesIO
import warnings
import zipfile

import pytest

from swegca_vrs2.checkpoint import decode, encode
from swegca_vrs2.store import Main


def test_duplicate_member_fails_closed_but_nonduplicate_extra_is_accepted(tmp_path):
    main = Main(tmp_path / "state", allow_ingest=True)
    try:
        main.ingest(dict(request_id="one", text="original", source="test:one", revision="1"))
        view = main.checkpoint_view()
        original = encode(view)
        arguments = dict(identity=view.identity, seq=view.sequence, pair=view.pair.snapshot_id)
        assert decode(original, **arguments)[0].episode_count == 1
        extra = BytesIO(original)
        with zipfile.ZipFile(extra, "a") as archive:
            archive.writestr("unused.txt", "unrelated")
        assert decode(extra.getvalue(), **arguments)[0].episode_count == 1
        duplicate = BytesIO(original)
        with zipfile.ZipFile(BytesIO(original)) as archive:
            metadata = archive.read("metadata.json")
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(duplicate, "a") as archive:
                archive.writestr("metadata.json", metadata)
        with pytest.raises(ValueError, match="checkpoint_integrity_failed"):
            decode(duplicate.getvalue(), **arguments)
    finally:
        main.close()
