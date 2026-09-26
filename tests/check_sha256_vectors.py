"""Compare native and forced-scalar C++ digests with Python's independent SHA-256."""
import hashlib
import subprocess
import sys
payload=bytes((i*131+(i>>8))&255 for i in range((1<<20)+4))
previous=None
for exe in sys.argv[1:]:
    output=subprocess.check_output([exe],text=True)
    rows=output.splitlines()
    assert len(rows)==1052
    for row in rows:
        offset,length,digest=row.split()
        offset,length=int(offset),int(length)
        assert digest==hashlib.sha256(payload[offset:offset+length]).hexdigest(),row
    if previous is not None:assert output==previous
    previous=output
    print(f'PASS: {exe}: {len(rows)} independent digests, six streaming partitions each')
