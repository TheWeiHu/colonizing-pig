const {test, expect} = require('@playwright/test');

test('exit returns home, preserves progress across reload, and allows resume or a new game', async ({page}) => {
  await page.addInitScript(() => {
    const random=crypto.getRandomValues.bind(crypto);
    crypto.getRandomValues=values=>{random(values);if(values.length===2)values[1]=0;return values;};
  });
  await page.goto('/');
  await expect(page.locator('#start')).toBeEnabled({timeout:30000});
  await page.locator('#start').click();
  await expect(page.locator('#game')).toBeVisible();
  await page.locator('#board [role=button]').first().click();
  await expect(page.locator('#status')).toHaveText('Place A Road');
  const history=await page.evaluate(()=>window.pigDiagnostics.lastState.history);
  await page.getByRole('button',{name:'Exit Game',exact:true}).click();
  await expect(page.locator('#welcome')).toBeVisible();
  await expect(page.locator('#game')).toBeHidden();
  await expect(page.locator('#resume-game')).toBeVisible({timeout:30000});
  await expect(page.locator('#resume-game')).toBeEnabled({timeout:30000});
  await page.reload();
  await expect(page.locator('#resume-game')).toBeVisible({timeout:30000});
  await expect(page.locator('#resume-game')).toBeEnabled({timeout:30000});
  await expect(page.locator('#game')).toBeHidden();
  await page.locator('#resume-game').click();
  await expect(page.locator('#game')).toBeVisible();
  await expect(page.locator('#status')).toHaveText('Place A Road');
  expect(await page.evaluate(()=>window.pigDiagnostics.lastState.history)).toEqual(history);
  await page.locator('#exit-game').click();
  await expect(page.locator('#start')).toBeEnabled({timeout:30000});
  await page.locator('#start').click();
  await page.getByRole('button',{name:'Keep Playing',exact:true}).click();
  expect(await page.evaluate(()=>JSON.parse(localStorage.getItem('colonizing-pig-v1-porto-ruling')).history)).toEqual(history);
  await page.locator('#start').click();
  await page.locator('#new-dialog button[value=new]').click();
  await expect(page.locator('#game')).toBeVisible();
  await expect(page.locator('#status')).toHaveText('Place A Settlement');
  expect(await page.evaluate(()=>window.pigDiagnostics.lastState.history)).toEqual([]);
});

test('exit interrupts a thinking bot without overwriting the saved position', async ({page}) => {
  const fs=require('node:fs');
  const f=JSON.parse(fs.readFileSync('build/fixtures.json')).find(x=>x.history.length===150);
  await page.addInitScript(save=>{if(!localStorage.getItem('colonizing-pig-v1-porto-ruling'))localStorage.setItem('colonizing-pig-v1-porto-ruling',JSON.stringify(save));},{version:1,seed:f.seed,human:f.human,history:f.history,paused:false,log:[]});
  await page.goto('/');
  await expect(page.locator('#game')).toBeVisible({timeout:30000});
  await page.locator('#exit-game').click();
  await expect(page.locator('#resume-game')).toBeVisible({timeout:30000});
  await expect(page.locator('#resume-game')).toBeEnabled({timeout:30000});
  const saved=await page.evaluate(()=>JSON.parse(localStorage.getItem('colonizing-pig-v1-porto-ruling')));
  expect(saved.exited).toBe(true);
  await page.reload();
  await expect(page.locator('#resume-game')).toBeVisible({timeout:30000});
  await expect(page.locator('#resume-game')).toBeEnabled({timeout:30000});
  await expect(page.locator('#game')).toBeHidden();
  expect(await page.evaluate(()=>JSON.parse(localStorage.getItem('colonizing-pig-v1-porto-ruling')).history)).toEqual(saved.history);
});
