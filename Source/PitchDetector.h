/*
  ==============================================================================

    PitchDetector.h
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#pragma once

#include <vector>

/**
    A high-accuracy pitch detector based on the YIN Algorithm.
    It precisely extracts the fundamental frequency (F0) of vocal signals
    while eliminating octave errors and harmonic jumps.
*/
class PitchDetector
{
public:
    PitchDetector();
    ~PitchDetector();

    /** Prepares the detector with the given sample rate and window size. */
    void prepare (double sampleRate, int maxBlockSize);

    /** Adds new samples to the internal buffer and processes them (real-time mode). */
    void process (const float* inputSamples, int numSamples);

    /** Direct offline pitch detection on a specific window of samples. Returns Hz (0.0 if unvoiced/silent). */
    float detectPitchDirect (const float* samples, int numSamples);

    /** Returns the last detected frequency in Hz. Returns 0.0f if no pitch is detected (silence/unvoiced). */
    float getDetectedFrequency() const noexcept { return currentFrequency; }

    /** Returns if a valid pitch is currently detected. */
    bool isPitchDetected() const noexcept { return currentFrequency > 0.0f; }

private:
    double sampleRate { 44100.0 };
    float currentFrequency { 0.0f };

    // Ring buffer for streaming processing
    std::vector<float> ringBuffer;
    int writeIndex { 0 };
    int bufferSize { 2048 };

    // Internal buffers for YIN calculation
    std::vector<float> analysisBuffer;
    std::vector<float> yinBuffer;

    // YIN threshold (values around 0.10 to 0.15 provide optimal vocal tracking)
    float yinThreshold { 0.15f };

    // Power threshold to ignore silence (in RMS)
    float silenceThreshold { 0.010f };

    // Pitch range constraints for vocals (in Hz)
    float minFreq { 55.0f };   // A1
    float maxFreq { 1050.0f }; // C6
};
