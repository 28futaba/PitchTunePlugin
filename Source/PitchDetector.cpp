/*
  ==============================================================================

    PitchDetector.cpp
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#include "PitchDetector.h"
#include <cmath>
#include <numeric>
#include <algorithm>

PitchDetector::PitchDetector()
{
}

PitchDetector::~PitchDetector()
{
}

void PitchDetector::prepare (double sampleRateIn, int maxBlockSize)
{
    (void) maxBlockSize;

    sampleRate = (sampleRateIn > 0.0 && std::isfinite (sampleRateIn)) ? sampleRateIn : 44100.0;
    
    // 2048 samples window allows reliable detection down to 50 Hz at up to 96kHz.
    bufferSize = 2048;
    
    ringBuffer.assign (static_cast<size_t> (bufferSize), 0.0f);
    analysisBuffer.assign (static_cast<size_t> (bufferSize), 0.0f);
    yinBuffer.assign (static_cast<size_t> (bufferSize / 2), 0.0f);
    writeIndex = 0;
    currentFrequency = 0.0f;
}

void PitchDetector::process (const float* inputSamples, int numSamples)
{
    if (inputSamples == nullptr || numSamples <= 0 || bufferSize <= 0)
        return;

    // Stream incoming samples into ring buffer
    for (int i = 0; i < numSamples; ++i)
    {
        const float s = inputSamples[i];
        ringBuffer[static_cast<size_t> (writeIndex)] = std::isfinite (s) ? s : 0.0f;
        writeIndex = (writeIndex + 1) % bufferSize;
    }

    // Copy window for analysis
    int readIdx = writeIndex;
    for (int i = 0; i < bufferSize; ++i)
    {
        readIdx = (readIdx - 1 + bufferSize) % bufferSize;
        analysisBuffer[static_cast<size_t> (bufferSize - 1 - i)] = ringBuffer[static_cast<size_t> (readIdx)];
    }

    const float detected = detectPitchDirect (analysisBuffer.data(), bufferSize);

    if (detected > 0.0f)
    {
        if (currentFrequency == 0.0f)
            currentFrequency = detected;
        else
            currentFrequency = 0.6f * currentFrequency + 0.4f * detected;
    }
    else
    {
        currentFrequency = 0.0f;
    }
}

float PitchDetector::detectPitchDirect (const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples < bufferSize || bufferSize < 16)
        return 0.0f;

    // 1. Calculate Signal RMS & Zero-Crossing Rate to discard silence and unvoiced breath/sibilants
    float sumSquares = 0.0f;
    int zeroCrossings = 0;
    bool prevNonNegative = (samples[0] >= 0.0f);

    for (int i = 0; i < bufferSize; ++i)
    {
        const float s = samples[i];
        if (! std::isfinite (s))
            return 0.0f;

        sumSquares += s * s;

        if (i > 0)
        {
            const bool currNonNegative = (s >= 0.0f);
            if (currNonNegative != prevNonNegative)
                ++zeroCrossings;
            prevNonNegative = currNonNegative;
        }
    }

    if (! std::isfinite (sumSquares))
        return 0.0f;

    const float rms = std::sqrt (sumSquares / static_cast<float> (bufferSize));
    if (! std::isfinite (rms) || rms < silenceThreshold)
        return 0.0f;

    const float zcrHz = 0.5f * static_cast<float> (zeroCrossings)
                        * static_cast<float> (sampleRate / static_cast<double> (bufferSize));
    if (! std::isfinite (zcrHz) || zcrHz > 3800.0f)
        return 0.0f;

    const int halfBuffer = bufferSize / 2;
    int minLag = static_cast<int> (std::floor (sampleRate / maxFreq));
    int maxLag = static_cast<int> (std::ceil (sampleRate / minFreq));

    minLag = std::clamp (minLag, 2, halfBuffer - 3);
    maxLag = std::clamp (maxLag, minLag + 2, halfBuffer - 1);

    // 2. YIN Step 1: Difference function d_t(tau)
    yinBuffer.assign (static_cast<size_t> (halfBuffer), 0.0f);

    for (int tau = 1; tau < halfBuffer; ++tau)
    {
        float diffSum = 0.0f;
        for (int i = 0; i < halfBuffer; ++i)
        {
            const float delta = samples[i] - samples[i + tau];
            diffSum += delta * delta;
        }
        yinBuffer[static_cast<size_t> (tau)] = diffSum;
    }

    // 3. YIN Step 2: Cumulative mean normalized difference function d'_t(tau)
    yinBuffer[0] = 1.0f;
    float runningSum = 0.0f;

    for (int tau = 1; tau < halfBuffer; ++tau)
    {
        runningSum += yinBuffer[static_cast<size_t> (tau)];
        if (runningSum > 0.0f)
            yinBuffer[static_cast<size_t> (tau)] = (yinBuffer[static_cast<size_t> (tau)] * static_cast<float> (tau)) / runningSum;
        else
            yinBuffer[static_cast<size_t> (tau)] = 1.0f;
    }

    // 4. YIN Step 3: Absolute threshold peak selection (true interior local minima only)
    int bestTau = -1;
    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (yinBuffer[static_cast<size_t> (tau)] < yinThreshold)
        {
            while (tau + 1 <= maxLag && yinBuffer[static_cast<size_t> (tau + 1)] < yinBuffer[static_cast<size_t> (tau)])
                ++tau;

            if (tau == maxLag && yinBuffer[static_cast<size_t> (maxLag)] < yinBuffer[static_cast<size_t> (maxLag - 1)])
                continue;

            if (tau > minLag && tau < maxLag)
            {
                bestTau = tau;
                break;
            }
        }
    }

    // Fallback: search interior local minima in allowable range with tighter unvoiced rejection
    if (bestTau == -1)
    {
        float minVal = 1e10f;
        for (int tau = minLag + 1; tau <= maxLag - 1; ++tau)
        {
            const float val = yinBuffer[static_cast<size_t> (tau)];
            if (val <= yinBuffer[static_cast<size_t> (tau - 1)]
                && val <= yinBuffer[static_cast<size_t> (tau + 1)]
                && val < minVal)
            {
                minVal = val;
                bestTau = tau;
            }
        }

        if (bestTau == -1 || minVal > 0.22f)
            return 0.0f;
    }

    // 5. YIN Step 4: Parabolic (sub-sample) interpolation
    float interpolatedTau = static_cast<float> (bestTau);
    if (bestTau > 1 && bestTau < halfBuffer - 1)
    {
        const float s0 = yinBuffer[static_cast<size_t> (bestTau - 1)];
        const float s1 = yinBuffer[static_cast<size_t> (bestTau)];
        const float s2 = yinBuffer[static_cast<size_t> (bestTau + 1)];

        const float denom = (s0 - 2.0f * s1 + s2);
        if (std::abs (denom) > 1e-6f)
        {
            const float delta = std::clamp ((s0 - s2) / (2.0f * denom), -0.5f, 0.5f);
            interpolatedTau += delta;
        }
    }

    if (! std::isfinite (interpolatedTau) || interpolatedTau <= 0.0f)
        return 0.0f;

    const float frequency = static_cast<float> (sampleRate) / interpolatedTau;

    if (std::isfinite (frequency) && frequency >= minFreq && frequency <= maxFreq)
        return frequency;

    return 0.0f;
}
