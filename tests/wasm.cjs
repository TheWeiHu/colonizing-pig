const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

(async () => {
  const variant = process.argv[2] || 'single';
  const fixtures = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
  const createPig = require(path.resolve(`dist/pig-${variant}.js`));
  const engine = await createPig();
  assert.equal(engine._pig_init(variant === 'single' ? 1 : 4), 1);
  const results = [];
  for (const f of fixtures) {
    engine._pig_new(f.seed, f.human);
    for (const action of f.history) assert.equal(engine._pig_replay(action), 1);
    const start = performance.now();
    const action = engine._pig_bot();
    const state = JSON.parse(engine.UTF8ToString(engine._pig_snapshot()));
    assert.equal(action, f.action, `phase ${f.phase}, move ${f.history.length}`);
    assert.equal(state.players[1-f.human].dev, null);
    results.push({move: f.history.length, phase: f.phase, action, ms: Math.round(performance.now()-start)});
  }
  console.log(JSON.stringify({variant, checked: results.length, heapMB: engine.HEAPU8?.length / 1048576, results}));
  process.exit(0);
})().catch(error => { console.error(error); process.exit(1); });
