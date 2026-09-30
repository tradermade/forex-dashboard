import { test, expect } from '@playwright/test';

// Existing chart fixtures keep their starter list; market catalogue coverage is in markets.spec.mjs.
test.beforeEach(async ({ page }) => {
  await page.route('**/api/instruments', route => route.fulfill({ status: 503, json: { error: 'Fixture: catalogue offline' } }));
});

const history = (symbol = 'EURUSD') => Array.from({ length: 120 }, (_, i) => {
  const base = symbol === 'USDJPY' ? 150 : 1.13;
  const open = base + Math.sin(i / 8) * .005 + i * .00003;
  const close = open + Math.cos(i) * .0005;
  return { time: 1790002800 + i * 3600, open, high: Math.max(open, close) + .0003, low: Math.min(open, close) - .0003, close };
});
async function mockStream(page) {
  await page.routeWebSocket('**/ws', ws => ws.onMessage(raw => {
    const { symbol } = JSON.parse(raw);
    ws.send(JSON.stringify({ type: 'status', state: 'waiting', message: 'Connected · awaiting ticks', symbol }));
  }));
}
test('renders charts, switches pair and timeframe, toggles SMA, and exports', async ({ page }) => {
  const errors = []; page.on('pageerror', error => errors.push(error.message));
  await mockStream(page);
  await page.route('**/api/history?**', route => {
    const symbol = new URL(route.request().url()).searchParams.get('symbol');
    return route.fulfill({ json: { candles: history(symbol) } });
  });
  await page.goto('/');
  await expect(page.locator('#bar-count')).toHaveText('120 candles');
  await expect(page.locator('#chart canvas').first()).toBeVisible();
  await page.locator('#average').click(); await expect(page.locator('#average-label')).toBeVisible();
  await page.locator('#chart-type').click(); await expect(page.locator('#chart-type')).toHaveAttribute('aria-label', 'Switch to candlestick chart');
  await page.locator('[data-timeframe="5m"]').click(); await expect(page.locator('[data-timeframe="5m"]')).toHaveAttribute('aria-pressed', 'true');
  await page.locator('[data-symbol="USDJPY"]').click(); await expect(page.locator('#symbol-name')).toContainText('USD / JPY');
  await expect(page.locator('#price')).toHaveText(/150\.\d{3}/);
  const downloadEvent = page.waitForEvent('download'); await page.locator('#export').click();
  expect((await downloadEvent).suggestedFilename()).toBe('USDJPY-5m.csv');
  await page.locator('#search').fill('pound'); await expect(page.locator('.pair-row')).toHaveCount(2);
  expect(errors).toEqual([]);
});
test('buffers streaming ticks during history load and ignores stale history after switching', async ({ page }) => {
  const errors = []; page.on('pageerror', error => errors.push(error.message));
  await page.routeWebSocket('**/ws', ws => ws.onMessage(raw => {
    const { symbol } = JSON.parse(raw);
    ws.send(JSON.stringify({ type: 'quote', quote: { symbol, bid: 1.15, ask: 1.16, mid: 1.155, time: 1790605206, cached: true } }));
  }));
  await page.route('**/api/history?**', async route => {
    const symbol = new URL(route.request().url()).searchParams.get('symbol');
    await new Promise(resolve => setTimeout(resolve, symbol === 'EURUSD' ? 500 : 100));
    await route.fulfill({ json: { candles: history(symbol) } }).catch(() => {});
  });
  await page.goto('/'); await page.locator('[data-symbol="GBPUSD"]').click();
  await expect(page.locator('#bar-count')).toHaveText('121 candles');
  await expect(page.locator('#symbol-name')).toContainText('GBP / USD');
  await expect(page.locator('#ohlc')).toContainText('1.15500');
  await expect(page.locator('#connection')).toContainText('cached');
  expect(errors).toEqual([]);
});
test('mobile layout and provider errors remain usable without fabricated candles', async ({ page }) => {
  await page.setViewportSize({ width: 390, height: 844 }); await mockStream(page);
  await page.route('**/api/history?**', route => route.fulfill({ status: 502, json: { error: 'TraderMade denied historical data.' } }));
  await page.goto('/'); await expect(page.locator('#notice')).toContainText('denied');
  await expect(page.locator('#chart-empty')).toBeVisible(); await expect(page.locator('#bar-count')).toHaveText('0 candles');
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  await page.screenshot({ path: 'test-results/portal-mobile.png', fullPage: true });
});
test('desktop chart screenshot', async ({ page }) => {
  await page.setViewportSize({ width: 1440, height: 1080 }); await mockStream(page);
  await page.route('**/api/history?**', route => route.fulfill({ json: { candles: history() } }));
  await page.goto('/'); await expect(page.locator('#bar-count')).toHaveText('120 candles');
  await page.screenshot({ path: 'test-results/portal-desktop.png', fullPage: true });
});

