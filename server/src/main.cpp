#include "utils/logger.h"
#include "utils/config.h"

int main(){
    LOG_INFO("server start");
    Config config = Config::load("../config.json");
    
}