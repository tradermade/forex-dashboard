import { test, expect } from '@playwright/test';

const instruments = [
  { symbol: 'EURUSD', base: 'EUR', quote: 'USD', name: 'Euro / US Dollar', category: 'Forex', digits: 5 },
  { symbol: 'GBPUSD', base: 'GBP', quote: 'USD', name: 'British Pound / US Dollar', category: 'Forex', digits: 5 },
  { symbol: 'BTCUSD', base: 'BTC', quote: 'USD', name: 'Bitcoin / US Dollar', category: 'Crypto', digits: 8 },
  { symbol: 'DOGEUSD', base: 'DOGE', quote: 'USD', name: 'Dogecoin / US Dollar', category: 'Crypto', digits: 8 },
  { symbol: 'UK100', base: 'UK100', quote: '', name: 'FTSE 100', category: 'CFD', digits: 4 },
  { symbol: 'AAPL', base: 'AAPL', quote: '', name: 'Apple', category: 'CFD', digits: 4 },
  { symbol: 'XAUUSD', base: 'XAU', quote: 'USD', name: 'Gold / US Dollar', category: 'Metals', digits: 4 },
];
async function setup(page, catalogue = instruments) {
  await page.route('**/api/instruments', route => route.fulfill({ json: { instruments: catalogue, unavailable: [], stale: false } }));
  const requests = [], subscriptions = [];
  let stream;
  await page.routeWebSocket('**/ws', ws => {
    stream = ws;
    ws.onMessage(raw => subscriptions.push(JSON.parse(raw).symbol));
  });
  await page.route('**/api/history?**', route => {
    requests.push(new URL(route.request().url()).searchParams.get('symbol'));
    return route.fulfill({ json: { candles: [{ time: 1790600400, open: 1.1, high: 1.2, low: 1, close: 1.15 }] } });
  });
  await page.goto('/');
  await expect(page.locator('#catalogue-status')).toContainText('access varies');
  return { requests, subscriptions, quote: (symbol = 'EURUSD', bid = 1.1, ask = 1.2) => stream.send(JSON.stringify({ type: 'quote', quote: { symbol, bid, ask, mid: (bid + ask) / 2, time: 1790600420, cached: false } })) };
}

test('one click selects a symbol when ticks arrive during pointer down', async ({ page }) => {
  const market = await setup(page);
  const row = page.locator('[data-symbol="GBPUSD"]');
  const box = await row.locator('.pair-info strong').boundingBox();
  await page.mouse.move(box.x + 8, box.y + 5);
  await page.mouse.down();
  // Previously this quote replaced the pressed GBPUSD button before mouse-up.
  market.quote();
  await expect(page.locator('[data-symbol="EURUSD"] .price-value')).toHaveText('1.15000');
  await page.mouse.up();
  await expect(row).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('#symbol-name')).toContainText('GBP / USD');
  expect(market.subscriptions.filter(s => s === 'GBPUSD')).toHaveLength(1);
  expect(market.requests.filter(s => s === 'GBPUSD')).toHaveLength(1);
});

test('streaming updates preserve keyboard focus and allow Enter selection', async ({ page }) => {
  const market = await setup(page);
  const row = page.locator('[data-symbol="BTCUSD"]');
  await row.focus(); market.quote();
  await expect(page.locator('[data-symbol="EURUSD"] .price-value')).toHaveText('1.15000');
  await expect(row).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(row).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('#symbol-name')).toContainText('CRYPTO');
});

test('searches and selects crypto, CFD and metals with matching labels and units', async ({ page }) => {
  const market = await setup(page);
  await page.locator('[data-category="Crypto"]').click();
  await expect(page.locator('.pair-row')).toHaveCount(2);
  await page.locator('#search').fill('dogecoin');
  await page.locator('[data-symbol="DOGEUSD"]').click(); market.quote('DOGEUSD', 0.00000123, 0.00000125);
  await expect(page.locator('#price')).toHaveText('0.00000124');
  await expect(page.locator('#spread-unit')).toHaveText('Ask − bid · USD');
  await expect(page.locator('#spread')).toHaveText('0.00000002');
  await page.locator('#search').fill(''); await page.locator('[data-category="CFD"]').click();
  await page.locator('[data-symbol="UK100"]').click(); market.quote('UK100', 8000, 8001);
  await expect(page.locator('#symbol-name')).toHaveText('UK100 CFD');
  await expect(page.locator('#spread-unit')).toHaveText('Ask − bid · price units');
  await expect(page.locator('#spread')).toHaveText('1.00');
  await page.locator('[data-category="Metals"]').click(); await page.locator('[data-symbol="XAUUSD"]').click();
  await expect(page.locator('#symbol-name')).toHaveText('XAU / USD METALS');
  expect(market.subscriptions).toEqual(['EURUSD', 'DOGEUSD', 'UK100', 'XAUUSD']);
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

test('large catalogues remain bounded and searchable, provider names render as text', async ({ page }) => {
  const many = Array.from({ length: 300 }, (_, n) => ({ symbol: `STOCK${n}`, base: `STOCK${n}`, quote: '', name: `Stock ${n}`, category: 'CFD', digits: 4 }));
  many[299].name = '<img src=x onerror="window.catalogueXss=true">';
  await setup(page, [...instruments, ...many]);
  await expect(page.locator('.pair-row')).toHaveCount(100);
  await page.locator('#more-symbols').click(); await expect(page.locator('.pair-row')).toHaveCount(200);
  await page.locator('#search').fill('STOCK299');
  await expect(page.locator('.pair-row')).toHaveCount(1);
  await expect(page.locator('.pair-info small')).toContainText('<img');
  expect(await page.evaluate(() => window.catalogueXss)).toBeUndefined();
  await page.locator('[data-symbol="STOCK299"]').click();
  await expect(page.locator('#symbol-name')).toHaveText('STOCK299 CFD');
});

test('catalogue errors offer retry without interrupting the selected chart', async ({ page }) => {
  let attempts = 0;
  await page.routeWebSocket('**/ws', () => {});
  await page.route('**/api/history?**', route => route.fulfill({ json: { candles: [] } }));
  await page.route('**/api/instruments', route => ++attempts === 1 ? route.fulfill({ status: 502, json: { error: 'Unavailable' } }) : route.fulfill({ json: { instruments } }));
  await page.goto('/'); await expect(page.locator('#retry-catalogue')).toBeVisible();
  await page.locator('[data-symbol="GBPUSD"]').click();
  await page.locator('#retry-catalogue').click();
  await expect(page.locator('#catalogue-status')).toContainText('access varies');
  await expect(page.locator('[data-symbol="GBPUSD"]')).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('#symbol-name')).toContainText('GBP / USD');
});
