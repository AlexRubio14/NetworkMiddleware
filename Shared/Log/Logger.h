#pragma once
#include "LogEntry.h"
#include <queue>
#include <mutex>

 namespace NetworkMiddleware::Shared {
     class Logger {
     private:
         static void ProcessLogs();
         static std::string GetLevelString(LogLevel level);
         static std::string GetChannelString(LogChannel channel);
         static std::string FormatPacket(const std::vector<uint8_t>& data);

         inline static std::queue<LogEntry> m_logQueue;
         inline static std::mutex m_mutex;
         inline static std::condition_variable m_cv;
         inline static std::jthread m_logThread;
         inline static bool m_running = false;

     public:
         static void Start();
         static void Stop();

         static void Log(LogLevel level, LogChannel channel, const std::string& msg);
         static void LogPacket(LogChannel channel, std::shared_ptr<std::vector<uint8_t>> data);
     };
 }