#include "terminal.hpp"
#include <curl/curl.h>
#include <iostream>

int main(int argc, char** argv) {
  try {
    auto config = terminal::load_config(argc, argv);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("HTTP initialization failed");
    terminal::HistoryCache cache(config.cache_directory);
    terminal::InstrumentCatalog catalog([&config](const std::string& endpoint){
      auto response=terminal::rest_request(config,endpoint);
      if(response.status!=200)throw std::runtime_error("Instrument catalogue is unavailable.");
      return response.data;
    });
    terminal::HistoryProvider history(config.rest_key, cache, terminal::make_rest_fetch(config),terminal::now_ms,
      [&catalog](const std::string& symbol){return catalog.continuous(symbol);});
    terminal::run_gateway(config, history, catalog);
    curl_global_cleanup();
    return 0;
  } catch (const std::exception&) {
    std::cerr << "Gateway could not start. Check the port, TLS certificates, and backend configuration.\n";
    return 1;
  }
}
