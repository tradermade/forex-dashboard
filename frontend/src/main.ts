import './style.css';
import { createChart, CandlestickSeries, LineSeries, ColorType, CrosshairMode, type UTCTimestamp } from 'lightweight-charts';
import { instruments, instrumentLabel, formatPrice, timeframes, applyTick, type Candle, type Quote, type Timeframe, type Instrument, type Category } from './market';

const icons = {
  chart: '<path d="M3 17l5-6 4 3 8-10M14 4h6v6"/>',
  search: '<circle cx="10" cy="10" r="6"/><path d="m15 15 5 5"/>',
  expand: '<path d="M8 3H3v5m13-5h5v5M3 16v5h5m13-5v5h-5"/>',
  refresh: '<path d="M20 7a9 9 0 1 0 1 8M20 3v5h-5"/>',
  download: '<path d="M12 3v12m-4-4 4 4 4-4M4 16v5h16v-5"/>',
  candles: '<path d="M6 3v4m0 8v6m12-18v8m0 6v4M3 7h6v8H3zm12 4h6v6h-6z"/>',
};
const icon = (name: keyof typeof icons) => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${icons[name]}</svg>`;
const $ = <T extends HTMLElement = HTMLElement>(selector: string) => document.querySelector<T>(selector)!;
let selected = instruments[0]!;
let timeframe: Timeframe = '1h';
let candles: Candle[] = [];
let loading = false;
let streamState = 'connecting';
let historyNotice = '';
let requestId = 0;
let abort: AbortController | undefined;
let olderAbort: AbortController | undefined;
let olderBefore: number | null = null;
let loadingOlder = false;
let olderFailed = false;
let autoPagesRemaining = 0;
let olderTimer: ReturnType<typeof setTimeout>;
let pendingTicks: Quote[] = [];
let showAverage = false;
let lineMode = false;
const quotes = new Map<string, Quote>();
const ticks: Quote[] = [];
let category: Category | 'All' = 'All';
let visibleLimit = 100;
let catalogueLoading = false;
const pairRows = new Map<string, HTMLButtonElement>();

$('#app').innerHTML = `
<header class="topbar">
  <a class="brand" href="/" aria-label="TraderMade terminal"><span class="brand-icon">${icon('chart')}</span>trader<span>made</span><span class="product-tag">TERMINAL</span></a>
  <div class="top-right"><span class="workspace-label">Personal workspace</span><span class="divider"></span><span class="avatar">OM</span></div>
