export type Category = 'Forex' | 'Crypto' | 'CFD' | 'Metals';
export type Instrument = { symbol: string; name: string; base: string; quote: string; digits: number; category: Category };
export const instruments: Instrument[] = [
  { symbol: 'EURUSD', name: 'Euro / US Dollar', base: 'EUR', quote: 'USD', digits: 5 },
  { symbol: 'GBPUSD', name: 'British Pound / US Dollar', base: 'GBP', quote: 'USD', digits: 5 },
  { symbol: 'USDJPY', name: 'US Dollar / Japanese Yen', base: 'USD', quote: 'JPY', digits: 3 },
  { symbol: 'AUDUSD', name: 'Australian Dollar / US Dollar', base: 'AUD', quote: 'USD', digits: 5 },
  { symbol: 'USDCHF', name: 'US Dollar / Swiss Franc', base: 'USD', quote: 'CHF', digits: 5 },
  { symbol: 'USDCAD', name: 'US Dollar / Canadian Dollar', base: 'USD', quote: 'CAD', digits: 5 },
  { symbol: 'NZDUSD', name: 'New Zealand Dollar / US Dollar', base: 'NZD', quote: 'USD', digits: 5 },
  { symbol: 'EURGBP', name: 'Euro / British Pound', base: 'EUR', quote: 'GBP', digits: 5 },
].map(instrument => ({ ...instrument, category: 'Forex' }));
export function instrumentLabel(instrument: Instrument): string {
  return instrument.quote ? `${instrument.base} / ${instrument.quote}` : instrument.symbol;
}
export function formatPrice(value: number | undefined, instrument: Instrument): string {
  if (value === undefined) return '—';
  const fixed = value.toFixed(instrument.digits);
  return instrument.category === 'Forex' ? fixed : fixed.replace(/(\.\d{2,}?)0+$/, '$1');
}
export const timeframes = {
  '1m': { seconds: 60, interval: 'minute', period: 1, days: 1 },
  '5m': { seconds: 300, interval: 'minute', period: 5, days: 1 },
  '15m': { seconds: 900, interval: 'minute', period: 15, days: 1 },
  '30m': { seconds: 1800, interval: 'minute', period: 30, days: 1 },
  '1h': { seconds: 3600, interval: 'hourly', period: 1, days: 14 },
  '4h': { seconds: 14400, interval: 'hourly', period: 4, days: 28 },
  '1D': { seconds: 86400, interval: 'daily', period: 1, days: 180 },
} as const;
export type Timeframe = keyof typeof timeframes;
export type Candle = { time: number; open: number; high: number; low: number; close: number };
export type Quote = { symbol: string; bid: number; ask: number; mid: number; time: number; cached: boolean };
// Do not synthesize candles for periods without ticks or overwrite newer candles with late ticks.
export function applyTick(last: Candle | undefined, quote: Quote, timeframe: Timeframe): Candle | null {
  const seconds = timeframes[timeframe].seconds;
  const time = Math.floor(quote.time / seconds) * seconds;
  if (last && time < last.time) return null;
  if (last && time === last.time) return { ...last, high: Math.max(last.high, quote.mid), low: Math.min(last.low, quote.mid), close: quote.mid };
  return { time, open: quote.mid, high: quote.mid, low: quote.mid, close: quote.mid };
}
