/*
  ==============================================================================

    PluginProcessor.cpp
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>
#include <set>

namespace
{
    constexpr int32_t kArchiveMagic   = 0x50545031; // "PTP1"
    constexpr int32_t kArchiveVersion = 2;

    void sanitizeAudioBuffer (juce::AudioBuffer<float>& buffer) noexcept
    {
        const int numCh = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();
        for (int ch = 0; ch < numCh; ++ch)
        {
            float* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                if (! std::isfinite (data[i]))
                    data[i] = 0.0f;
            }
        }
    }

    float computeMedianValue (std::vector<float>& scratch)
    {
        if (scratch.empty())
            return 0.0f;

        const size_t mid = scratch.size() / 2;
        std::nth_element (scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t> (mid), scratch.end());
        float median = scratch[mid];
        if ((scratch.size() % 2) == 0 && mid > 0)
        {
            const float leftMax = *std::max_element (scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t> (mid));
            median = 0.5f * (leftMax + median);
        }
        return median;
    }

    float computeNoteShiftAtTime (const PitchTunePluginPlaybackRegion::Note& note,
                                  double timeInSeconds,
                                  float correctionAmount) noexcept
    {
        const float manualShift = static_cast<float> (note.targetMidiNote - note.originalMidiNote)
                                  + (note.targetCentsOffset / 100.0f);
        const float clampedCorrection = std::clamp (correctionAmount, 0.0f, 1.0f);

        const double noteDuration = note.endTime - note.startTime;
        const double attackFade = std::min (0.045, std::max (0.0, noteDuration * 0.30));
        const double releaseFade = std::min (0.035, std::max (0.0, noteDuration * 0.25));

        float env = 1.0f;
        if (attackFade > 1e-6 && (timeInSeconds - note.startTime) < attackFade)
        {
            const double u = std::clamp ((timeInSeconds - note.startTime) / attackFade, 0.0, 1.0);
            env *= 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * static_cast<float> (u)));
        }
        if (releaseFade > 1e-6 && (note.endTime - timeInSeconds) < releaseFade)
        {
            const double u = std::clamp ((note.endTime - timeInSeconds) / releaseFade, 0.0, 1.0);
            env *= 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * static_cast<float> (u)));
        }

        if (clampedCorrection <= 0.0f || note.pitchCurve.empty())
        {
            const float out = manualShift * env;
            return std::isfinite (out) ? out : 0.0f;
        }

        float currentMidiPitch = static_cast<float> (note.originalMidiNote);
        bool hasValidMidi = false;

        if (note.pitchCurve.size() == 1 || noteDuration <= 1e-6)
        {
            const float f = note.pitchCurve.front();
            if (f > 0.0f && std::isfinite (f))
            {
                currentMidiPitch = 69.0f + 12.0f * std::log2 (f / 440.0f);
                hasValidMidi = std::isfinite (currentMidiPitch);
            }
        }
        else
        {
            const double normPos = std::clamp ((timeInSeconds - note.startTime) / noteDuration, 0.0, 1.0);
            const double floatIdx = normPos * static_cast<double> (note.pitchCurve.size() - 1);
            const size_t idx0 = static_cast<size_t> (std::floor (floatIdx));
            const size_t idx1 = std::min (idx0 + 1, note.pitchCurve.size() - 1);
            const float frac = static_cast<float> (floatIdx - static_cast<double> (idx0));
            const float f0 = note.pitchCurve[idx0];
            const float f1 = note.pitchCurve[idx1];

            if (f0 > 0.0f && f1 > 0.0f && std::isfinite (f0) && std::isfinite (f1))
            {
                const float m0 = 69.0f + 12.0f * std::log2 (f0 / 440.0f);
                const float m1 = 69.0f + 12.0f * std::log2 (f1 / 440.0f);
                currentMidiPitch = m0 + frac * (m1 - m0);
                hasValidMidi = std::isfinite (currentMidiPitch);
            }
            else
            {
                const float fValid = (f0 > 0.0f && std::isfinite (f0)) ? f0 : f1;
                if (fValid > 0.0f && std::isfinite (fValid))
                {
                    currentMidiPitch = 69.0f + 12.0f * std::log2 (fValid / 440.0f);
                    hasValidMidi = std::isfinite (currentMidiPitch);
                }
            }
        }

        if (hasValidMidi)
        {
            const float deviation = std::clamp (currentMidiPitch - static_cast<float> (note.originalMidiNote), -2.0f, 2.0f);
            const float totalShift = (manualShift - (clampedCorrection * deviation)) * env;
            return std::isfinite (totalShift) ? totalShift : 0.0f;
        }

        const float fallbackOut = manualShift * env;
        return std::isfinite (fallbackOut) ? fallbackOut : 0.0f;
    }
}

//==============================================================================
// ARA Playback Region Implementation
//==============================================================================

PitchTunePluginPlaybackRegion::PitchTunePluginPlaybackRegion (juce::ARAAudioModification* modification, ARA::ARAPlaybackRegionHostRef hostRef)
    : juce::ARAPlaybackRegion (modification, hostRef)
{
    if (auto* source = getAudioModification()->getAudioSource())
    {
        sourceSampleRate = source->getSampleRate();
        sourceLengthSamples = static_cast<int64_t> (source->getSampleCount());
    }
}

PitchTunePluginPlaybackRegion::~PitchTunePluginPlaybackRegion()
{
    requestAbortAnalysis();

    if (auto* mod = getAudioModification())
    {
        if (auto* source = mod->getAudioSource())
        {
            if (auto* doc = source->getDocument())
            {
                if (auto* dc = doc->getDocumentController())
                {
                    if (auto* customDoc = juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PitchTunePluginDocumentController> (dc))
                    {
                        customDoc->unregisterRegion (this);
                    }
                }
            }
        }
    }
}

void PitchTunePluginPlaybackRegion::restoreNotes (const std::vector<Note>& restoredNotes)
{
    std::vector<Note> sortedNotes = restoredNotes;
    std::sort (sortedNotes.begin(), sortedNotes.end(),
               [] (const Note& a, const Note& b) { return a.startTime < b.startTime; });

    const juce::SpinLock::ScopedLockType sl (notesLock);
    notes = std::move (sortedNotes);
    analysisCompleted.store (true);
    incrementNotesVersion();
}

std::vector<PitchTunePluginPlaybackRegion::Note> PitchTunePluginPlaybackRegion::getNotesCopy() const
{
    const juce::SpinLock::ScopedLockType sl (notesLock);
    return notes;
}

bool PitchTunePluginPlaybackRegion::splitNoteAtTime (size_t noteIndex, double splitTimeSeconds)
{
    const juce::SpinLock::ScopedLockType sl (notesLock);
    if (noteIndex >= notes.size())
        return false;

    const Note origNote = notes[noteIndex];
    if (splitTimeSeconds - origNote.startTime < 0.04 || origNote.endTime - splitTimeSeconds < 0.04)
        return false;

    Note leftNote;
    leftNote.startTime = origNote.startTime;
    leftNote.endTime = splitTimeSeconds;

    Note rightNote;
    rightNote.startTime = splitTimeSeconds;
    rightNote.endTime = origNote.endTime;

    if (! origNote.pitchCurve.empty())
    {
        const double totalDuration = origNote.endTime - origNote.startTime;
        const double ratio = (totalDuration > 1e-6)
                                 ? std::clamp ((splitTimeSeconds - origNote.startTime) / totalDuration, 0.0, 1.0)
                                 : 0.5;
        const size_t splitIdx = std::clamp<size_t> (
            static_cast<size_t> (std::llround (ratio * static_cast<double> (origNote.pitchCurve.size()))),
            size_t { 0 },
            origNote.pitchCurve.size());

        leftNote.pitchCurve.assign (origNote.pitchCurve.begin(), origNote.pitchCurve.begin() + static_cast<std::ptrdiff_t> (splitIdx));
        rightNote.pitchCurve.assign (origNote.pitchCurve.begin() + static_cast<std::ptrdiff_t> (splitIdx), origNote.pitchCurve.end());
    }

    auto computeMedianMidi = [] (const std::vector<float>& curve, int fallbackMidi) -> int
    {
        std::vector<float> midiVals;
        midiVals.reserve (curve.size());
        for (float freq : curve)
        {
            if (freq > 0.0f && std::isfinite (freq))
            {
                const float m = 69.0f + 12.0f * std::log2 (freq / 440.0f);
                if (std::isfinite (m))
                    midiVals.push_back (m);
            }
        }
        return ! midiVals.empty() ? static_cast<int> (std::round (computeMedianValue (midiVals))) : fallbackMidi;
    };

    const bool wasUnedited = (origNote.targetMidiNote == origNote.originalMidiNote
                              && std::abs (origNote.targetCentsOffset) < 0.5f);

    if (wasUnedited)
    {
        const int leftMidi = computeMedianMidi (leftNote.pitchCurve, origNote.originalMidiNote);
        const int rightMidi = computeMedianMidi (rightNote.pitchCurve, origNote.originalMidiNote);

        leftNote.originalMidiNote = leftMidi;
        leftNote.targetMidiNote = leftMidi;
        leftNote.targetCentsOffset = 0.0f;

        rightNote.originalMidiNote = rightMidi;
        rightNote.targetMidiNote = rightMidi;
        rightNote.targetCentsOffset = 0.0f;
    }
    else
    {
        leftNote.originalMidiNote = origNote.originalMidiNote;
        leftNote.targetMidiNote = origNote.targetMidiNote;
        leftNote.targetCentsOffset = origNote.targetCentsOffset;

        rightNote.originalMidiNote = origNote.originalMidiNote;
        rightNote.targetMidiNote = origNote.targetMidiNote;
        rightNote.targetCentsOffset = origNote.targetCentsOffset;
    }

    notes[noteIndex] = std::move (leftNote);
    notes.insert (notes.begin() + static_cast<std::ptrdiff_t> (noteIndex + 1), std::move (rightNote));
    incrementNotesVersion();
    return true;
}

bool PitchTunePluginPlaybackRegion::mergeNoteWithAdjacent (size_t noteIndex, double clickedSourceTime)
{
    const juce::SpinLock::ScopedLockType sl (notesLock);
    if (noteIndex >= notes.size() || notes.size() < 2)
        return false;

    const bool hasPrev = (noteIndex > 0);
    const bool hasNext = (noteIndex + 1 < notes.size());
    if (! hasPrev && ! hasNext)
        return false;

    const double prevGap = hasPrev ? (notes[noteIndex].startTime - notes[noteIndex - 1].endTime) : 1e9;
    const double nextGap = hasNext ? (notes[noteIndex + 1].startTime - notes[noteIndex].endTime) : 1e9;
    const double distLeft = std::abs (clickedSourceTime - notes[noteIndex].startTime);
    const double distRight = std::abs (notes[noteIndex].endTime - clickedSourceTime);

    size_t neighborIndex = noteIndex;
    if (hasPrev && hasNext)
    {
        if ((prevGap < 0.5 && nextGap < 0.5) || (prevGap >= 0.5 && nextGap >= 0.5))
            neighborIndex = (distRight <= distLeft) ? (noteIndex + 1) : (noteIndex - 1);
        else if (nextGap < 0.5)
            neighborIndex = noteIndex + 1;
        else
            neighborIndex = noteIndex - 1;
    }
    else
    {
        neighborIndex = hasNext ? (noteIndex + 1) : (noteIndex - 1);
    }

    const size_t firstIdx = std::min (noteIndex, neighborIndex);
    const size_t secondIdx = std::max (noteIndex, neighborIndex);
    const Note clickedCopy = notes[noteIndex];

    Note merged;
    merged.startTime = notes[firstIdx].startTime;
    merged.endTime = notes[secondIdx].endTime;
    merged.originalMidiNote = clickedCopy.originalMidiNote;
    merged.targetMidiNote = clickedCopy.targetMidiNote;
    merged.targetCentsOffset = clickedCopy.targetCentsOffset;
    merged.pitchCurve = std::move (notes[firstIdx].pitchCurve);
    merged.pitchCurve.insert (merged.pitchCurve.end(),
                                notes[secondIdx].pitchCurve.begin(),
                                notes[secondIdx].pitchCurve.end());

    notes[firstIdx] = std::move (merged);
    notes.erase (notes.begin() + static_cast<std::ptrdiff_t> (secondIdx));
    incrementNotesVersion();
    return true;
}

double PitchTunePluginPlaybackRegion::playbackToSourceTime (double playbackTime) const noexcept
{
    const double playbackDuration = getDurationInPlaybackTime();
    const double modDuration = getDurationInAudioModificationTime();
    const double scale = (playbackDuration > 1e-6 && modDuration > 1e-6) ? (modDuration / playbackDuration) : 1.0;
    return getStartInAudioModificationTime() + (playbackTime - getStartInPlaybackTime()) * scale;
}

double PitchTunePluginPlaybackRegion::sourceToPlaybackTime (double sourceTime) const noexcept
{
    const double playbackDuration = getDurationInPlaybackTime();
    const double modDuration = getDurationInAudioModificationTime();
    const double scale = (playbackDuration > 1e-6 && modDuration > 1e-6) ? (modDuration / playbackDuration) : 1.0;
    return getStartInPlaybackTime() + (sourceTime - getStartInAudioModificationTime()) / scale;
}

double PitchTunePluginPlaybackRegion::getMaxTimelineSeconds() const noexcept
{
    const double playbackEnd = getEndInPlaybackTime();
    const double modStart = getStartInAudioModificationTime();
    const double modEnd = getEndInAudioModificationTime();
    const bool hasValidPlayback = (getDurationInPlaybackTime() > 1e-6);
    const bool hasValidModRange = (modEnd > modStart + 1e-4);

    double maxSec = hasValidPlayback ? playbackEnd : modEnd;

    if (sourceSampleRate > 0.0 && sourceLengthSamples > 0)
    {
        const double sourceDuration = static_cast<double> (sourceLengthSamples) / sourceSampleRate;
        if (! hasValidPlayback)
            maxSec = std::max (maxSec, sourceDuration);
    }

    const juce::SpinLock::ScopedTryLockType stl (notesLock);
    if (stl.isLocked())
    {
        for (const auto& note : notes)
        {
            if (hasValidModRange && (note.endTime <= modStart || note.startTime >= modEnd))
                continue;

            const double visEndSrc = hasValidModRange ? std::min (note.endTime, modEnd) : note.endTime;
            maxSec = std::max (maxSec, sourceToPlaybackTime (visEndSrc));
        }
    }

    return maxSec;
}

void PitchTunePluginPlaybackRegion::analyzeAudio()
{
    if (analysisCompleted.load() || abortAnalysis.load())
        return;

    auto* mod = getAudioModification();
    auto* source = (mod != nullptr) ? mod->getAudioSource() : nullptr;
    if (source == nullptr || ! source->isSampleAccessEnabled())
        return;

    juce::ARAAudioSourceReader reader (source);
    if (! reader.isValid())
        return;

    sourceLengthSamples = reader.lengthInSamples;
    const double sr = (reader.sampleRate > 0.0) ? reader.sampleRate : sourceSampleRate;
    sourceSampleRate = sr;

    if (sourceLengthSamples <= 0 || sr <= 0.0)
        return;

    const int windowSize = 2048;
    const int hopSize = 256; // High time resolution: ~5.8ms at 44.1kHz
    const double hopSec = static_cast<double> (hopSize) / sr;
    const double totalSourceSec = static_cast<double> (sourceLengthSamples) / sr;
    constexpr size_t kMinNoteFrames = 8;           // ~46ms minimum note length
    constexpr int kTransitionFramesRequired = 5;   // ~29ms hysteresis to split legato pitch jumps
    constexpr float kLegatoSplitThreshold = 0.75f; // Semitones deviation required to trigger split

    juce::AudioBuffer<float> windowBuffer (1, windowSize);
    PitchDetector analysisDetector;
    analysisDetector.prepare (sr, windowSize);

    // Pass 1: Extract raw pitch frames across the audio source
    const size_t approxFrames = (sourceLengthSamples >= windowSize)
                                    ? static_cast<size_t> ((sourceLengthSamples - windowSize) / hopSize + 1)
                                    : size_t { 0 };
    std::vector<float> rawMidi;
    std::vector<double> frameTimes;
    rawMidi.reserve (approxFrames);
    frameTimes.reserve (approxFrames);

    for (int64_t currentSample = 0; currentSample + windowSize <= sourceLengthSamples; currentSample += hopSize)
    {
        if (abortAnalysis.load() || analysisCompleted.load())
            return;

        if (! reader.read (&windowBuffer, 0, windowSize, currentSample, true, false))
            break;

        const float freq = analysisDetector.detectPitchDirect (windowBuffer.getReadPointer (0), windowSize);
        const double currentTime = static_cast<double> (currentSample + (windowSize / 2)) / sr;

        float midiPitch = 0.0f;
        if (freq > 0.0f && std::isfinite (freq))
        {
            const float m = 69.0f + 12.0f * std::log2 (freq / 440.0f);
            if (std::isfinite (m))
                midiPitch = m;
        }

        rawMidi.push_back (midiPitch);
        frameTimes.push_back (currentTime);
    }

    if (abortAnalysis.load() || analysisCompleted.load())
        return;

    // Pass 2: Suppress isolated unvoiced blips, apply 5-frame median filter for octave spikes,
    // and lightly smooth consecutive within-note jitter while keeping legato transitions sharp.
    const size_t numFrames = rawMidi.size();
    std::vector<float> medianMidi (numFrames, 0.0f);
    std::vector<float> medianScratch;
    medianScratch.reserve (5);

    for (size_t i = 0; i < numFrames; ++i)
    {
        medianScratch.clear();
        const size_t winStart = (i >= 2) ? (i - 2) : 0;
        const size_t winEnd = std::min (numFrames - 1, i + 2);

        for (size_t j = winStart; j <= winEnd; ++j)
        {
            if (rawMidi[j] > 0.0f)
                medianScratch.push_back (rawMidi[j]);
        }

        if (medianScratch.size() >= 3 && rawMidi[i] > 0.0f)
            medianMidi[i] = computeMedianValue (medianScratch);
    }

    std::vector<float> smoothedMidi (numFrames, 0.0f);
    for (size_t i = 0; i < numFrames; ++i)
    {
        const float center = medianMidi[i];
        if (center <= 0.0f)
            continue;

        float weightedSum = 0.50f * center;
        float weightSum = 0.50f;

        if (i > 0 && medianMidi[i - 1] > 0.0f && std::abs (medianMidi[i - 1] - center) < 1.5f)
        {
            weightedSum += 0.25f * medianMidi[i - 1];
            weightSum += 0.25f;
        }
        if (i + 1 < numFrames && medianMidi[i + 1] > 0.0f && std::abs (medianMidi[i + 1] - center) < 1.5f)
        {
            weightedSum += 0.25f * medianMidi[i + 1];
            weightSum += 0.25f;
        }

        smoothedMidi[i] = weightedSum / weightSum;
    }

    // Pass 3: Segment cleaned trajectory into discrete notes using hysteresis
    std::vector<Note> detectedNotes;
    std::vector<float> interiorScratch;

    auto pushNoteIfValid = [&] (double startT, double endT,
                                const std::vector<float>& curve,
                                const std::vector<float>& logPitches)
    {
        if (curve.size() < kMinNoteFrames || logPitches.empty())
            return;

        interiorScratch.clear();
        if (logPitches.size() >= 10)
        {
            const size_t trim = logPitches.size() / 5;
            interiorScratch.assign (logPitches.begin() + static_cast<std::ptrdiff_t> (trim),
                                    logPitches.end() - static_cast<std::ptrdiff_t> (trim));
        }
        else
        {
            interiorScratch = logPitches;
        }

        const float medianNoteMidi = computeMedianValue (interiorScratch);
        const int midiNote = static_cast<int> (std::round (medianNoteMidi));

        Note newNote;
        newNote.startTime = startT;
        newNote.endTime = std::max (startT, std::min (totalSourceSec, endT));
        newNote.originalMidiNote = midiNote;
        newNote.targetMidiNote = midiNote; // Default: no pitch change
        newNote.targetCentsOffset = 0.0f;
        newNote.pitchCurve = curve;

        detectedNotes.push_back (std::move (newNote));
    };

    bool inNote = false;
    double noteStartTime = 0.0;
    std::vector<float> currentPitchCurve;
    std::vector<float> activeLogPitches;
    std::vector<double> activeFrameTimes;

    int transitionCount = 0;
    int candidateMidi = -1;

    for (size_t i = 0; i < numFrames; ++i)
    {
        if (abortAnalysis.load() || analysisCompleted.load())
            return;

        const float midiPitch = smoothedMidi[i];
        const double currentTime = frameTimes[i];

        if (midiPitch > 0.0f)
        {
            const float freq = 440.0f * std::pow (2.0f, (midiPitch - 69.0f) / 12.0f);
            const int frameMidi = static_cast<int> (std::round (midiPitch));

            if (! inNote)
            {
                inNote = true;
                noteStartTime = currentTime;
                currentPitchCurve.clear();
                activeLogPitches.clear();
                activeFrameTimes.clear();
                transitionCount = 0;
                candidateMidi = -1;

                currentPitchCurve.push_back (freq);
                activeLogPitches.push_back (midiPitch);
                activeFrameTimes.push_back (currentTime);
            }
            else
            {
                const size_t stableLen = activeLogPitches.size() - static_cast<size_t> (transitionCount);

                if (stableLen >= kMinNoteFrames)
                {
                    float stableSum = 0.0f;
                    for (size_t k = 0; k < stableLen; ++k)
                        stableSum += activeLogPitches[k];
                    const float stableMean = stableSum / static_cast<float> (stableLen);
                    const int currentNoteMidi = static_cast<int> (std::round (stableMean));

                    if (frameMidi != currentNoteMidi
                        && std::abs (midiPitch - static_cast<float> (currentNoteMidi)) >= kLegatoSplitThreshold)
                    {
                        if (transitionCount == 0 || frameMidi != candidateMidi)
                        {
                            candidateMidi = frameMidi;
                            transitionCount = 1;
                        }
                        else
                        {
                            ++transitionCount;
                        }
                    }
                    else
                    {
                        transitionCount = 0;
                        candidateMidi = -1;
                    }
                }
                else
                {
                    transitionCount = 0;
                    candidateMidi = -1;
                }

                currentPitchCurve.push_back (freq);
                activeLogPitches.push_back (midiPitch);
                activeFrameTimes.push_back (currentTime);

                if (transitionCount >= kTransitionFramesRequired)
                {
                    const size_t splitIdx = activeLogPitches.size() - static_cast<size_t> (kTransitionFramesRequired);
                    const double splitTime = activeFrameTimes[splitIdx];

                    std::vector<float> prefixCurve (currentPitchCurve.begin(), currentPitchCurve.begin() + static_cast<std::ptrdiff_t> (splitIdx));
                    std::vector<float> prefixLogPitches (activeLogPitches.begin(), activeLogPitches.begin() + static_cast<std::ptrdiff_t> (splitIdx));
                    pushNoteIfValid (noteStartTime, splitTime, prefixCurve, prefixLogPitches);

                    currentPitchCurve.erase (currentPitchCurve.begin(), currentPitchCurve.begin() + static_cast<std::ptrdiff_t> (splitIdx));
                    activeLogPitches.erase (activeLogPitches.begin(), activeLogPitches.begin() + static_cast<std::ptrdiff_t> (splitIdx));
                    activeFrameTimes.erase (activeFrameTimes.begin(), activeFrameTimes.begin() + static_cast<std::ptrdiff_t> (splitIdx));

                    noteStartTime = splitTime;
                    transitionCount = 0;
                    candidateMidi = -1;
                }
            }
        }
        else
        {
            if (inNote)
            {
                inNote = false;
                const double noteEndTime = activeFrameTimes.back() + hopSec;
                pushNoteIfValid (noteStartTime, noteEndTime, currentPitchCurve, activeLogPitches);
                currentPitchCurve.clear();
                activeLogPitches.clear();
                activeFrameTimes.clear();
                transitionCount = 0;
                candidateMidi = -1;
            }
        }
    }

    // Process trailing note if active at audio end
    if (inNote && ! activeFrameTimes.empty())
    {
        const double noteEndTime = activeFrameTimes.back() + hopSec;
        pushNoteIfValid (noteStartTime, noteEndTime, currentPitchCurve, activeLogPitches);
    }

    if (abortAnalysis.load())
        return;

    const juce::SpinLock::ScopedLockType sl (notesLock);
    if (! analysisCompleted.load())
    {
        notes = std::move (detectedNotes);
        analysisCompleted.store (true);
        incrementNotesVersion();
    }
}

bool PitchTunePluginPlaybackRegion::computePitchShiftsForBlock (double startSourceTime,
                                                                 double sourceTimeStepPerSample,
                                                                 int numSamples,
                                                                 float correctionAmount,
                                                                 float* outShifts,
                                                                 float& lastValidShift) const noexcept
{
    if (outShifts == nullptr || numSamples <= 0)
        return false;

    if (! analysisCompleted.load())
    {
        std::fill_n (outShifts, numSamples, 0.0f);
        lastValidShift = 0.0f;
        return true;
    }

    const juce::SpinLock::ScopedTryLockType stl (notesLock);
    if (! stl.isLocked())
    {
        std::fill_n (outShifts, numSamples, std::isfinite (lastValidShift) ? lastValidShift : 0.0f);
        return false;
    }

    if (notes.empty())
    {
        std::fill_n (outShifts, numSamples, 0.0f);
        lastValidShift = 0.0f;
        return true;
    }

    size_t noteIdx = 0;
    if (sourceTimeStepPerSample >= 0.0)
    {
        auto it = std::lower_bound (notes.begin(), notes.end(), startSourceTime,
                                    [] (const Note& n, double t) { return n.endTime < t; });
        noteIdx = static_cast<size_t> (std::distance (notes.begin(), it));
    }

    for (int s = 0; s < numSamples; ++s)
    {
        const double t = startSourceTime + static_cast<double> (s) * sourceTimeStepPerSample;

        if (sourceTimeStepPerSample >= 0.0)
        {
            while (noteIdx < notes.size() && notes[noteIdx].endTime < t)
                ++noteIdx;
        }
        else
        {
            auto it = std::lower_bound (notes.begin(), notes.end(), t,
                                        [] (const Note& n, double timeVal) { return n.endTime < timeVal; });
            noteIdx = static_cast<size_t> (std::distance (notes.begin(), it));
        }

        float shift = 0.0f;
        if (noteIdx < notes.size())
        {
            const auto& note = notes[noteIdx];
            if (t >= note.startTime && t <= note.endTime)
                shift = computeNoteShiftAtTime (note, t, correctionAmount);
        }

        outShifts[s] = shift;
        lastValidShift = shift;
    }

    return true;
}

float PitchTunePluginPlaybackRegion::getPitchShiftSemitones (double timeInSeconds, float correctionAmount) const
{
    if (! analysisCompleted.load())
        return 0.0f;

    const juce::SpinLock::ScopedTryLockType stl (notesLock);
    if (! stl.isLocked())
        return 0.0f;

    auto it = std::lower_bound (notes.begin(), notes.end(), timeInSeconds,
                                [] (const Note& n, double t) { return n.endTime < t; });
    if (it != notes.end() && timeInSeconds >= it->startTime && timeInSeconds <= it->endTime)
        return computeNoteShiftAtTime (*it, timeInSeconds, correctionAmount);

    return 0.0f;
}

//==============================================================================
// ARA Playback Renderer Implementation
//==============================================================================

PitchTunePluginPlaybackRenderer::PitchTunePluginPlaybackRenderer (ARA::PlugIn::DocumentController* dc)
    : juce::ARAPlaybackRenderer (dc)
{
}

PitchTunePluginPlaybackRenderer::~PitchTunePluginPlaybackRenderer()
{
    cancelPendingUpdate();

    if (auto* customDoc = juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PitchTunePluginDocumentController> (getDocumentController()))
        customDoc->unregisterPlaybackRenderer (this);

    releaseResources();
}

void PitchTunePluginPlaybackRenderer::didAddPlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    juce::ignoreUnused (playbackRegion);
    triggerAsyncUpdate();
}

void PitchTunePluginPlaybackRenderer::willRemovePlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    juce::ignoreUnused (playbackRegion);
    triggerAsyncUpdate();
}

void PitchTunePluginPlaybackRenderer::handleAsyncUpdate()
{
    rebuildReaders();
}

void PitchTunePluginPlaybackRenderer::rebuildReaders()
{
    std::map<juce::ARAAudioSource*, PossiblyBufferedReader> oldReaders;

    {
        const juce::SpinLock::ScopedLockType sl (readerLock);
        oldReaders.swap (audioSourceReaders);
    }

    // Destroy outside readerLock so TimeSliceThread never deadlocks against the audio thread
    oldReaders.clear();

    if (! isPreparedFlag)
        return;

    std::map<juce::ARAAudioSource*, PossiblyBufferedReader> newReaders;
    const int readAheadSize = juce::jmax (8 * currentSamplesPerBlock, juce::roundToInt (3.0 * currentSampleRate));

    for (auto* region : getPlaybackRegions())
    {
        if (region == nullptr)
            continue;

        auto* mod = region->getAudioModification();
        auto* audioSource = (mod != nullptr) ? mod->getAudioSource() : nullptr;
        if (audioSource == nullptr || ! audioSource->isSampleAccessEnabled())
            continue;

        if (newReaders.find (audioSource) != newReaders.end())
            continue;

        auto rawReader = std::make_unique<juce::ARAAudioSourceReader> (audioSource);
        if (! rawReader->isValid())
            continue;

        if (! useBufferedReaders)
        {
            newReaders.emplace (audioSource, PossiblyBufferedReader { std::move (rawReader) });
        }
        else
        {
            newReaders.emplace (audioSource, PossiblyBufferedReader {
                std::make_unique<juce::BufferingAudioReader> (rawReader.release(),
                                                              *sharedTimesliceThread,
                                                              readAheadSize) });
        }
    }

    {
        const juce::SpinLock::ScopedLockType sl (readerLock);
        audioSourceReaders.swap (newReaders);
    }

    if (useBufferedReaders)
        sharedTimesliceThread->notify();
}

void PitchTunePluginPlaybackRenderer::prepareToPlay (double sampleRate, int maximumSamplesPerBlock, int numChannels,
                                                    juce::AudioProcessor::ProcessingPrecision precision,
                                                    AlwaysNonRealtime alwaysNonRealtime)
{
    juce::ARARenderer::prepareToPlay (sampleRate, maximumSamplesPerBlock, numChannels, precision, alwaysNonRealtime);

    currentSampleRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
    currentSamplesPerBlock = std::max (maximumSamplesPerBlock, 1);
    preparedNumChannels = std::max (numChannels, 2);
    useBufferedReaders = (alwaysNonRealtime == AlwaysNonRealtime::no);
    isPreparedFlag = true;

    smoothedShiftSemitones = 0.0f;
    lastValidTargetShift = 0.0f;
    lastNextBlockSample = -1;
    wasPlaying = false;
    needsReprime = true;
    fadeInSamplesRemaining = 0;
    fadeInTotalSamples = std::max (32, static_cast<int> (std::round (currentSampleRate * 0.004)));
    lastActiveRegion = nullptr;

    shifters.clear();
    for (int i = 0; i < preparedNumChannels; ++i)
    {
        auto shifter = std::make_unique<PitchShifter>();
        shifter->prepare (currentSampleRate, currentSamplesPerBlock);
        shifters.push_back (std::move (shifter));
    }

    const int latencySamples = (! shifters.empty() && shifters[0] != nullptr) ? shifters[0]->getLatencySamples() : 0;
    tempProcessingBuffer.setSize (preparedNumChannels, std::max (currentSamplesPerBlock, 8192));
    resampleScratchBuffer.setSize (preparedNumChannels, std::max (currentSamplesPerBlock * 4 + 64, 16384));
    primeBuffer.setSize (preparedNumChannels, std::max (latencySamples + 64, 8192));
    shiftScratchBuffer.assign (static_cast<size_t> (std::max (currentSamplesPerBlock, 8192)), 0.0f);

    rebuildReaders();
}

void PitchTunePluginPlaybackRenderer::releaseResources()
{
    isPreparedFlag = false;

    std::map<juce::ARAAudioSource*, PossiblyBufferedReader> oldReaders;
    {
        const juce::SpinLock::ScopedLockType sl (readerLock);
        oldReaders.swap (audioSourceReaders);
    }
    oldReaders.clear();

    shifters.clear();
    tempProcessingBuffer.setSize (0, 0);
    resampleScratchBuffer.setSize (0, 0);
    primeBuffer.setSize (0, 0);
    shiftScratchBuffer.clear();
    lastNextBlockSample = -1;
    wasPlaying = false;
    needsReprime = true;
    fadeInSamplesRemaining = 0;
    lastValidTargetShift = 0.0f;
    lastActiveRegion = nullptr;
}

bool PitchTunePluginPlaybackRenderer::processBlock (juce::AudioBuffer<float>& buffer,
                                                   juce::AudioProcessor::Realtime realtime,
                                                   const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    buffer.clear();

    if (numSamples <= 0 || numChannels <= 0)
        return true;

    auto* customDoc = juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PitchTunePluginDocumentController> (getDocumentController());
    if (customDoc == nullptr)
        return true;

    const auto editLock = customDoc->getProcessingTryReadLock();
    if (! editLock.isLocked())
        return true;

    const double safePlaybackRate = (currentSampleRate > 0.0) ? currentSampleRate : 44100.0;

    if (auto timeSec = positionInfo.getTimeInSeconds())
        customDoc->setPlayheadTimeSeconds (*timeSec);
    else if (auto timeSamples = positionInfo.getTimeInSamples())
        customDoc->setPlayheadTimeSeconds (static_cast<double> (*timeSamples) / safePlaybackRate);

    if (realtime == juce::AudioProcessor::Realtime::yes && ! positionInfo.getIsPlaying())
    {
        lastNextBlockSample = -1;
        wasPlaying = false;
        lastActiveRegion = nullptr;
        needsReprime = true;
        fadeInSamplesRemaining = 0;
        return true;
    }

    const auto& regions = getPlaybackRegions();
    if (regions.empty())
        return true;

    double blockStartTime = 0.0;
    if (auto timeSec = positionInfo.getTimeInSeconds())
        blockStartTime = *timeSec;
    else if (auto timeSamples = positionInfo.getTimeInSamples())
        blockStartTime = static_cast<double> (*timeSamples) / safePlaybackRate;
    else
        return true;

    const juce::SpinLock::ScopedTryLockType readerSl (readerLock);
    if (! readerSl.isLocked())
        return true;

    const int64_t currentBlockSample = positionInfo.getTimeInSamples()
                                           ? *positionInfo.getTimeInSamples()
                                           : static_cast<int64_t> (std::llround (blockStartTime * safePlaybackRate));
    const bool isPlayingNow = positionInfo.getIsPlaying();
    const bool transportDiscontinuity = (isPlayingNow != wasPlaying)
                                        || (lastNextBlockSample < 0)
                                        || (std::abs (currentBlockSample - lastNextBlockSample) > 8);
    lastNextBlockSample = currentBlockSample + numSamples;
    wasPlaying = isPlayingNow;

    const double sampleDuration = 1.0 / safePlaybackRate;
    const double blockEndTime = blockStartTime + (static_cast<double> (numSamples) * sampleDuration);

    // ~18ms smoothing factor for natural vocal pitch transitions
    const float smoothCoeff = std::exp (-1.0f / static_cast<float> (std::max (1.0, 0.018 * safePlaybackRate)));

    const float correctionAmount = customDoc->getPitchCorrectionAmount();
    const int latencySamples = (! shifters.empty() && shifters[0] != nullptr) ? shifters[0]->getLatencySamples() : 0;

    const int scratchChannels = std::max (numChannels, preparedNumChannels);
    tempProcessingBuffer.setSize (scratchChannels, numSamples, false, false, true);
    if (latencySamples > 0)
        primeBuffer.setSize (scratchChannels, latencySamples, false, false, true);

    auto readSourceWindow = [this, realtime, scratchChannels] (const PossiblyBufferedReader& bufferedReader,
                                                               juce::AudioBuffer<float>& dest,
                                                               int destStart,
                                                               int count,
                                                               double exactStartSourceSample,
                                                               double srcRate,
                                                               double pbRate) noexcept -> bool
    {
        dest.clear (destStart, count);
        auto* reader = bufferedReader.get();
        if (reader == nullptr || count <= 0 || reader->lengthInSamples <= 0)
            return false;

        bufferedReader.setReadTimeout (realtime == juce::AudioProcessor::Realtime::no ? 100 : 0);

        // Fast path when source sample rate matches effective playback rate
        if (std::abs (srcRate - pbRate) < 1.0)
        {
            const int64_t srcOffset = static_cast<int64_t> (std::llround (exactStartSourceSample));
            if (srcOffset >= reader->lengthInSamples || srcOffset + count <= 0)
                return true;

            int localSkip = 0;
            int64_t readerPos = srcOffset;
            if (readerPos < 0)
            {
                localSkip = static_cast<int> (std::min<int64_t> (count, -readerPos));
                readerPos = 0;
            }

            const int available = count - localSkip;
            if (available <= 0 || readerPos >= reader->lengthInSamples)
                return true;

            const int readCount = static_cast<int> (std::min<int64_t> (available, reader->lengthInSamples - readerPos));
            if (readCount <= 0)
                return true;

            const bool ok = reader->read (&dest, destStart + localSkip, readCount, readerPos, true, true);
            if (! ok)
            {
                if (useBufferedReaders)
                    sharedTimesliceThread->notify();
                return false;
            }

            if (reader->numChannels == 1 && dest.getNumChannels() > 1)
            {
                for (int ch = 1; ch < dest.getNumChannels(); ++ch)
                    dest.copyFrom (ch, destStart + localSkip, dest, 0, destStart + localSkip, readCount);
            }

            return true;
        }

        // Resampling path via Catmull-Rom cubic interpolation when source and playback sample rates differ
        const double ratio = (pbRate > 0.0 && srcRate > 0.0) ? (srcRate / pbRate) : 1.0;
        const double endSourceSample = exactStartSourceSample + static_cast<double> (count - 1) * ratio;

        if (exactStartSourceSample >= static_cast<double> (reader->lengthInSamples) || endSourceSample < -2.0)
            return true;

        const int64_t minSample = std::min (static_cast<int64_t> (std::floor (exactStartSourceSample)),
                                            static_cast<int64_t> (std::floor (endSourceSample))) - 1;
        const int64_t maxSample = std::max (static_cast<int64_t> (std::ceil (exactStartSourceSample)),
                                            static_cast<int64_t> (std::ceil (endSourceSample))) + 2;

        const int64_t readStart = std::max<int64_t> (0, minSample);
        const int64_t readEnd = std::min<int64_t> (reader->lengthInSamples, maxSample + 1);
        const int readCount = static_cast<int> (std::max<int64_t> (0, readEnd - readStart));

        if (readCount <= 0)
            return true;

        if (readCount > resampleScratchBuffer.getNumSamples())
        {
            if (realtime == juce::AudioProcessor::Realtime::no)
                resampleScratchBuffer.setSize (scratchChannels, readCount, false, false, true);
            else
                return false;
        }

        const bool ok = reader->read (&resampleScratchBuffer, 0, readCount, readStart, true, true);
        if (! ok)
        {
            if (useBufferedReaders)
                sharedTimesliceThread->notify();
            return false;
        }

        const int copyCh = std::min (static_cast<int> (reader->numChannels), dest.getNumChannels());
        for (int ch = 0; ch < copyCh; ++ch)
        {
            const float* srcPtr = resampleScratchBuffer.getReadPointer (ch);
            float* dstPtr = dest.getWritePointer (ch);

            for (int j = 0; j < count; ++j)
            {
                const double sPos = exactStartSourceSample + static_cast<double> (j) * ratio;
                if (sPos < 0.0 || sPos >= static_cast<double> (reader->lengthInSamples))
                {
                    dstPtr[destStart + j] = 0.0f;
                    continue;
                }

                const int64_t i1 = static_cast<int64_t> (std::floor (sPos));
                const float frac = static_cast<float> (sPos - static_cast<double> (i1));

                auto sampleAt = [srcPtr, readStart, readCount] (int64_t absIdx) noexcept -> float
                {
                    const int64_t rel = absIdx - readStart;
                    if (rel < 0 || rel >= readCount)
                        return 0.0f;
                    return srcPtr[rel];
                };

                const float y0 = sampleAt (i1 - 1);
                const float y1 = sampleAt (i1);
                const float y2 = sampleAt (i1 + 1);
                const float y3 = sampleAt (i1 + 2);

                const float a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
                const float a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const float a2 = -0.5f * y0 + 0.5f * y2;
                const float a3 = y1;

                const float val = ((a0 * frac + a1) * frac + a2) * frac + a3;
                dstPtr[destStart + j] = std::isfinite (val) ? val : 0.0f;
            }
        }

        if (reader->numChannels == 1 && dest.getNumChannels() > 1)
        {
            for (int ch = 1; ch < dest.getNumChannels(); ++ch)
                dest.copyFrom (ch, destStart, dest, 0, destStart, count);
        }

        return true;
    };

    bool anyRegionOverlapped = false;

    for (auto* region : regions)
    {
        if (region == nullptr)
            continue;

        const double regionStart = region->getStartInPlaybackTime();
        const double regionEnd = region->getEndInPlaybackTime();

        if (blockEndTime <= regionStart || blockStartTime >= regionEnd)
            continue;

        auto* mod = region->getAudioModification();
        auto* source = (mod != nullptr) ? mod->getAudioSource() : nullptr;
        if (source == nullptr || ! source->isSampleAccessEnabled())
            continue;

        auto readerIt = audioSourceReaders.find (source);
        if (readerIt == audioSourceReaders.end())
        {
            const_cast<PitchTunePluginPlaybackRenderer*> (this)->triggerAsyncUpdate();
            continue;
        }

        auto* readerPtr = readerIt->second.get();
        if (readerPtr == nullptr || readerPtr->lengthInSamples <= 0)
            continue;

        const int startSample = std::clamp (static_cast<int> (std::round ((regionStart - blockStartTime) * safePlaybackRate)), 0, numSamples);
        const int endSample = std::clamp (static_cast<int> (std::round ((regionEnd - blockStartTime) * safePlaybackRate)), 0, numSamples);
        const int overlapSamples = endSample - startSample;
        if (overlapSamples <= 0)
            continue;

        auto* pitchRegion = dynamic_cast<PitchTunePluginPlaybackRegion*> (region);
        const double sourceSampleRate = (source->getSampleRate() > 0.0) ? source->getSampleRate() : safePlaybackRate;
        const double playbackOverlapStart = blockStartTime + (static_cast<double> (startSample) * sampleDuration);

        double sourceStartTime = region->getStartInAudioModificationTime() + (playbackOverlapStart - regionStart);
        double scale = 1.0;
        if (pitchRegion != nullptr)
        {
            sourceStartTime = pitchRegion->playbackToSourceTime (playbackOverlapStart);
            const double pbDur = pitchRegion->getDurationInPlaybackTime();
            const double modDur = pitchRegion->getDurationInAudioModificationTime();
            if (pbDur > 1e-6 && modDur > 1e-6)
                scale = modDur / pbDur;
        }

        const double sourceTimeStepPerSample = scale * sampleDuration;
        const double effectivePlaybackRate = (scale > 1e-6) ? (safePlaybackRate / scale) : safePlaybackRate;
        const double exactSourceSample = sourceStartTime * sourceSampleRate;

        if (pitchRegion == nullptr || ! pitchRegion->isAnalysisCompleted())
        {
            const bool ok = readSourceWindow (readerIt->second,
                                              tempProcessingBuffer,
                                              startSample,
                                              overlapSamples,
                                              exactSourceSample,
                                              sourceSampleRate,
                                              effectivePlaybackRate);
            if (! ok)
            {
                needsReprime = true;
                continue;
            }

            anyRegionOverlapped = true;
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.addFrom (ch, startSample, tempProcessingBuffer, ch, startSample, overlapSamples);
            continue;
        }

        const bool mustPrime = transportDiscontinuity || (pitchRegion != lastActiveRegion) || (startSample > 0) || needsReprime;
        if (mustPrime)
        {
            if (latencySamples > 0)
            {
                const bool primeOk = readSourceWindow (readerIt->second,
                                                       primeBuffer,
                                                       0,
                                                       latencySamples,
                                                       exactSourceSample,
                                                       sourceSampleRate,
                                                       effectivePlaybackRate);
                if (! primeOk)
                {
                    needsReprime = true;
                    continue;
                }

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    if (ch < static_cast<int> (shifters.size()) && shifters[ch] != nullptr)
                        shifters[ch]->prime (primeBuffer.getReadPointer (ch), latencySamples);
                }
            }
            else
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    if (ch < static_cast<int> (shifters.size()) && shifters[ch] != nullptr)
                        shifters[ch]->reset();
                }
            }

            needsReprime = false;
            fadeInSamplesRemaining = fadeInTotalSamples;
        }

        const double sourceLatencySamples = static_cast<double> (latencySamples) * (sourceSampleRate / effectivePlaybackRate);
        const bool mainOk = readSourceWindow (readerIt->second,
                                              tempProcessingBuffer,
                                              startSample,
                                              overlapSamples,
                                              exactSourceSample + sourceLatencySamples,
                                              sourceSampleRate,
                                              effectivePlaybackRate);
        if (! mainOk)
        {
            needsReprime = true;
            continue;
        }

        anyRegionOverlapped = true;

        if (static_cast<int> (shiftScratchBuffer.size()) < overlapSamples)
            shiftScratchBuffer.resize (static_cast<size_t> (overlapSamples), 0.0f);

        pitchRegion->computePitchShiftsForBlock (sourceStartTime,
                                                 sourceTimeStepPerSample,
                                                 overlapSamples,
                                                 correctionAmount,
                                                 shiftScratchBuffer.data(),
                                                 lastValidTargetShift);

        if (mustPrime)
        {
            const float initialShift = shiftScratchBuffer[0];
            smoothedShiftSemitones = std::isfinite (initialShift) ? initialShift : 0.0f;
            lastActiveRegion = pitchRegion;
        }

        const bool isMonoSource = (readerPtr->numChannels == 1);

        // Apply WSOLA pitch shifting per overlapping sample with ~18ms natural pitch glide
        for (int i = startSample; i < endSample; ++i)
        {
            float shiftAmount = shiftScratchBuffer[static_cast<size_t> (i - startSample)];
            if (! std::isfinite (shiftAmount))
                shiftAmount = 0.0f;

            smoothedShiftSemitones = smoothCoeff * smoothedShiftSemitones + (1.0f - smoothCoeff) * shiftAmount;
            if (! std::isfinite (smoothedShiftSemitones))
                smoothedShiftSemitones = 0.0f;

            float fadeGain = 1.0f;
            if (fadeInSamplesRemaining > 0)
            {
                const int elapsed = fadeInTotalSamples - fadeInSamplesRemaining;
                const float progress = static_cast<float> (elapsed + 1) / static_cast<float> (std::max (1, fadeInTotalSamples));
                fadeGain = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * std::clamp (progress, 0.0f, 1.0f)));
                --fadeInSamplesRemaining;
            }

            if (isMonoSource && ! shifters.empty() && shifters[0] != nullptr)
            {
                shifters[0]->setPitchShift (smoothedShiftSemitones);

                const float sampleInput = tempProcessingBuffer.getSample (0, i);
                float sampleOutput = 0.0f;
                shifters[0]->process (&sampleInput, &sampleOutput, 1);
                sampleOutput *= fadeGain;

                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.setSample (ch, i, buffer.getSample (ch, i) + sampleOutput);
            }
            else
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    if (ch < static_cast<int> (shifters.size()) && shifters[ch] != nullptr)
                    {
                        shifters[ch]->setPitchShift (smoothedShiftSemitones);

                        const float sampleInput = tempProcessingBuffer.getSample (ch, i);
                        float sampleOutput = 0.0f;
                        shifters[ch]->process (&sampleInput, &sampleOutput, 1);

                        buffer.setSample (ch, i, buffer.getSample (ch, i) + sampleOutput * fadeGain);
                    }
                }
            }
        }
    }

    if (! anyRegionOverlapped)
        lastActiveRegion = nullptr;

    sanitizeAudioBuffer (buffer);
    return true;
}

//==============================================================================
// ARA Editor Renderer Implementation
//==============================================================================

PitchTunePluginEditorRenderer::PitchTunePluginEditorRenderer (ARA::PlugIn::DocumentController* dc)
    : juce::ARAEditorRenderer (dc)
{
}

PitchTunePluginEditorRenderer::~PitchTunePluginEditorRenderer()
{
}

void PitchTunePluginEditorRenderer::prepareToPlay (double sampleRate, int maximumSamplesPerBlock, int numChannels,
                                                  juce::AudioProcessor::ProcessingPrecision precision,
                                                  AlwaysNonRealtime alwaysNonRealtime)
{
    juce::ARARenderer::prepareToPlay (sampleRate, maximumSamplesPerBlock, numChannels, precision, alwaysNonRealtime);
    currentSampleRate = sampleRate;
}

void PitchTunePluginEditorRenderer::releaseResources()
{
}

bool PitchTunePluginEditorRenderer::processBlock (juce::AudioBuffer<float>& buffer,
                                                 juce::AudioProcessor::Realtime realtime,
                                                 const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    // Leave shared buffer untouched so hosts that bind PlaybackRenderer + EditorRenderer
    // to the same AudioProcessor instance (e.g. Cakewalk SONAR) retain PlaybackRenderer's audio.
    juce::ignoreUnused (buffer, realtime, positionInfo);
    return true;
}

//==============================================================================
// ARA Document Controller Implementation
//==============================================================================

PitchTunePluginDocumentController::~PitchTunePluginDocumentController()
{
    controllerAlive->store (false);

    {
        const juce::ScopedLock sl (regionLock);
        for (auto* region : activeRegions)
        {
            if (region != nullptr)
                region->requestAbortAnalysis();
        }
        playbackRenderers.clear();
    }
    threadPool.removeAllJobs (true, 5000);
}

void PitchTunePluginDocumentController::willBeginEditing (juce::ARADocument* document)
{
    juce::ignoreUnused (document);
    processBlockLock.enterWrite();
}

void PitchTunePluginDocumentController::didEndEditing (juce::ARADocument* document)
{
    juce::ignoreUnused (document);
    processBlockLock.exitWrite();
}

void PitchTunePluginDocumentController::didEnableAudioSourceSamplesAccess (juce::ARAAudioSource* audioSource, bool enable)
{
    juce::ignoreUnused (audioSource);
    if (! enable)
        return;

    auto alive = controllerAlive;
    juce::MessageManager::callAsync ([this, alive]
    {
        if (! alive->load())
            return;

        triggerAsyncAnalysis (false);
        notifyRenderersToRebuildReaders();
    });
}

void PitchTunePluginDocumentController::registerPlaybackRenderer (PitchTunePluginPlaybackRenderer* renderer)
{
    if (renderer == nullptr)
        return;

    const juce::ScopedLock sl (regionLock);
    if (std::find (playbackRenderers.begin(), playbackRenderers.end(), renderer) == playbackRenderers.end())
        playbackRenderers.push_back (renderer);
}

void PitchTunePluginDocumentController::unregisterPlaybackRenderer (PitchTunePluginPlaybackRenderer* renderer)
{
    if (renderer == nullptr)
        return;

    const juce::ScopedLock sl (regionLock);
    playbackRenderers.erase (std::remove (playbackRenderers.begin(), playbackRenderers.end(), renderer), playbackRenderers.end());
}

void PitchTunePluginDocumentController::notifyRenderersToRebuildReaders()
{
    std::vector<PitchTunePluginPlaybackRenderer*> renderersCopy;
    {
        const juce::ScopedLock sl (regionLock);
        renderersCopy = playbackRenderers;
    }

    for (auto* renderer : renderersCopy)
    {
        if (renderer != nullptr)
            renderer->notifyAudioSourceAccessChanged();
    }
}

void PitchTunePluginDocumentController::cacheRegionNotes (PitchTunePluginPlaybackRegion* region)
{
    if (region == nullptr || ! region->isAnalysisCompleted())
        return;

    auto* mod = region->getAudioModification();
    if (mod == nullptr)
        return;

    auto* source = mod->getAudioSource();
    const std::string modID = mod->getPersistentID();
    const std::string sourceID = (source != nullptr) ? source->getPersistentID() : std::string();
    auto notesCopy = region->getNotesCopy();

    {
        const juce::ScopedLock sl (regionLock);
        if (! modID.empty())
        {
            archivedModificationNotes[modID] = notesCopy;
            if (! sourceID.empty())
                modToSourceId[modID] = sourceID;
        }
        if (! sourceID.empty())
            archivedSourceNotes[sourceID] = std::move (notesCopy);
    }

    if (auto* dc = getDocumentController())
        dc->notifyDocumentDataChanged();

    region->notifyContentChanged (ARA::ContentUpdateScopes::notesAreAffected(), true);
    mod->notifyContentChanged (ARA::ContentUpdateScopes::notesAreAffected(), true);
}

bool PitchTunePluginDocumentController::doRestoreObjectsFromStream (juce::ARAInputStream& input, const juce::ARARestoreObjectsFilter* filter)
{
    if (input.getTotalLength() <= 0)
        return true;

    const int32_t magic = input.readInt();
    const int32_t version = input.readInt();
    const int32_t entryCount = input.readInt();

    if (input.failed() || magic != kArchiveMagic || (version != 1 && version != 2) || entryCount < 0)
        return false;

    struct RestoredEntry
    {
        juce::String modID;
        juce::String sourceID;
        std::vector<PitchTunePluginPlaybackRegion::Note> notes;
    };

    std::vector<RestoredEntry> entries;
    entries.reserve (static_cast<size_t> (entryCount));

    for (int32_t e = 0; e < entryCount; ++e)
    {
        RestoredEntry entry;
        entry.modID = input.readString();
        entry.sourceID = input.readString();
        const int32_t noteCount = input.readInt();

        if (input.failed() || noteCount < 0 || noteCount > 1000000)
            return false;

        entry.notes.reserve (static_cast<size_t> (noteCount));

        for (int32_t n = 0; n < noteCount; ++n)
        {
            PitchTunePluginPlaybackRegion::Note note;
            note.startTime = input.readDouble();
            note.endTime = input.readDouble();
            note.originalMidiNote = input.readInt();
            note.targetMidiNote = input.readInt();
            note.targetCentsOffset = (version >= 2) ? input.readFloat() : 0.0f;
            const int32_t curveSize = input.readInt();

            if (input.failed() || curveSize < 0 || curveSize > 10000000)
                return false;

            note.pitchCurve.resize (static_cast<size_t> (curveSize));
            for (int32_t c = 0; c < curveSize; ++c)
                note.pitchCurve[static_cast<size_t> (c)] = input.readFloat();

            if (input.failed())
                return false;

            entry.notes.push_back (std::move (note));
        }

        entries.push_back (std::move (entry));
    }

    std::vector<std::pair<PitchTunePluginPlaybackRegion*, std::vector<PitchTunePluginPlaybackRegion::Note>>> regionsToUpdate;

    {
        const juce::ScopedLock sl (regionLock);

        for (const auto& entry : entries)
        {
            std::string resolvedModID;
            std::string resolvedSourceID;

            if (filter != nullptr)
            {
                juce::ARAAudioModification* restoredMod = nullptr;
                juce::ARAAudioSource* restoredSource = nullptr;

                if (entry.modID.isNotEmpty())
                    restoredMod = filter->getAudioModificationToRestoreStateWithID<juce::ARAAudioModification> (entry.modID.toRawUTF8());

                if (entry.sourceID.isNotEmpty())
                    restoredSource = filter->getAudioSourceToRestoreStateWithID<juce::ARAAudioSource> (entry.sourceID.toRawUTF8());

                if (restoredMod != nullptr)
                {
                    resolvedModID = restoredMod->getPersistentID();
                    if (restoredSource == nullptr)
                        restoredSource = restoredMod->getAudioSource();
                }

                if (restoredSource != nullptr)
                    resolvedSourceID = restoredSource->getPersistentID();

                if (resolvedModID.empty() && resolvedSourceID.empty())
                    continue;
            }
            else
            {
                resolvedModID = entry.modID.toStdString();
                resolvedSourceID = entry.sourceID.toStdString();
            }

            if (! resolvedModID.empty())
            {
                archivedModificationNotes[resolvedModID] = entry.notes;
                if (! resolvedSourceID.empty())
                    modToSourceId[resolvedModID] = resolvedSourceID;
            }

            if (! resolvedSourceID.empty() && (resolvedModID.empty() || archivedSourceNotes.find (resolvedSourceID) == archivedSourceNotes.end()))
            {
                archivedSourceNotes[resolvedSourceID] = entry.notes;
            }

            for (auto* region : activeRegions)
            {
                if (region == nullptr)
                    continue;

                auto* mod = region->getAudioModification();
                if (mod == nullptr)
                    continue;

                auto* src = mod->getAudioSource();
                const bool matchMod = (! resolvedModID.empty() && mod->getPersistentID() == resolvedModID);
                const bool matchSrc = (resolvedModID.empty() && ! resolvedSourceID.empty() && src != nullptr && src->getPersistentID() == resolvedSourceID);

                if (matchMod || matchSrc)
                {
                    region->requestAbortAnalysis();
                    regionsToUpdate.emplace_back (region, entry.notes);
                }
            }
        }
    }

    for (auto& pair : regionsToUpdate)
    {
        pair.first->restoreNotes (pair.second);
        if (auto* mod = pair.first->getAudioModification())
        {
            pair.first->notifyContentChanged (ARA::ContentUpdateScopes::notesAreAffected(), false);
            mod->notifyContentChanged (ARA::ContentUpdateScopes::notesAreAffected(), false);
        }
    }

    return true;
}

bool PitchTunePluginDocumentController::doStoreObjectsToStream (juce::ARAOutputStream& output, const juce::ARAStoreObjectsFilter* filter)
{
    // Sync all active regions into archive maps before writing
    auto regionsSnapshot = getActivePitchRegions();
    for (auto* region : regionsSnapshot)
    {
        if (region == nullptr || ! region->isAnalysisCompleted())
            continue;

        if (auto* mod = region->getAudioModification())
        {
            auto* source = mod->getAudioSource();
            const std::string modID = mod->getPersistentID();
            const std::string sourceID = (source != nullptr) ? source->getPersistentID() : std::string();
            auto notesCopy = region->getNotesCopy();

            const juce::ScopedLock sl (regionLock);
            if (! modID.empty())
            {
                archivedModificationNotes[modID] = notesCopy;
                if (! sourceID.empty())
                    modToSourceId[modID] = sourceID;
            }
            if (! sourceID.empty())
                archivedSourceNotes[sourceID] = std::move (notesCopy);
        }
    }

    struct StoreEntry
    {
        std::string modID;
        std::string sourceID;
        std::vector<PitchTunePluginPlaybackRegion::Note> notes;
    };

    std::vector<StoreEntry> entriesToWrite;

    {
        const juce::ScopedLock sl (regionLock);
        std::set<std::string> storedSourceIds;

        if (filter != nullptr)
        {
            for (const auto* mod : filter->getAudioModificationsToStore())
            {
                if (mod == nullptr)
                    continue;

                const std::string modID = mod->getPersistentID();
                const std::string sourceID = (mod->getAudioSource() != nullptr) ? mod->getAudioSource()->getPersistentID() : std::string();

                auto modIt = archivedModificationNotes.find (modID);
                if (modIt != archivedModificationNotes.end())
                {
                    entriesToWrite.push_back ({ modID, sourceID, modIt->second });
                    if (! sourceID.empty())
                        storedSourceIds.insert (sourceID);
                }
                else if (! sourceID.empty())
                {
                    auto srcIt = archivedSourceNotes.find (sourceID);
                    if (srcIt != archivedSourceNotes.end())
                    {
                        entriesToWrite.push_back ({ modID, sourceID, srcIt->second });
                        storedSourceIds.insert (sourceID);
                    }
                }
            }

            for (const auto* source : filter->getAudioSourcesToStore())
            {
                if (source == nullptr)
                    continue;

                const std::string sourceID = source->getPersistentID();
                if (storedSourceIds.count (sourceID) == 0)
                {
                    auto srcIt = archivedSourceNotes.find (sourceID);
                    if (srcIt != archivedSourceNotes.end())
                    {
                        entriesToWrite.push_back ({ std::string(), sourceID, srcIt->second });
                        storedSourceIds.insert (sourceID);
                    }
                }
            }
        }
        else
        {
            for (const auto& kv : archivedModificationNotes)
            {
                std::string sourceID;
                auto mapIt = modToSourceId.find (kv.first);
                if (mapIt != modToSourceId.end())
                    sourceID = mapIt->second;

                entriesToWrite.push_back ({ kv.first, sourceID, kv.second });
                if (! sourceID.empty())
                    storedSourceIds.insert (sourceID);
            }

            for (const auto& kv : archivedSourceNotes)
            {
                if (storedSourceIds.count (kv.first) == 0)
                    entriesToWrite.push_back ({ std::string(), kv.first, kv.second });
            }
        }
    }

    bool ok = true;
    ok = ok && output.writeInt (kArchiveMagic);
    ok = ok && output.writeInt (kArchiveVersion);
    ok = ok && output.writeInt (static_cast<int32_t> (entriesToWrite.size()));

    for (const auto& entry : entriesToWrite)
    {
        ok = ok && output.writeString (juce::String (entry.modID));
        ok = ok && output.writeString (juce::String (entry.sourceID));
        ok = ok && output.writeInt (static_cast<int32_t> (entry.notes.size()));

        for (const auto& note : entry.notes)
        {
            ok = ok && output.writeDouble (note.startTime);
            ok = ok && output.writeDouble (note.endTime);
            ok = ok && output.writeInt (note.originalMidiNote);
            ok = ok && output.writeInt (note.targetMidiNote);
            ok = ok && output.writeFloat (note.targetCentsOffset);
            ok = ok && output.writeInt (static_cast<int32_t> (note.pitchCurve.size()));

            for (float pitchHz : note.pitchCurve)
                ok = ok && output.writeFloat (pitchHz);
        }

        if (! ok)
            return false;
    }

    return ok;
}

std::vector<PitchTunePluginPlaybackRegion*> PitchTunePluginDocumentController::getActivePitchRegions() const
{
    const juce::ScopedLock sl (regionLock);
    return activeRegions;
}

void PitchTunePluginDocumentController::unregisterRegion (PitchTunePluginPlaybackRegion* region)
{
    if (region == nullptr)
        return;

    region->requestAbortAnalysis();

    {
        const juce::ScopedLock sl (regionLock);
        activeRegions.erase (std::remove (activeRegions.begin(), activeRegions.end(), region), activeRegions.end());
    }

    while (true)
    {
        {
            const juce::ScopedLock sl (regionLock);
            if (currentlyAnalyzingRegion != region)
                break;
        }
        juce::Thread::sleep (1);
    }
}

void PitchTunePluginDocumentController::triggerAsyncAnalysis (bool forceReanalyze)
{
    if (forceReanalyze)
    {
        {
            const juce::ScopedLock sl (regionLock);
            for (auto* r : activeRegions)
            {
                if (r != nullptr)
                    r->requestAbortAnalysis();
            }
        }
        threadPool.removeAllJobs (true, 2000);

        const juce::ScopedLock sl (regionLock);
        archivedModificationNotes.clear();
        archivedSourceNotes.clear();
        modToSourceId.clear();

        for (auto* r : activeRegions)
        {
            if (r != nullptr)
                r->resetForReanalysis();
        }
    }

    auto alive = controllerAlive;
    auto regionsCopy = getActivePitchRegions();
    for (auto* pitchRegion : regionsCopy)
    {
        if (pitchRegion != nullptr && ! pitchRegion->isAnalysisCompleted())
        {
            threadPool.addJob ([this, pitchRegion, forceReanalyze, alive]
            {
                if (! alive->load())
                    return;

                {
                    const juce::ScopedLock sl (regionLock);
                    if (std::find (activeRegions.begin(), activeRegions.end(), pitchRegion) == activeRegions.end())
                        return;
                    currentlyAnalyzingRegion = pitchRegion;
                }

                pitchRegion->analyzeAudio();

                {
                    const juce::ScopedLock sl (regionLock);
                    currentlyAnalyzingRegion = nullptr;
                }

                if (forceReanalyze && pitchRegion->isAnalysisCompleted())
                {
                    juce::MessageManager::callAsync ([this, pitchRegion, alive]
                    {
                        if (! alive->load())
                            return;

                        bool stillActive = false;
                        {
                            const juce::ScopedLock sl (regionLock);
                            stillActive = (std::find (activeRegions.begin(), activeRegions.end(), pitchRegion) != activeRegions.end());
                        }
                        if (stillActive)
                            cacheRegionNotes (pitchRegion);
                    });
                }
            });
        }
    }
}

juce::ARAPlaybackRenderer* PitchTunePluginDocumentController::doCreatePlaybackRenderer()
{
    auto* renderer = new PitchTunePluginPlaybackRenderer (getDocumentController());
    registerPlaybackRenderer (renderer);
    return renderer;
}

juce::ARAEditorRenderer* PitchTunePluginDocumentController::doCreateEditorRenderer()
{
    return new PitchTunePluginEditorRenderer (getDocumentController());
}

juce::ARAEditorView* PitchTunePluginDocumentController::doCreateEditorView()
{
    return new PitchTunePluginEditorView (getDocumentController());
}

juce::ARAPlaybackRegion* PitchTunePluginDocumentController::doCreatePlaybackRegion (juce::ARAAudioModification* modification,
                                                                                  ARA::ARAPlaybackRegionHostRef hostRef)
{
    auto* region = new PitchTunePluginPlaybackRegion (modification, hostRef);

    bool restoredFromArchive = false;
    std::vector<PitchTunePluginPlaybackRegion::Note> restoredNotes;

    {
        const juce::ScopedLock sl (regionLock);
        activeRegions.push_back (region);

        if (modification != nullptr)
        {
            const std::string modID = modification->getPersistentID();
            auto modIt = archivedModificationNotes.find (modID);
            if (modIt != archivedModificationNotes.end())
            {
                restoredNotes = modIt->second;
                restoredFromArchive = true;
            }
            else if (auto* source = modification->getAudioSource())
            {
                const std::string sourceID = source->getPersistentID();
                auto srcIt = archivedSourceNotes.find (sourceID);
                if (srcIt != archivedSourceNotes.end())
                {
                    restoredNotes = srcIt->second;
                    restoredFromArchive = true;
                }
            }
        }
    }

    if (restoredFromArchive)
    {
        region->restoreNotes (restoredNotes);
    }
    else
    {
        auto alive = controllerAlive;
        juce::MessageManager::callAsync ([this, alive]
        {
            if (alive->load())
                triggerAsyncAnalysis();
        });
    }
    
    return region;
}

//==============================================================================
// ARA Creator / Factory
//==============================================================================

#if JucePlugin_Enable_ARA
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<PitchTunePluginDocumentController>();
}
#endif

//==============================================================================
// Audio Processor Implementation
//==============================================================================

PitchTunePluginAudioProcessor::PitchTunePluginAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
{
    addParameter (retuneSpeedParam = new juce::AudioParameterFloat (
        "retune_speed",
        "Retune Speed",
        0.0f,
        300.0f,
        20.0f
    ));

    addParameter (scaleParam = new juce::AudioParameterChoice (
        "scale",
        "Scale",
        { "Chromatic", "C Major" },
        0
    ));

    addParameter (correctionAmountParam = new juce::AudioParameterFloat (
        "correction_amount",
        "Correction",
        0.0f,
        100.0f,
        50.0f
    ));
}

PitchTunePluginAudioProcessor::~PitchTunePluginAudioProcessor()
{
}

void PitchTunePluginAudioProcessor::didBindToARA() noexcept
{
    juce::AudioProcessorARAExtension::didBindToARA();
    setLatencySamples (0);
}

PitchTunePluginDocumentController* PitchTunePluginAudioProcessor::getPitchDocumentController() const noexcept
{
    if (auto* araExtension = dynamic_cast<const juce::AudioProcessorARAExtension*> (this))
    {
        if (auto* playbackRenderer = araExtension->getPlaybackRenderer())
        {
            if (auto* dc = playbackRenderer->getDocumentController())
                return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PitchTunePluginDocumentController> (dc);
        }
        if (auto* editorRenderer = araExtension->getEditorRenderer())
        {
            if (auto* dc = editorRenderer->getDocumentController())
                return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PitchTunePluginDocumentController> (dc);
        }
        if (auto* editorView = araExtension->getEditorView())
        {
            if (auto* dc = editorView->getDocumentController())
                return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PitchTunePluginDocumentController> (dc);
        }
    }
    return nullptr;
}

const juce::String PitchTunePluginAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool PitchTunePluginAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool PitchTunePluginAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool PitchTunePluginAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double PitchTunePluginAudioProcessor::getTailLengthSeconds() const
{
    double tailLength = 0.0;
    if (getTailLengthSecondsForARA (tailLength))
        return tailLength;
    return 0.0;
}

int PitchTunePluginAudioProcessor::getNumPrograms()
{
    return 1;
}

int PitchTunePluginAudioProcessor::getCurrentProgram()
{
    return 0;
}

void PitchTunePluginAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String PitchTunePluginAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void PitchTunePluginAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void PitchTunePluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
    
    const int araChannels = std::max (getMainBusNumOutputChannels(), 2);
    const bool boundToARA = prepareToPlayForARA (currentSampleRate, samplesPerBlock, araChannels, getProcessingPrecision());

    fallbackDetector.prepare (currentSampleRate, samplesPerBlock);
    fallbackShifters[0].prepare (currentSampleRate, samplesPerBlock);
    fallbackShifters[1].prepare (currentSampleRate, samplesPerBlock);
    smoothedShiftSemitones = 0.0f;

    setLatencySamples (boundToARA ? 0 : fallbackShifters[0].getLatencySamples());
}

void PitchTunePluginAudioProcessor::releaseResources()
{
    releaseResourcesForARA();
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool PitchTunePluginAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    const auto mainOut = layouts.getMainOutputChannelSet();
    const auto mainIn  = layouts.getMainInputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono()
     && mainOut != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (mainIn != juce::AudioChannelSet::disabled()
     && mainIn != juce::AudioChannelSet::mono()
     && mainIn != juce::AudioChannelSet::stereo())
        return false;
   #endif

    return true;
  #endif
}
#endif

void PitchTunePluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    if (correctionAmountParam != nullptr)
    {
        if (auto* customDoc = getPitchDocumentController())
            customDoc->setPitchCorrectionAmount (correctionAmountParam->get() / 100.0f);
    }

    if (isBoundToARA())
    {
        if (! isPlaybackRenderer())
            buffer.clear();

        processBlockForARA (buffer, isNonRealtime() ? juce::AudioProcessor::Realtime::no : juce::AudioProcessor::Realtime::yes, getPlayHead());
        return;
    }

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto t = pos->getTimeInSeconds())
                fallbackPlayheadSeconds.store (*t, std::memory_order_relaxed);
        }
    }

    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    if (totalNumInputChannels > 0)
    {
        auto* channelData = buffer.getReadPointer (0);
        fallbackDetector.process (channelData, numSamples);
    }

    float detectedFreq = fallbackDetector.getDetectedFrequency();
    int scaleIndex = scaleParam->getIndex();
    float targetFreq = getTargetFrequency (detectedFreq, scaleIndex);

    float targetShiftSemitones = 0.0f;
    if (detectedFreq > 0.0f && targetFreq > 0.0f)
    {
        const float rawShift = 12.0f * std::log2 (targetFreq / detectedFreq);
        const float correctionFactor = (correctionAmountParam != nullptr) ? (correctionAmountParam->get() / 100.0f) : 0.50f;
        targetShiftSemitones = rawShift * std::clamp (correctionFactor, 0.0f, 1.0f);
    }
    
    if (! std::isfinite (targetShiftSemitones))
        targetShiftSemitones = 0.0f;

    float retuneSpeedMs = retuneSpeedParam->get();
    if (retuneSpeedMs <= 1.0f || detectedFreq <= 0.0f)
    {
        smoothedShiftSemitones = targetShiftSemitones;
    }
    else
    {
        float blockDurationSec = static_cast<float> (numSamples) / static_cast<float> (currentSampleRate);
        float tau = retuneSpeedMs / 1000.0f;
        float alpha = std::exp (-blockDurationSec / tau);
        smoothedShiftSemitones = alpha * smoothedShiftSemitones + (1.0f - alpha) * targetShiftSemitones;
    }

    if (! std::isfinite (smoothedShiftSemitones))
        smoothedShiftSemitones = 0.0f;

    for (int channel = 0; channel < totalNumInputChannels; ++channel)
    {
        if (channel < 2)
        {
            auto* channelData = buffer.getWritePointer (channel);
            fallbackShifters[channel].setPitchShift (smoothedShiftSemitones);
            fallbackShifters[channel].process (channelData, channelData, numSamples);
        }
    }

    if (totalNumInputChannels == 1 && totalNumOutputChannels >= 2)
        buffer.copyFrom (1, 0, buffer, 0, 0, numSamples);

    sanitizeAudioBuffer (buffer);
}

//==============================================================================
bool PitchTunePluginAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* PitchTunePluginAudioProcessor::createEditor()
{
    return new PitchTunePluginAudioProcessorEditor (*this);
}

//==============================================================================
void PitchTunePluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    std::unique_ptr<juce::XmlElement> xml (new juce::XmlElement ("PitchTunePluginSettings"));
    xml->setAttribute ("retuneSpeed", (double) retuneSpeedParam->get());
    xml->setAttribute ("scaleIndex", scaleParam->getIndex());
    if (correctionAmountParam != nullptr)
        xml->setAttribute ("correctionAmount", (double) correctionAmountParam->get());
    copyXmlToBinary (*xml, destData);
}

void PitchTunePluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr)
    {
        if (xmlState->hasTagName ("PitchTunePluginSettings"))
        {
            retuneSpeedParam->setValueNotifyingHost (
                (float) xmlState->getDoubleAttribute ("retuneSpeed", 20.0) / 300.0f);
            *scaleParam = xmlState->getIntAttribute ("scaleIndex", 0);

            if (correctionAmountParam != nullptr)
            {
                const float savedCorrection = std::clamp (static_cast<float> (xmlState->getDoubleAttribute ("correctionAmount", 50.0)), 0.0f, 100.0f);
                *correctionAmountParam = savedCorrection;
                if (auto* customDoc = getPitchDocumentController())
                    customDoc->setPitchCorrectionAmount (savedCorrection / 100.0f);
            }
        }
    }
}

//==============================================================================
// Utility Math Helpers
//==============================================================================

float PitchTunePluginAudioProcessor::getTargetFrequency (float detectedFreq, int scaleIndex)
{
    if (detectedFreq <= 0.0f) 
        return 0.0f;
    
    float midiNote = 69.0f + 12.0f * std::log2 (detectedFreq / 440.0f);
    int noteFloor = static_cast<int> (std::floor (midiNote));
    
    auto isNoteInScale = [](int note, int scaleIdx) -> bool 
    {
        int noteInOctave = note % 12;
        if (noteInOctave < 0) 
            noteInOctave += 12;
        
        if (scaleIdx == 0) // Chromatic
            return true;
        
        if (scaleIdx == 1) // C Major
        {
            const int cMajorNotes[] = { 0, 2, 4, 5, 7, 9, 11 };
            for (int n : cMajorNotes) 
            {
                if (noteInOctave == n) 
                    return true;
            }
        }
        return false;
    };
    
    int closestNote = -1;
    float minDistance = 1e10f;
    
    for (int n = noteFloor - 12; n <= noteFloor + 12; ++n)
    {
        if (isNoteInScale (n, scaleIndex))
        {
            float dist = std::abs (static_cast<float> (n) - midiNote);
            if (dist < minDistance)
            {
                minDistance = dist;
                closestNote = n;
            }
        }
    }
    
    if (closestNote == -1) 
        return detectedFreq;

    return 440.0f * std::pow (2.0f, (static_cast<float> (closestNote) - 69.0f) / 12.0f);
}

juce::String PitchTunePluginAudioProcessor::getNoteNameFromFrequency (float freq)
{
    if (freq <= 0.0f) 
        return "---";
    
    float midiNote = 69.0f + 12.0f * std::log2 (freq / 440.0f);
    int noteNumber = static_cast<int> (std::round (midiNote));
    
    const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    int octave = (noteNumber / 12) - 1;
    int noteIdx = noteNumber % 12;
    if (noteIdx < 0) 
        noteIdx += 12;
    
    return juce::String (noteNames[noteIdx]) + juce::String (octave);
}

//==============================================================================
// Factory method to instantiate the plugin
//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PitchTunePluginAudioProcessor();
}