</header>
<div class="workspace">
  <aside class="watchlist">
    <div class="section-heading"><h2>Markets</h2><span class="count">${instruments.length}</span></div>
    <label class="search">${icon('search')}<input id="search" placeholder="Search symbols or names" aria-label="Search symbols or names" autocomplete="off"/><kbd>/</kbd></label>
    <div class="market-filters" aria-label="Market type">${['All', 'Forex', 'Crypto', 'CFD', 'Metals'].map(type => `<button data-category="${type}" aria-pressed="${type === 'All'}">${type}</button>`).join('')}</div>
    <div class="catalogue-status"><span id="catalogue-status" role="status">Loading markets…</span><button id="retry-catalogue" hidden>Retry</button></div>
    <div class="watch-labels"><span id="market-label">ALL MARKETS</span><span>MID PRICE</span></div>
    <div id="pairs" class="pairs"></div>
    <button id="more-symbols" hidden>Show more symbols</button>
    <div class="watch-footer"><span class="small-dot"></span> Prices provided by TraderMade<br><span>Quotes update with the selected symbol.<br>History and streaming depend on your plan.</span></div>
  </aside>
  <main>
    <div class="page-heading"><div class="eyebrow">MARKET OVERVIEW</div><div class="heading-row"><h1>Chart workspace</h1><span id="connection" class="status"><i></i>Connecting</span></div></div>
    <section class="chart-panel">
      <div class="instrument-header"><div class="instrument-title"><span class="pair-avatar" id="pair-avatar">€</span><div><h2 id="symbol-name">EUR / USD <span>FOREX</span></h2><p id="instrument-name">Euro / US Dollar</p></div></div><div class="price-block"><strong id="price">—</strong><span id="change">Waiting for market data</span></div></div>
      <div class="chart-toolbar"><div class="timeframes" aria-label="Chart timeframe">${Object.keys(timeframes).map(t => `<button data-timeframe="${t}" class="${t === timeframe ? 'active' : ''}" aria-pressed="${t === timeframe}">${t}</button>`).join('')}</div><div class="chart-actions"><button id="chart-type" class="tool-button" title="Switch to line chart" aria-label="Switch to line chart">${icon('candles')}</button><button id="average" aria-pressed="false" title="Toggle 20-period simple moving average">ƒ<span class="indicator-label"> Indicators</span></button><span class="toolbar-divider"></span><button id="fit" class="tool-button" title="Fit chart" aria-label="Fit chart">${icon('expand')}</button><button id="refresh" class="tool-button" title="Reload history" aria-label="Reload history">${icon('refresh')}</button><button id="export" class="tool-button" title="Export candles as CSV" aria-label="Export candles as CSV">${icon('download')}</button></div></div>
      <div class="chart-stage"><div class="ohlc" id="ohlc">Loading historical candles…</div><div id="chart"></div><div id="chart-empty" class="chart-empty" hidden><span>${icon('chart')}</span><h3>Waiting for market data</h3><p>Historical candles and live quotes will appear here.</p></div></div>
      <div class="chart-bottom"><span id="bar-count">0 candles</span><button id="load-older" disabled>Load older</button><span id="history-progress" role="status"></span><span id="average-label" hidden>SMA 20</span><span class="chart-time">UTC <span id="clock"></span></span></div>
    </section>
    <div id="notice" class="notice" role="status" hidden></div>
    <section class="quote-grid" aria-label="Current quote"><div class="quote-card"><span>BID PRICE</span><strong id="bid" class="down">—</strong><small>Best available bid</small></div><div class="quote-card"><span>ASK PRICE</span><strong id="ask" class="up">—</strong><small>Best available ask</small></div><div class="quote-card"><span>SPREAD</span><strong id="spread">—</strong><small id="spread-unit">Ask − bid · pips</small></div><div class="quote-card"><span>LAST UPDATE</span><strong id="last-update">—</strong><small id="quote-source">Awaiting first quote · UTC</small></div></section>
    <section class="activity-panel"><div class="section-heading"><h2>Quote activity <span class="subtle">${icon('chart')}</span></h2><span class="subtle">Latest 8 ticks · UTC</span></div><div class="table-wrap"><table><thead><tr><th>TIME</th><th>SYMBOL</th><th>BID</th><th>ASK</th><th>MID</th><th>TYPE</th></tr></thead><tbody id="activity"><tr><td colspan="6" class="empty-row">Listening for quotes from TraderMade…</td></tr></tbody></table></div></section>
    <footer><span>Market data by <a href="https://tradermade.com" target="_blank" rel="noreferrer">TraderMade</a></span><span>Charts by <a href="https://www.tradingview.com/" target="_blank" rel="noreferrer">TradingView Lightweight Charts™</a> · <a href="/NOTICE.txt" target="_blank">License notice</a></span></footer>
  </main>
