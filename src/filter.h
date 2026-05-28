// filter.h / synth.h / textfile.h / compress.h — forwarding headers
// These are thin wrappers that just include zepto8.h first

// filter.h
#pragma once
#include "zepto8.h"

namespace z8
{

class filter
{
public:
    enum class type { lpf, hpf, lowshelf, highshelf };

    filter(type t, float freq, float q, float gain);

    void  init(type t, float freq, float q, float gain);
    float run(float input);

    float c1, c2, c3, c4, c5;
    float linput   = 0;
    float llinput  = 0;
    float loutput  = 0;
    float lloutput = 0;
};

} // namespace z8
