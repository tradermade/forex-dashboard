#include "terminal.hpp"
#include <cstdlib>
#include <fstream>

namespace terminal {
static std::string trim(std::string s) { auto first=s.find_first_not_of(" \r\n\t"); if(first==std::string::npos)return {}; return s.substr(first,s.find_last_not_of(" \r\n\t")-first+1); }
Config load_config(int argc,char** argv) {
  Config config; config.root=std::filesystem::weakly_canonical(TM_PROJECT_ROOT);
  for(int i=1;i<argc;++i) {
    if(std::string(argv[i])=="--root"&&i+1<argc) config.root=std::filesystem::weakly_canonical(argv[++i]);
    else throw std::runtime_error("Usage: tradermade_backend [--root project-directory]");
  }
  std::map<std::string,std::string> env;
  std::ifstream file(config.root/".env"); std::string line;
  while(std::getline(file,line)) {
    if(line.starts_with("\xef\xbb\xbf")) line.erase(0,3);
    auto eq=line.find('='); if(eq==std::string::npos||trim(line).starts_with('#'))continue;
    auto value=trim(line.substr(eq+1)); if(value.size()>=2&&((value.front()=='"'&&value.back()=='"')||(value.front()=='\''&&value.back()=='\'')))value=value.substr(1,value.size()-2);
    env[trim(line.substr(0,eq))]=value;
  }
  auto get=[&](const std::string& key,std::string fallback="") { const char* value=std::getenv(key.c_str()); return value?std::string(value):env.contains(key)?env[key]:fallback; };
  config.rest_key=get("TRADERMADE_REST_KEY"); config.stream_key=get("TRADERMADE_WS_KEY");
  config.ca_file=get("SSL_CERT_FILE");
  if(config.ca_file.empty()&&std::filesystem::exists("C:/msys64/ucrt64/etc/ssl/certs/ca-bundle.crt")) config.ca_file="C:/msys64/ucrt64/etc/ssl/certs/ca-bundle.crt";
  auto port=get("PORT","3001"); size_t used=0; auto parsed=std::stoul(port,&used); if(used!=port.size()||parsed<1||parsed>65535)throw std::runtime_error("PORT must be between 1 and 65535");
  config.port=static_cast<unsigned short>(parsed);
  config.static_root=config.root/"frontend"/"dist"; config.cache_directory=config.root/".cache"/"parquet";
  return config;
}
}
