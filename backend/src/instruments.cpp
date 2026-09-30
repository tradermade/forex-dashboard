#include "terminal.hpp"
#include <algorithm>
#include <set>

namespace terminal {
Json normalize_instruments(const Json& currencies,const Json& crypto,const Json& cfds) {
  std::map<std::string,std::string> names;
  std::set<std::string> crypto_codes;
  const std::set<std::string> metals={"XAU","XAG","XPD","XPT"};
  auto add_codes=[&](const Json& source,bool is_crypto) {
    if(!source.is_object())throw std::runtime_error("Invalid instrument catalogue");
    for(auto it=source.begin();it!=source.end();++it) {
      if(!valid_symbol(it.key())||!it.value().is_string())continue;
      names[it.key()]=it.value().get<std::string>().substr(0,120);
      if(is_crypto)crypto_codes.insert(it.key());
    }
  };
  add_codes(currencies,false);add_codes(crypto,true);
  if((names.empty()&&cfds.empty())||names.size()>500||!cfds.is_object()||cfds.size()>5000)throw std::runtime_error("Invalid instrument catalogue");
  std::map<std::string,Json> unique;
  for(auto& [base,base_name]:names)for(auto& [quote,quote_name]:names) {
    if(base==quote||!valid_symbol(base+quote))continue;
    // Do not discard weekend history for crypto; the provider determines coverage.
    bool has_crypto=crypto_codes.contains(base)||crypto_codes.contains(quote);
    auto category=has_crypto?"Crypto":metals.contains(base)||metals.contains(quote)?"Metals":"Forex";
    unique[base+quote]={{"symbol",base+quote},{"name",base_name+" / "+quote_name},{"base",base},{"quote",quote},{"category",category},
      {"digits",has_crypto?8:std::string(category)=="Metals"?4:quote=="JPY"?3:5},{"continuous",has_crypto}};
  }
  for(auto it=cfds.begin();it!=cfds.end();++it)if(valid_symbol(it.key())&&it.value().is_string())
    unique[it.key()]={{"symbol",it.key()},{"name",it.value().get<std::string>().substr(0,120)},{"base",it.key()},{"quote",""},{"category","CFD"},{"digits",4},{"continuous",false}};
  Json result=Json::array();for(auto& [symbol,item]:unique)result.push_back(item);
  return result;
}
InstrumentCatalog::InstrumentCatalog(Loader loader,Clock clock):loader_(std::move(loader)),clock_(std::move(clock)){}
Json InstrumentCatalog::get() {
  std::lock_guard lock(mutex_);
  if(clock_()<expires_){if(!cached_.is_null())return cached_;throw std::runtime_error("Instrument catalogue is unavailable. Try again shortly.");}
  try {
    // Fetch independently so a plan-specific failure does not hide other markets.
    Json currencies=Json::object(),crypto=Json::object(),cfds=Json::object(),unavailable=Json::array();
    auto fetch=[&](const char* endpoint,const char* field,Json& output){try{auto data=loader_(endpoint);if(!data.contains(field)||!data[field].is_object()||data[field].empty())throw std::runtime_error("Unavailable");output=data[field];}catch(...){unavailable.push_back(endpoint);}};
    fetch("live_currencies_list","available_currencies",currencies);
    fetch("live_crypto_list","available_currencies",crypto);
    fetch("cfd_list","available_cfds",cfds);
    if(currencies.empty()&&crypto.empty()&&cfds.empty())throw std::runtime_error("Instrument catalogue is unavailable. Try again shortly.");
    // Keep previously discovered codes when just one endpoint is temporarily unavailable.
    if(!unavailable.empty()&&!cached_.is_null()){cached_["stale"]=true;expires_=clock_()+60000;return cached_;}
    auto items=normalize_instruments(currencies,crypto,cfds);
    cached_={{"instruments",items},{"source","TraderMade"},{"stale",false},{"unavailable",unavailable}};
    expires_=clock_()+(unavailable.empty()?86400000:60000);return cached_;
  }catch(...){expires_=clock_()+60000;if(!cached_.is_null()){cached_["stale"]=true;return cached_;}throw;}
}
bool InstrumentCatalog::continuous(const std::string& symbol) {
  // Catalogue outages must not prevent cached or direct historical requests.
  try {auto data=get();for(auto& item:data["instruments"])if(item["symbol"]==symbol)return item["continuous"].get<bool>();}
  catch(...){return true;}
  return true;
}
}
