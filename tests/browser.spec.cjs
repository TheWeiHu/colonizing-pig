const {test, expect} = require('@playwright/test');
const fs = require('node:fs');
const fixtures = () => JSON.parse(fs.readFileSync('build/fixtures.json','utf8'));
const humanFirst = () => {
  const random = crypto.getRandomValues.bind(crypto);
  crypto.getRandomValues = values => { random(values); if (values.length === 2) values[1] = 0; return values; };
};
const saved = (f, human = f.human) => ({version:1, seed:f.seed, human, history:f.history, paused:false, log:[]});

test('opening, pause, reload, save file, and mobile layout', async ({page}, info) => {
  await page.addInitScript(humanFirst);
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto('/');
  await expect(page.locator('#start')).toBeEnabled({timeout:30000});
  await page.locator('#start').click();
  await expect(page.locator('#game')).toBeVisible();
  await expect(page.locator('#status')).toHaveText('Place A Settlement');
  await page.locator('#board [role=button]').first().click();
  await expect(page.locator('#status')).toHaveText('Place A Road');
  await page.locator('#board [role=button]').first().click();
  await expect(page.locator('#status')).toHaveText('Place A Settlement',{timeout:45000});
  await page.locator('#board [role=button]').first().click();
  await expect(page.locator('#status')).toHaveText('Place A Road');
  await page.locator('#board [role=button]').first().click();
  await expect(page.locator('#status')).toHaveText('Roll The Dice');
  const original = await page.evaluate(()=>window.pigDiagnostics.lastState.history);
  expect(original).toHaveLength(8);
  await page.locator('#pause').click();
  await expect(page.locator('#status')).toHaveText('Game Paused');
  await page.reload();
  await expect(page.locator('#status')).toHaveText('Game Paused',{timeout:30000});
  expect(await page.evaluate(()=>window.pigDiagnostics.lastState.history)).toEqual(original);
  await page.locator('#pause').click();
  await expect(page.locator('#status')).toHaveText('Roll The Dice',{timeout:30000});
  const downloadPromise = page.waitForEvent('download');
  await page.locator('#export').click();
  const download = await downloadPromise;
  const file = await download.path();
  const content = JSON.parse(fs.readFileSync(file,'utf8'));
  expect(content.history).toEqual(original);
  await page.locator('#new-game').click();
  await page.getByRole('button',{name:'Keep Playing',exact:true}).click();
  expect(await page.evaluate(()=>window.pigDiagnostics.lastState.history)).toEqual(original);
  await page.locator('#load-file').setInputFiles(file);
  await expect(page.locator('#status')).toHaveText('Game Paused',{timeout:30000});
  await page.locator('#pause').click();
  await expect(page.locator('#status')).toHaveText('Roll The Dice',{timeout:30000});
  expect(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth)).toBe(true);
  await page.screenshot({path:`build/${info.project.name}.png`,fullPage:true});
  await page.locator('#zoom').click();
  expect(await page.locator('#zoom').getAttribute('aria-pressed')).toBe('true');
  expect(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth)).toBe(true);
  await page.locator('#zoom').click();
  await page.getByRole('button',{name:'Roll Dice',exact:true}).click();
  await expect(page.locator('#status')).not.toHaveText('Roll The Dice');
  expect(errors).toEqual([]);
});

test('real midgame decision equals native; opponent cards remain hidden', async ({page}) => {
  const f=fixtures().find(x=>x.history.length===150);
  await page.addInitScript(save=>localStorage.setItem('colonizing-pig-v1-porto-ruling',JSON.stringify(save)),saved(f));
  await page.goto('/');
  await page.waitForFunction(length=>window.pigDiagnostics.lastState?.history.length>length,f.history.length,{timeout:60000});
  await page.locator('#pause').click();
  const state=await page.evaluate(()=>window.pigDiagnostics.lastState);
  expect(state.history[f.history.length]).toBe(f.action);
  expect(state.players[1-state.human].dev).toBe(null);
  expect(state.players[1-state.human].bought).toBe(null);
  expect(await page.evaluate(()=>window.pigDiagnostics.timings.length)).toBeGreaterThan(0);
});

