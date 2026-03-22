#include "Logger.h"

#include <bitset>
#include <iostream>
#include <utility>

namespace NetworkMiddleware::Shared {

     void Logger::Start()
     {
         if (m_running)
             return;

         m_running = true;
         m_logThread = std::jthread(&ProcessLogs);
     }

     void Logger::Stop()
     {
         {
             std::lock_guard lock(m_mutex);
             m_running = false;
         }
         m_cv.notify_one();
     }

     void Logger::Log(LogLevel level, LogChannel channel, const std::string &msg) \
     {
         {
             std::lock_guard lock(m_mutex);
             m_logQueue.push({level,channel,msg,nullptr,std::chrono::system_clock::now()});
             m_cv.notify_one();
         }
     }

     void Logger::LogPacket(LogChannel channel, std::shared_ptr<std::vector<uint8_t>> data)
     {
         {
             std::lock_guard lock(m_mutex);
             m_logQueue.push({
             LogLevel::Packet, channel, "Data of packet received", std::move(data), std::chrono::system_clock::now()});
         }
         m_cv.notify_one();
     }

     void Logger::ProcessLogs()
     {
         while (true)
         {
             LogEntry entry;
             {
                 std::unique_lock lock(m_mutex);
                 m_cv.wait(lock, [] { return !m_logQueue.empty() || !m_running; });

                 if (!m_running && m_logQueue.empty())
                     break;

                 entry = std::move(m_logQueue.front());
                 m_logQueue.pop();
             }
             std::time_t time_now = std::chrono::system_clock::to_time_t(entry.timestamp);
             char time_str[10];
             std::strftime(time_str, sizeof(time_str), "%H:%M:%S", std::localtime(&time_now));

             std::cout << "[" << time_str << "]"
                      << "[" << GetChannelString(entry.channel) << "]"
                      << "[" << GetLevelString(entry.level) << "] "
                      << entry.message << std::endl;

             if (entry.level == LogLevel::Packet && entry.rawData)
                 std::cout << FormatPacket(*entry.rawData) << std::endl;
         }
     }

    std::string Logger::GetLevelString(LogLevel level)
    {
         switch (level)
         {
             case LogLevel::Info:    return "INFO";
             case LogLevel::Debug:   return "DEBUG";
             case LogLevel::Warning: return "WARNING";
             case LogLevel::Error:   return "ERROR";
             case LogLevel::Packet:  return "PACKET";
             default:                return "UNKNOWN";
         }


    }

    std::string Logger::GetChannelString(LogChannel channel)
    {
         switch (channel)
         {
             case LogChannel::Core:      return "CORE";
             case LogChannel::Transport: return "TRANSPORT";
             case LogChannel::Brain:     return "BRAIN";
             case LogChannel::General:   return "GENERAL";
             default:                    return "UNK";
         }
    }

    std::string Logger::FormatPacket(const std::vector<uint8_t> &data)
    {
         std::stringstream ss;
         ss << "   Hex: ";
         for (auto byte : data)
             ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte) << " ";

         ss << "\n   Bin: ";
         for (auto byte : data)
             ss << std::bitset<8>(byte) << " | ";

         ss << "\n   Total Size: " << std::dec << data.size() << " bytes";
         return ss.str();
     }
}
