#include "terminal.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/file_reader.h>
#include <curl/curl.h>
#include <openssl/rand.h>
#include <algorithm>
#include <iostream>
#include <regex>
#ifdef _WIN32
#include <windows.h>
#endif

namespace terminal {
static constexpr int64_t closed_ttl=30LL*86400000;
static void check(const arrow::Status& status) { if(!status.ok())throw std::runtime_error("Parquet operation failed"); }
template<class T> static T unwrap(arrow::Result<T> result) { if(!result.ok())throw std::runtime_error("Parquet operation failed"); return std::move(result).ValueUnsafe(); }
static std::string archive_key(const std::string& id,const Json& query) { return sha256(Json::array({id,query.at("range")}).dump()); }
static Json with_cache(Json value,const std::string& layer,int64_t age) { value["cache"]={{"hit",layer!="provider"},{"layer",layer},{"ageSeconds",std::max<int64_t>(0,age)}}; return value; }

HistoryCache::HistoryCache(std::filesystem::path directory,Clock clock,size_t capacity):directory_(std::move(directory)),clock_(std::move(clock)),capacity_(capacity){}
void HistoryCache::warn() { if(!warned_) {std::cerr<<"Parquet history cache unavailable or invalid; using memory and TraderMade.\n";warned_=true;} }
void HistoryCache::remember(const std::string& id,CacheEntry entry) {
  entry.access=++access_; entries_[id]=std::move(entry);
  for(auto it=entries_.begin();it!=entries_.end();) {if(it->second.expires_at<=clock_())it=entries_.erase(it);else ++it;}
  while(entries_.size()>capacity_) {auto oldest=std::min_element(entries_.begin(),entries_.end(),[](auto& a,auto& b){return a.second.access<b.second.access;});entries_.erase(oldest);}
}
std::filesystem::path HistoryCache::archive_path(const std::string& id,const Json& query)const {
  auto account=id.substr(0,id.find(':')); static const std::regex hash(R"(^[a-f0-9]{64}$)");
  auto symbol=query.at("symbol").get<std::string>(),timeframe=query.at("timeframe").get<std::string>();
  if(!std::regex_match(account,hash)||!valid_symbol(symbol)||!timeframes().contains(timeframe))throw std::runtime_error("Invalid archive identifier");
  auto& config=timeframes().at(timeframe);
  auto label=query.at("range").at("start_date").get<std::string>()+"_"+query.at("range").at("end_date").get<std::string>();
  label=std::regex_replace(label,std::regex("[^0-9_-]"),"-");
  return directory_/account/symbol/config.interval/std::to_string(config.period)/(label+"_"+archive_key(id,query).substr(0,16)+".parquet");
}
std::optional<Json> HistoryCache::get(const std::string& id,const Json& query) {
  std::lock_guard lock(mutex_); auto it=entries_.find(id);
  if(it!=entries_.end()&&it->second.expires_at>clock_()) {it->second.access=++access_;return with_cache(it->second.value,"memory",(clock_()-it->second.fetched_at)/1000);}
  entries_.erase(id);
  if(directory_.empty()||!archiveable(query,clock_()))return {};
  try {
    auto path=archive_path(id,query); if(!std::filesystem::exists(path))return {};
    if(std::filesystem::file_size(path)>32*1024*1024)throw std::runtime_error("Archive too large");
    auto input=unwrap(arrow::io::ReadableFile::Open(path.string()));
    auto reader=unwrap(parquet::arrow::OpenFile(input,arrow::default_memory_pool()));
    auto metadata=reader->parquet_reader()->metadata(); if(metadata->num_rows()>5000||metadata->num_rows()<1)throw std::runtime_error("Invalid archive size");
    auto kv=metadata->key_value_metadata(); if(!kv||!kv->Contains("tradermade.cache"))throw std::runtime_error("Missing archive metadata");
    auto stored=Json::parse(unwrap(kv->Get("tradermade.cache")));
    if(stored.at("version")!=1||stored.at("archiveKey")!=archive_key(id,query)||!stored.at("fetchedAt").is_number())throw std::runtime_error("Invalid archive metadata");
    std::shared_ptr<arrow::Table> table; check(reader->ReadTable(&table)); table=unwrap(table->CombineChunks());
    if(table->num_columns()!=5||table->num_rows()!=metadata->num_rows())throw std::runtime_error("Invalid archive schema");
    const std::vector<std::string> names={"time","open","high","low","close"};
    for(int i=0;i<5;++i)if(table->field(i)->name()!=names[i]||table->column(i)->null_count()!=0||table->field(i)->type()->id()!=(i==0?arrow::Type::INT64:arrow::Type::DOUBLE))throw std::runtime_error("Invalid archive column");
    Json candles=Json::array(); auto times=std::static_pointer_cast<arrow::Int64Array>(table->column(0)->chunk(0));
    for(int64_t row=0;row<table->num_rows();++row) {Json candle={{"time",times->Value(row)}};for(int c=1;c<5;++c)candle[names[c]]=std::static_pointer_cast<arrow::DoubleArray>(table->column(c)->chunk(0))->Value(row);candles.push_back(candle);}
    Json value={{"symbol",stored.at("symbol")},{"timeframe",stored.at("timeframe")},{"candles",candles},{"nextBefore",stored.at("nextBefore")},{"range",stored.at("range")},{"source","TraderMade"}};
    if(!valid_history(value)||value["symbol"]!=query["symbol"]||value["timeframe"]!=query["timeframe"]||value["range"]!=query["range"]||!archiveable(value,clock_()))throw std::runtime_error("Invalid archive data");
    auto fetched=stored.at("fetchedAt").get<int64_t>();remember(id,{value,fetched,clock_()+closed_ttl,0});
    return with_cache(value,"parquet",(clock_()-fetched)/1000);
  }catch(...){warn();return {};}
}
void HistoryCache::set(const std::string& id,const Json& value,int64_t ttl) {
  std::lock_guard lock(mutex_); if(!valid_history(value))return;
  const auto fetched=clock_(); remember(id,{value,fetched,fetched+ttl,0});
  if(directory_.empty()||value["candles"].empty()||!archiveable(value,fetched))return;
  std::filesystem::path temporary;
  try {
    auto path=archive_path(id,value);auto& candles=value["candles"];
    arrow::Int64Builder times; std::vector<std::shared_ptr<arrow::Array>> arrays;
    for(auto& row:candles)check(times.Append(row["time"].get<int64_t>()));
    arrays.push_back(unwrap(times.Finish()));
    for(auto name:{"open","high","low","close"}) {arrow::DoubleBuilder builder;for(auto& row:candles)check(builder.Append(row[name].get<double>()));arrays.push_back(unwrap(builder.Finish()));}
    auto schema=arrow::schema({arrow::field("time",arrow::int64(),false),arrow::field("open",arrow::float64(),false),arrow::field("high",arrow::float64(),false),arrow::field("low",arrow::float64(),false),arrow::field("close",arrow::float64(),false)});
    auto table=arrow::Table::Make(schema,arrays); Json stored={{"version",1},{"archiveKey",archive_key(id,value)},{"fetchedAt",fetched}};
    for(auto it=value.begin();it!=value.end();++it)if(it.key()!="candles")stored[it.key()]=it.value();
    auto kv=arrow::key_value_metadata({"tradermade.cache","time.unit"},{stored.dump(),"UTC Unix seconds"});
    unsigned char random[16]; if(RAND_bytes(random,sizeof(random))!=1)throw std::runtime_error("Random generator failed");
    temporary=path.string()+"."+sha256(std::string(reinterpret_cast<char*>(random),sizeof(random))).substr(0,16)+".tmp";
    std::filesystem::create_directories(path.parent_path());
    auto output=unwrap(arrow::io::FileOutputStream::Open(temporary.string()));
    auto properties=parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
    auto writer=unwrap(parquet::arrow::FileWriter::Open(*schema,arrow::default_memory_pool(),output,properties));
    check(writer->AddKeyValueMetadata(kv));check(writer->WriteTable(*table,1000));check(writer->Close());check(output->Close());
    if(std::filesystem::file_size(temporary)>32*1024*1024)throw std::runtime_error("Archive too large");
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Atomic archive replacement failed");
#else
    std::filesystem::rename(temporary,path);
#endif
  }catch(...){warn();}
  if(!temporary.empty()){std::error_code ec;std::filesystem::remove(temporary,ec);}
}

HistoryProvider::HistoryProvider(std::string key,HistoryCache& cache,Fetch fetch,Clock clock,std::function<bool(const std::string&)> continuous):key_(std::move(key)),account_(sha256(key_)),cache_(cache),fetch_(std::move(fetch)),clock_(std::move(clock)),continuous_(std::move(continuous)){}
Json HistoryProvider::get(const std::string& symbol,const std::string& timeframe,std::optional<int64_t> before,bool refresh) {
  if(key_.empty())throw std::runtime_error("REST API key is missing. Set TRADERMADE_REST_KEY in .env.");
  if(!valid_symbol(symbol)||!timeframes().contains(timeframe))throw std::runtime_error("Unsupported symbol or timeframe.");
  const auto id=account_+":"+symbol+":"+timeframe+":"+(before?std::to_string(*before):"latest");
  std::shared_future<Json> future;std::shared_ptr<std::promise<Json>> promise;
  {std::lock_guard lock(mutex_);if(pending_.contains(id))future=pending_[id];else{promise=std::make_shared<std::promise<Json>>();future=promise->get_future().share();pending_[id]=future;}}
  if(!promise)return future.get();
  try {
    auto range=history_range(timeframe,clock_(),before,continuous_&&continuous_(symbol));Json query={{"symbol",symbol},{"timeframe",timeframe},{"range",range}};
    auto cached=refresh?std::optional<Json>{}:cache_.get(id,query);
    if(cached)promise->set_value(*cached);
    else {
      ProviderResponse response;
      try{response=fetch_(symbol,range);}catch(...){throw std::runtime_error("TraderMade history is unreachable or timed out. Try reloading history.");}
      auto& data=response.data;
      if(response.status<200||response.status>=300||!data.contains("quotes")||!data["quotes"].is_array()) {
        auto detail=data.dump();std::transform(detail.begin(),detail.end(),detail.begin(),[](unsigned char c){return std::tolower(c);});
        if(detail.find("invalid")!=std::string::npos&&detail.find("key")!=std::string::npos)throw std::runtime_error("TraderMade reports that the REST API key is invalid. Update TRADERMADE_REST_KEY and restart the server.");
        if(response.status==429||detail.find("quota")!=std::string::npos||detail.find("limit")!=std::string::npos)throw std::runtime_error("TraderMade REST request limit reached. Wait before reloading history.");
        if(response.status==401||detail.find("plan")!=std::string::npos||detail.find("access")!=std::string::npos)throw std::runtime_error("TraderMade denied historical data. Check your REST key and plan.");
        throw std::runtime_error("TraderMade could not return history for this symbol and timeframe.");
      }
      auto normalized=normalize_candles(data["quotes"]);if(!data["quotes"].empty()&&normalized.empty())throw std::runtime_error("TraderMade returned an unsupported candle format.");
      Json candles=Json::array();for(auto& c:normalized)if(!before||c["time"].get<int64_t>()<*before)candles.push_back(c);
      auto next=timestamp(range["start_date"]);if(!candles.empty())next=std::min(next,candles[0]["time"].get<int64_t>());
      Json value={{"symbol",symbol},{"timeframe",timeframe},{"candles",candles},{"nextBefore",next},{"range",range},{"source","TraderMade"}};
      int64_t period=static_cast<int64_t>(timeframes().at(timeframe).seconds)*1000,current=clock_();
      int64_t ttl=archiveable(value,current)&&!candles.empty()?closed_ttl:std::min<int64_t>(60000,period-current%period);
      cache_.set(id,value,ttl);promise->set_value(with_cache(value,"provider",0));
    }
  }catch(...){promise->set_exception(std::current_exception());}
  {std::lock_guard lock(mutex_);pending_.erase(id);}return future.get();
}
static size_t collect_body(char* data,size_t size,size_t count,void* userdata) {
  auto& body=*static_cast<std::string*>(userdata);auto bytes=size*count;if(body.size()+bytes>8*1024*1024)return 0;body.append(data,bytes);return bytes;
}
ProviderResponse rest_request(const Config& config,const std::string& endpoint,const Json& params) {
    auto curl=std::unique_ptr<CURL,decltype(&curl_easy_cleanup)>(curl_easy_init(),curl_easy_cleanup);if(!curl)throw std::runtime_error("HTTP initialization failed");
    std::string url="https://marketdata.tradermade.com/api/v1/"+endpoint+"?api_key="+url_encode(config.rest_key);
    for(auto it=params.begin();it!=params.end();++it)url+="&"+it.key()+"="+url_encode(it.value().get<std::string>());
    std::string body;curl_easy_setopt(curl.get(),CURLOPT_URL,url.c_str());curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,collect_body);curl_easy_setopt(curl.get(),CURLOPT_WRITEDATA,&body);
    curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT,15L);curl_easy_setopt(curl.get(),CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYHOST,2L);
    if(!config.ca_file.empty())curl_easy_setopt(curl.get(),CURLOPT_CAINFO,config.ca_file.c_str());
    if(curl_easy_perform(curl.get())!=CURLE_OK)throw std::runtime_error("Provider unavailable");
    long status=0;curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);return ProviderResponse{status,Json::parse(body)};
}
Fetch make_rest_fetch(const Config& config) {
  return [config](const std::string& symbol,const Json& range) {
    auto params=range;params["currency"]=symbol;params["format"]="records";
    return rest_request(config,"timeseries",params);
  };
}
}
