#include "terminal.hpp"
#include <arrow/io/api.h>
#include <parquet/file_reader.h>
#include <atomic>
#include <barrier>
#include <fstream>
#include <iostream>
#include <thread>
using namespace terminal;
namespace fs=std::filesystem;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static int passed=0;
template<class F> void test(const char* name,F run){run();++passed;std::cout<<"PASS "<<name<<'\n';}
static const auto before=timestamp("2026-05-15");
static const auto fixed_now=timestamp("2026-09-28T12:00:00Z")*1000;
static Json rows(){return Json::array({{{"date","2026-05-13"},{"open",1.11},{"high",1.15},{"low",1.10},{"close",1.14}},{{"date","2026-05-14"},{"open",1.14},{"high",1.17},{"low",1.13},{"close",1.16}}});}
static ProviderResponse response(){return {200,{{"quotes",rows()}}};}
static std::vector<fs::path> files(const fs::path& root){std::vector<fs::path> result;for(auto& f:fs::recursive_directory_iterator(root))if(f.is_regular_file())result.push_back(f.path());return result;}
int main(){
  auto root=fs::absolute("test-cache-"+std::to_string(now_ms()));fs::create_directories(root);
  auto folder=[&](const std::string& name){auto path=root/name;fs::create_directories(path);return path;};
  auto clock=[] {return fixed_now;};
  try{
    test("stream rejection distinguishes plan access and unsupported symbols safely",[]{
      require(subscription_error({{"denied_reasons",{{"UK100:QUOTE","cfds_not_allowed"}}}},"UK100:QUOTE",false).find("not enabled")!=std::string::npos,"CFD entitlement");
      require(subscription_error({{"denied_reasons",{{"EURUSD:QUOTE","limit_reached"}}}},"EURUSD:QUOTE",false).find("limit reached")!=std::string::npos,"subscription cap");
      require(subscription_error(Json::object(),"UK100:QUOTE",true).find("unavailable")!=std::string::npos,"unsupported stream");
      require(subscription_error({{"denied_reasons",{{"UK100:QUOTE","private-provider-secret"}}}},"UK100:QUOTE",false).find("secret")==std::string::npos,"sanitize provider reason");
    });
    test("catalogue includes forex, crypto, metals and exact CFD codes",[]{
      auto items=normalize_instruments({{"USD","US Dollar"},{"EUR","Euro"},{"XAU","Gold"}},{{"BTC","Bitcoin"},{"DOGE","Dogecoin"}},{{"UK100","FTSE 100"},{"AAPL","Apple"}});
      auto find=[&](const char* symbol){for(auto& item:items)if(item["symbol"]==symbol)return item;throw std::runtime_error("Symbol missing");};
      require(find("EURUSD")["category"]=="Forex","forex");require(find("BTCUSD")["category"]=="Crypto"&&find("DOGEUSD")["digits"]==8,"crypto precision");
      require(find("XAUUSD")["category"]=="Metals","metals");require(find("UK100")["quote"]==""&&find("AAPL")["category"]=="CFD","standalone CFD");
      require(valid_symbol("UK100")&&valid_symbol("V")&&valid_symbol("DOGEUSD")&&!valid_symbol("../EURUSD")&&!valid_symbol("EURUSD:QUOTE"),"safe symbol validation");
      require(normalize_instruments(Json::object(),Json::object(),{{"AAPL","Apple"}}).size()==1,"CFD only catalogue");
    });
    test("catalogue coalesces refresh and retains stale markets on failure",[&]{
      int calls=0;int64_t now=fixed_now;bool fail=false;
      InstrumentCatalog catalog([&](const std::string& endpoint)->Json{++calls;if(fail)throw std::runtime_error("Offline");return endpoint=="cfd_list"?Json{{"available_cfds",{{"UK100","FTSE"}}}}:Json{{"available_currencies",endpoint=="live_crypto_list"?Json{{"BTC","Bitcoin"}}:Json{{"EUR","Euro"},{"USD","US Dollar"}}}};},[&]{return now;});
      auto first=catalog.get();require(calls==3&&catalog.continuous("BTCUSD")&&!catalog.continuous("EURUSD"),"catalogue and calendar cache");
      catalog.get();require(calls==3,"cached catalogue");now+=86400001;fail=true;auto stale=catalog.get();require(stale["stale"]==true&&stale["instruments"]==first["instruments"],"stale fallback");
      int failures=0;InstrumentCatalog offline([&](auto&)->Json{++failures;throw std::runtime_error("Offline");},clock);require(offline.continuous("BTCUSD"),"history independent of catalogue outage");offline.continuous("BTCUSD");require(failures==3,"failure backoff");
    });
    test("crypto pagination retains weekend dates and quotes",[&]{
      auto sunday=timestamp("2026-09-27T12:00:00Z")*1000;
      require(history_range("1m",sunday,{},true)["end_date"]=="2026-09-27-12:00","Sunday crypto history");
      HistoryCache cache({},[&]{return sunday;});Json requested;
      HistoryProvider p("test-key",cache,[&](auto&,const Json& range){requested=range;return response();},[&]{return sunday;},[](auto&){return true;});
      p.get("BTCUSD","1m");require(requested["end_date"]=="2026-09-27-12:00","provider calendar");
      for(auto symbol:{"BTCUSD","UK100","AAPL","XAUUSD"})require(normalize_quote({{"s",symbol},{"b",100},{"a",101},{"ts",before}}).has_value(),"multi asset quote");
    });
    test("UTC timestamps and compact streaming timestamps",[]{
      require(timestamp("20260515-12:36:35.588")==timestamp("2026-05-15T12:36:35Z"),"compact timestamp");
      require(timestamp(1778846400000LL)==1778846400,"millisecond timestamp");
      require(timestamp("2026-02-30")==-1&&timestamp("bad")==-1,"reject invalid dates");
    });
    test("exclusive pagination and provider date limits",[]{
      auto range=history_range("1D",fixed_now,before);require(range["end_date"]=="2026-05-14","exclusive daily cursor");
      auto weekend=history_range("1m",timestamp("2026-09-27T12:00:00Z")*1000);
      require(weekend["end_date"]=="2026-09-25-23:59","weekend end");
      require(timestamp(weekend["end_date"])-timestamp(weekend["start_date"])<2*86400,"minute window limit");
      require(history_range("4h",fixed_now)["period"]=="4","four hour aggregation");
    });
    test("sort, deduplicate and validate OHLC",[]{auto data=rows();data.push_back(data[0]);data.push_back({{"date","2026-05-12"},{"open",1},{"high",0.5},{"low",0.1},{"close",1}});auto value=normalize_candles(data);require(value.size()==2&&value[0]["time"]==timestamp("2026-05-13"),"normalized candles");});
    test("live and cached quote normalization",[]{auto q=normalize_quote({{"t","LAST_QUOTE"},{"s","EURUSD:QUOTE"},{"b","1.1"},{"a","1.2"},{"ts","20260515-12:36:35.588"}});require(q&&(*q)["cached"]==true&&(*q)["symbol"]=="EURUSD","cached quote");require(!normalize_quote({{"s","EURUSD"},{"b",2},{"a",1},{"ts",before}}),"crossed market rejected");});
    test("Snappy Parquet round trip, schema, restart and memory hit",[&]{
      auto dir=folder("roundtrip");int calls=0;HistoryCache cache(dir,clock);HistoryProvider provider("test-key",cache,[&](auto&,auto&){++calls;return response();},clock);
      auto initial=provider.get("EURUSD","1D",before);require(initial["cache"]["layer"]=="provider","cold provider");require(provider.get("EURUSD","1D",before)["cache"]["layer"]=="memory"&&calls==1,"warm memory");
      auto paths=files(dir);require(paths.size()==1,"single archive");auto reader=parquet::ParquetFileReader::OpenFile(paths[0].string());auto metadata=reader->metadata();require(metadata->num_rows()==2&&metadata->num_columns()==5,"Parquet schema");
      for(int c=0;c<5;++c)require(metadata->RowGroup(0)->ColumnChunk(c)->compression()==parquet::Compression::SNAPPY,"Snappy codec");
      require(metadata->schema()->Column(0)->physical_type()==parquet::Type::INT64,"integer time");reader->Close();
      HistoryCache restored(dir,clock);HistoryProvider restart("test-key",restored,[](auto&,auto&)->ProviderResponse{throw std::runtime_error("must not fetch");},clock);
      auto value=restart.get("EURUSD","1D",before);require(value["cache"]["layer"]=="parquet"&&value["candles"]==initial["candles"],"disk restart");
    });
    test("refresh atomically replaces archived corrections",[&]{auto dir=folder("refresh");int calls=0;HistoryCache cache(dir,clock);HistoryProvider provider("test-key",cache,[&](auto&,auto&){auto data=response();if(++calls>1)data.data["quotes"][0]["close"]=1.145;return data;},clock);provider.get("EURUSD","1D",before);provider.get("EURUSD","1D",before,true);HistoryCache cache2(dir,clock);HistoryProvider restart("test-key",cache2,[](auto&,auto&)->ProviderResponse{throw std::runtime_error("must not fetch");},clock);require(restart.get("EURUSD","1D",before)["candles"][0]["close"]==1.145&&calls==2&&files(dir).size()==1,"refreshed disk");});
    test("corrupt archive falls back and repairs",[&]{auto dir=folder("corrupt");{HistoryCache cache(dir,clock);HistoryProvider p("test-key",cache,[](auto&,auto&){return response();},clock);p.get("EURUSD","1D",before);}auto path=files(dir)[0];{std::ofstream out(path);out<<"corrupt";}HistoryCache cache(dir,clock);HistoryProvider p("test-key",cache,[](auto&,auto&){return response();},clock);require(p.get("EURUSD","1D",before)["cache"]["layer"]=="provider","corrupt fallback");std::ifstream in(path,std::ios::binary);char magic[4];in.read(magic,4);require(std::string(magic,4)=="PAR1","archive repaired");});
    test("forming and empty pages expire without disk archives",[&]{auto dir=folder("forming");int64_t now=fixed_now;int calls=0;auto mutable_clock=[&]{return now;};HistoryCache cache(dir,mutable_clock);HistoryProvider p("test-key",cache,[&](auto&,auto&){++calls;auto data=response();data.data["quotes"][0]["date"]="2026-09-28T12:00:00Z";return data;},mutable_clock);p.get("EURUSD","1m");p.get("EURUSD","1m");require(calls==1,"forming memory");now+=60001;p.get("EURUSD","1m");require(calls==2&&files(dir).empty(),"forming expires");HistoryProvider empty("empty-key",cache,[](auto&,auto&){return ProviderResponse{200,{{"quotes",Json::array()}}};},mutable_clock);auto value=empty.get("EURUSD","1D",before);require(value["candles"].empty()&&value["nextBefore"].get<int64_t>()<before&&files(dir).empty(),"empty cursor and no archive");});
    test("memory expiry loads disk and accounts are isolated",[&]{auto dir=folder("expiry");int64_t now=fixed_now;int calls=0;auto mutable_clock=[&]{return now;};HistoryCache cache(dir,mutable_clock);auto fetch=[&](auto&,auto&){++calls;return response();};HistoryProvider p("test-key",cache,fetch,mutable_clock);p.get("EURUSD","1D",before);now+=31LL*86400000;require(p.get("EURUSD","1D",before)["cache"]["layer"]=="parquet"&&calls==1,"expired memory reads disk");HistoryProvider other("other-key",cache,fetch,mutable_clock);require(other.get("EURUSD","1D",before)["cache"]["layer"]=="provider"&&calls==2,"account isolation");});
    test("disk failures keep memory usable and errors are not cached",[&]{auto dir=folder("disk-failure")/"file";{std::ofstream out(dir);out<<"file";}int calls=0;HistoryCache cache(dir,clock);HistoryProvider p("test-key",cache,[&](auto&,auto&){++calls;return response();},clock);p.get("EURUSD","1D",before);require(p.get("EURUSD","1D",before)["cache"]["layer"]=="memory"&&calls==1,"disk failure memory");HistoryCache errors(folder("errors"),clock);HistoryProvider rejected("test-key",errors,[&](auto&,auto&){++calls;return ProviderResponse{401,{{"message","API key is invalid"}}};},clock);for(int i=0;i<2;++i){bool failed=false;try{rejected.get("EURUSD","1D",before);}catch(const std::exception& e){failed=std::string(e.what()).find("invalid")!=std::string::npos;}require(failed,"safe error");}require(calls==3&&files(folder("errors")).empty(),"errors never cached");});
    test("concurrent requests share one provider fetch",[&]{auto dir=folder("concurrent");HistoryCache cache(dir,clock);std::atomic<int> calls=0;HistoryProvider p("test-key",cache,[&](auto&,auto&){++calls;std::this_thread::sleep_for(std::chrono::milliseconds(100));return response();},clock);std::barrier gate(8);std::vector<std::future<Json>> futures;for(int i=0;i<8;++i)futures.push_back(std::async(std::launch::async,[&]{gate.arrive_and_wait();return p.get("EURUSD","1D",before);}));for(auto& future:futures)require(future.get()["candles"].size()==2,"concurrent results");require(calls==1&&files(dir).size()==1,"one fetch");});
    test("exclusive cursor removes overlapping provider candles",[&]{HistoryCache cache({},clock);HistoryProvider p("test-key",cache,[](auto&,auto&){return response();},clock);auto value=p.get("EURUSD","1D",timestamp("2026-05-14"));require(value["candles"].size()==1,"cursor filter");});
    fs::remove_all(root);std::cout<<passed<<" tests passed\n";return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" (test files: "<<root<<")\n";return 1;}
}
