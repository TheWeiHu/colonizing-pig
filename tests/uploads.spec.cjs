const {test, expect} = require('@playwright/test');
const createPig = require('../dist/pig-single.js');
const config = require('../web/replay-config.json');
const SAVE = 'colonizing-pig-v1-porto-ruling', ENDPOINT = 'https://collector.example.test/games';
const engine = 'a'.repeat(64), model = 'b'.repeat(64);
let completed;

test.beforeAll(async () => {
  const wasm = await createPig(); wasm._pig_init(1); wasm._pig_new(2026110919, 0);
  let random = 12345, state;
  for (let n = 0; n < 10000; n++) {
    state = JSON.parse(wasm.UTF8ToString(wasm._pig_snapshot()));
    if (state.done) break;
    let moved = false;
    for (let k = 0; k < 249; k++) {
      random = (Math.imul(random, 1664525) + 1013904223) >>> 0;
      if (wasm._pig_replay(random % 249)) { moved = true; break; }
    }
    if (!moved) for (let a = 0; a < 249; a++) if (wasm._pig_replay(a)) break;
  }
  state = JSON.parse(wasm.UTF8ToString(wasm._pig_snapshot()));
  expect(state.done).toBe(true);
  completed = {version:1, seed:state.seed, human:state.human, history:state.history, paused:true, log:[]};
});

async function collector(page, reply, hosted = true) {
  await page.route('**/replay-config.js', route => route.fulfill({
    contentType:'application/javascript',
    body:`window.PIG_REPLAY_CONFIG={origin:${hosted ? 'location.origin' : JSON.stringify(config.origin)},endpoint:${JSON.stringify(ENDPOINT)},engine:${JSON.stringify(engine)},model:${JSON.stringify(model)}};`,
  }));
  await page.route(ENDPOINT, reply);
}
const pending = page => page.evaluate(() => Object.keys(localStorage).filter(k => k.startsWith('pig-replay-pending-v1:')).length);

test('resignation uploads automatically once and preserves the local save', async ({page}, info) => {
  const uploads = [];
  await collector(page, route => { uploads.push(route.request().postDataJSON()); return route.fulfill({status:201,json:{saved:true}}); });
  await page.addInitScript(() => {
    const random = crypto.getRandomValues.bind(crypto);
    crypto.getRandomValues = values => { random(values); if (values.length === 2) values[1] = 0; return values; };
  });
  await page.goto('/');
  await expect(page.locator('#welcome .replay-notice')).toBeVisible();
  await expect(page.locator('#start')).toBeEnabled({timeout:30000});
  await page.locator('#start').click();
  await page.locator('#resign').click();
  await page.locator('#resign-dialog button[value=resign]').click();
  await expect.poll(() => uploads.length).toBe(1);
  expect(Object.keys(uploads[0]).sort()).toEqual(['engine','human','model','moves','result','seed','version','winner']);
  expect(uploads[0]).toMatchObject({version:1, engine, model, human:0, moves:[], result:'resigned', winner:1});
  await expect.poll(() => pending(page)).toBe(0);
  expect(await page.evaluate(key => JSON.parse(localStorage.getItem(key)).resigned, SAVE)).toBe(true);
  await page.reload(); await expect(page.locator('#hint')).toContainText('You resigned', {timeout:30000});
  await expect.poll(() => pending(page)).toBe(0);
  await page.waitForTimeout(300);
  expect(uploads).toHaveLength(1);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({path:`build/uploads-${info.project.name}.png`,fullPage:true});
});

test('completed games survive a failed upload and retry after reload', async ({page}) => {
  const uploads = [];
  await collector(page, route => {
    uploads.push(route.request().postDataJSON());
    return route.fulfill(uploads.length === 1 ? {status:503,headers:{'Retry-After':'1'},json:{error:'busy'}} : {status:201,json:{saved:true}});
  });
  await page.addInitScript(({key, save}) => { if (!localStorage.getItem(key)) localStorage.setItem(key, JSON.stringify(save)); }, {key:SAVE, save:completed});
  await page.goto('/');
  await expect.poll(() => uploads.length, {timeout:30000}).toBe(1);
  await expect(page.locator('#status')).toHaveText('You Win!');
  await expect.poll(() => pending(page)).toBe(1);
  await page.reload();
  await expect.poll(() => uploads.length, {timeout:15000}).toBe(2);
  expect(uploads[0]).toEqual(uploads[1]);
  expect(uploads[0]).toMatchObject({result:'completed', winner:0, moves:completed.history});
  await expect.poll(() => pending(page)).toBe(0);
  expect(await page.evaluate(key => JSON.parse(localStorage.getItem(key)).history, SAVE)).toEqual(completed.history);
});

test('local games do not upload or show the hosted recording notice', async ({page}) => {
  let requests = 0;
  await collector(page, route => { requests++; return route.fulfill({status:201,json:{saved:true}}); }, false);
  await page.addInitScript(({key,save}) => localStorage.setItem(key,JSON.stringify(save)), {key:SAVE, save:completed});
  await page.goto('/'); await expect(page.locator('#status')).toHaveText('You Win!', {timeout:30000});
  await expect(page.locator('#game .replay-notice')).toBeHidden();
  expect(await page.evaluate(() => window.pigReplayUploads.enabled)).toBe(false);
  expect(await pending(page)).toBe(0); expect(requests).toBe(0);
});
