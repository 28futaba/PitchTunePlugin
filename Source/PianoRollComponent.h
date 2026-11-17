/*
  ==============================================================================

    PianoRollComponent.h
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

//==============================================================================
/**
    High-reliability, instant tone player using Windows PlaySound API.
    Plays clean piano/synth audio directly to the default Windows sound endpoint.
    Zero background threads, zero polling, zero continuous buffer underruns.
*/
class PianoTonePlayer
{
public:
    PianoTonePlayer() = default;

    void playNote (int midiNote)
    {
        if (midiNote < 24 || midiNote > 108)
            return;

        auto& wav = getWavForNote (midiNote);
        if (! wav.empty())
        {
            ::PlaySoundA (reinterpret_cast<LPCSTR> (wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        }
    }

    void stop()
    {
        ::PlaySoundA (nullptr, nullptr, 0);
    }

private:
    std::unordered_map<int, std::vector<uint8_t>> cache;

    std::vector<uint8_t>& getWavForNote (int midiNote)
    {
        auto it = cache.find (midiNote);
        if (it != cache.end())
            return it->second;

        float freq = 440.0f * std::pow (2.0f, (static_cast<float> (midiNote) - 69.0f) / 12.0f);
        const int sampleRate = 44100;
        const float durationSec = 0.85f; // 850ms natural piano tone
        const int numSamples = static_cast<int> (sampleRate * durationSec);
        const int dataSize = numSamples * static_cast<int> (sizeof (int16_t));
        const int fileSize = 44 + dataSize;

        std::vector<uint8_t>& buffer = cache[midiNote];
        buffer.resize (fileSize);

        uint8_t* p = buffer.data();

        // 44-byte standard RIFF WAVE header
        std::memcpy (p + 0, "RIFF", 4);
        int32_t chunkSize = 36 + dataSize;
        std::memcpy (p + 4, &chunkSize, 4);
        std::memcpy (p + 8, "WAVEfmt ", 8);
        int32_t subchunk1Size = 16;
        std::memcpy (p + 16, &subchunk1Size, 4);
        int16_t audioFormat = 1; // PCM
        std::memcpy (p + 20, &audioFormat, 2);
        int16_t numChannels = 1; // Mono
        std::memcpy (p + 22, &numChannels, 2);
        int32_t sRate = sampleRate;
        std::memcpy (p + 24, &sRate, 4);
        int32_t byteRate = sampleRate * 2;
        std::memcpy (p + 28, &byteRate, 4);
        int16_t blockAlign = 2;
        std::memcpy (p + 32, &blockAlign, 2);
        int16_t bitsPerSample = 16;
        std::memcpy (p + 34, &bitsPerSample, 2);
        std::memcpy (p + 36, "data", 4);
        std::memcpy (p + 40, &dataSize, 4);

        int16_t* samples = reinterpret_cast<int16_t*> (p + 44);
        const double twoPi = 2.0 * 3.14159265358979323846;
        double phase = 0.0;
        double phaseDelta = (freq * twoPi) / sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            double t = static_cast<double> (i) / sampleRate;

            // Fast 5ms attack, natural piano-like exponential decay
            double env = (t < 0.005) ? (t / 0.005) : std::exp (-(t - 0.005) * 3.2);

            // Smooth fade-out at the end to guarantee zero DC offset / click
            if (i > numSamples - 1000)
            {
                double fade = static_cast<double> (numSamples - 1 - i) / 1000.0;
                env *= fade;
            }

            // Warm electric piano timbre (fundamental + 2nd, 3rd, 4th harmonics)
            double s1 = std::sin (phase);
            double s2 = std::sin (phase * 2.0);
            double s3 = std::sin (phase * 3.0);
            double s4 = std::sin (phase * 4.0);
            double val = (s1 * 0.65 + s2 * 0.20 + s3 * 0.10 + s4 * 0.05) * env;

            phase += phaseDelta;
            if (phase >= twoPi)
                phase -= twoPi;

            int32_t intVal = static_cast<int32_t> (val * 24000.0);
            samples[i] = static_cast<int16_t> (std::clamp (intVal, -32767, 32767));
        }

        return buffer;
    }
};
#endif

//==============================================================================
/**
    A timeline component that draws a grid of MIDI notes (vertical) against time in seconds (horizontal).
    It renders detected audio blocks as rectangles with pitch curve overlays and handles drag-to-transpose.
    Also handles clicking/dragging on the piano keys on the left margin to audition notes.
*/
class PianoRollComponent : public juce::Component,
                           public juce::Timer
{
public:
    PianoRollComponent (PitchTunePluginAudioProcessor& processor);
    ~PianoRollComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    
    // Timer callback to poll DAW playback position and refresh region status
    void timerCallback() override;

    // Mouse handlers to audition piano keys and drag/transpose/reset note blocks
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseExit (const juce::MouseEvent& event) override;
    void mouseDoubleClick (const juce::MouseEvent& event) override;

private:
    // Helper coordinates mappings
    float timeToX (double timeInSeconds) const noexcept;
    double xToTime (float x) const noexcept;
    float noteToY (int midiNote) const noexcept;
    float pitchToY (float midiPitch) const noexcept;
    int yToNote (float y) const noexcept;

    void triggerAudition (int midiNote);
    void endAudition();

    PitchTunePluginAudioProcessor& processor;
   #if JUCE_WINDOWS
    PianoTonePlayer tonePlayer;
   #endif

    // Bounds configuration
    const int numNotes { 61 };     // From C2 (36) to C7 (96)
    const int startNote { 36 };
    const float noteHeight { 18.0f };
    const float pixelsPerSecond { 120.0f };
    const float keyboardWidth { 60.0f };

    // Active piano keyboard preview state
    bool isPressingKeyboard { false };
    int pressedKeyboardNote { -1 };

    // Active drag-and-drop state (stored by index under region notesLock)
    int activeDragNoteIndex { -1 };
    PitchTunePluginPlaybackRegion* activeDragRegion { nullptr };
    int initialDragNoteMidi { 0 };
    juce::Point<int> dragStartPos;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};
