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
{
    prepare (44100.0, 512);
}

PitchShifter::~PitchShifter()
{
}

void PitchShifter::prepare (double sampleRateIn, int maxBlockSize)
{
    (void) maxBlockSize;

    sampleRate = (sampleRateIn > 0.0 && std::isfinite (sampleRateIn)) ? sampleRateIn : 44100.0;
    bufferSize = std::max (16384, static_cast<int> (std::ceil (sampleRate * 0.25)) + 256);

    minDelaySamples = 16.0f;
    lowTriggerSamples = static_cast<float> (std::round (sampleRate * 0.014));
    centerDelaySamples = static_cast<float> (std::round (sampleRate * 0.031));
    highTriggerSamples = static_cast<float> (std::round (sampleRate * 0.048));
    minJumpSamples = std::max (64, static_cast<int> (std::round (sampleRate * 0.014)));
    maxJumpSamples = std::max (minJumpSamples + 64, static_cast<int> (std::round (sampleRate * 0.034)));
    xfadeLengthSamples = std::max (64, static_cast<int> (std::round (sampleRate * 0.010)));
    corrWindowSamples = std::max (64, static_cast<int> (std::round (sampleRate * 0.008)));

    delayBuffer.assign (static_cast<size_t> (bufferSize), 0.0f);
    writeIndex = 0;

    activeDelay = centerDelaySamples;
    incomingDelay = centerDelaySamples;
    isCrossfading = false;
    xfadePos = 0;
    activeXfadeLen = xfadeLengthSamples;
    recenteredForZeroShift = true;
    recentEnergy = 0.0f;

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
    activeDelay = centerDelaySamples;
    incomingDelay = centerDelaySamples;
    isCrossfading = false;
    xfadePos = 0;
    activeXfadeLen = xfadeLengthSamples;
    recenteredForZeroShift = true;
    recentEnergy = 0.0f;
}

int PitchShifter::getLatencySamples() const noexcept
{
    return static_cast<int> (std::round (centerDelaySamples));
}

void PitchShifter::prime (const float* samples, int numSamples) noexcept
{
    if (delayBuffer.empty() || bufferSize <= 0)
        return;

    std::fill (delayBuffer.begin(), delayBuffer.end(), 0.0f);
    writeIndex = 0;
    activeDelay = centerDelaySamples;
    incomingDelay = centerDelaySamples;
    isCrossfading = false;
    xfadePos = 0;
    activeXfadeLen = xfadeLengthSamples;
    recenteredForZeroShift = true;
    recentEnergy = 0.0f;

    if (samples == nullptr || numSamples <= 0)
        return;

    const int startIdx = std::max (0, numSamples - bufferSize);
    for (int i = startIdx; i < numSamples; ++i)
    {
        const float s = samples[i];
        const float finiteSample = std::isfinite (s) ? s : 0.0f;
        delayBuffer[static_cast<size_t> (writeIndex)] = finiteSample;
        writeIndex = (writeIndex + 1) % bufferSize;
        recentEnergy = 0.995f * recentEnergy + 0.005f * (finiteSample * finiteSample);
    }
}

float PitchShifter::readDelayLineCubic (float delayInSamples) const noexcept
{
    if (delayBuffer.empty() || bufferSize <= 4 || ! std::isfinite (delayInSamples))
        return 0.0f;

    const float fBufSize = static_cast<float> (bufferSize);
    float readPos = std::fmod (static_cast<float> (writeIndex) - delayInSamples, fBufSize);
    if (readPos < 0.0f)
        readPos += fBufSize;

    int idx1 = static_cast<int> (std::floor (readPos));
    const float frac = std::clamp (readPos - static_cast<float> (idx1), 0.0f, 1.0f);
    idx1 %= bufferSize;
    if (idx1 < 0)
        idx1 += bufferSize;

    const int idx0 = (idx1 - 1 + bufferSize) % bufferSize;
    const int idx2 = (idx1 + 1) % bufferSize;
    const int idx3 = (idx1 + 2) % bufferSize;

    const float y0 = delayBuffer[static_cast<size_t> (idx0)];
    const float y1 = delayBuffer[static_cast<size_t> (idx1)];
    const float y2 = delayBuffer[static_cast<size_t> (idx2)];
    const float y3 = delayBuffer[static_cast<size_t> (idx3)];

    // Catmull-Rom cubic Hermite interpolation
    const float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float a2 = -0.5f * y0 + 0.5f * y2;
    const float a3 = y1;

    const float out = ((a0 * frac + a1) * frac + a2) * frac + a3;
    return std::isfinite (out) ? out : 0.0f;
}

