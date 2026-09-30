# TraderMade Terminal

A local market dashboard for forex, crypto, metals and CFDs. Explore historical candles, follow live quotes, add a moving average, and export chart data to CSV.

Built with **C++20**, **TypeScript/Vite**, and **TradingView Lightweight Charts 5**. TraderMade API keys stay on the backend. This application displays market data; it does not place trades.

[Setup](#setup-windows) · [Development](#development) · [Tests](#tests) · [Troubleshooting](#troubleshooting) · [Publishing checklist](#github-publishing-checklist)

## Features

- Search instruments and filter by Forex, Crypto, Metals or CFD.
- Switch between candlestick and line charts across `1m`, `5m`, `15m`, `30m`, `1h`, `4h` and `1D` timeframes.
- See live bid, ask, spread and quote status through a shared backend WebSocket connection.
- Drag back through history or use **Load older** to fetch earlier candles.
- Toggle SMA 20, fit the chart, refresh data, and export loaded candles to CSV.
- Reuse historical data through memory and persistent Parquet caches.
- Use the dashboard on desktop or mobile, with provider errors shown in the interface.

Available symbols, historical depth and streaming access depend on your TraderMade account. A listed instrument may not support every data service. Missing data is never replaced with demo prices.

## Setup (Windows)

The supplied scripts target **Windows x64 with MSYS2 UCRT64**. Linux, macOS and alternative toolchains have not been verified in this publishing review.

### 1. Install the prerequisites

| Requirement | What you need |
| --- | --- |
| Node.js and npm | Node.js 24.x is the locally tested version. The installed Vite package requires Node.js `^20.19.0` or `>=22.12.0`. |
| MSYS2 | Install from [msys2.org](https://www.msys2.org/), using `C:\msys64` for the commands below. |
| TraderMade credentials | A REST key for history and instrument discovery, and a separate streaming key for live quotes. |
| Git | Needed if you clone the repository; downloading and extracting its ZIP also works. |

Open **MSYS2 UCRT64** from the Start menu. Follow the [MSYS2 update instructions](https://www.msys2.org/docs/updating/) first, then install the native dependencies:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-boost mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-nlohmann-json mingw-w64-ucrt-x86_64-arrow
```

These provide the compiler, build tools and C++ libraries, including [Apache Arrow/Parquet](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-arrow).

### 2. Get the project and add your keys

Clone this repository using its GitHub **Code** button, or download and extract its ZIP. Open **PowerShell in the project root**: the folder containing this README, `frontend/` and `backend/`.

Create your local configuration if it does not already exist:

```powershell
if (!(Test-Path .env)) { Copy-Item .env.example .env }
notepad .env
```

Replace the example values and save:

```dotenv
TRADERMADE_REST_KEY=your_rest_key
TRADERMADE_WS_KEY=your_streaming_key
PORT=3001
```

Use each full key exactly as issued, including any leading `-`. `.env` is ignored by Git; keep real keys out of source files, screenshots and commits. Restart the backend after changing configuration.

### 3. Install and build

Run these commands in **PowerShell**, one at a time. Continue only when each command succeeds:

```powershell
npm.cmd --prefix frontend ci
npm.cmd --prefix frontend run build
powershell -ExecutionPolicy Bypass -File backend/build.ps1 -Test
```

The frontend build creates `frontend/dist/`. The native build creates `backend/build/tradermade_backend.exe` and runs the C++ tests.

### 4. Start the dashboard

```powershell
powershell -ExecutionPolicy Bypass -File backend/start.ps1
```

Open **http://127.0.0.1:3001**. Keep the terminal open while using the app; press **Ctrl+C** to stop it. Stop the backend before rebuilding its executable.

To check that it is running, open http://127.0.0.1:3001/api/health. This reports the backend type and whether keys are present; it does not verify that your credentials are valid.

Once built, the dashboard runs through the C++ backend. Node.js is only needed for frontend development, builds and browser tests. The executable still needs the MSYS2 runtime DLLs and the built frontend.

<details>
<summary>MSYS2 installed somewhere else?</summary>

Pass your UCRT64 directory to both scripts, for example:

```powershell
powershell -ExecutionPolicy Bypass -File backend/build.ps1 -Test -ToolchainPrefix D:/msys64/ucrt64
powershell -ExecutionPolicy Bypass -File backend/start.ps1 -ToolchainPrefix D:/msys64/ucrt64
```

If TLS certificate discovery fails, add the CA bundle path to `.env`:

```dotenv
SSL_CERT_FILE=D:/msys64/ucrt64/etc/ssl/certs/ca-bundle.crt
```

</details>

## Development

Start the backend as above. In a second PowerShell terminal at the project root, run:

```powershell
npm.cmd --prefix frontend run dev
```

Open the URL printed by Vite for frontend hot reload. Vite forwards `/api` and `/ws` to the C++ backend on port `3001`.

After changing C++ code, stop the backend, rebuild it, and start it again. To see frontend edits at the backend URL instead of the Vite URL, rebuild the frontend.

## Tests

Run from the project root after building both parts:

```powershell
powershell -ExecutionPolicy Bypass -File backend/build.ps1 -Test
npm.cmd --prefix frontend run build
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
npm.cmd --prefix frontend run test:browser
```

The PATH line lets the browser test runner start the native backend with its required DLLs. Adjust it if you installed MSYS2 elsewhere. Tests expect port `3001` and a built frontend; Playwright starts the backend automatically or reuses an existing instance.

- **Native tests:** dates, pagination, normalization, cache expiry, Parquet persistence, refresh, corrupt archives, disk failures, account isolation and concurrent fetches.
- **Browser tests:** chart interactions, market selection, mobile layout and local gateway validation. Market data is mocked; these tests do not verify real provider access.

Browser tests use installed **Microsoft Edge** by default. To use installed Chrome instead, set `$env:PLAYWRIGHT_CHANNEL = 'chrome'` before running them.

For an optional check against real TraderMade data, keep the backend running with valid keys and run:

```powershell
powershell -ExecutionPolicy Bypass -File backend/check-live.ps1
# Other markets, subject to your account's access:
powershell -ExecutionPolicy Bypass -File backend/check-live.ps1 -Symbol BTCUSD
powershell -ExecutionPolicy Bypass -File backend/check-live.ps1 -Symbol UK100
```

This uses your provider account and checks both history and receipt of a streaming quote. No incoming quote within the timeout can also mean the market is inactive.

## Configuration

| Variable | Purpose | Default |
| --- | --- | --- |
| `TRADERMADE_REST_KEY` | Historical data and instrument catalogue | None |
| `TRADERMADE_WS_KEY` | Live streaming quotes | None |
| `PORT` | Local backend port | `3001` |
| `SSL_CERT_FILE` | Optional CA certificate bundle | Auto-detected at the default MSYS2 path when available |

Process environment variables override values in the root `.env`. If you change `PORT`, also update the Vite proxy, Playwright configuration and live-check script, which currently target `3001`.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| `npm` is unavailable or Vite reports an unsupported Node version | Install a compatible Node.js version and reopen PowerShell. Use `npm.cmd` for the documented Windows commands. |
| CMake, Ninja or a native library is missing | Complete the MSYS2 package installation in the **UCRT64** shell. Check `-ToolchainPrefix` if using a custom location. |
| Missing DLL when starting the executable | Use `backend/start.ps1`, which adds MSYS2 to PATH. Copying the `.exe` alone is insufficient. |
| Backend cannot start | Check whether port `3001` is in use and confirm `PORT` is a number between 1 and 65535. |
| Dashboard is missing or shows old UI | Run `npm.cmd --prefix frontend run build`, then reload the browser. |
| Missing/invalid key or denied history | Check the two key names in `.env`, restart the backend, and confirm your account has access to that data. |
| History loads but live quotes do not | Check the separate streaming key, streaming symbol access and whether the market is active. |
| TLS certificate error | Check `SSL_CERT_FILE` and your CA bundle path. |
| Browser tests cannot launch | Install Edge or select installed Chrome. Ensure the MSYS2 DLL directory is on PATH. |

## Project structure

```text
backend/                 C++ API, streaming gateway, cache and native tests
  build.ps1              Configure, compile and optionally test
  start.ps1              Start the local backend
  check-live.ps1         Optional real-provider smoke check
frontend/                TypeScript UI, Vite build and Playwright tests
  public/NOTICE.txt      TradingView attribution notice
docs/                    Tutorials and editorial drafts; some describe the older backend
.env.example             Configuration template (safe to commit)
.env                     Your local credentials (ignored)
.cache/                  Local market-data cache (ignored)
```

## How it works

The browser requests history and subscribes to quotes through the local C++ gateway. The gateway calls TraderMade, shares an upstream streaming connection across tabs, and serves the built frontend. API credentials remain on the server.

History is read from **memory → Parquet → TraderMade**. Completed non-empty date ranges are stored as Snappy-compressed Parquet files under `.cache/parquet/<account-hash>/`. Recent and empty ranges stay in memory briefly; refresh bypasses cached data. Cache files contain timestamps and OHLC values, not API keys or raw provider responses. Disk usage grows as you explore history. To clear it, stop the backend, remove `.cache/parquet/`, and restart.

Historical pages load on demand; the app does not preload years of prices or invent candles for missing periods. Crypto history preserves weekends. Live daily candles use UTC boundaries; historical bars follow the provider's session conventions.

<details>
<summary>Local API reference</summary>

| Endpoint | Purpose |
| --- | --- |
| `GET /api/health` | Backend type and credential-presence flags |
| `GET /api/instruments` | Market catalogue, categories, precision and catalogue status |
| `GET /api/history?symbol=EURUSD&timeframe=1D` | Recent OHLC candles |
| `WS /ws` | Quote subscriptions and connection status |

Add `before=<exclusive Unix seconds>` to history requests to paginate backward. Add `refresh=1` to bypass the cache, including when refreshing an older page. Responses report `cache.layer` as `memory`, `parquet` or `provider`.

Subscribe over WebSocket with:

```json
{"type":"subscribe","symbol":"EURUSD"}
```

The server returns normalized `status` and `quote` messages.

</details>

## GitHub publishing checklist

Publishing the source and hosting a public dashboard are separate tasks. The current backend binds to loopback and checks local browser origins. Public hosting requires additional deployment work, including authentication and an explicit origin policy.

Local review on **30 September 2026**: frontend production build passed, the native CTest suite passed, all **16 browser tests passed**, and npm audit reported **0 known vulnerabilities**. Git ignore rules were checked using a temporary Git directory; 42 candidate source files contained no matches for the local API key values. This was a targeted source check, not a full secret or native-dependency security audit. Real-provider checks and clean-machine setup were not run.

Before the first public push:

- [ ] Confirm the copyright holder and replace the bracketed fields in [LICENSE](LICENSE).
- [ ] Review the articles in `docs/` for obsolete Node-backend instructions, local details and editorial metadata. In particular, `docs/tut.md` still includes Node examples without a historical-draft notice; the older named tutorial and editorial notes already have notices.
- [ ] Verify setup on a clean Windows machine or CI runner. Local builds do not establish that a fresh machine has every prerequisite.
- [ ] Initialize Git, choose the GitHub owner/repository and configure its remote. This workspace had no `.git` directory during the publishing review.
- [ ] Review staged files before committing: include `.env.example` and `frontend/package-lock.json`; exclude `.env`, `.cache/`, `node_modules/`, build output, test reports and personal editor settings. Run a secret scan on the staged content and any history before pushing.
- [ ] Run the build and test commands above, then commit and push the reviewed source.

Recommended follow-ups:

- [ ] Add GitHub Actions for the frontend build, native tests and browser tests.
- [ ] Add a dashboard screenshot and a short repository description/topics.
- [ ] Add contribution and security-reporting instructions if accepting public contributions.
- [ ] Remove unused starter files such as `frontend/src/counter.ts`, `frontend/src/assets/vite.svg` and `frontend/src/assets/typescript.svg` if they are no longer needed.
- [ ] Before distributing prebuilt binaries, document the runtime DLLs and include the relevant dependency license notices.

## License and references

Project license: [MIT](LICENSE). TradingView Lightweight Charts carries its own [notice](frontend/public/NOTICE.txt); retain the chart attribution and footer link when modifying the UI.

- [TraderMade REST API](https://marketdata.tradermade.com/docs/restful-api)
- [TraderMade streaming API](https://marketdata.tradermade.com/docs/streaming-data-api)
- [TradingView Lightweight Charts](https://tradingview.github.io/lightweight-charts/)
- [Apache Arrow C++ Parquet](https://arrow.apache.org/docs/cpp/parquet.html)