test('dragging into older history prepends once, preserves the viewport and live ticks', async ({ page }) => {
  const errors = []; page.on('pageerror', error => errors.push(error.message));
  let stream;
  await page.routeWebSocket('**/ws', ws => { stream = ws; });
  const initial = history(); const cursor = initial[0].time;
  const older = initial.map(c => ({ ...c, time: c.time - 120 * 3600 }));
  let olderRequests = 0, release;
  const hold = new Promise(resolve => { release = resolve; });
  await page.route('**/api/history?**', async route => {
    const before = new URL(route.request().url()).searchParams.get('before');
    if (before) {
      olderRequests++; expect(Number(before)).toBe(cursor); await hold;
      return route.fulfill({ json: { candles: [...older, ...older.slice(-1), initial[0]], nextBefore: older[0].time } });
    }
    return route.fulfill({ json: { candles: initial, nextBefore: cursor } });
  });
  await page.goto('/'); await expect(page.locator('#bar-count')).toHaveText('120 candles');
  expect(olderRequests).toBe(0);
  const box = await page.locator('#chart').boundingBox();
  const x = box.x + box.width * .4, y = box.y + box.height * .5;
  await page.mouse.move(x, y); await page.mouse.down(); await page.mouse.move(x + 100, y, { steps: 12 }); await page.mouse.up();
  await expect(page.locator('#load-older')).toHaveText('Loading older…');
  stream.send(JSON.stringify({ type: 'quote', quote: { symbol: 'EURUSD', bid: 1.154, ask: 1.156, mid: 1.155, time: initial.at(-1).time + 20, cached: false } }));
  await expect(page.locator('#price')).toHaveText('1.15500');
  // Read the candle under the same crosshair before/after prepending.
  await page.mouse.move(x + 130, y); await page.mouse.move(x + 120, y);
  const anchor = await page.locator('#ohlc').getAttribute('title');
  release(); await expect(page.locator('#bar-count')).toHaveText('240 candles');
  await page.mouse.move(x + 130, y); await page.mouse.move(x + 120, y);
  await expect(page.locator('#ohlc')).toHaveAttribute('title', anchor);
  await expect(page.locator('#price')).toHaveText('1.15500');
  expect(olderRequests).toBe(1); expect(errors).toEqual([]);
});

test('older-history failures retain candles and offer an explicit retry', async ({ page }) => {
  await mockStream(page); let olderRequests = 0;
  const initial = history(), cursor = initial[0].time;
  await page.route('**/api/history?**', route => {
    if (new URL(route.request().url()).searchParams.has('before')) {
      olderRequests++;
      if (olderRequests === 1) return route.fulfill({ status: 502, json: { error: 'Historical plan limit reached.' } });
      return route.fulfill({ json: { candles: initial.map(c => ({ ...c, time: c.time - 120 * 3600 })), nextBefore: cursor - 120 * 3600 } });
    }
    return route.fulfill({ json: { candles: initial, nextBefore: cursor } });
  });
  await page.goto('/'); await expect(page.locator('#load-older')).toBeEnabled();
  await page.locator('#load-older').click(); await expect(page.locator('#notice')).toContainText('Historical plan limit');
  await expect(page.locator('#bar-count')).toHaveText('120 candles');
  await expect(page.locator('#load-older')).toHaveText('Retry older history');
  expect(olderRequests).toBe(1);
  await page.locator('#load-older').click(); await expect(page.locator('#bar-count')).toHaveText('240 candles');
  await expect(page.locator('#notice')).toBeHidden(); expect(olderRequests).toBe(2);
});

test('switching pair discards an in-flight older page', async ({ page }) => {
  await mockStream(page); let release;
  const hold = new Promise(resolve => { release = resolve; });
  await page.route('**/api/history?**', async route => {
    const url = new URL(route.request().url());
    if (url.searchParams.has('before')) {
      await hold;
      return route.fulfill({ json: { candles: history().map(c => ({ ...c, time: c.time - 120 * 3600 })), nextBefore: 1780000000 } }).catch(() => {});
    }
    const candles = history(url.searchParams.get('symbol'));
    return route.fulfill({ json: { candles, nextBefore: candles[0].time } });
  });
  await page.goto('/'); await expect(page.locator('#load-older')).toBeEnabled(); await page.locator('#load-older').click();
  await expect(page.locator('#load-older')).toHaveText('Loading older…');
  await page.locator('[data-symbol="USDJPY"]').click(); release();
  await expect(page.locator('#symbol-name')).toContainText('USD / JPY');
  await expect(page.locator('#price')).toHaveText(/150\.\d{3}/); await expect(page.locator('#bar-count')).toHaveText('120 candles');
  await expect(page.locator('#load-older')).toBeEnabled();
});

test('empty history windows advance the cursor without repeated automatic requests', async ({ page }) => {
  await mockStream(page); const cursors = [];
  const initial = history(), cursor = initial[0].time;
  await page.route('**/api/history?**', route => {
    const before = new URL(route.request().url()).searchParams.get('before');
    if (!before) return route.fulfill({ json: { candles: initial, nextBefore: cursor } });
    cursors.push(Number(before));
    return route.fulfill({ json: { candles: [], nextBefore: Number(before) - 86400 } });
  });
  await page.goto('/'); await expect(page.locator('#load-older')).toBeEnabled(); await page.locator('#load-older').click();
  await expect(page.locator('#history-progress')).toContainText('No candles in this window');
  await expect(page.locator('#load-older')).toBeEnabled(); await page.locator('#load-older').click();
  await expect(page.locator('#load-older')).toBeEnabled();
  expect(cursors).toEqual([cursor, cursor - 86400]); await expect(page.locator('#bar-count')).toHaveText('120 candles');
});

test('refresh explicitly bypasses the history cache', async ({ page }) => {
  await mockStream(page); const requests = [];
  await page.route('**/api/history?**', route => {
    requests.push(new URL(route.request().url()));
    return route.fulfill({ json: { candles: history() } });
  });
  await page.goto('/'); await expect(page.locator('#bar-count')).toHaveText('120 candles');
  await page.locator('#refresh').click();
  await expect.poll(() => requests.length).toBe(2);
  expect(requests[0].searchParams.has('refresh')).toBe(false);
  expect(requests[1].searchParams.get('refresh')).toBe('1');
});
