#pragma once

// Test-only shadow of CommonLibF4's <RE/Fallout.h>. FallUIHudEditor.cpp needs
// just RE::NiColor and a callable REL::Relocation for the one engine call it
// makes (HUDMenuUtils::GetGameplayHUDColor). The stub returns Pip-Boy green so
// COLOR_HUD resolution is deterministic in offline tests.

namespace RE {
    class NiColor {
    public:
        float r;
        float g;
        float b;
    };
}

namespace REL {
    class ID {
    public:
        explicit constexpr ID(unsigned long long a_id) noexcept : id(a_id) {}
        unsigned long long id;
    };

    template <class F>
    class Relocation {
    public:
        explicit Relocation(ID) noexcept {}
        RE::NiColor operator()() const { return RE::NiColor{ 0.09f, 0.56f, 0.07f }; }
    };
}
