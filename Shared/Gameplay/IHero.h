#pragma once
#include "../INetworkable.h"
#include <string>

namespace NetworkMiddleware::Shared::Gameplay {

    /**
     * Interface representing a Hero entity in the game world.
     * Inherits from INetworkable to ensure synchronization capabilities.
     */
    class IHero : public INetworkable {
    public:
        virtual ~IHero() = default;

        // --- Identity & Metadata ---
        virtual std::string GetHeroName() const = 0;
        virtual uint16_t GetHeroTypeID() const = 0;

        // --- Combat & Vitality ---
        virtual void TakeDamage(float amount) = 0;
        virtual void Heal(float amount) = 0;
        virtual void UseMana(float amount) = 0;

        // --- Progression System ---
        virtual uint32_t GetLevel() const = 0;
        virtual float GetExperience() const = 0;
        virtual void AddExperience(float amount) = 0;
        virtual void LevelUp() = 0;

        // --- Ability System (Agnostic Slots) ---
        virtual void CastAbility(int slot) = 0;
        virtual void LevelUpAbility(int slot) = 0;
        virtual float GetAbilityCooldown(int slot) const = 0;

        // --- Movement & Transform ---
        // Note: The Serializer will quantize these floats into 14-bit integers
        virtual void SetPosition(float x, float y) = 0;
        virtual float GetX() const = 0;
        virtual float GetY() const = 0;

        // --- Status Flags ---
        virtual bool IsDead() const = 0;
        virtual bool IsStunned() const = 0;
    };
}