float PitchShifter::findBestAlignedDelay (float refDelay, float searchMinDelay, float searchMaxDelay) const noexcept
{
    const float maxAllowed = static_cast<float> (bufferSize - 16);
    const float clampedMin = std::clamp (searchMinDelay, minDelaySamples, maxAllowed);
    const float clampedMax = std::clamp (searchMaxDelay, clampedMin, maxAllowed);

    if (delayBuffer.empty() || bufferSize <= 32 || clampedMax <= clampedMin + 1.0f)
        return clampedMin;

    const int refBase = std::clamp (static_cast<int> (std::llround (refDelay)), 8, bufferSize - 16);
    const int minInt = static_cast<int> (std::ceil (clampedMin));
    const int maxInt = static_cast<int> (std::floor (clampedMax));
    if (maxInt <= minInt)
        return clampedMin;

    // Keep refDelay - k and candDelay - k >= 4 so cubic taps never wrap across writeIndex
    const int maxSafeCorr = std::min (refBase, minInt) - 4;
    const int corrLen = std::min (corrWindowSamples, maxSafeCorr);
    if (corrLen < 16)
        return std::clamp (0.5f * (clampedMin + clampedMax), minDelaySamples, maxAllowed);

    auto readAtIntDelay = [this] (int d) noexcept -> float
    {
        int idx = writeIndex - d;
        while (idx < 0)
            idx += bufferSize;
        if (idx >= bufferSize)
            idx %= bufferSize;
        return delayBuffer[static_cast<size_t> (idx)];
    };

    auto scoreCandidate = [&] (int candDelay, int stride) noexcept -> float
    {
        float dot = 0.0f;
        float refE = 0.0f;
        float candE = 0.0f;

        for (int k = 0; k < corrLen; k += stride)
        {
            const float r = readAtIntDelay (refBase - k);
            const float c = readAtIntDelay (candDelay - k);
            dot += r * c;
            refE += r * r;
            candE += c * c;
        }

        if (refE < 1e-8f)
            return 2.0f; // Sentinel for silent reference signal

        return dot / std::sqrt (refE * candE + 1e-12f);
    };

    float refCoarseEnergy = 0.0f;
    for (int k = 0; k < corrLen; k += 4)
    {
        const float r = readAtIntDelay (refBase - k);
        refCoarseEnergy += r * r;
    }

    if (refCoarseEnergy < 1e-8f)
        return std::clamp (0.5f * (clampedMin + clampedMax), minDelaySamples, maxAllowed);

    int bestCoarse = (minInt + maxInt) / 2;
    float bestCoarseScore = -2.0f;

    for (int cand = minInt; cand <= maxInt; cand += 4)
    {
        const float score = scoreCandidate (cand, 4);
        if (score > bestCoarseScore)
        {
            bestCoarseScore = score;
            bestCoarse = cand;
        }
    }

    const int fineStart = std::max (minInt, bestCoarse - 4);
    const int fineEnd = std::min (maxInt, bestCoarse + 4);
    int bestFine = bestCoarse;
    float bestFineScore = -2.0f;

    for (int cand = fineStart; cand <= fineEnd; ++cand)
    {
        const float score = scoreCandidate (cand, 2);
        if (score > bestFineScore)
        {
            bestFineScore = score;
            bestFine = cand;
        }
    }

    return std::clamp (static_cast<float> (bestFine), minDelaySamples, maxAllowed);
}

