#pragma once

#include <mutex>
#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <memory>
#include <queue>
#include <condition_variable>
#include <atomic>

enum class LogLevel
{
    DEBUG,
    INFO,
    WARNING,
    ERROR
};

class Log
{
public:
    static Log& getInstance();
    void logBase(std::string str, LogLevel level);
    ~Log();
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void setLogLevel(LogLevel level);
    bool setOutputFile(const std::string& filepath);
    void setConsoleOutput(bool enable);

    void logDebug(std::string str);
    void logInfo(std::string str);
    void logWarning(std::string str);
    void logError(std::string str);

private:
    Log();
    std::string currentTimeToString() const;
    std::string levelToString(LogLevel level) const;
    void WriteThreadFunc();

private:
    LogLevel m_curLevel = LogLevel::DEBUG;
    std::unique_ptr<std::ofstream> m_fileStream;
    bool m_bConsoleOutput = true;
    std::mutex m_mutex;
    std::string logFilePath;
    std::queue<std::string> m_queue;
    std::condition_variable m_cv;
    std::atomic<bool> m_bRunning{true};
    std::thread m_writeThread;
};

#define LOG_DEBUG(msg) Log::getInstance().logDebug(std::string("[")+__FILE__+":"+\
                std::to_string(__LINE__)+"]"+msg)
#define LOG_INFO(msg) Log::getInstance().logInfo(std::string("[")+__FILE__+":"+\
                std::to_string(__LINE__)+"]"+msg)
#define LOG_WARNING(msg) Log::getInstance().logWarning(std::string("[")+__FILE__+":"+\
                std::to_string(__LINE__)+"]"+msg)
#define LOG_ERROR(msg) Log::getInstance().logError(std::string("[")+__FILE__+":"+\
                std::to_string(__LINE__)+"]"+msg)
