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
    sampleRate = sampleRateIn;
    
    // 2048 samples window allows reliable detection down to 50 Hz at up to 96kHz.
    bufferSize = 2048;
    
    ringBuffer.assign (bufferSize, 0.0f);
    analysisBuffer.assign (bufferSize, 0.0f);
    yinBuffer.assign (bufferSize / 2, 0.0f);
    writeIndex = 0;
    currentFrequency = 0.0f;
}

void PitchDetector::process (const float* inputSamples, int numSamples)
{
    if (numSamples <= 0)
        return;

    // Stream incoming samples into ring buffer
    for (int i = 0; i < numSamples; ++i)
    {
        ringBuffer[writeIndex] = inputSamples[i];
        writeIndex = (writeIndex + 1) % bufferSize;
    }

    // Copy window for analysis
    int readIdx = writeIndex;
    for (int i = 0; i < bufferSize; ++i)
    {
        readIdx = (readIdx - 1 + bufferSize) % bufferSize;
        analysisBuffer[bufferSize - 1 - i] = ringBuffer[readIdx];
    }

    float detected = detectPitchDirect (analysisBuffer.data(), bufferSize);

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
    if (numSamples < bufferSize)
        return 0.0f;

    // 1. Calculate Signal RMS to discard silence/unvoiced breath
    float sumSquares = 0.0f;
    for (int i = 0; i < bufferSize; ++i)
        sumSquares += samples[i] * samples[i];

    float rms = std::sqrt (sumSquares / static_cast<float> (bufferSize));
    if (rms < silenceThreshold)
        return 0.0f;

    int halfBuffer = bufferSize / 2;
    int minLag = static_cast<int> (std::floor (sampleRate / maxFreq));
    int maxLag = static_cast<int> (std::ceil (sampleRate / minFreq));

    minLag = std::clamp (minLag, 2, halfBuffer - 2);
    maxLag = std::clamp (maxLag, minLag + 1, halfBuffer - 1);

    // 2. YIN Step 1: Difference function d_t(tau)
    yinBuffer.assign (halfBuffer, 0.0f);

    for (int tau = 1; tau < halfBuffer; ++tau)
    {
        float diffSum = 0.0f;
        for (int i = 0; i < halfBuffer; ++i)
        {
            float delta = samples[i] - samples[i + tau];
            diffSum += delta * delta;
        }
        yinBuffer[tau] = diffSum;
    }

    // 3. YIN Step 2: Cumulative mean normalized difference function d'_t(tau)
    yinBuffer[0] = 1.0f;
    float runningSum = 0.0f;

    for (int tau = 1; tau < halfBuffer; ++tau)
    {
        runningSum += yinBuffer[tau];
        if (runningSum > 0.0f)
            yinBuffer[tau] = (yinBuffer[tau] * static_cast<float> (tau)) / runningSum;
        else
            yinBuffer[tau] = 1.0f;
    }

    // 4. YIN Step 3: Absolute threshold peak selection
    int bestTau = -1;
    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (yinBuffer[tau] < yinThreshold)
        {
            // Find local minimum
            while (tau + 1 <= maxLag && yinBuffer[tau + 1] < yinBuffer[tau])
                tau++;

            bestTau = tau;
            break;
        }
    }

    // Fallback: If no value fell below threshold, find global minimum in allowable range
    if (bestTau == -1)
    {
        float minVal = 1e10f;
        for (int tau = minLag; tau <= maxLag; ++tau)
        {
            if (yinBuffer[tau] < minVal)
            {
                minVal = yinBuffer[tau];
                bestTau = tau;
            }
        }

        // If the best minimum is still too noisy / unvoiced, reject
        if (minVal > 0.45f)
            return 0.0f;
    }

    // 5. YIN Step 4: Parabolic (sub-sample) interpolation
    float interpolatedTau = static_cast<float> (bestTau);
    if (bestTau > 1 && bestTau < halfBuffer - 1)
    {
        float s0 = yinBuffer[bestTau - 1];
        float s1 = yinBuffer[bestTau];
        float s2 = yinBuffer[bestTau + 1];

        float denom = (s0 - 2.0f * s1 + s2);
        if (std::abs (denom) > 1e-6f)
        {
            float delta = (s0 - s2) / (2.0f * denom);
            interpolatedTau += delta;
        }
    }

    if (interpolatedTau <= 0.0f)
        return 0.0f;

    float frequency = static_cast<float> (sampleRate) / interpolatedTau;

    if (frequency >= minFreq && frequency <= maxFreq)
        return frequency;

    return 0.0f;
}
