#pragma once
#include <string>
#include "db/database.h"

struct ServerConfig {
  std::string host_ip_ = "0.0.0.0";
  unsigned int port_ = 8081;
};

using DatabaseConfig = DatabaseOptions;

struct SslConfig {
  bool enabled_ = false;
  std::string cert_path_ = "../cert/cert.pem";
  std::string key_path_ = "../cert/key.pem";
};

class Config {
public:
  static Config load(const char* json_path);

  ServerConfig server_config_;
  DatabaseConfig database_config_;
  SslConfig ssl_config_;
  int workers_ = 0;
};