test('single-worker fallback keeps the same midgame decision without isolation headers', async ({page}) => {
  const f=fixtures().find(x=>x.history.length===125);
  await page.route('**/*',async route=>{
    const response=await route.fetch(); const headers={...response.headers()};
    delete headers['cross-origin-opener-policy'];delete headers['cross-origin-embedder-policy'];
    await route.fulfill({response,headers});
  });
  await page.addInitScript(save=>localStorage.setItem('colonizing-pig-v1-porto-ruling',JSON.stringify(save)),saved(f));
  await page.goto('/');
  await page.waitForFunction(length=>window.pigDiagnostics.lastState?.history.length>length,f.history.length,{timeout:60000});
  await page.locator('#pause').click();
  expect(await page.evaluate(()=>crossOriginIsolated)).toBe(false);
  expect(await page.evaluate(()=>window.pigDiagnostics.workers)).toBe(1);
  expect(await page.evaluate(length=>window.pigDiagnostics.lastState.history[length],f.history.length)).toBe(f.action);
});

test('illegal imported moves preserve the previous saved game', async ({page}) => {
  await page.addInitScript(humanFirst);
  await page.goto('/');await expect(page.locator('#start')).toBeEnabled({timeout:30000});
  await page.locator('#start').click();await expect(page.locator('#game')).toBeVisible();await expect(page.locator('#status')).toHaveText('Place A Settlement');
  const before=await page.evaluate(()=>localStorage.getItem('colonizing-pig-v1-porto-ruling'));
  await page.locator('#load-file').setInputFiles({name:'bad.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify({version:1,seed:1,human:0,history:[181]}))});
  await expect(page.locator('#error')).toBeVisible({timeout:30000});
  expect(await page.evaluate(()=>localStorage.getItem('colonizing-pig-v1-porto-ruling'))).toBe(before);
  await page.locator('#retry').click();
  await expect(page.locator('#error')).toBeHidden();
  await expect(page.locator('#status')).toHaveText('Place A Settlement',{timeout:30000});
});


test('starting order is randomly assigned for each new game and survives reload', async ({page}) => {
  await page.addInitScript(() => {
    const random = crypto.getRandomValues.bind(crypto); let draw = 0;
    crypto.getRandomValues = values => { random(values); if (values.length === 2) values[1] = draw++ % 2; return values; };
  });
  await page.goto('/');
  await expect(page.locator('#start')).toBeEnabled({timeout:30000});
  await expect(page.locator('#start-form select')).toHaveCount(0);
  await page.locator('#start').click();
  await expect(page.locator('#game')).toBeVisible();
  await expect(page.locator('#status')).toHaveText('Place A Settlement');
  expect(await page.evaluate(() => window.pigDiagnostics.lastState.human)).toBe(0);
  expect(await page.evaluate(() => window.pigDiagnostics.lastState.history)).toEqual([]);
  await page.locator('#new-game').click();
  await page.locator('#new-dialog button[value=new]').click();
  await expect(page.locator('#start')).toBeEnabled({timeout:30000});
  await page.locator('#start').click();
  await page.waitForFunction(() => window.pigDiagnostics.lastState?.human === 1 && window.pigDiagnostics.lastState.history.length === 2, null, {timeout:45000});
  await expect(page.locator('#status')).toHaveText('Place A Settlement');
  await page.reload();
  await expect(page.locator('#game')).toBeVisible({timeout:30000});
  await expect(page.locator('#status')).toHaveText('Place A Settlement',{timeout:30000});
  expect(await page.evaluate(() => window.pigDiagnostics.lastState.human)).toBe(1);
  expect(await page.evaluate(() => window.pigDiagnostics.lastState.history.length)).toBe(2);
});