void PitchShifter::process (const float* inputSamples, float* outputSamples, int numSamples)
{
    if (delayBuffer.empty() || bufferSize <= 32 || numSamples <= 0
        || inputSamples == nullptr || outputSamples == nullptr)
        return;

    const float maxDelay = static_cast<float> (bufferSize - 16);
    const float maxSearchUpper = static_cast<float> (bufferSize - 32);

    const bool nearZeroShift = (std::abs (pitchShiftSemitones) < 0.005f);
    if (! nearZeroShift)
        recenteredForZeroShift = false;

    const float effectiveRatio = nearZeroShift ? 1.0f : pitchShiftRatio;
    float delayDelta = 1.0f - effectiveRatio;
    if (! std::isfinite (delayDelta))
        delayDelta = 0.0f;

    if (! std::isfinite (activeDelay))
        activeDelay = centerDelaySamples;
    if (! std::isfinite (incomingDelay))
        incomingDelay = centerDelaySamples;

    for (int i = 0; i < numSamples; ++i)
    {
        // Must preserve raw input before overwriting outputSamples (in-place buffer safety)
        const float rawIn = inputSamples[i];
        const float inSample = std::isfinite (rawIn) ? rawIn : 0.0f;
        recentEnergy = 0.995f * recentEnergy + 0.005f * (inSample * inSample);

        if (! isCrossfading)
        {
            if (delayDelta < 0.0f && activeDelay <= lowTriggerSamples)
            {
                // Shifting pitch UP: delay is shrinking, jump backward in delay
                const float dynMinJump = std::max (
                    static_cast<float> (minJumpSamples),
                    (-delayDelta) * static_cast<float> (xfadeLengthSamples) + 0.004f * static_cast<float> (sampleRate));
                const float sMin = std::min (maxSearchUpper, activeDelay + dynMinJump);
                const float sMax = std::min (maxSearchUpper, activeDelay + static_cast<float> (maxJumpSamples));
                incomingDelay = findBestAlignedDelay (activeDelay, sMin, std::max (sMin, sMax));
                isCrossfading = true;
                xfadePos = 0;
                activeXfadeLen = std::max (32, xfadeLengthSamples);
            }
            else if (delayDelta > 0.0f && activeDelay >= highTriggerSamples)
            {
                // Shifting pitch DOWN: delay is growing, jump forward in delay
                const float sMin = std::max (lowTriggerSamples, activeDelay - static_cast<float> (maxJumpSamples));
                const float sMax = std::max (sMin, activeDelay - static_cast<float> (minJumpSamples));
                incomingDelay = findBestAlignedDelay (activeDelay, sMin, sMax);
                isCrossfading = true;
                xfadePos = 0;
                activeXfadeLen = std::max (32, xfadeLengthSamples);
            }
            else if (nearZeroShift && ! recenteredForZeroShift)
            {
                if (recentEnergy < 1e-6f && std::abs (activeDelay - centerDelaySamples) > 1.0f)
                {
                    activeDelay = centerDelaySamples;
                    incomingDelay = centerDelaySamples;
                    recenteredForZeroShift = true;
                }
                else if (std::abs (activeDelay - centerDelaySamples) > 0.004f * static_cast<float> (sampleRate))
                {
                    const float halfSpan = 0.010f * static_cast<float> (sampleRate);
                    const float sMin = std::max (lowTriggerSamples, centerDelaySamples - halfSpan);
                    const float sMax = std::min (highTriggerSamples, centerDelaySamples + halfSpan);
                    incomingDelay = findBestAlignedDelay (activeDelay, sMin, std::max (sMin, sMax));
                    isCrossfading = true;
                    xfadePos = 0;
                    activeXfadeLen = std::max (32, xfadeLengthSamples);
                    recenteredForZeroShift = true;
                }
                else
                {
                    recenteredForZeroShift = true;
                }
            }
        }

        float outSample = 0.0f;

        if (isCrossfading)
        {
            const float outActive = readDelayLineCubic (activeDelay);
            const float outIncoming = readDelayLineCubic (incomingDelay);
            const float t = static_cast<float> (xfadePos) / static_cast<float> (std::max (1, activeXfadeLen));
            const float wIn = 0.5f * (1.0f - std::cos (static_cast<float> (M_PI) * t));
            outSample = (1.0f - wIn) * outActive + wIn * outIncoming;

            activeDelay = std::clamp (activeDelay + delayDelta, minDelaySamples, maxDelay);
            incomingDelay = std::clamp (incomingDelay + delayDelta, minDelaySamples, maxDelay);

            if (++xfadePos >= activeXfadeLen)
            {
                activeDelay = incomingDelay;
                isCrossfading = false;
                xfadePos = 0;
            }
        }
        else
        {
            outSample = readDelayLineCubic (activeDelay);
            activeDelay = std::clamp (activeDelay + delayDelta, minDelaySamples, maxDelay);
        }

        outputSamples[i] = std::isfinite (outSample) ? outSample : 0.0f;

        delayBuffer[static_cast<size_t> (writeIndex)] = inSample;
        writeIndex = (writeIndex + 1) % bufferSize;
    }
}
