#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser(description='Verify feature source against the release snapshot')
p.add_argument('--source',required=True,type=Path)
p.add_argument('--manifest',required=True,type=Path)
a=p.parse_args();manifest=json.loads(a.manifest.read_text(encoding='utf-8'))
bad=[]
for name,digest in manifest['feature_file_sha256'].items():
    file=a.source/name
    if not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest()!=digest:bad.append(name)
if bad:raise SystemExit('SOURCE CHANGED: '+', '.join(bad))
print('All '+str(len(manifest['feature_file_sha256']))+' feature files match the snapshot.')
