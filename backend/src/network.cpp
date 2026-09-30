#include "terminal.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/ssl.h>
#include <deque>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace terminal {
namespace net=boost::asio; namespace beast=boost::beast; namespace http=beast::http; namespace ws=beast::websocket;
using tcp=net::ip::tcp; using net::awaitable; using net::use_awaitable; using namespace std::chrono_literals;
class Gateway; class Browser; class Upstream;
static Json status(std::string state,std::string message,std::string symbol="") {
  Json data={{"type","status"},{"state",state},{"message",message}};if(!symbol.empty())data["symbol"]=symbol;return data;
}
class Gateway : public std::enable_shared_from_this<Gateway> {
public:
  net::io_context& io; net::thread_pool& workers; net::ssl::context tls{net::ssl::context::tls_client};
  Config config; HistoryProvider& history; InstrumentCatalog& catalog; net::steady_timer retry;
  struct Subscriber {std::shared_ptr<Browser> browser;std::string symbol;};
  std::map<Browser*,Subscriber> clients;std::shared_ptr<Upstream> upstream;
  std::set<std::string> requested,accepted;std::map<std::string,Json> quotes;
  Json state=status("connecting","Connecting to TraderMade");
  bool authenticated=false,terminal=false,stopped=false,retrying=false;int delay=1;
  int64_t rate_start=now_ms();int requests=0;
  Gateway(net::io_context& i,net::thread_pool& w,Config c,HistoryProvider& h,InstrumentCatalog& instruments):io(i),workers(w),config(std::move(c)),history(h),catalog(instruments),retry(i) {
    tls.set_default_verify_paths();tls.set_verify_mode(net::ssl::verify_peer);
    if(!config.ca_file.empty())tls.load_verify_file(config.ca_file);
#ifdef _WIN32
    auto store=CertOpenSystemStoreA(0,"ROOT");
    if(store){PCCERT_CONTEXT certificate=nullptr;while((certificate=CertEnumCertificatesInStore(store,certificate))!=nullptr){const unsigned char* bytes=certificate->pbCertEncoded;auto x509=d2i_X509(nullptr,&bytes,certificate->cbCertEncoded);if(x509){X509_STORE_add_cert(SSL_CTX_get_cert_store(tls.native_handle()),x509);X509_free(x509);}}CertCloseStore(store,0);}
#endif
  }
  void add(std::shared_ptr<Browser> browser);
  void remove(Browser* browser);
  void subscribe(Browser* browser,const std::string& symbol);
  void broadcast(Json message);
  void publish(const std::string& symbol,const Json& message);
  void connect();void sync();void on_message(const Json& message);void failed(Upstream* source);void stop();
};
class Browser : public std::enable_shared_from_this<Browser> {
  ws::stream<beast::tcp_stream> socket_;std::weak_ptr<Gateway> gateway_;std::deque<std::string> output_;bool writing_=false,closed_=false;
public:
  Browser(beast::tcp_stream stream,std::shared_ptr<Gateway> gateway):socket_(std::move(stream)),gateway_(gateway){}
  awaitable<void> run(http::request<http::string_body> request) {
    try {
      socket_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));socket_.read_message_max(2048);
      co_await socket_.async_accept(request,use_awaitable);
      if(auto gateway=gateway_.lock())gateway->add(shared_from_this());
      beast::flat_buffer buffer;
      while(!closed_) {
        co_await socket_.async_read(buffer,use_awaitable);
        try {
          auto data=Json::parse(beast::buffers_to_string(buffer.data()));
          if(!data.is_object()||data.value("type",std::string())!="subscribe"||!data.contains("symbol")||!data["symbol"].is_string()||!valid_symbol(data["symbol"]))send(status("error","Invalid symbol format"));
          else if(auto gateway=gateway_.lock())gateway->subscribe(this,data["symbol"]);
        }catch(...){send(status("error","Invalid subscription request"));}
        buffer.consume(buffer.size());
      }
    }catch(...){}close();co_return;
  }
  void send(const Json& message) {
    if(closed_)return;
    if(output_.size()>=256){close();return;}
    output_.push_back(message.dump());
    if(!writing_){writing_=true;net::co_spawn(socket_.get_executor(),[self=shared_from_this()]()->awaitable<void>{co_await self->write();},net::detached);}
  }
  awaitable<void> write() {
    try{while(!output_.empty()&&!closed_){socket_.text(true);co_await socket_.async_write(net::buffer(output_.front()),use_awaitable);output_.pop_front();}}catch(...){close();}writing_=false;
  }
  void close() {
    if(closed_)return;
    closed_=true;beast::error_code ec;beast::get_lowest_layer(socket_).socket().close(ec);
    if(auto gateway=gateway_.lock())gateway->remove(this);
  }
};
class Upstream : public std::enable_shared_from_this<Upstream> {
  std::weak_ptr<Gateway> gateway_;tcp::resolver resolver_;net::steady_timer login_timer_;
  ws::stream<beast::ssl_stream<beast::tcp_stream>> socket_;std::deque<std::string> output_;bool writing_=false,closed_=false;
public:
  explicit Upstream(std::shared_ptr<Gateway> gateway):gateway_(gateway),resolver_(gateway->io),login_timer_(gateway->io),socket_(gateway->io,gateway->tls){}
  awaitable<void> run() {
    auto gateway=gateway_.lock();if(!gateway)co_return;
    login_timer_.expires_after(15s);login_timer_.async_wait([self=shared_from_this()](beast::error_code ec){if(!ec)self->close();});
    try {
      auto& config=gateway->config;
      socket_.next_layer().set_verify_callback(net::ssl::host_name_verification(config.stream_host));
      if(!SSL_set_tlsext_host_name(socket_.next_layer().native_handle(),config.stream_host.c_str()))throw std::runtime_error("TLS setup failed");
      auto addresses=co_await resolver_.async_resolve(config.stream_host,config.stream_port,use_awaitable);
      beast::get_lowest_layer(socket_).expires_after(12s);
      co_await beast::get_lowest_layer(socket_).async_connect(addresses,use_awaitable);
      co_await socket_.next_layer().async_handshake(net::ssl::stream_base::client,use_awaitable);
      beast::get_lowest_layer(socket_).expires_never();
      socket_.set_option(ws::stream_base::timeout{10s,60s,true});socket_.read_message_max(1024*1024);
      co_await socket_.async_handshake(config.stream_host,config.stream_path,use_awaitable);
      send({{"action","login"},{"key",config.stream_key},{"fmt","JSON"}});
      beast::flat_buffer buffer;
      while(!closed_) {
        co_await socket_.async_read(buffer,use_awaitable);
        try {auto data=Json::parse(beast::buffers_to_string(buffer.data()));if(data.value("type",std::string())=="login_ok")login_timer_.cancel();gateway->on_message(data);}catch(...){}
        buffer.consume(buffer.size());
      }
    }catch(...){}close();co_return;
  }
  void send(const Json& message) {
    if(closed_)return;
    if(output_.size()>256){close();return;}output_.push_back(message.dump());
    if(!writing_){writing_=true;net::co_spawn(socket_.get_executor(),[self=shared_from_this()]()->awaitable<void>{co_await self->write();},net::detached);}
  }
  awaitable<void> write() {
    try{while(!output_.empty()&&!closed_){socket_.text(true);co_await socket_.async_write(net::buffer(output_.front()),use_awaitable);output_.pop_front();}}catch(...){close();}writing_=false;
  }
  void close() {
    if(closed_)return;
    closed_=true;resolver_.cancel();login_timer_.cancel();beast::error_code ec;beast::get_lowest_layer(socket_).socket().close(ec);
    if(auto gateway=gateway_.lock())gateway->failed(this);
  }
};
void Gateway::add(std::shared_ptr<Browser> browser){clients[browser.get()]={browser,""};browser->send(state);}
void Gateway::remove(Browser* browser) {
  clients.erase(browser);
  if(clients.empty()){retry.cancel();retrying=false;if(upstream){auto connection=upstream;connection->close();}}else sync();
}
void Gateway::subscribe(Browser* browser,const std::string& symbol) {
  auto it=clients.find(browser);if(it==clients.end())return;it->second.symbol=symbol;
  if(terminal){browser->send(state);return;}
  browser->send(status(authenticated&&accepted.contains(symbol)?"waiting":"connecting",authenticated&&accepted.contains(symbol)?"Connected - awaiting ticks":"Subscribing",symbol));
  if(authenticated&&accepted.contains(symbol)&&quotes.contains(symbol)){auto q=quotes[symbol];q["cached"]=true;browser->send({{"type","quote"},{"quote",q}});}
  if(!upstream&&!retrying)connect();else sync();
}
void Gateway::broadcast(Json message){state=std::move(message);auto snapshot=clients;for(auto& [key,client]:snapshot)client.browser->send(state);}
void Gateway::publish(const std::string& symbol,const Json& message){auto snapshot=clients;for(auto& [key,client]:snapshot)if(client.symbol==symbol)client.browser->send(message);}
void Gateway::connect() {
  if(stopped||terminal||clients.empty())return;
  if(config.stream_key.empty()){terminal=true;broadcast(status("error","Streaming API key is missing"));return;}
  broadcast(status("connecting","Connecting to TraderMade"));upstream=std::make_shared<Upstream>(shared_from_this());
  net::co_spawn(io,[connection=upstream]()->awaitable<void>{co_await connection->run();},net::detached);
}
void Gateway::sync() {
  if(!authenticated||!upstream)return;
  std::set<std::string> wanted;for(auto& [key,client]:clients)if(!client.symbol.empty())wanted.insert(client.symbol);
  Json remove=Json::array(),add=Json::array();
  for(auto& symbol:requested)if(!wanted.contains(symbol))remove.push_back(symbol+":QUOTE");
  for(auto& symbol:wanted)if(!requested.contains(symbol))add.push_back(symbol+":QUOTE");
  if(!remove.empty())upstream->send({{"action","unsubscribe"},{"symbols",remove}});
  for(auto& s:remove){auto raw=s.get<std::string>();auto symbol=raw.substr(0,raw.find(':'));requested.erase(symbol);accepted.erase(symbol);}
  if(!add.empty())upstream->send({{"action","subscribe"},{"symbols",add},{"send_last",true}});
  requested=std::move(wanted);
}
void Gateway::on_message(const Json& data) {
  auto type=data.value("type",std::string());
  if(type=="login_ok"){authenticated=true;delay=1;broadcast(status("waiting","Connected - awaiting subscription"));sync();}
  else if(type=="login_reject"||type=="logout") {
    terminal=true;broadcast(status("error",type=="logout"?"Stream logged out. Close other sessions and restart the server.":"TraderMade rejected the streaming key. Check streaming access."));auto connection=upstream;if(connection)connection->close();
  }else if(type=="sub_ack") {
    for(auto& raw:data.value("accepted",Json::array())) {auto symbol=raw.get<std::string>();symbol=symbol.substr(0,symbol.find(':'));accepted.insert(symbol);publish(symbol,status("waiting","Connected - awaiting ticks",symbol));}
    for(auto group:{"denied","invalid"})for(auto& raw:data.value(group,Json::array())){
      auto requested_symbol=raw.get<std::string>();auto symbol=requested_symbol.substr(0,requested_symbol.find(':'));
      publish(symbol,status("error",subscription_error(data,requested_symbol,std::string(group)=="invalid"),symbol));
    }
  }else if(type=="error")broadcast(status("error","TraderMade reported a streaming error. Check your subscription."));
  else if(auto quote=normalize_quote(data)){auto symbol=quote->at("symbol").get<std::string>();quotes[symbol]=*quote;publish(symbol,{{"type","quote"},{"quote",*quote}});}
}
void Gateway::failed(Upstream* source) {
  if(upstream.get()!=source)return;
  upstream.reset();authenticated=false;requested.clear();accepted.clear();
  if(stopped||terminal||clients.empty())return;
  broadcast(status("offline","Stream disconnected - retrying"));retrying=true;retry.expires_after(std::chrono::seconds(delay));delay=std::min(delay*2,30);
  retry.async_wait([self=shared_from_this()](beast::error_code ec){self->retrying=false;if(!ec)self->connect();});
}
void Gateway::stop(){stopped=true;retry.cancel();if(upstream){auto connection=upstream;connection->close();}auto snapshot=clients;for(auto& [key,client]:snapshot)client.browser->close();}

