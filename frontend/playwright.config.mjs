import { defineConfig } from '@playwright/test';
import { fileURLToPath } from 'node:url';
const projectRoot = fileURLToPath(new URL('..', import.meta.url)).replace(/[\\/]$/, '');
const backendExecutable = fileURLToPath(new URL(`../backend/build/tradermade_backend${process.platform === 'win32' ? '.exe' : ''}`, import.meta.url));
export default defineConfig({
  testDir: './tests/browser',
  fullyParallel: false,
  use: { baseURL: 'http://127.0.0.1:3001', browserName: 'chromium', channel: process.env.PLAYWRIGHT_CHANNEL || 'msedge', headless: true },
  webServer: { command: `"${backendExecutable}" --root "${projectRoot}"`, url: 'http://127.0.0.1:3001/api/health', reuseExistingServer: true },
});
