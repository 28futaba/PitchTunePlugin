/*
  ==============================================================================

    PitchShifter.cpp
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#include "PitchShifter.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PitchShifter::PitchShifter()
    : delayBuffer (16384, 0.0f)
{
}

PitchShifter::~PitchShifter()
{
}

void PitchShifter::prepare (double sampleRateIn, int maxBlockSize)
{
    (void) maxBlockSize;

    sampleRate = (sampleRateIn > 0.0 && std::isfinite (sampleRateIn)) ? sampleRateIn : 44100.0;
    
    // Set a window size of approximately 40ms.
    windowSizeSamples = static_cast<float> (sampleRate * 0.040); 
    
    bufferSize = std::max (16384, static_cast<int> (std::ceil (windowSizeSamples * 2.0f)) + 256);
    delayBuffer.assign (static_cast<size_t> (bufferSize), 0.0f);
    
    writeIndex = 0;
    phase1 = 0.0f;
    phase2 = 0.5f; // Single stationary center tap (w1 = 0.0, w2 = 1.0)
    
    pitchShiftSemitones = 0.0f;
    pitchShiftRatio = 1.0f;
}

void PitchShifter::setPitchShift (float semitones)
{
    if (! std::isfinite (semitones))
        semitones = 0.0f;

    pitchShiftSemitones = std::clamp (semitones, -12.0f, 12.0f);
    pitchShiftRatio = std::pow (2.0f, pitchShiftSemitones / 12.0f);
}

void PitchShifter::reset()
{
    std::fill (delayBuffer.begin(), delayBuffer.end(), 0.0f);
    writeIndex = 0;
    phase1 = 0.0f;
    phase2 = 0.5f;
}

int PitchShifter::getLatencySamples() const noexcept
{
    return static_cast<int> (std::round (32.0f + 0.5f * windowSizeSamples));
}

void PitchShifter::prime (const float* samples, int numSamples) noexcept
{
    if (delayBuffer.empty() || bufferSize <= 0)
        return;

    std::fill (delayBuffer.begin(), delayBuffer.end(), 0.0f);
    writeIndex = 0;
    phase1 = 0.0f;
    phase2 = 0.5f;

    if (samples == nullptr || numSamples <= 0)
        return;

    const int startIdx = std::max (0, numSamples - bufferSize);
    for (int i = startIdx; i < numSamples; ++i)
    {
        const float s = samples[i];
        delayBuffer[static_cast<size_t> (writeIndex)] = std::isfinite (s) ? s : 0.0f;
        writeIndex = (writeIndex + 1) % bufferSize;
    }
}

float PitchShifter::readDelayLine (float delayInSamples)
{
    if (delayBuffer.empty() || bufferSize <= 0 || ! std::isfinite (delayInSamples))
        return 0.0f;

    const float fBufSize = static_cast<float> (bufferSize);
    float readPos = std::fmod (static_cast<float> (writeIndex) - delayInSamples, fBufSize);
    if (readPos < 0.0f)
        readPos += fBufSize;

    const int idx1 = std::clamp (static_cast<int> (readPos), 0, bufferSize - 1);
    const int idx2 = (idx1 + 1) % bufferSize;
    const float frac = std::clamp (readPos - static_cast<float> (idx1), 0.0f, 1.0f);

    return (1.0f - frac) * delayBuffer[static_cast<size_t> (idx1)] + frac * delayBuffer[static_cast<size_t> (idx2)];
}

void PitchShifter::process (const float* inputSamples, float* outputSamples, int numSamples)
{
    if (delayBuffer.empty() || bufferSize <= 0 || numSamples <= 0 || windowSizeSamples <= 1.0f
        || inputSamples == nullptr || outputSamples == nullptr)
        return;

    if (! std::isfinite (phase1)) phase1 = 0.0f;
    if (! std::isfinite (phase2)) phase2 = 0.5f;

    const float minDelayOffset = 32.0f;
    float phaseStep = 0.0f;

    if (std::abs (pitchShiftSemitones) < 0.001f)
    {
        // Keep constant latency (~20ms) when unshifted so transitions between
        // shifted and unshifted notes never introduce a ~20ms timing/phase jump.
        const float w1Now = std::abs (std::sin (static_cast<float> (M_PI) * phase1));
        const float w2Now = std::abs (std::sin (static_cast<float> (M_PI) * phase2));

        if (w1Now < 0.02f)
        {
            phase1 = 0.0f;
            phase2 = 0.5f;
            phaseStep = 0.0f;
        }
        else if (w2Now < 0.02f)
        {
            phase2 = 0.0f;
            phase1 = 0.5f;
            phaseStep = 0.0f;
        }
        else
        {
            phaseStep = 0.008f / windowSizeSamples;
        }
    }
    else
    {
        phaseStep = (1.0f - pitchShiftRatio) / windowSizeSamples;
        if (! std::isfinite (phaseStep))
            phaseStep = 0.0f;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // Must preserve raw input before overwriting outputSamples (in-place buffer safety)
        const float rawIn = inputSamples[i];
        const float inSample = std::isfinite (rawIn) ? rawIn : 0.0f;

        if (phaseStep != 0.0f)
        {
            phase1 += phaseStep;
            phase2 += phaseStep;

            if (phase1 >= 1.0f) phase1 -= 1.0f;
            else if (phase1 < 0.0f) phase1 += 1.0f;

            if (phase2 >= 1.0f) phase2 -= 1.0f;
            else if (phase2 < 0.0f) phase2 += 1.0f;

            if (! std::isfinite (phase1) || phase1 < 0.0f || phase1 >= 1.0f) phase1 = 0.0f;
            if (! std::isfinite (phase2) || phase2 < 0.0f || phase2 >= 1.0f) phase2 = 0.5f;
        }

        const float delay1 = minDelayOffset + phase1 * windowSizeSamples;
        const float delay2 = minDelayOffset + phase2 * windowSizeSamples;

        const float out1 = readDelayLine (delay1);
        const float out2 = readDelayLine (delay2);

        float w1 = std::abs (std::sin (static_cast<float> (M_PI) * phase1));
        float w2 = std::abs (std::sin (static_cast<float> (M_PI) * phase2));
        const float wSum = w1 + w2;
        if (wSum > 1e-6f)
        {
            w1 /= wSum;
            w2 /= wSum;
        }

        const float outSample = (w1 * out1) + (w2 * out2);
        outputSamples[i] = std::isfinite (outSample) ? outSample : 0.0f;

        delayBuffer[static_cast<size_t> (writeIndex)] = inSample;
        writeIndex = (writeIndex + 1) % bufferSize;
    }
}
