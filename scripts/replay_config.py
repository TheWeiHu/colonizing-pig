"""Pin replay provenance and the optional hosted-game collector in the static build."""
import hashlib, json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
engine = hashlib.sha256()
for path in sorted((root / 'engine').iterdir()):
    if path.suffix in {'.cpp', '.hpp'}:
        engine.update(path.name.encode() + b'\0' + path.read_bytes() + b'\0')
config = json.loads((root / 'web/replay-config.json').read_text())
config.update(engine=engine.hexdigest(), model=hashlib.sha256((root / 'model.json').read_bytes()).hexdigest())
(root / 'dist').mkdir(exist_ok=True)
(root / 'dist/replay-config.js').write_text('window.PIG_REPLAY_CONFIG = ' + json.dumps(config, separators=(',', ':')) + ';\n')
