#include "Logger.h"

#include <bitset>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace NetworkMiddleware::Shared {

    // ─── ANSI colour + style codes ────────────────────────────────────────────

    namespace Ansi {
        static constexpr const char* Reset   = "\033[0m";
        static constexpr const char* Bold    = "\033[1m";
        static constexpr const char* Dim     = "\033[2m";

        // Foreground colours
        static constexpr const char* White   = "\033[97m";
        static constexpr const char* Cyan    = "\033[96m";
        static constexpr const char* Green   = "\033[92m";
        static constexpr const char* Yellow  = "\033[93m";
        static constexpr const char* Red     = "\033[91m";
        static constexpr const char* Magenta = "\033[95m";
        static constexpr const char* Blue    = "\033[94m";
        static constexpr const char* Gray    = "\033[90m";
    }

    // ─── Lifecycle ────────────────────────────────────────────────────────────

    void Logger::EnableAnsiOnWindows() {
#ifdef _WIN32
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            GetConsoleMode(hOut, &mode);
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
        SetConsoleOutputCP(CP_UTF8);
#endif
    }

    void Logger::Start() {
        if (m_running)
            return;

        EnableAnsiOnWindows();
        m_running   = true;
        m_logThread = std::jthread(&ProcessLogs);
    }

    void Logger::Stop() {
        {
            std::lock_guard lock(m_mutex);
            m_running = false;
        }
        m_cv.notify_one();
    }

    void Logger::Sync() {
        // Push a SYNC sentinel. The consumer thread fires syncCallback when it
        // processes the sentinel, which unblocks the waiting caller.
        // wait_for(5s) guards against deadlock if the logger thread has stopped.
        std::mutex              doneMutex;
        std::condition_variable doneCv;
        bool                    done = false;

        {
            std::lock_guard lock(m_mutex);
            LogEntry sentinel{};
            sentinel.level     = LogLevel::Debug;
            sentinel.channel   = LogChannel::General;
            sentinel.message   = "\x01SYNC";
            sentinel.timestamp = std::chrono::system_clock::now();
            sentinel.syncCallback = [&doneMutex, &doneCv, &done]() {
                std::lock_guard lg(doneMutex);
                done = true;
                doneCv.notify_one();
            };
            m_logQueue.push(std::move(sentinel));
        }
        m_cv.notify_one();

        std::unique_lock doneLock(doneMutex);
        doneCv.wait_for(doneLock, std::chrono::seconds(5), [&done] { return done; });
        std::cout.flush();
    }

    // ─── Enqueue ──────────────────────────────────────────────────────────────

    void Logger::Log(LogLevel level, LogChannel channel, const std::string& msg) {
        std::lock_guard lock(m_mutex);
        m_logQueue.push({level, channel, msg, nullptr, std::chrono::system_clock::now()});
        m_cv.notify_one();
    }

    void Logger::LogPacket(LogChannel channel, std::shared_ptr<std::vector<uint8_t>> data,
                           DumpMode mode) {
        std::lock_guard lock(m_mutex);
        m_logQueue.push({LogLevel::Packet, channel, "Packet dump", std::move(data),
                         std::chrono::system_clock::now(), mode});
        m_cv.notify_one();
    }

    // ─── Direct-write helpers (no queue, safe to call from main thread) ───────

    void Logger::Banner(const std::string& title) {
        const int totalWidth = 72;
        const int padded     = static_cast<int>(title.size()) + 2;  // " title "
        const int left       = (totalWidth - padded) / 2;
        const int right      = totalWidth - padded - left;

        std::string line(left, '=');
        line += ' ';
        line += title;
        line += ' ';
        line += std::string(right, '=');

        std::cout << '\n'
                  << Ansi::Bold << Ansi::Blue << line << Ansi::Reset
                  << '\n';
    }

    void Logger::Separator(const std::string& label) {
        if (label.empty()) {
            std::cout << Ansi::Gray << std::string(72, '-') << Ansi::Reset << '\n';
        } else {
            const int totalWidth = 72;
            const int padded     = static_cast<int>(label.size()) + 2;
            const int left       = 4;
            const int right      = totalWidth - left - padded;

            std::string line(left, '-');
            line += ' ';
            line += label;
            line += ' ';
            line += std::string(right, '-');
            std::cout << Ansi::Gray << line << Ansi::Reset << '\n';
        }
    }

    // ─── Consumer thread ──────────────────────────────────────────────────────

    void Logger::ProcessLogs() {
        while (true) {
            LogEntry entry;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [] { return !m_logQueue.empty() || !m_running; });

                if (!m_running && m_logQueue.empty())
                    break;

                entry = std::move(m_logQueue.front());
                m_logQueue.pop();
            }

            // Consume the sync sentinel — fire callback to unblock Sync() caller
            if (entry.message == "\x01SYNC") {
                if (entry.syncCallback) entry.syncCallback();
                continue;
            }

            // ── Timestamp: HH:MM:SS.mmm ──
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.timestamp.time_since_epoch()).count() % 1000;
            const std::time_t tSec = std::chrono::system_clock::to_time_t(entry.timestamp);
            char tsBuf[16];
            std::strftime(tsBuf, sizeof(tsBuf), "%H:%M:%S", std::localtime(&tSec));
            std::ostringstream ts;
            ts << tsBuf << '.' << std::setw(3) << std::setfill('0') << ms;

            // ── Channel column (fixed 9 chars) ──
            const std::string chanStr = GetChannelString(entry.channel);
            std::ostringstream chanCol;
            chanCol << std::left << std::setw(9) << chanStr;

            // ── Color + icon ──
            const std::string color = GetLevelColor(entry.level);
            const std::string icon  = GetLevelIcon(entry.level);

            // ── Assemble ──
            // Format:  HH:MM:SS.mmm  CHANNEL   ICON  message
            std::cout << Ansi::Gray    << ts.str()       << Ansi::Reset
                      << "  "
                      << Ansi::Dim    << chanCol.str()   << Ansi::Reset
                      << "  "
                      << color        << icon            << Ansi::Reset
                      << "  "
                      << color        << entry.message   << Ansi::Reset
                      << '\n';

            if (entry.level == LogLevel::Packet && entry.rawData && !entry.rawData->empty()) {
                switch (entry.dumpMode) {
                    case DumpMode::Binary:    std::cout << FormatPacketBinary(*entry.rawData)    << '\n'; break;
                    case DumpMode::HexBinary: std::cout << FormatPacketHexBinary(*entry.rawData) << '\n'; break;
                    default:                  std::cout << FormatPacketHex(*entry.rawData)        << '\n'; break;
                }
            }
        }
    }

    // ─── Level helpers ────────────────────────────────────────────────────────

    std::string Logger::GetLevelString(LogLevel level) {
        switch (level) {
            case LogLevel::Info:    return "INFO";
            case LogLevel::Success: return "OK";
            case LogLevel::Debug:   return "DEBUG";
            case LogLevel::Warning: return "WARN";
            case LogLevel::Error:   return "ERROR";
            case LogLevel::Packet:  return "PKT";
            default:                return "???";
        }
    }

    std::string Logger::GetLevelColor(LogLevel level) {
        switch (level) {
            case LogLevel::Info:    return Ansi::White;
            case LogLevel::Success: return Ansi::Green;
            case LogLevel::Debug:   return Ansi::Cyan;
            case LogLevel::Warning: return Ansi::Yellow;
            case LogLevel::Error:   return Ansi::Red;
            case LogLevel::Packet:  return Ansi::Magenta;
            default:                return Ansi::White;
        }
    }

    std::string Logger::GetLevelIcon(LogLevel level) {
        switch (level) {
            case LogLevel::Info:    return "\xC2\xB7";         // ·  (U+00B7)
            case LogLevel::Success: return "\xE2\x9C\x93";     // ✓  (U+2713)
            case LogLevel::Debug:   return "\xE2\x97\x86";     // ◆  (U+25C6)
            case LogLevel::Warning: return "\xE2\x9A\xA0";     // ⚠  (U+26A0)
            case LogLevel::Error:   return "\xE2\x9C\x96";     // ✖  (U+2716)
            case LogLevel::Packet:  return "\xE2\x86\x92";     // →  (U+2192)
            default:                return " ";
        }
    }

    std::string Logger::GetChannelString(LogChannel channel) {
        switch (channel) {
            case LogChannel::Core:      return "CORE";
            case LogChannel::Transport: return "TRANSPORT";
            case LogChannel::Brain:     return "BRAIN";
            case LogChannel::General:   return "GENERAL";
            default:                    return "UNK";
        }
    }

    // ─── Public FormatPacket dispatcher ──────────────────────────────────────

    std::string Logger::FormatPacket(const std::vector<uint8_t>& data, DumpMode mode) {
        switch (mode) {
            case DumpMode::Binary:    return FormatPacketBinary(data);
            case DumpMode::HexBinary: return FormatPacketHexBinary(data);
            default:                  return FormatPacketHex(data);
        }
    }

    // ─── Packet dump formatters ───────────────────────────────────────────────

    // Helper: printable char or dot
    static char PrintableChar(uint8_t b) {
        return std::isprint(static_cast<unsigned char>(b)) ? static_cast<char>(b) : '.';
    }

    // Helper: 8-bit binary string, MSB first
    static std::string ToBinary8(uint8_t b) {
        std::string s(8, '0');
        for (int i = 7; i >= 0; --i)
            s[7 - i] = ((b >> i) & 1) ? '1' : '0';
        return s;
    }

    // ── Mode 1: Hex ───────────────────────────────────────────────────────────
    // Format (8 bytes/row):
    //   0000  a3 1f 48 65 6c 6c 6f 20  |..Hello |
    std::string Logger::FormatPacketHex(const std::vector<uint8_t>& data) {
        std::ostringstream ss;
        const size_t total = data.size();
        constexpr size_t kRow = 8;

        for (size_t offset = 0; offset < total; offset += kRow) {
            ss << Ansi::Gray
               << "   " << std::hex << std::setw(4) << std::setfill('0') << offset
               << "  " << Ansi::Reset;

            for (size_t i = 0; i < kRow; ++i) {
                if (offset + i < total)
                    ss << Ansi::Magenta
                       << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<int>(data[offset + i])
                       << ' ' << Ansi::Reset;
                else
                    ss << "   ";
            }

            ss << Ansi::Gray << " |" << Ansi::Reset;
            for (size_t i = 0; i < kRow && offset + i < total; ++i)
                ss << PrintableChar(data[offset + i]);
            ss << Ansi::Gray << "|" << Ansi::Reset << '\n';
        }

        ss << Ansi::Gray << "   " << std::dec << total << " bytes" << Ansi::Reset;
        return ss.str();
    }

    // ── Mode 2: Binary ────────────────────────────────────────────────────────
    // Format (1 byte/row):
    //   [00] 0xa3  10100011   163  .
    //   [01] 0x48  01001000    72  H
    std::string Logger::FormatPacketBinary(const std::vector<uint8_t>& data) {
        std::ostringstream ss;
        const size_t total = data.size();

        for (size_t i = 0; i < total; ++i) {
            const uint8_t b = data[i];
            ss << Ansi::Gray  << "   [" << std::dec << std::setw(2) << std::setfill('0') << i << "] "
               << Ansi::Reset
               << Ansi::Magenta << "0x" << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<int>(b) << "  " << Ansi::Reset
               << Ansi::Cyan   << ToBinary8(b) << "  " << Ansi::Reset
               << Ansi::White  << std::dec << std::setw(3) << std::setfill(' ')
               << static_cast<int>(b) << "  " << Ansi::Reset
               << Ansi::Gray   << PrintableChar(b) << Ansi::Reset
               << '\n';
        }

        ss << Ansi::Gray << "   " << std::dec << total << " bytes" << Ansi::Reset;
        return ss.str();
    }

    // ── Mode 3: HexBinary ─────────────────────────────────────────────────────
    // Format (4 bytes/row):
    //   0000  a3 1f 48 65  10100011 00011111 01001000 01100101  |..He|
    std::string Logger::FormatPacketHexBinary(const std::vector<uint8_t>& data) {
        std::ostringstream ss;
        const size_t total = data.size();
        constexpr size_t kRow = 4;

        for (size_t offset = 0; offset < total; offset += kRow) {
            // Offset
            ss << Ansi::Gray
               << "   " << std::hex << std::setw(4) << std::setfill('0') << offset
               << "  " << Ansi::Reset;

            // Hex group
            for (size_t i = 0; i < kRow; ++i) {
                if (offset + i < total)
                    ss << Ansi::Magenta
                       << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<int>(data[offset + i])
                       << ' ' << Ansi::Reset;
                else
                    ss << "   ";
            }
            ss << ' ';

            // Binary group
            for (size_t i = 0; i < kRow; ++i) {
                if (offset + i < total)
                    ss << Ansi::Cyan << ToBinary8(data[offset + i]) << ' ' << Ansi::Reset;
                else
                    ss << "         ";
            }

            // ASCII
            ss << Ansi::Gray << " |" << Ansi::Reset;
            for (size_t i = 0; i < kRow && offset + i < total; ++i)
                ss << PrintableChar(data[offset + i]);
            ss << Ansi::Gray << "|" << Ansi::Reset << '\n';
        }

        ss << Ansi::Gray << "   " << std::dec << total << " bytes" << Ansi::Reset;
        return ss.str();
    }

}  // namespace NetworkMiddleware::Shared
