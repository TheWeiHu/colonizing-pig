const {test, expect} = require('@playwright/test');
const fs = require('node:fs');
const SAVE_KEY = 'colonizing-pig-v1-porto-ruling';

test('resignation preserves the result through reload, export, import, and exit', async ({page}, info) => {
  await page.addInitScript(() => {
    const random=crypto.getRandomValues.bind(crypto);
    crypto.getRandomValues=values=>{random(values);if(values.length===2)values[1]=0;return values;};
  });
  await page.goto('/');
  await expect(page.locator('#start')).toBeEnabled({timeout:30000});
  await page.locator('#start').click();
  await expect(page.locator('#status')).toHaveText('Place A Settlement');
  await page.locator('#resign').click();
  await page.locator('#resign-dialog button[value=cancel]').click();
  await expect(page.locator('#status')).toHaveText('Place A Settlement');
  await page.locator('#resign').click();
  await page.screenshot({path:`build/resign-dialog-${info.project.name}.png`,fullPage:true});
  await page.locator('#resign-dialog button[value=resign]').click();
  await expect(page.locator('#status')).toHaveText('Oversized Pig Wins');
  await expect(page.locator('#hint')).toContainText('You resigned');
  await expect(page.locator('#resign')).toBeDisabled();
  await expect(page.locator('#actions button')).toHaveCount(0);
  expect(await page.evaluate(key=>JSON.parse(localStorage.getItem(key)).resigned,SAVE_KEY)).toBe(true);
  await page.reload();
  await expect(page.locator('#hint')).toContainText('You resigned',{timeout:30000});
  const downloading=page.waitForEvent('download');
  await page.locator('#export').click();
  const file=await (await downloading).path();
  const save=JSON.parse(fs.readFileSync(file,'utf8'));
  expect(save.resigned).toBe(true);
  expect(save.history).toEqual([]);
  await page.locator('#load-file').setInputFiles(file);
  await expect(page.locator('#hint')).toContainText('You resigned',{timeout:30000});
  await page.locator('#exit-game').click();
  await expect(page.locator('#resume-game')).toHaveText('View Result');
  await expect(page.locator('#resume-game')).toBeEnabled({timeout:30000});
  await page.locator('#resume-game').click();
  await expect(page.locator('#hint')).toContainText('You resigned',{timeout:30000});
  expect(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth)).toBe(true);
  await page.screenshot({path:`build/resigned-${info.project.name}.png`,fullPage:true});
  await page.locator('#new-game').click();
  await page.locator('#new-dialog button[value=new]').click();
  await expect(page.locator('#start')).toBeEnabled({timeout:30000});
  await page.locator('#start').click();
  await expect(page.locator('#status')).toHaveText('Place A Settlement');
  await expect(page.locator('#resign')).toBeEnabled();
});

test('resigning during search cancels work and stays final after reload', async ({page}) => {
  const f=JSON.parse(fs.readFileSync('build/fixtures.json')).find(x=>x.history.length===150);
  await page.addInitScript(({key,save})=>{if(!localStorage.getItem(key))localStorage.setItem(key,JSON.stringify(save));},
    {key:SAVE_KEY,save:{version:1,seed:f.seed,human:f.human,history:f.history,paused:false,log:[]}});
  await page.goto('/');
  await expect(page.locator('#game')).toHaveClass(/thinking/,{timeout:30000});
  await page.locator('#resign').click();
  await page.locator('#resign-dialog button[value=resign]').click();
  await expect(page.locator('#hint')).toContainText('You resigned');
  const history=await page.evaluate(()=>window.pigDiagnostics.lastState.history);
  await page.reload();
  await expect(page.locator('#hint')).toContainText('You resigned',{timeout:30000});
  await expect(page.locator('#game')).not.toHaveClass(/thinking/);
  expect(await page.evaluate(()=>window.pigDiagnostics.lastState.history)).toEqual(history);
  expect(await page.evaluate(()=>window.pigDiagnostics.lastState.done)).toBe(true);
});