static bool allowed_host(const std::string& host){static const std::regex pattern(R"(^(localhost|127\.0\.0\.1)(:\d+)?$)",std::regex::icase);return std::regex_match(host,pattern);}
static bool allowed_origin(const std::string& origin){static const std::regex pattern(R"(^https?://(localhost|127\.0\.0\.1)(:\d+)?$)",std::regex::icase);return std::regex_match(origin,pattern);}
using Response=http::response<http::string_body>;
static Response json_response(http::status code,const Json& data){Response response{code,11};response.set(http::field::content_type,"application/json");response.set(http::field::cache_control,"no-store");response.set("X-Content-Type-Options","nosniff");response.keep_alive(false);response.body()=data.dump();response.prepare_payload();return response;}
class HttpSession : public std::enable_shared_from_this<HttpSession> {
  beast::tcp_stream stream_;beast::flat_buffer buffer_;std::shared_ptr<Gateway> gateway_;
public:
  HttpSession(tcp::socket socket,std::shared_ptr<Gateway> gateway):stream_(std::move(socket)),gateway_(std::move(gateway)){}
  awaitable<void> send(Response response) {
    try{stream_.expires_after(20s);co_await http::async_write(stream_,response,use_awaitable);}catch(...){}
    beast::error_code ec;stream_.socket().shutdown(tcp::socket::shutdown_send,ec);
  }
  awaitable<void> run() {
    try {
      stream_.expires_after(15s);http::request_parser<http::string_body> parser;parser.body_limit(2048);parser.header_limit(16384);
      co_await http::async_read(stream_,buffer_,parser,use_awaitable);auto request=parser.release();
      auto host=std::string(request[http::field::host]),origin=std::string(request[http::field::origin]);
      if(!allowed_host(host)||(!origin.empty()&&!allowed_origin(origin))) {co_await send(json_response(http::status::forbidden,{{"error","Local access only."}}));co_return;}
      std::string target(request.target());auto q=target.find('?');auto path=target.substr(0,q);
      if(ws::is_upgrade(request)&&path=="/ws") {
        if(gateway_->clients.size()>=100){co_await send(json_response(http::status::service_unavailable,{{"error","Too many connections."}}));co_return;}
        stream_.expires_never();auto browser=std::make_shared<Browser>(std::move(stream_),gateway_);co_await browser->run(std::move(request));co_return;
      }
      if(request.method()!=http::verb::get){co_await send(json_response(http::status::method_not_allowed,{{"error","Method not allowed."}}));co_return;}
      if(path=="/api/health"){co_await send(json_response(http::status::ok,{{"status","ok"},{"backend","cpp"},{"restConfigured",!gateway_->config.rest_key.empty()},{"streamConfigured",!gateway_->config.stream_key.empty()}}));co_return;}
      if(path=="/api/instruments") {
        net::post(gateway_->workers,[self=shared_from_this()] {
          Response response;
          try{response=json_response(http::status::ok,self->gateway_->catalog.get());}
          catch(...){response=json_response(http::status::bad_gateway,{{"error","Instrument catalogue is unavailable. Try again shortly."}});}
          net::post(self->gateway_->io,[self,response=std::move(response)]()mutable{net::co_spawn(self->gateway_->io,[self,response=std::move(response)]()mutable->awaitable<void>{co_await self->send(std::move(response));},net::detached);});
        });co_return;
      }
      if(path=="/api/history") {
        if(now_ms()-gateway_->rate_start>60000){gateway_->rate_start=now_ms();gateway_->requests=0;}
        if(++gateway_->requests>60){co_await send(json_response(http::status::too_many_requests,{{"error","Too many requests. Please wait a minute."}}));co_return;}
        std::map<std::string,std::string> params;bool invalid=false;
        try{if(q!=std::string::npos){std::istringstream query(target.substr(q+1));std::string part;while(std::getline(query,part,'&')){auto eq=part.find('=');if(eq!=std::string::npos)params[url_decode(part.substr(0,eq))]=url_decode(part.substr(eq+1));}}}catch(...){invalid=true;}
        auto symbol=params["symbol"],timeframe=params["timeframe"];std::optional<int64_t> before;
        if(params.contains("before")){try{size_t used;auto n=std::stoll(params["before"],&used);if(used!=params["before"].size()||n<=86400||n>now_ms()/1000)invalid=true;else before=n;}catch(...){invalid=true;}}
        if(invalid||!valid_symbol(symbol)||!timeframes().contains(timeframe)){co_await send(json_response(http::status::bad_request,{{"error","Unsupported symbol, timeframe, or history cursor."}}));co_return;}
        bool refresh=params["refresh"]=="1";
        net::post(gateway_->workers,[self=shared_from_this(),symbol,timeframe,before,refresh] {
          Response response;
          try{response=json_response(http::status::ok,self->gateway_->history.get(symbol,timeframe,before,refresh));}
          catch(const std::exception& e){response=json_response(http::status::bad_gateway,{{"error",e.what()}});}
          net::post(self->gateway_->io,[self,response=std::move(response)]()mutable{net::co_spawn(self->gateway_->io,[self,response=std::move(response)]()mutable->awaitable<void>{co_await self->send(std::move(response));},net::detached);});
        });co_return;
      }
      if(path.starts_with("/api/")){co_await send(json_response(http::status::not_found,{{"error","Endpoint not found."}}));co_return;}
      Response response;
      try {
        path=url_decode(path);if(path=="/")path="/index.html";
        if(path.find('\0')!=std::string::npos||path.find('\\')!=std::string::npos||path.find(':')!=std::string::npos)throw std::runtime_error("Invalid path");
        auto root=std::filesystem::weakly_canonical(gateway_->config.static_root);auto file=std::filesystem::weakly_canonical(root/path.substr(1));
        auto relative=file.lexically_relative(root);if(relative.empty()||*relative.begin()==".."||!std::filesystem::is_regular_file(file)||std::filesystem::file_size(file)>16*1024*1024)throw std::runtime_error("Not found");
        std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("Not found");
        std::map<std::string,std::string> types={{".html","text/html"},{".js","text/javascript"},{".css","text/css"},{".svg","image/svg+xml"},{".txt","text/plain"},{".png","image/png"}};
        auto extension=file.extension().string();response=Response{http::status::ok,11};response.set(http::field::content_type,types.contains(extension)?types[extension]:"application/octet-stream");response.set("X-Content-Type-Options","nosniff");response.keep_alive(false);response.body()=std::string(std::istreambuf_iterator<char>(input),{});response.prepare_payload();
      }catch(...){response=json_response(http::status::not_found,{{"error","Page not found. Build the frontend or use Vite."}});}
      co_await send(std::move(response));
    }catch(...){}co_return;
  }
};
static awaitable<void> listen(std::shared_ptr<Gateway> gateway,std::shared_ptr<tcp::acceptor> acceptor) {
  try{while(!gateway->stopped){auto socket=co_await acceptor->async_accept(use_awaitable);auto session=std::make_shared<HttpSession>(std::move(socket),gateway);net::co_spawn(gateway->io,[session]()->awaitable<void>{co_await session->run();},net::detached);}}catch(...){}co_return;
}
void run_gateway(const Config& config,HistoryProvider& history,InstrumentCatalog& catalog) {
  net::io_context io;net::thread_pool workers(4);auto gateway=std::make_shared<Gateway>(io,workers,config,history,catalog);
  auto acceptor=std::make_shared<tcp::acceptor>(io,tcp::endpoint(net::ip::make_address("127.0.0.1"),config.port));
  net::signal_set signals(io,SIGINT,SIGTERM);signals.async_wait([&](beast::error_code,int){gateway->stop();acceptor->close();io.stop();});
  net::co_spawn(io,listen(gateway,acceptor),net::detached);
  std::cout<<"TraderMade C++ gateway: http://127.0.0.1:"<<config.port<<std::endl;
  io.run();workers.join();
}
}
