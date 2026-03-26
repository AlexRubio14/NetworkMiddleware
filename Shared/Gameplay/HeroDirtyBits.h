#pragma once
#include <cstdint>

enum class HeroDirtyBits : uint32_t
{
    Position    = 0,
    Health      = 1,
    MaxHealth   = 2,
    Mana        = 3,
    MaxMana     = 4,
    Level       = 5,
    Experience  = 6,
    StateFlags  = 7
};