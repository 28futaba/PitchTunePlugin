#include "PitchShifter.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PitchShifter::PitchShifter()
{
}

PitchShifter::~PitchShifter()
{
}

void PitchShifter::prepare (double sampleRateIn, int maxBlockSize)
{
    sampleRate = sampleRateIn;
    
    // Set a window size of approximately 40ms.
    windowSizeSamples = static_cast<float> (sampleRate * 0.040); 
    
    bufferSize = 16384;
    delayBuffer.assign (bufferSize, 0.0f);
    
    writeIndex = 0;
    phase1 = 0.0f;
    phase2 = 0.5f; // 180 degrees out of phase
    
    pitchShiftSemitones = 0.0f;
    pitchShiftRatio = 1.0f;
}

void PitchShifter::setPitchShift (float semitones)
{
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

float PitchShifter::readDelayLine (float delayInSamples)
{
    float readPos = static_cast<float> (writeIndex) - delayInSamples;
    
    while (readPos < 0.0f)
        readPos += static_cast<float> (bufferSize);
    while (readPos >= static_cast<float> (bufferSize))
        readPos -= static_cast<float> (bufferSize);

    int idx1 = static_cast<int> (readPos);
    int idx2 = (idx1 + 1) % bufferSize;
    float frac = readPos - static_cast<float> (idx1);

    return (1.0f - frac) * delayBuffer[idx1] + frac * delayBuffer[idx2];
}

void PitchShifter::process (const float* inputSamples, float* outputSamples, int numSamples)
{
    if (numSamples <= 0)
        return;

    // Check input energy (silence gate) to avoid modulating floor noise
    float maxAbs = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        float val = std::abs (inputSamples[i]);
        if (val > maxAbs)
            maxAbs = val;
    }

    // Direct bypass when pitch shift is 0 semitones or input is silent
    if (std::abs (pitchShiftSemitones) < 0.01f || maxAbs < 1e-4f)
    {
        if (inputSamples != outputSamples)
            std::copy (inputSamples, inputSamples + numSamples, outputSamples);

        // Keep delay line updated so transitions to pitch shift are smooth
        for (int i = 0; i < numSamples; ++i)
        {
            delayBuffer[writeIndex] = inputSamples[i];
            writeIndex = (writeIndex + 1) % bufferSize;
        }
        return;
    }

    float s = pitchShiftRatio;
    float phaseStep = (1.0f - s) / windowSizeSamples;
    const float minDelayOffset = 32.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        // Must preserve raw input before overwriting outputSamples (in-place buffer safety)
        float inSample = inputSamples[i];

        phase1 += phaseStep;
        phase2 += phaseStep;

        if (phase1 >= 1.0f) phase1 -= 1.0f;
        else if (phase1 < 0.0f) phase1 += 1.0f;

        if (phase2 >= 1.0f) phase2 -= 1.0f;
        else if (phase2 < 0.0f) phase2 += 1.0f;

        float delay1 = minDelayOffset + phase1 * windowSizeSamples;
        float delay2 = minDelayOffset + phase2 * windowSizeSamples;

        float out1 = readDelayLine (delay1);
        float out2 = readDelayLine (delay2);

        float w1 = std::sin (static_cast<float> (M_PI) * phase1);
        float w2 = std::sin (static_cast<float> (M_PI) * phase2);
        float wSum = w1 + w2;
        if (wSum > 1e-6f)
        {
            w1 /= wSum;
            w2 /= wSum;
        }

        outputSamples[i] = (w1 * out1) + (w2 * out2);

        delayBuffer[writeIndex] = inSample;
        writeIndex = (writeIndex + 1) % bufferSize;
    }
}
