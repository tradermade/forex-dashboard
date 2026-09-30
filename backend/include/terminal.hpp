#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace terminal {
using Json = nlohmann::ordered_json;
using Clock = std::function<int64_t()>;
struct Timeframe { int seconds; std::string interval; int period; int days; };
const std::map<std::string, Timeframe>& timeframes();
bool valid_symbol(const std::string& symbol);
int64_t now_ms();
int64_t timestamp(const Json& value);
double number(const Json& value);
std::string sha256(const std::string& value);
std::string url_encode(const std::string& value);
std::string url_decode(const std::string& value);
Json history_range(const std::string& timeframe, int64_t now, std::optional<int64_t> before = {}, bool continuous = false);
Json normalize_candles(const Json& rows);
std::optional<Json> normalize_quote(const Json& row);
std::string subscription_error(const Json& acknowledgement,const std::string& symbol,bool invalid);
bool archiveable(const Json& value, int64_t now);
bool valid_history(const Json& value);
struct Config {
  std::filesystem::path root, cache_directory, static_root;
  std::string rest_key, stream_key, ca_file;
  std::string stream_host = "stream.tradermade.com", stream_port = "443", stream_path = "/feedAdv";
  unsigned short port = 3001;
};
Config load_config(int argc, char** argv);
struct ProviderResponse { long status; Json data; };
using Fetch = std::function<ProviderResponse(const std::string&, const Json&)>;
Fetch make_rest_fetch(const Config& config);
ProviderResponse rest_request(const Config& config, const std::string& endpoint, const Json& params = Json::object());
Json normalize_instruments(const Json& currencies, const Json& crypto, const Json& cfds);
class InstrumentCatalog {
public:
  using Loader = std::function<Json(const std::string&)>;
  explicit InstrumentCatalog(Loader loader, Clock clock = now_ms);
  Json get();
  bool continuous(const std::string& symbol);
private:
  Loader loader_;
  Clock clock_;
  std::mutex mutex_;
  Json cached_;
  int64_t expires_ = 0;
};
struct CacheEntry { Json value; int64_t fetched_at, expires_at; uint64_t access; };
class HistoryCache {
public:
  explicit HistoryCache(std::filesystem::path directory = {}, Clock clock = now_ms, size_t capacity = 200);
  std::optional<Json> get(const std::string& id, const Json& query);
  void set(const std::string& id, const Json& value, int64_t ttl);
  std::filesystem::path archive_path(const std::string& id, const Json& query) const;
private:
  std::filesystem::path directory_;
  Clock clock_;
  size_t capacity_;
  std::mutex mutex_;
  std::map<std::string, CacheEntry> entries_;
  uint64_t access_ = 0;
  bool warned_ = false;
  void remember(const std::string& id, CacheEntry entry);
  void warn();
};
class HistoryProvider {
public:
  HistoryProvider(std::string key, HistoryCache& cache, Fetch fetch, Clock clock = now_ms, std::function<bool(const std::string&)> continuous = {});
  Json get(const std::string& symbol, const std::string& timeframe, std::optional<int64_t> before = {}, bool refresh = false);
private:
  std::string key_, account_;
  HistoryCache& cache_;
  Fetch fetch_;
  Clock clock_;
  std::function<bool(const std::string&)> continuous_;
  std::mutex mutex_;
  std::map<std::string, std::shared_future<Json>> pending_;
};
void run_gateway(const Config& config, HistoryProvider& history, InstrumentCatalog& catalog);
}
