// playwright.config.js
// Configuration for running Playwright regression tests for the Pico Web Serial Config Portal.
const { defineConfig, devices } = require('@playwright/test');

module.exports = defineConfig({
  testDir: './tests',
  timeout: 15000,
  expect: {
    timeout: 5000
  },
  fullyParallel: true,
  reporter: [['list']],
  use: {
    headless: true,
    ...devices['Desktop Chrome']
  }
});
