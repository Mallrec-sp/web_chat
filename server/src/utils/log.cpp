#include "log.h"
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

Log::Log()
{
    m_writeThread = std::thread(&Log::WriteThreadFunc, this);
}

Log::~Log()
{
    m_bRunning.store(false);
    m_cv.notify_all();
    if (m_writeThread.joinable())
    {
        m_writeThread.join();
    }
    if (m_fileStream && m_fileStream->is_open())
    {
        m_fileStream->close();
    }
}

Log& Log::getInstance()
{
    static Log instance;
    return instance;
}

void Log::logBase(std::string msg, LogLevel level)
{
    if (level < m_curLevel) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    std::string prefix;
    prefix += "[" + currentTimeToString() + "]"
           + "[" + levelToString(level) + "]";

    std::string logMessage = std::string(prefix) + msg;

    if (m_bConsoleOutput) {
        switch (level)
        {
            case LogLevel::WARNING:
                std::cout << "\033[33m" << logMessage << "\033[0m" << std::endl;
                break;
            case LogLevel::ERROR:
                std::cout << "\033[31m" << logMessage << "\033[0m" << std::endl;
                break;
            default:
                std::cout << logMessage << std::endl;
                break;
        }
    }

    m_queue.push(logMessage);
    m_cv.notify_one();
}

void Log::setLogLevel(LogLevel level)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_curLevel = level;
}

bool Log::setOutputFile(const std::string &filepath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto newStream = std::make_unique<std::ofstream>(filepath, std::ios::app);
    if (newStream->is_open())
    {
        m_fileStream = std::move(newStream);
        return true;
    }
    return false;
}

void Log::setConsoleOutput(bool enable)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bConsoleOutput = enable;
}

void Log::WriteThreadFunc() {
    while (m_bRunning.load()) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_queue.empty() || !m_bRunning; });

        std::queue<std::string> local_queue;
        local_queue.swap(m_queue);
        lock.unlock();

        while (!local_queue.empty()) {
            if (m_fileStream && m_fileStream->is_open())
            {
                (*m_fileStream) << local_queue.front() << std::endl;
                m_fileStream->flush();
            }
            local_queue.pop();
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_queue.empty()) {
        if (m_fileStream && m_fileStream->is_open())
        {
            (*m_fileStream) << m_queue.front() << std::endl;
            m_queue.pop();
        }
    }
    if (m_fileStream && m_fileStream->is_open())
    {
        m_fileStream->flush();
    }
}

std::string Log::currentTimeToString() const {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &in_time_t);
#else
    localtime_r(&in_time_t, &tm_buf);
#endif

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

std::string Log::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR";
        default:                return "UNKNOWN";
    }
}

void Log::logDebug(std::string str)
{
    logBase(str, LogLevel::DEBUG);
}
void Log::logInfo(std::string str)
{
    logBase(str, LogLevel::INFO);
}
void Log::logWarning(std::string str)
{
    logBase(str, LogLevel::WARNING);
}
void Log::logError(std::string str)
{
    logBase(str, LogLevel::ERROR);
}
