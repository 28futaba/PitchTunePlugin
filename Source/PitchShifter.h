/*
  ==============================================================================

    PitchShifter.h
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#pragma once

#include <vector>

/**
    A real-time monophonic pitch shifter using WSOLA-style waveform-aligned
    crossfading, Catmull-Rom cubic interpolation, and stationary zero-shift parking.
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

    /** Returns the constant center-tap delay latency in samples (~31ms). */
    int getLatencySamples() const noexcept;

    /** Resets the delay line and pre-fills it with lookahead samples for zero-latency ARA playback. */
    void prime (const float* samples, int numSamples) noexcept;

private:
    float readDelayLineCubic (float delayInSamples) const noexcept;
    float findBestAlignedDelay (float refDelay, float searchMinDelay, float searchMaxDelay) const noexcept;

    double sampleRate { 44100.0 };
    float pitchShiftSemitones { 0.0f };
    float pitchShiftRatio { 1.0f };

    // Circular delay buffer
    std::vector<float> delayBuffer;
    int bufferSize { 16384 };
    int writeIndex { 0 };

    // WSOLA geometry (in samples, scaled to sampleRate in prepare())
    float minDelaySamples { 16.0f };
    float lowTriggerSamples { 617.0f };
    float centerDelaySamples { 1367.0f };
    float highTriggerSamples { 2117.0f };
    int minJumpSamples { 617 };
    int maxJumpSamples { 1499 };
    int xfadeLengthSamples { 441 };
    int corrWindowSamples { 353 };

    // Active tap and WSOLA crossfade state
    float activeDelay { 1367.0f };
    float incomingDelay { 1367.0f };
    bool isCrossfading { false };
    int xfadePos { 0 };
    int activeXfadeLen { 441 };
    bool recenteredForZeroShift { true };
    float recentEnergy { 0.0f };
};
