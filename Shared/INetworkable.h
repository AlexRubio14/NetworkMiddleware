#pragma once
#include <cstdint>

namespace NetworkMiddleware::Shared {

    class BitReader;
    class BitWriter;

    class INetworkable {
    public:
        virtual ~INetworkable() = default;

        virtual void Serialize(BitWriter& writer) const = 0;
        virtual void Unserialize(BitReader& reader) = 0;

        virtual uint32_t GetDirtyMask() const = 0;
        virtual void ClearDirtyMask() = 0;

        virtual uint32_t GetNetworkID() const = 0;
    };
}