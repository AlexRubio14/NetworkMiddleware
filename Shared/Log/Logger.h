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
         static std::string GetLevelColor(LogLevel level);
         static std::string GetLevelIcon(LogLevel level);
         static std::string FormatPacketHex(const std::vector<uint8_t>& data);
        static std::string FormatPacketBinary(const std::vector<uint8_t>& data);
        static std::string FormatPacketHexBinary(const std::vector<uint8_t>& data);
         static void EnableAnsiOnWindows();

         inline static std::queue<LogEntry> m_logQueue;
         inline static std::mutex m_mutex;
         inline static std::condition_variable m_cv;
         inline static std::jthread m_logThread;
         inline static bool m_running = false;

     public:
         static void Start();
         static void Stop();
         static void Sync();   // Block until all queued entries are flushed to stdout

         static void Log(LogLevel level, LogChannel channel, const std::string& msg);
         static void LogPacket(LogChannel channel, std::shared_ptr<std::vector<uint8_t>> data,
                              DumpMode mode = DumpMode::Hex);

         // Public formatting — usable in tests and tools without going through the queue.
         static std::string FormatPacket(const std::vector<uint8_t>& data,
                                         DumpMode mode = DumpMode::Hex);

         // Visual helpers — write directly to stdout (no queue)
         static void Banner(const std::string& title);
         static void Separator(const std::string& label = "");
     };
 }