</div>`;

const chart = createChart($('#chart'), {
  autoSize: true,
  layout: { background: { type: ColorType.Solid, color: '#10151e' }, textColor: '#7c879c', fontFamily: 'Segoe UI, sans-serif', fontSize: 11, attributionLogo: true },
  grid: { vertLines: { color: '#1b2230' }, horzLines: { color: '#1b2230' } },
  rightPriceScale: { borderColor: '#252c3a', scaleMargins: { top: 0.17, bottom: 0.12 } },
  timeScale: { borderColor: '#252c3a', timeVisible: true, secondsVisible: false, rightOffset: 7 },
  crosshair: { mode: CrosshairMode.Normal, vertLine: { color: '#677287', labelBackgroundColor: '#333e50' }, horzLine: { color: '#677287', labelBackgroundColor: '#333e50' } },
});
const candleSeries = chart.addSeries(CandlestickSeries, { upColor: '#2dceb0', downColor: '#ef6b7b', borderVisible: false, wickUpColor: '#2dceb0', wickDownColor: '#ef6b7b' });
const lineSeries = chart.addSeries(LineSeries, { color: '#55b9fa', lineWidth: 2, visible: false });
const averageSeries = chart.addSeries(LineSeries, { color: '#e1b36b', lineWidth: 1, priceLineVisible: false, lastValueVisible: false, visible: false });
const price = (value: number | undefined) => formatPrice(value, selected);
const timeLabel = (seconds: number) => new Date(seconds * 1000).toLocaleTimeString('en-GB', { timeZone: 'UTC' });
function renderPairs() {
  const query = $<HTMLInputElement>('#search').value.trim().toLowerCase().replace(/\s*\/\s*/g, '');
  const filtered = instruments.filter(i => (category === 'All' || i.category === category) && `${i.symbol} ${i.name}`.toLowerCase().includes(query));
  const visible = filtered.slice(0, visibleLimit), keep = new Set(visible.map(i => i.symbol));
  const list = $('#pairs');
  list.querySelector('.no-results')?.remove();
  for (const [symbol, row] of pairRows) if (!keep.has(symbol)) { row.remove(); pairRows.delete(symbol); }
  visible.forEach((i, index) => {
    let row = pairRows.get(i.symbol);
    if (!row) {
      row = document.createElement('button'); row.className = 'pair-row'; row.dataset.symbol = i.symbol;
      row.innerHTML = '<span class="currency-dot"></span><span class="pair-info"><strong></strong><small></small></span><span class="pair-price"><span class="price-value"></span><small></small></span>';
      row.querySelector('.currency-dot')!.textContent = i.base.slice(0, 1);
      row.querySelector('.currency-dot')!.classList.add(i.base.toLowerCase());
      row.querySelector('.pair-info strong')!.textContent = instrumentLabel(i);
      row.querySelector('.pair-info small')!.textContent = i.name.split(' / ')[0]!;
      row.title = `${i.name} · ${i.category}`;
      pairRows.set(i.symbol, row);
    }
    // Preserve button nodes, focus and pointer targets, including when the catalogue arrives.
    if (list.children[index] !== row) list.insertBefore(row, list.children[index] ?? null);
    updatePair(i);
  });
  if (!visible.length) { const empty = document.createElement('p'); empty.className = 'no-results'; empty.textContent = 'No matching symbols'; list.append(empty); }
  $('.count').textContent = String(instruments.length);
  $('#market-label').textContent = `${category === 'All' ? 'ALL MARKETS' : category.toUpperCase()} · ${filtered.length}`;
  $('#more-symbols').hidden = filtered.length <= visibleLimit;
}
function updatePair(instrument: Instrument) {
  const row = pairRows.get(instrument.symbol); if (!row) return;
  row.classList.toggle('selected', instrument.symbol === selected.symbol);
  row.setAttribute('aria-pressed', String(instrument.symbol === selected.symbol));
  const value = formatPrice(quotes.get(instrument.symbol)?.mid, instrument);
  const label = instrument.symbol === selected.symbol ? 'Selected' : quotes.has(instrument.symbol) ? 'Last seen' : 'Select to stream';
  const priceNode = row.querySelector('.price-value')!, statusNode = row.querySelector('.pair-price small')!;
  if (priceNode.textContent !== value) priceNode.textContent = value;
  if (statusNode.textContent !== label) statusNode.textContent = label;
}
function renderInstrument() {
  const tag = document.createElement('span'); tag.textContent = selected.category.toUpperCase();
  $('#symbol-name').replaceChildren(document.createTextNode(instrumentLabel(selected) + ' '), tag);
  $('#instrument-name').textContent = selected.name;
  $('#pair-avatar').textContent = selected.base === 'EUR' ? '€' : selected.base === 'GBP' ? '£' : selected.base.slice(0, 1);
}
async function loadInstruments() {
  if (catalogueLoading) return;
  catalogueLoading = true; $('#retry-catalogue').hidden = true;
  $('#catalogue-status').textContent = 'Loading markets…';
  try {
    const response = await fetch('/api/instruments'); const data = await response.json();
    if (!response.ok || !Array.isArray(data.instruments)) throw new Error('Catalogue unavailable');
    const valid = data.instruments.filter((i: Instrument) => i && /^[A-Z0-9][A-Z0-9_-]{0,23}$/.test(i.symbol) && typeof i.name === 'string' && typeof i.base === 'string' && /^[A-Z0-9_-]+$/.test(i.base) && typeof i.quote === 'string' && ['Forex', 'Crypto', 'CFD', 'Metals'].includes(i.category) && Number.isInteger(i.digits) && i.digits >= 0 && i.digits <= 8) as Instrument[];
    if (!valid.length) throw new Error('Catalogue empty');
    const preferred = ['EURUSD', 'GBPUSD', 'USDJPY', 'AUDUSD', 'USDCHF', 'USDCAD', 'NZDUSD', 'EURGBP', 'BTCUSD', 'ETHUSD', 'XAUUSD', 'XAGUSD', 'SPX500', 'NAS100', 'UK100', 'OIL', 'AAPL'];
    const rank = (i: Instrument) => { const index = preferred.indexOf(i.symbol); return index >= 0 ? index : i.category === 'CFD' || i.quote === 'USD' ? 100 : 200; };
    const unique = new Map(valid.map(i => [i.symbol, i]));
    if (!unique.has(selected.symbol)) unique.set(selected.symbol, selected);
    instruments.splice(0, instruments.length, ...[...unique.values()].sort((a, b) => rank(a) - rank(b) || a.symbol.localeCompare(b.symbol)));
    selected = unique.get(selected.symbol)!;
    renderInstrument(); renderPairs(); renderChart(); renderQuote();
    const partial = data.stale || data.unavailable?.length;
    $('#catalogue-status').textContent = partial ? 'Some markets could not refresh. Retry shortly.' : 'TraderMade catalogue · access varies by plan';
    $('#retry-catalogue').hidden = !partial;
  } catch {
    $('#catalogue-status').textContent = 'Catalogue unavailable · showing starter symbols';
    $('#retry-catalogue').hidden = false;
  } finally { catalogueLoading = false; }
}
function renderOHLC(candle = candles.at(-1)) {
  $('#ohlc').title = candle ? new Date(candle.time * 1000).toISOString() : '';
  $('#ohlc').innerHTML = candle ? `<strong>${selected.symbol}</strong><span>${timeframe} · TraderMade</span>${(['open', 'high', 'low', 'close'] as const).map(k => `<span>${k[0]!.toUpperCase()} <b class="${candle.close >= candle.open ? 'up' : 'down'}">${price(candle[k])}</b></span>`).join('')}` : loading ? 'Loading historical candles…' : 'Waiting for candles';
}
function renderAverage() {
  let sum = 0;
  averageSeries.setData(candles.flatMap((c, index) => {
    sum += c.close; if (index >= 20) sum -= candles[index - 20]!.close;
    return index < 19 ? [] : [{ time: c.time as UTCTimestamp, value: sum / 20 }];
  }));
}
function renderChart() {
  const format = { type: 'price' as const, precision: selected.digits, minMove: 10 ** -selected.digits };
  candleSeries.applyOptions({ priceFormat: format }); lineSeries.applyOptions({ priceFormat: format }); averageSeries.applyOptions({ priceFormat: format });
  candleSeries.setData(candles.map(c => ({ ...c, time: c.time as UTCTimestamp })));
  lineSeries.setData(candles.map(c => ({ time: c.time as UTCTimestamp, value: c.close })));
  renderAverage(); renderOHLC();
  $('#bar-count').textContent = `${candles.length} candles`;
  $('#chart-empty').hidden = candles.length > 0 || loading;
}
function renderQuote() {
  const q = quotes.get(selected.symbol);
  $('#price').textContent = price(q?.mid ?? candles.at(-1)?.close);
  $('#bid').textContent = price(q?.bid); $('#ask').textContent = price(q?.ask);
  $('#spread').textContent = q ? selected.category === 'Forex' ? ((q.ask - q.bid) * (selected.quote === 'JPY' ? 100 : 10000)).toFixed(1) : price(q.ask - q.bid) : '—';
  $('#spread-unit').textContent = selected.category === 'Forex' ? 'Ask − bid · pips' : `Ask − bid · ${selected.quote || 'price units'}`;
  $('#last-update').textContent = q ? timeLabel(q.time) : '—';
  $('#quote-source').textContent = q ? `${q.cached ? 'Cached quote' : 'Streaming quote'} · UTC` : 'Awaiting first quote · UTC';
  const last = candles.at(-1);
  const change = last ? (last.close - last.open) / last.open * 100 : undefined;
  $('#change').textContent = change !== undefined ? `${change >= 0 ? '+' : ''}${change.toFixed(3)}% · current candle` : 'Waiting for market data';
  $('#change').className = change === undefined ? '' : change >= 0 ? 'up' : 'down';
}
function renderNotice() { $('#notice').textContent = historyNotice; $('#notice').hidden = !historyNotice; }
function setStatus(state: string, message: string) {
  streamState = state; $('#connection').className = `status ${state}`;
  $('#connection').replaceChildren(document.createElement('i'), document.createTextNode(message));
}
function updateCandle(q: Quote) {
  const candle = applyTick(candles.at(-1), q, timeframe);
  if (!candle) return;
  if (candles.at(-1)?.time === candle.time) candles[candles.length - 1] = candle; else candles.push(candle);
  candleSeries.update({ ...candle, time: candle.time as UTCTimestamp });
  lineSeries.update({ time: candle.time as UTCTimestamp, value: candle.close });
  if (showAverage) renderAverage();
  $('#chart-empty').hidden = true; $('#bar-count').textContent = `${candles.length} candles`; renderOHLC();
}
async function loadHistory(refresh = false) {
  const id = ++requestId;
  abort?.abort(); abort = new AbortController();
  olderAbort?.abort(); clearTimeout(olderTimer);
  olderBefore = null; loadingOlder = false; olderFailed = false; autoPagesRemaining = 0;
  $('#history-progress').textContent = ''; renderOlderButton();
  loading = true; pendingTicks = []; candles = []; historyNotice = '';
  renderChart(); renderQuote(); renderNotice();
  try {
    const response = await fetch(`/api/history?symbol=${selected.symbol}&timeframe=${timeframe}${refresh ? '&refresh=1' : ''}`, { signal: abort.signal });
    const data = await response.json();
    if (id !== requestId) return;
    if (!response.ok) throw new Error(data.error ?? 'History could not be loaded.');
    candles = data.candles;
    olderBefore = data.nextBefore ?? candles[0]?.time ?? null;
    if (!candles.length) historyNotice = 'No historical candles in this range. Live quotes will build new candles as they arrive.';
  } catch (error) {
    if (id !== requestId) return;
    historyNotice = error instanceof Error ? error.message : 'Could not load history.';
  } finally {
    if (id === requestId) {
      loading = false; renderChart();
      for (const q of pendingTicks) updateCandle(q);
      pendingTicks = []; chart.timeScale().fitContent(); renderQuote(); renderNotice(); renderOlderButton();
    }
  }
}
function renderOlderButton() {
  const button = $<HTMLButtonElement>('#load-older');
  button.disabled = loading || loadingOlder || olderBefore === null;
  button.textContent = loadingOlder ? 'Loading older…' : olderFailed ? 'Retry older history' : 'Load older';
}
function scheduleOlderHistory() {
  clearTimeout(olderTimer);
  if (loading || loadingOlder || olderFailed || olderBefore === null || autoPagesRemaining <= 0) return;
  const range = chart.timeScale().getVisibleLogicalRange();
  if (!range || range.from >= 20) return;
  olderTimer = setTimeout(() => { autoPagesRemaining--; void loadOlderHistory(); }, 180);
}
async function loadOlderHistory() {
  if (loading || loadingOlder || olderBefore === null) return;
  const id = requestId, cursor = olderBefore;
  loadingOlder = true; olderFailed = false;
  olderAbort = new AbortController(); renderOlderButton();
  $('#history-progress').textContent = '';
  try {
    const response = await fetch(`/api/history?symbol=${selected.symbol}&timeframe=${timeframe}&before=${cursor}`, { signal: olderAbort.signal });
    const data = await response.json();
    if (id !== requestId) return;
    if (!response.ok) throw new Error(data.error ?? 'Could not load older history.');
    // Only prepend older bars; retain every live update received during this request.
    const firstTime = candles[0]?.time ?? Infinity;
    const unique = new Map<number, Candle>();
    for (const candle of data.candles as Candle[]) if (candle.time < firstTime) unique.set(candle.time, candle);
    const older = [...unique.values()].sort((a, b) => a.time - b.time);
    const visible = chart.timeScale().getVisibleLogicalRange();
    candles = [...older, ...candles];
    const next = data.nextBefore ?? older[0]?.time;
    olderBefore = Number.isFinite(next) && next > 0 && next < cursor ? next : null;
    renderChart();
    // Prepending shifts logical indices. Restore the same timestamps and zoom, not fitContent().
    if (visible) chart.timeScale().setVisibleLogicalRange({ from: visible.from + older.length, to: visible.to + older.length });
    renderQuote(); historyNotice = ''; renderNotice();
    $('#history-progress').textContent = older.length ? '' : olderBefore === null ? 'No earlier candles returned.' : 'No candles in this window. Continue with Load older.';
  } catch (error) {
    if (id !== requestId) return;
    olderFailed = true; autoPagesRemaining = 0;
    historyNotice = `Older history: ${error instanceof Error ? error.message : 'Request failed.'} Existing candles are still available.`;
    renderNotice();
  } finally {
    if (id === requestId) { loadingOlder = false; renderOlderButton(); scheduleOlderHistory(); }
  }
}
chart.timeScale().subscribeVisibleLogicalRangeChange(scheduleOlderHistory);
// Avoid downloading older pages merely because initial fitContent() exposes the left edge.
for (const event of ['pointerdown', 'wheel', 'touchstart']) $('#chart').addEventListener(event, () => {
  autoPagesRemaining = 3;
}, { passive: true });
$('#load-older').addEventListener('click', () => { clearTimeout(olderTimer); autoPagesRemaining = 0; void loadOlderHistory(); });
let socket: WebSocket;
let retryTimer: ReturnType<typeof setTimeout>;
let reconnectDelay = 1000;
function connect() {
  setStatus('connecting', 'Connecting');
  socket = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/ws`);
  socket.onopen = () => { reconnectDelay = 1000; socket.send(JSON.stringify({ type: 'subscribe', symbol: selected.symbol })); };
  socket.onmessage = event => {
    let data;
    try { data = JSON.parse(event.data); } catch { return; }
    if (data.type === 'status') { if (!data.symbol || data.symbol === selected.symbol) setStatus(data.state, data.message); return; }
    if (data.type !== 'quote') return;
    const q: Quote = data.quote;
    if (quotes.has(q.symbol) && q.time < quotes.get(q.symbol)!.time) return;
    quotes.set(q.symbol, q);
    const instrument = instruments.find(i => i.symbol === q.symbol); if (instrument) updatePair(instrument);
    if (q.symbol !== selected.symbol) return;
    if (loading) { pendingTicks.push(q); if (pendingTicks.length > 5000) pendingTicks.shift(); } else updateCandle(q);
    ticks.unshift(q); ticks.splice(8);
    $('#activity').innerHTML = ticks.map(t => `<tr><td>${timeLabel(t.time)}</td><td class="table-symbol">${t.symbol}</td><td class="down">${price(t.bid)}</td><td class="up">${price(t.ask)}</td><td>${price(t.mid)}</td><td><span class="tick-tag">${t.cached ? 'CACHED' : 'QUOTE'}</span></td></tr>`).join('');
    renderQuote();
    setStatus(q.cached || Date.now() / 1000 - q.time > 60 ? 'waiting' : 'live', q.cached ? 'Connected · cached quote' : Date.now() / 1000 - q.time > 60 ? 'Connected · stale quote' : 'Live feed');
  };
  socket.onclose = () => { setStatus('offline', 'Disconnected · retrying'); retryTimer = setTimeout(connect, reconnectDelay); reconnectDelay = Math.min(reconnectDelay * 2, 30000); };
  socket.onerror = () => socket.close();
}
$('#pairs').addEventListener('click', event => {
  const button = (event.target as HTMLElement).closest<HTMLButtonElement>('[data-symbol]');
  if (!button || button.dataset.symbol === selected.symbol) return;
  const previous = selected;
  selected = instruments.find(i => i.symbol === button.dataset.symbol)!;
  renderInstrument(); updatePair(previous); updatePair(selected);
  ticks.length = 0; $('#activity').innerHTML = '<tr><td colspan="6" class="empty-row">Waiting for quotes…</td></tr>';
  setStatus('connecting', 'Subscribing');
  if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify({ type: 'subscribe', symbol: selected.symbol }));
  void loadHistory();
});
$('#search').addEventListener('input', () => { visibleLimit = 100; renderPairs(); });
$('#more-symbols').addEventListener('click', () => { visibleLimit += 100; renderPairs(); });
$('#retry-catalogue').addEventListener('click', () => void loadInstruments());
document.querySelectorAll<HTMLButtonElement>('[data-category]').forEach(button => button.addEventListener('click', () => {
  category = button.dataset.category as Category | 'All'; visibleLimit = 100;
  document.querySelectorAll<HTMLButtonElement>('[data-category]').forEach(b => b.setAttribute('aria-pressed', String(b === button)));
  renderPairs();
}));
document.addEventListener('keydown', e => { if (e.key === '/' && document.activeElement?.tagName !== 'INPUT') { e.preventDefault(); $('#search').focus(); } });
document.querySelectorAll<HTMLButtonElement>('[data-timeframe]').forEach(button => button.addEventListener('click', () => {
  timeframe = button.dataset.timeframe as Timeframe;
  document.querySelectorAll<HTMLButtonElement>('[data-timeframe]').forEach(b => { b.classList.toggle('active', b === button); b.setAttribute('aria-pressed', String(b === button)); });
  chart.applyOptions({ timeScale: { timeVisible: timeframe !== '1D' } }); void loadHistory();
}));
$('#chart-type').addEventListener('click', () => {
  lineMode = !lineMode; candleSeries.applyOptions({ visible: !lineMode }); lineSeries.applyOptions({ visible: lineMode });
  $('#chart-type').classList.toggle('active', lineMode); $('#chart-type').title = lineMode ? 'Switch to candlestick chart' : 'Switch to line chart';
  $('#chart-type').setAttribute('aria-label', $('#chart-type').title);
});
$('#average').addEventListener('click', () => { showAverage = !showAverage; renderAverage(); averageSeries.applyOptions({ visible: showAverage }); $('#average').classList.toggle('active', showAverage); $('#average').setAttribute('aria-pressed', String(showAverage)); $('#average-label').hidden = !showAverage; });
$('#fit').addEventListener('click', () => { autoPagesRemaining = 0; clearTimeout(olderTimer); chart.timeScale().fitContent(); });
$('#refresh').addEventListener('click', () => void loadHistory(true));
$('#export').addEventListener('click', () => {
  if (!candles.length) { historyNotice = 'There are no candles to export yet.'; renderNotice(); return; }
  const csv = ['time,open,high,low,close', ...candles.map(c => `${new Date(c.time * 1000).toISOString()},${c.open},${c.high},${c.low},${c.close}`)].join('\n');
  const url = URL.createObjectURL(new Blob([csv], { type: 'text/csv' }));
  const a = document.createElement('a'); a.href = url; a.download = `${selected.symbol}-${timeframe}.csv`; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
});
chart.subscribeCrosshairMove(param => {
  const data = param.seriesData.get(candleSeries);
  renderOHLC(data && 'open' in data ? { ...data, time: Number(data.time) } : undefined);
});
const clockTimer = setInterval(() => {
  $('#clock').textContent = new Date().toLocaleTimeString('en-GB', { timeZone: 'UTC' });
  const q = quotes.get(selected.symbol);
  if (streamState === 'live' && q && Date.now() / 1000 - q.time > 60) setStatus('waiting', 'Connected · awaiting ticks');
}, 1000);
window.addEventListener('beforeunload', () => { clearTimeout(retryTimer); clearTimeout(olderTimer); clearInterval(clockTimer); socket.onclose = null; socket.close(); abort?.abort(); olderAbort?.abort(); chart.remove(); });
renderPairs(); connect(); void loadHistory(); void loadInstruments();

