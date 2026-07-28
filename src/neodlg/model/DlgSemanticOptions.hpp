#pragma once

#include <array>

namespace neodlg {

struct DlgIntegerOption {
    int value;
    const char* label;
};

inline constexpr std::array<DlgIntegerOption, 5> kCameraAngleOptions{{
    {0, "Automatic"},
    {1, "Calculated camera preset 1"},
    {2, "Calculated camera preset 2"},
    {3, "Calculated camera preset 3"},
    {6, "Placed camera"},
}};

inline constexpr std::array<DlgIntegerOption, 2> kFadeTypeOptions{{
    {0, "Fade out"},
    {1, "Fade in"},
}};

inline constexpr std::array<DlgIntegerOption, 3> kConversationTypeOptions{{
    {0, "Normal conversation"},
    {1, "Computer conversation"},
    {2, "Full conversation (disable one-line bark shortcut)"},
}};

// Row labels from the supplied KotOR I videoeffects.2da.
inline constexpr std::array<DlgIntegerOption, 4> kKotorVideoEffectOptions{{
    {-1, "None"},
    {0, "VIDEO_EFFECT_SECURITY_CAMERA"},
    {1, "VIDEO_EFFECT_FREELOOK_T3M4"},
    {2, "VIDEO_EFFECT_FREELOOK_HK47"},
}};

// Row labels from the supplied KotOR II videoeffects.2da. The typo in row 15
// is preserved because it is the label stored by the game data.
inline constexpr std::array<DlgIntegerOption, 17> kKotor2VideoEffectOptions{{
    {-1, "None"},
    {0, "VIDEO_EFFECT_SECURITY_CAMERA"},
    {1, "VIDEO_EFFECT_FREELOOK_T3M4"},
    {2, "VIDEO_EFFECT_FREELOOK_HK47"},
    {3, "VIDEO_EFFECT_CLAIRVOYANCE"},
    {4, "VIDEO_EFFECT_FORCESIGHT"},
    {5, "VIDEO_EFFECT_VISAS_FREELOOK"},
    {6, "VIDEO_EFFECT_CLAIRVOYANCEFULL"},
    {7, "VIDEO_EFFECT_FURY_1"},
    {8, "VIDEO_EFFECT_FURY_2"},
    {9, "VIDEO_EFFECT_FURY_3"},
    {10, "VIDEO_EFFECT_SECURITY_NO_LABEL"},
    {11, "VIDEO_EFFECT_FREELOOK_B4D4"},
    {12, "VIDEO_EFFECT_FREELOOK_GOTO"},
    {13, "VIDEO_EFFECT_FREELOOK_SENBALL"},
    {14, "VIDEO_EFFECT_FREELOOK_3CFD"},
    {15, "VIDEO_EFFECT_KRIEA_FREELOOK"},
}};

} // namespace neodlg
