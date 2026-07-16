#include "config.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include "utils/logger.h"

Config Config::load(const char* json_path){
  Config config;
  std::ifstream file(json_path);
  if(!file.is_open()){
    LOG_WARNING("fail to open config json,use default config");
    return config;
  }

  nlohmann::json json;
  file >> json;
  if(json.contains("server")){
    auto& json_obj = json["server"];
    if(json_obj.contains("host")){
      config.server_config_.host_ip_ = json_obj["host"];
    }
    if(json_obj.contains("port")){
      config.server_config_.port_ = json_obj["port"];
    }
  }
  config.workers_ = json.value("workers", config.workers_);
  if (json.contains("database") && json["database"].contains("path"))
    config.database_config_.database_path_ = json["database"]["path"];

  if (json.contains("ssl")) {
    auto& ssl = json["ssl"];
    config.ssl_config_.enabled_ = ssl.value("enabled", false);
    config.ssl_config_.cert_path_ = ssl.value("cert_path", config.ssl_config_.cert_path_);
    config.ssl_config_.key_path_ = ssl.value("key_path", config.ssl_config_.key_path_);
  }

  return config;
}