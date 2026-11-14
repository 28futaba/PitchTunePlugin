#pragma once

#include <vector>

/**
    A real-time monophonic pitch shifter using a dual-tap crossfading delay line.
    It shifts the pitch in semitones without changing the speed of the output.
*/
class PitchShifter
{
public:
    PitchShifter();
    ~PitchShifter();

    /** Prepares the buffer and parameters for processing. */
    void prepare (double sampleRate, int maxBlockSize);

    /** Sets the target pitch shift in semitones (e.g. +1.0 for a half step up, -2.0 for a whole step down). */
    void setPitchShift (float semitones);

    /** Processes a block of samples (mono). */
    void process (const float* inputSamples, float* outputSamples, int numSamples);

    /** Resets the internal state. */
    void reset();

private:
    // Helper to read from the delay line with linear interpolation
    float readDelayLine (float delayInSamples);

    double sampleRate { 44100.0 };
    float pitchShiftSemitones { 0.0f };
    float pitchShiftRatio { 1.0f };

    // Delay buffer
    std::vector<float> delayBuffer;
    int bufferSize { 16384 };
    int writeIndex { 0 };

    // Size of the delay window in samples (around 40ms)
    float windowSizeSamples { 1764.0f };

    // Phases for the two reading taps (0.0 to 1.0)
    float phase1 { 0.0f };
    float phase2 { 0.5f }; // Out of phase by 180 degrees
};
