# C++ migration record

The backend now lives entirely in `backend/`. It uses C++20, Boost.Asio/Beast, OpenSSL, libcurl, nlohmann JSON, and Apache Arrow/Parquet.

Completed:
- REST history and exclusive-cursor pagination; browser API unchanged.
- Shared authenticated TraderMade v2 WebSocket, subscription changes, bounded write queues, heartbeat and reconnect backoff.
- Native memory cache and Snappy Parquet archives with atomic writes, refresh, failure fallback and concurrent request coalescing.
- Existing cache compatibility confirmed: 129 EURUSD daily candles read directly from an old archive (before 2026-04-01).
- Real REST request returned 240 hourly candles; real authenticated WebSocket returned an EURUSD cached quote.
- CTest passed all 12 native scenarios. All 9 chart browser tests and 2 C++ gateway browser tests passed. Frontend production build passed.
- Removed `server/`, root `node_modules/`, root npm manifests, old Node launchers/tests, and root npm cache.
- Frontend types/helpers, npm dependencies, Playwright configuration and browser tests now live under `frontend/`.
- C++ build/start/live-check PowerShell scripts and current setup instructions added. Old editorial articles marked as historical drafts.

Keys remain in the ignored root `.env`; the leading minus in the REST credential is preserved. Existing `.cache/parquet` history remains intact.

Run: `powershell -ExecutionPolicy Bypass -File backend/start.ps1`, then open http://127.0.0.1:3001.

## Multi-market update

- Removed the eight-symbol backend allowlist. Symbols are syntax-validated for safe API/cache use; TraderMade determines data availability and account entitlement.
- Added a cached native catalogue endpoint covering forex/crypto crosses and exact CFD codes, with metals categorized separately.
- Added category filters, full-catalogue search, bounded row rendering and catalogue retry.
- Fixed missed clicks by preserving button elements across streaming ticks. Quote updates only touch the relevant price/status nodes.
- Added crypto weekend history, crypto display precision and asset-appropriate spread units.
- Verified live BTCUSD history/stream and UK100 CFD history. Streaming availability is reported separately from historical access.
