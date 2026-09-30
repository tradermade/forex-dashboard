#include "terminal.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>

namespace terminal {
const std::map<std::string, Timeframe>& timeframes() {
  static const std::map<std::string, Timeframe> result = {
    {"1m", {60,"minute",1,1}}, {"5m",{300,"minute",5,1}}, {"15m",{900,"minute",15,1}},
    {"30m",{1800,"minute",30,1}}, {"1h",{3600,"hourly",1,14}}, {"4h",{14400,"hourly",4,28}}, {"1D",{86400,"daily",1,180}}
  }; return result;
}
bool valid_symbol(const std::string& symbol) {
  // Provider decides entitlement and availability. Keep symbols safe for URLs and cache paths.
  static const std::regex pattern("^[A-Z0-9][A-Z0-9_-]{0,23}$");
  return std::regex_match(symbol, pattern);
}
int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
double number(const Json& value) {
  try {
    if (value.is_number()) return value.get<double>();
    if (value.is_string()) { auto s = value.get<std::string>(); size_t used; double n = std::stod(s, &used); if (used == s.size()) return n; }
  } catch (...) {} return std::numeric_limits<double>::quiet_NaN();
}
static std::tm utc(int64_t seconds) {
  time_t t = static_cast<time_t>(seconds); std::tm result{};
#ifdef _WIN32
  gmtime_s(&result, &t);
#else
  gmtime_r(&t, &result);
#endif
  return result;
}
int64_t timestamp(const Json& value) {
  if (value.is_number()) { auto n=number(value); return std::isfinite(n) && n>0 && n<1e15 ? static_cast<int64_t>(n>1e12?n/1000:n) : -1; }
  if (!value.is_string()) return -1;
  std::string s = value.get<std::string>();
  static const std::regex numeric(R"(^\d+(\.\d+)?$)");
  if (std::regex_match(s,numeric)) return timestamp(Json(number(value)));
  static const std::regex compact(R"(^(\d{4})(\d{2})(\d{2})-)");
  s = std::regex_replace(s,compact,"$1-$2-$3T");
  static const std::regex date(R"(^(\d{4})-(\d{2})-(\d{2})(?:[T -](\d{2}):(\d{2})(?::(\d{2})(?:\.\d+)?)?)?Z?$)");
  std::smatch match; if (!std::regex_match(s,match,date)) return -1;
  std::tm t{}; t.tm_year=std::stoi(match[1])-1900; t.tm_mon=std::stoi(match[2])-1; t.tm_mday=std::stoi(match[3]);
  t.tm_hour=match[4].matched?std::stoi(match[4]):0; t.tm_min=match[5].matched?std::stoi(match[5]):0; t.tm_sec=match[6].matched?std::stoi(match[6]):0;
  if(t.tm_mon<0||t.tm_mon>11||t.tm_mday<1||t.tm_mday>31||t.tm_hour>23||t.tm_min>59||t.tm_sec>59) return -1;
  const auto original=t;
#ifdef _WIN32
  auto result=_mkgmtime(&t);
#else
  auto result=timegm(&t);
#endif
  if(t.tm_year!=original.tm_year||t.tm_mon!=original.tm_mon||t.tm_mday!=original.tm_mday) return -1;
  return result;
}
std::string sha256(const std::string& value) {
  unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length=0;
  if(EVP_Digest(value.data(),value.size(),digest,&length,EVP_sha256(),nullptr)!=1) throw std::runtime_error("Hashing failed");
  std::ostringstream out; for(unsigned int i=0;i<length;++i) out<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(digest[i]); return out.str();
}
std::string url_encode(const std::string& value) {
  std::ostringstream out;
  for(unsigned char c:value) { if(std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') out<<c; else out<<'%'<<std::uppercase<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<int>(c); }
  return out.str();
}
std::string url_decode(const std::string& value) {
  std::string out;
  for(size_t i=0;i<value.size();++i) {
    if(value[i]=='%') { if(i+2>=value.size()||!std::isxdigit(static_cast<unsigned char>(value[i+1]))||!std::isxdigit(static_cast<unsigned char>(value[i+2]))) throw std::runtime_error("Invalid URL"); out+=static_cast<char>(std::stoi(value.substr(i+1,2),nullptr,16)); i+=2; }
    else out+=value[i]=='+'?' ':value[i];
  } return out;
}
Json history_range(const std::string& timeframe, int64_t now, std::optional<int64_t> before, bool continuous) {
  const auto& config=timeframes().at(timeframe); int64_t end=before?*before-1:now/1000;
  auto day=utc(end).tm_wday;
  if(!continuous&&(day==0||day==6)) end=(end/86400-(day==0?2:1))*86400+86340;
  int64_t start=end-static_cast<int64_t>(config.days)*86400;
  if(config.interval=="minute") start=start/86400*86400;
  auto format=[&](int64_t seconds) { auto t=utc(seconds); std::ostringstream out; out<<std::put_time(&t,config.interval=="daily"?"%Y-%m-%d":"%Y-%m-%d-%H:%M"); return out.str(); };
  return {{"start_date",format(start)},{"end_date",format(end)},{"interval",config.interval},{"period",std::to_string(config.period)}};
}
Json normalize_candles(const Json& rows) {
  std::map<int64_t,Json> sorted;
  if(!rows.is_array()) return Json::array();
  for(const auto& row:rows) {
    if(!row.is_object()) continue;
    auto time=timestamp(row.value("date",row.value("time",Json())));
    double o=number(row.value("open",Json())),h=number(row.value("high",Json())),l=number(row.value("low",Json())),c=number(row.value("close",Json()));
    if(time>0&&std::isfinite(o)&&std::isfinite(h)&&std::isfinite(l)&&std::isfinite(c)&&l>0&&h>=std::max(o,c)&&l<=std::min(o,c)) sorted[time]={{"time",time},{"open",o},{"high",h},{"low",l},{"close",c}};
  }
  Json result=Json::array(); for(auto& [time,row]:sorted) result.push_back(row); return result;
}
std::optional<Json> normalize_quote(const Json& row) {
  try {
    auto symbol=row.value("s",row.value("symbol",std::string())); symbol=symbol.substr(0,symbol.find(':'));
    double bid=number(row.value("b",row.value("bid",Json()))),ask=number(row.value("a",row.value("ask",Json())));
    double mid=row.contains("m")?number(row["m"]):row.contains("mid")?number(row["mid"]):(bid+ask)/2;
    auto time=timestamp(row.value("ts",row.value("timestamp",row.value("time",Json()))));
    if(!valid_symbol(symbol)||!std::isfinite(bid)||!std::isfinite(ask)||!std::isfinite(mid)||bid<=0||ask<bid||mid<=0||time<=0) return {};
    return Json{{"symbol",symbol},{"bid",bid},{"ask",ask},{"mid",mid},{"time",time},{"cached",row.value("t",row.value("type",std::string()))=="LAST_QUOTE"}};
  } catch(...) { return {}; }
}
bool archiveable(const Json& value, int64_t now) {
  try {
    const auto& config=timeframes().at(value.at("timeframe").get<std::string>());
    int base=config.interval=="daily"?86400:config.interval=="hourly"?3600:60;
    int lag=config.interval=="daily"?172800:config.interval=="hourly"?10800:120;
    auto end=timestamp(value.at("range").at("end_date")); auto cutoff=now/1000-lag;
    if(end<0||end+base>cutoff) return false;
    if(value.contains("candles")) for(auto& c:value["candles"]) if(c.at("time").get<int64_t>()+config.seconds>cutoff) return false;
    return true;
  } catch(...) { return false; }
}
std::string subscription_error(const Json& acknowledgement,const std::string& symbol,bool invalid) {
  if(invalid)return "Streaming is unavailable for this symbol. Historical data may still be available.";
  const auto reasons=acknowledgement.value("denied_reasons",Json::object());
  const auto reason=reasons.is_object()?reasons.value(symbol,Json()):Json();
  if(reason=="cfds_not_allowed")return "CFD streaming is not enabled for this account. Historical data may still be available.";
  if(reason=="limit_reached")return "Streaming symbol limit reached. Close another chart or change your plan.";
  return "Subscription denied. Check streaming access for this symbol.";
}
bool valid_history(const Json& value) {
  try {
    if(!valid_symbol(value.at("symbol"))||!timeframes().contains(value.at("timeframe"))||value.at("source")!="TraderMade"||!value.at("nextBefore").is_number_integer()) return false;
    auto& rows=value.at("candles"); if(!rows.is_array()||rows.size()>5000) return false;
    for(auto key:{"start_date","end_date","interval","period"}) if(!value.at("range").at(key).is_string()) return false;
    auto normalized=normalize_candles(rows); return normalized==rows;
  } catch(...) { return false; }
}
}
