import { test, expect } from '@playwright/test';

test('C++ gateway serves assets and validates history requests', async ({ request }) => {
  const health = await request.get('/api/health');
  expect((await health.json()).backend).toBe('cpp');
  for (const query of ['symbol=..%2FBAD&timeframe=1D', 'symbol=EURUSD&timeframe=bad', 'symbol=EURUSD&timeframe=1D&before=-1', 'symbol=EURUSD&timeframe=1D&before=9999999999']) {
    expect((await request.get(`/api/history?${query}`)).status()).toBe(400);
  }
  expect((await request.post('/api/history')).status()).toBe(405);
  expect((await request.get('/.env')).status()).toBe(404);
  expect((await request.get('/%2e%2e/.env')).status()).toBe(404);
  expect((await request.get('/api/health', { headers: { Origin: 'https://example.com' } })).status()).toBe(403);
});

test('C++ browser WebSocket handles malformed subscriptions and stays usable', async ({ page }) => {
  // Fetch only static text so the portal itself does not open an upstream stream.
  await page.goto('/NOTICE.txt');
  const messages = await page.evaluate(() => new Promise((resolve, reject) => {
    const socket = new WebSocket(`ws://${location.host}/ws`);
    const errors = [];
    const timeout = setTimeout(() => { socket.close(); reject(new Error('WebSocket timed out')); }, 5000);
    socket.onopen = () => { socket.send('{invalid'); socket.send(JSON.stringify({ type: 'subscribe', symbol: 'INVALID/PAIR' })); };
    socket.onmessage = event => {
      const data = JSON.parse(event.data);
      if (data.state !== 'error') return;
      errors.push(data.message);
      if (errors.length === 2) { clearTimeout(timeout); socket.close(); resolve(errors); }
    };
  }));
  expect(messages).toEqual(['Invalid subscription request', 'Invalid symbol format']);
});
