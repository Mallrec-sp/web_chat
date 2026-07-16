#pragma once
#include <string>

struct ServerConfig{
  std::string host_ip_ = "0.0.0.0"; //监听ip
  unsigned int port_ = 8080;   //端口
};

struct DatabaseConfig{
  std::string database_path_ = "../data/netchat.db";
};

struct SslConfig{
  bool enabled_ = false;
  std::string cert_path_ = "../cert/cert.pem";
  std::string key_path_ = "../cert/key.pem";
};

class Config{
  public:
    static Config load(const char* json_path);

    ServerConfig server_config_;
    DatabaseConfig database_config_;
    SslConfig ssl_config_;
    int workers_ = 0;
};