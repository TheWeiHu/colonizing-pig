import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
model = json.loads((root / 'model.json').read_text())
for path, expected in model['sha256'].items():
    assert hashlib.sha256((root / path).read_bytes()).hexdigest() == expected, path
assert f'"{model["spec"]}"' in (root / 'engine/champion.hpp').read_text()
print(f'Verified {model["name"]}: exact search specification and all three weight checksums')
