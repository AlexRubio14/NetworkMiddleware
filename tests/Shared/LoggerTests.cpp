// Logger unit tests — DumpMode formatting and bounds safety.
// Tests use Logger::FormatPacket() (public wrapper) — no async queue involved.

#include <gtest/gtest.h>
#include "Shared/Log/Logger.h"

using namespace NetworkMiddleware::Shared;

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Strip ANSI escape sequences so we can do plain-text assertions.
static std::string StripAnsi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool inEsc = false;
    for (char c : s) {
        if (c == '\033') { inEsc = true; continue; }
        if (inEsc) { if (c == 'm') inEsc = false; continue; }
        out += c;
    }
    return out;
}

// ─── FormatPacket — empty buffer ──────────────────────────────────────────────

TEST(LoggerFormat, EmptyBuffer_HexMode_NocrashReturnsBytes) {
    const std::vector<uint8_t> empty;
    const std::string out = StripAnsi(Logger::FormatPacket(empty, DumpMode::Hex));
    EXPECT_NE(out.find("0 bytes"), std::string::npos);
}

TEST(LoggerFormat, EmptyBuffer_BinaryMode_NoCrash) {
    const std::vector<uint8_t> empty;
    EXPECT_NO_THROW(Logger::FormatPacket(empty, DumpMode::Binary));
}

TEST(LoggerFormat, EmptyBuffer_HexBinaryMode_NoCrash) {
    const std::vector<uint8_t> empty;
    EXPECT_NO_THROW(Logger::FormatPacket(empty, DumpMode::HexBinary));
}

// ─── FormatPacketHex ──────────────────────────────────────────────────────────

TEST(LoggerFormat, HexMode_ByteAppearsAsHex) {
    const std::vector<uint8_t> data = {0xa3};
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::Hex));
    EXPECT_NE(out.find("a3"), std::string::npos);
}

TEST(LoggerFormat, HexMode_PrintableCharAppearsInAsciiColumn) {
    const std::vector<uint8_t> data = {0x48};  // 'H'
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::Hex));
    EXPECT_NE(out.find("H"), std::string::npos);
}

TEST(LoggerFormat, HexMode_NonPrintableShowsDot) {
    const std::vector<uint8_t> data = {0x01};
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::Hex));
    EXPECT_NE(out.find("."), std::string::npos);
}

TEST(LoggerFormat, HexMode_ByteCountInFooter) {
    const std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::Hex));
    EXPECT_NE(out.find("3 bytes"), std::string::npos);
}

// ─── FormatPacketBinary ───────────────────────────────────────────────────────

TEST(LoggerFormat, BinaryMode_AllOnesIsEightOnes) {
    const std::vector<uint8_t> data = {0xFF};
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::Binary));
    EXPECT_NE(out.find("11111111"), std::string::npos);
}

TEST(LoggerFormat, BinaryMode_AllZerosIsEightZeros) {
    const std::vector<uint8_t> data = {0x00};
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::Binary));
    EXPECT_NE(out.find("00000000"), std::string::npos);
}

TEST(LoggerFormat, BinaryMode_KnownPattern_0xA3) {
    // 0xa3 = 163 = 10100011 (MSB first)
    const std::vector<uint8_t> data = {0xa3};
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::Binary));
    EXPECT_NE(out.find("10100011"), std::string::npos);
}

TEST(LoggerFormat, BinaryMode_HexOffsetAndDecimalPresent) {
    const std::vector<uint8_t> data = {0x48};  // decimal 72
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::Binary));
    EXPECT_NE(out.find("0x48"), std::string::npos);
    EXPECT_NE(out.find("72"),   std::string::npos);
}

TEST(LoggerFormat, BinaryMode_EachByteOnItsOwnRow) {
    const std::vector<uint8_t> data = {0x01, 0x02};
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::Binary));
    // Two offset tags [00] and [01] must be present
    EXPECT_NE(out.find("[00]"), std::string::npos);
    EXPECT_NE(out.find("[01]"), std::string::npos);
}

// ─── FormatPacketHexBinary ────────────────────────────────────────────────────

TEST(LoggerFormat, HexBinaryMode_ContainsBothHexAndBinary) {
    const std::vector<uint8_t> data = {0xa3};
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::HexBinary));
    EXPECT_NE(out.find("a3"),       std::string::npos);   // hex part
    EXPECT_NE(out.find("10100011"), std::string::npos);   // binary part
}

TEST(LoggerFormat, HexBinaryMode_ByteCountInFooter) {
    const std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
    const std::string out = StripAnsi(Logger::FormatPacket(data, DumpMode::HexBinary));
    EXPECT_NE(out.find("5 bytes"), std::string::npos);
}

// ─── Default mode is Hex ──────────────────────────────────────────────────────

TEST(LoggerFormat, DefaultMode_IsHex) {
    const std::vector<uint8_t> data = {0xff};
    const std::string defaultOut = StripAnsi(Logger::FormatPacket(data));
    const std::string hexOut     = StripAnsi(Logger::FormatPacket(data, DumpMode::Hex));
    EXPECT_EQ(defaultOut, hexOut);
}
