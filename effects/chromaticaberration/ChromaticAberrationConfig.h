#pragma once

struct ChromaticAberrationConfig
{
    bool enabled = true;
    float strength = 0.003f;
    bool debugView = false;
};

extern ChromaticAberrationConfig gCAConfig;
