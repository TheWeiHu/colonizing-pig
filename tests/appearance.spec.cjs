const {test, expect} = require('@playwright/test');
const fs = require('node:fs');

test('terrain stays colored across reloads; screenshots and details remain readable', async ({page}) => {
  const f = JSON.parse(fs.readFileSync('build/fixtures.json')).find(x => x.history.length === 150);
  await page.goto('/');
  await expect(page.locator('.ranking-proof')).toBeVisible();
  await expect(page.locator('.ranking-proof img')).toHaveCount(2);
  await expect(page.locator('#ranking-dialog')).toHaveCount(0);
  await page.evaluate(save => localStorage.setItem('colonizing-pig-v1-porto-ruling', JSON.stringify(save)), {version:1,seed:f.seed,human:1-f.human,history:f.history,paused:false,log:[]});
  for (let reload=0; reload<3; reload++) {
    await page.reload();
    await expect(page.locator('#game')).toBeVisible({timeout:30000});
    await expect(page.locator('.tile')).toHaveCount(19);
    const fills = await page.locator('.tile').evaluateAll(tiles => tiles.map(tile => getComputedStyle(tile).fill));
    expect(fills).not.toContain('rgb(0, 0, 0)');
    expect(new Set(fills).size).toBe(6);
  }
  await page.getByText('Game Details', {exact:true}).click();
  const state=await page.evaluate(() => window.pigDiagnostics.lastState);
  expect((await page.locator('#opponent td').allTextContents()).map(Number)).toEqual(state.players[1-state.human].hand);
  expect((await page.locator('#bank td').allTextContents()).map(Number)).toEqual(state.bank);
  expect(await page.locator('#opponent-dev').textContent()).toBe(String(state.players[1-state.human].devCount));
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});
