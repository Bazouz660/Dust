#pragma once

struct FilmGrainConfig
{
    bool enabled = true;
    float intensity = 0.05f;
    float size = 1.6f;
    bool colored = false;
    bool debugView = false;
};

extern FilmGrainConfig gFilmGrainConfig;
