const {defineConfig, devices} = require('@playwright/test');
module.exports = defineConfig({
  testDir: './tests', testMatch: '*.spec.cjs', timeout: 90000,
  workers: 1, outputDir: 'build/browser-results',
  use: {baseURL: 'http://127.0.0.1:8090', headless: true, screenshot: 'only-on-failure'},
  webServer: {command: 'python3 serve.py --port 8090', url: 'http://127.0.0.1:8090', reuseExistingServer: false},
  projects: [
    {name: 'chromium', use: {browserName:'chromium', viewport:{width:1280,height:900}}},
    {name: 'mobile-webkit', use: {...devices['iPhone 13'], browserName:'webkit'}},
  ],
});
