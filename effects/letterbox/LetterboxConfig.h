#pragma once

struct LetterboxConfig
{
    bool enabled = true;
    float aspectRatio = 2.35f;
    float colorR = 0.0f;
    float colorG = 0.0f;
    float colorB = 0.0f;
    float opacity = 1.0f;
    bool debugView = false;
};

extern LetterboxConfig gLetterboxConfig;
