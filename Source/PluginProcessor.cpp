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
    const juce::SpinLock::ScopedLockType sl (notesLock);
    notes = restoredNotes;
    analysisCompleted.store (true);
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

    auto computeMeanMidi = [] (const std::vector<float>& curve, int fallbackMidi) -> int
    {
        float sum = 0.0f;
        int count = 0;
        for (float freq : curve)
        {
            if (freq > 0.0f)
            {
                sum += 69.0f + 12.0f * std::log2 (freq / 440.0f);
                ++count;
            }
        }
        return (count > 0) ? static_cast<int> (std::round (sum / static_cast<float> (count))) : fallbackMidi;
    };

    const bool wasUnedited = (origNote.targetMidiNote == origNote.originalMidiNote
                              && std::abs (origNote.targetCentsOffset) < 0.5f);

    if (wasUnedited)
    {
        const int leftMidi = computeMeanMidi (leftNote.pitchCurve, origNote.originalMidiNote);
        const int rightMidi = computeMeanMidi (rightNote.pitchCurve, origNote.originalMidiNote);

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
    constexpr size_t kMinNoteFrames = 8;           // ~46ms minimum note length
    constexpr int kTransitionFramesRequired = 5;   // ~29ms hysteresis to split legato pitch jumps
    constexpr float kLegatoSplitThreshold = 0.75f; // Semitones deviation required to trigger split

    juce::AudioBuffer<float> windowBuffer (1, windowSize);
    PitchDetector analysisDetector;
    analysisDetector.prepare (sr, windowSize);

    std::vector<Note> detectedNotes;

    auto pushNoteIfValid = [&] (double startT, double endT,
                                const std::vector<float>& curve,
                                const std::vector<float>& logPitches)
    {
        if (curve.size() < kMinNoteFrames || logPitches.empty())
            return;

        float sumMidi = 0.0f;
        for (float m : logPitches)
            sumMidi += m;
        const float avgMidi = sumMidi / static_cast<float> (logPitches.size());
        const int midiNote = static_cast<int> (std::round (avgMidi));

        Note newNote;
        newNote.startTime = startT;
        newNote.endTime = std::max (startT, endT);
        newNote.originalMidiNote = midiNote;
        newNote.targetMidiNote = midiNote; // Default: no pitch change
        newNote.targetCentsOffset = 0.0f;
        newNote.pitchCurve = curve;

        detectedNotes.push_back (std::move (newNote));
    };

    int64_t currentSample = 0;

    bool inNote = false;
    double noteStartTime = 0.0;
    std::vector<float> currentPitchCurve;
    std::vector<float> activeLogPitches;
    std::vector<double> activeFrameTimes;

    int transitionCount = 0;
    int candidateMidi = -1;

    while (currentSample + windowSize <= sourceLengthSamples)
    {
        if (abortAnalysis.load() || analysisCompleted.load())
            return;

        if (! reader.read (&windowBuffer, 0, windowSize, currentSample, true, false))
            break;

        const float freq = analysisDetector.detectPitchDirect (windowBuffer.getReadPointer (0), windowSize);

        // Timeline position corresponding to the center of the analysis window
        const double currentTime = static_cast<double> (currentSample + (windowSize / 2)) / sr;

        if (freq > 0.0f)
        {
            const float midiPitch = 69.0f + 12.0f * std::log2 (freq / 440.0f);
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
                    for (size_t i = 0; i < stableLen; ++i)
                        stableSum += activeLogPitches[i];
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
                pushNoteIfValid (noteStartTime, currentTime, currentPitchCurve, activeLogPitches);
                currentPitchCurve.clear();
                activeLogPitches.clear();
                activeFrameTimes.clear();
                transitionCount = 0;
                candidateMidi = -1;
            }
        }

        currentSample += hopSize;
    }

    // Process trailing note if active at audio end
    if (inNote)
    {
        const double noteEndTime = static_cast<double> (sourceLengthSamples) / sr;
        pushNoteIfValid (noteStartTime, noteEndTime, currentPitchCurve, activeLogPitches);
    }

    if (abortAnalysis.load())
        return;

    const juce::SpinLock::ScopedLockType sl (notesLock);
    if (! analysisCompleted.load())
    {
        notes = std::move (detectedNotes);
        analysisCompleted.store (true);
    }
}

float PitchTunePluginPlaybackRegion::getPitchShiftSemitones (double timeInSeconds, float correctionAmount) const
{
    if (! analysisCompleted.load())
        return 0.0f;

    const juce::SpinLock::ScopedTryLockType stl (notesLock);
    if (! stl.isLocked())
        return 0.0f;

    // Search for a note encompassing the current source time position
    for (const auto& note : notes)
    {
        if (timeInSeconds >= note.startTime && timeInSeconds <= note.endTime)
        {
            const float manualShift = static_cast<float> (note.targetMidiNote - note.originalMidiNote)
                                      + (note.targetCentsOffset / 100.0f);
            const float clampedCorrection = std::clamp (correctionAmount, 0.0f, 1.0f);

            if (clampedCorrection <= 0.0f || note.pitchCurve.empty())
                return manualShift;

            float interpFreq = 0.0f;
            if (note.pitchCurve.size() == 1 || note.endTime <= note.startTime + 1e-6)
            {
                interpFreq = note.pitchCurve.front();
            }
            else
            {
                const double normPos = std::clamp ((timeInSeconds - note.startTime) / (note.endTime - note.startTime), 0.0, 1.0);
                const double floatIdx = normPos * static_cast<double> (note.pitchCurve.size() - 1);
                const size_t idx0 = static_cast<size_t> (std::floor (floatIdx));
                const size_t idx1 = std::min (idx0 + 1, note.pitchCurve.size() - 1);
                const float frac = static_cast<float> (floatIdx - static_cast<double> (idx0));
                const float f0 = note.pitchCurve[idx0];
                const float f1 = note.pitchCurve[idx1];

                if (f0 > 0.0f && f1 > 0.0f)
                    interpFreq = f0 + frac * (f1 - f0);
                else
                    interpFreq = (f0 > 0.0f) ? f0 : f1;
            }

            if (interpFreq > 0.0f)
            {
                const float currentMidiPitch = 69.0f + 12.0f * std::log2 (interpFreq / 440.0f);
                const float deviation = std::clamp (currentMidiPitch - static_cast<float> (note.originalMidiNote), -2.0f, 2.0f);
                return manualShift - (clampedCorrection * deviation);
            }

            return manualShift;
        }
    }
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
}

void PitchTunePluginPlaybackRenderer::prepareToPlay (double sampleRate, int maximumSamplesPerBlock, int numChannels,
                                                    juce::AudioProcessor::ProcessingPrecision precision,
                                                    AlwaysNonRealtime alwaysNonRealtime)
{
    juce::ARARenderer::prepareToPlay (sampleRate, maximumSamplesPerBlock, numChannels, precision, alwaysNonRealtime);

    currentSampleRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
    currentSamplesPerBlock = std::max (maximumSamplesPerBlock, 1);
    smoothedShiftSemitones = 0.0f;
    lastNextBlockSample = -1;
    wasPlaying = false;
    lastActiveRegion = nullptr;

    shifters.clear();
    for (int i = 0; i < numChannels; ++i)
    {
        auto shifter = std::make_unique<PitchShifter>();
        shifter->prepare (currentSampleRate, currentSamplesPerBlock);
        shifters.push_back (std::move (shifter));
    }

    tempProcessingBuffer.setSize (numChannels, std::max (currentSamplesPerBlock, 8192));
    primeBuffer.setSize (numChannels, 8192);
}

void PitchTunePluginPlaybackRenderer::releaseResources()
{
    shifters.clear();
    tempProcessingBuffer.setSize (0, 0);
    primeBuffer.setSize (0, 0);
    lastNextBlockSample = -1;
    wasPlaying = false;
    lastActiveRegion = nullptr;
}

bool PitchTunePluginPlaybackRenderer::processBlock (juce::AudioBuffer<float>& buffer,
                                                   juce::AudioProcessor::Realtime realtime,
                                                   const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    juce::ignoreUnused (realtime);

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    buffer.clear();

    if (numSamples <= 0 || numChannels <= 0)
        return true;

    const auto& regions = getPlaybackRegions();
    if (regions.empty())
        return true;

    const double safePlaybackRate = (currentSampleRate > 0.0) ? currentSampleRate : 44100.0;

    double blockStartTime = 0.0;
    if (auto timeSec = positionInfo.getTimeInSeconds())
        blockStartTime = *timeSec;
    else if (auto timeSamples = positionInfo.getTimeInSamples())
        blockStartTime = static_cast<double> (*timeSamples) / safePlaybackRate;
    else
        return true;

    const int64_t currentBlockSample = static_cast<int64_t> (std::llround (blockStartTime * safePlaybackRate));
    const bool isPlayingNow = positionInfo.getIsPlaying();
    const bool transportDiscontinuity = (isPlayingNow != wasPlaying)
                                        || (lastNextBlockSample < 0)
                                        || (std::abs (currentBlockSample - lastNextBlockSample) > 4);
    lastNextBlockSample = currentBlockSample + numSamples;
    wasPlaying = isPlayingNow;

    const double sampleDuration = 1.0 / safePlaybackRate;
    const double blockEndTime = blockStartTime + (static_cast<double> (numSamples) * sampleDuration);

    // ~3ms smoothing factor to avoid audible clicks across note boundaries
    const float smoothCoeff = std::exp (-1.0f / static_cast<float> (std::max (1.0, 0.003 * safePlaybackRate)));

    auto* customDoc = juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PitchTunePluginDocumentController> (getDocumentController());
    const float correctionAmount = (customDoc != nullptr) ? customDoc->getPitchCorrectionAmount() : 0.85f;
    const int latencySamples = (! shifters.empty() && shifters[0] != nullptr) ? shifters[0]->getLatencySamples() : 0;

    tempProcessingBuffer.setSize (numChannels, numSamples, false, false, true);
    if (latencySamples > 0)
        primeBuffer.setSize (numChannels, latencySamples, false, false, true);

    auto readSourceWindow = [] (juce::ARAAudioSourceReader& reader,
                                juce::AudioBuffer<float>& dest,
                                int destStart,
                                int count,
                                int64_t srcOffset) noexcept
    {
        dest.clear (destStart, count);
        if (count <= 0 || srcOffset >= reader.lengthInSamples)
            return;

        int localSkip = 0;
        int64_t readerPos = srcOffset;
        if (readerPos < 0)
        {
            localSkip = static_cast<int> (std::min<int64_t> (count, -readerPos));
            readerPos = 0;
        }

        const int available = count - localSkip;
        if (available <= 0 || readerPos >= reader.lengthInSamples)
            return;

        const int readCount = static_cast<int> (std::min<int64_t> (available, reader.lengthInSamples - readerPos));
        if (readCount > 0)
            reader.read (&dest, destStart + localSkip, readCount, readerPos, true, true);
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

        const int startSample = std::clamp (static_cast<int> (std::round ((regionStart - blockStartTime) * safePlaybackRate)), 0, numSamples);
        const int endSample = std::clamp (static_cast<int> (std::round ((regionEnd - blockStartTime) * safePlaybackRate)), 0, numSamples);
        const int overlapSamples = endSample - startSample;
        if (overlapSamples <= 0)
            continue;

        juce::ARAAudioSourceReader reader (source);
        if (! reader.isValid() || reader.lengthInSamples <= 0)
            continue;

        anyRegionOverlapped = true;

        auto* pitchRegion = dynamic_cast<PitchTunePluginPlaybackRegion*> (region);
        const double sourceSampleRate = (source->getSampleRate() > 0.0) ? source->getSampleRate() : safePlaybackRate;
        const double playbackOverlapStart = blockStartTime + (static_cast<double> (startSample) * sampleDuration);
        const double sourceStartTime = (pitchRegion != nullptr)
                                           ? pitchRegion->playbackToSourceTime (playbackOverlapStart)
                                           : (region->getStartInAudioModificationTime() + (playbackOverlapStart - regionStart));
        const int64_t sourceSampleOffset = static_cast<int64_t> (std::llround (sourceStartTime * sourceSampleRate));

        if (pitchRegion == nullptr || ! pitchRegion->isAnalysisCompleted())
        {
            readSourceWindow (reader, tempProcessingBuffer, startSample, overlapSamples, sourceSampleOffset);
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.addFrom (ch, startSample, tempProcessingBuffer, ch, startSample, overlapSamples);
            continue;
        }

        const int64_t sourceLatencyOffset = static_cast<int64_t> (std::llround (static_cast<double> (latencySamples) * (sourceSampleRate / safePlaybackRate)));

        if (transportDiscontinuity || pitchRegion != lastActiveRegion || startSample > 0)
        {
            if (latencySamples > 0)
            {
                readSourceWindow (reader, primeBuffer, 0, latencySamples, sourceSampleOffset);
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

            smoothedShiftSemitones = pitchRegion->getPitchShiftSemitones (sourceStartTime, correctionAmount);
            lastActiveRegion = pitchRegion;
        }

        const int64_t readAheadOffset = sourceSampleOffset + sourceLatencyOffset;
        readSourceWindow (reader, tempProcessingBuffer, startSample, overlapSamples, readAheadOffset);

        // Apply pitch shifting per overlapping sample with ~3ms note-boundary smoothing
        for (int i = startSample; i < endSample; ++i)
        {
            const double playbackTime = blockStartTime + (static_cast<double> (i) * sampleDuration);
            const double sourceTime = pitchRegion->playbackToSourceTime (playbackTime);
            const float shiftAmount = pitchRegion->getPitchShiftSemitones (sourceTime, correctionAmount);
            smoothedShiftSemitones = smoothCoeff * smoothedShiftSemitones + (1.0f - smoothCoeff) * shiftAmount;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                if (ch < static_cast<int> (shifters.size()) && shifters[ch] != nullptr)
                {
                    shifters[ch]->setPitchShift (smoothedShiftSemitones);

                    const float sampleInput = tempProcessingBuffer.getSample (ch, i);
                    float sampleOutput = 0.0f;
                    shifters[ch]->process (&sampleInput, &sampleOutput, 1);

                    buffer.setSample (ch, i, buffer.getSample (ch, i) + sampleOutput);
                }
            }
        }
    }

    if (! anyRegionOverlapped)
        lastActiveRegion = nullptr;

    if (customDoc != nullptr)
        customDoc->setPlayheadTimeSeconds (blockStartTime);

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
    juce::ignoreUnused (realtime, positionInfo);
    // Guarantees zero uninitialized DMA buffer recycling when editor is idle in DAW
    buffer.clear();
    return true;
}

//==============================================================================
// ARA Document Controller Implementation
//==============================================================================

PitchTunePluginDocumentController::~PitchTunePluginDocumentController()
{
    {
        const juce::ScopedLock sl (regionLock);
        for (auto* region : activeRegions)
        {
            if (region != nullptr)
                region->requestAbortAnalysis();
        }
    }
    threadPool.removeAllJobs (true, 5000);
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

    auto regionsCopy = getActivePitchRegions();
    for (auto* pitchRegion : regionsCopy)
    {
        if (pitchRegion != nullptr && ! pitchRegion->isAnalysisCompleted())
        {
            threadPool.addJob ([this, pitchRegion, forceReanalyze]
            {
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
                    juce::MessageManager::callAsync ([this, pitchRegion]
                    {
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
    return new PitchTunePluginPlaybackRenderer (getDocumentController());
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
        region->restoreNotes (restoredNotes);
    else
        juce::MessageManager::callAsync ([this] { triggerAsyncAnalysis(); });
    
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
        85.0f
    ));
}

PitchTunePluginAudioProcessor::~PitchTunePluginAudioProcessor()
{
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
    currentSampleRate = sampleRate;
    
    const bool boundToARA = prepareToPlayForARA (sampleRate, samplesPerBlock, getTotalNumOutputChannels(), getProcessingPrecision());

    fallbackDetector.prepare (sampleRate, samplesPerBlock);
    fallbackShifters[0].prepare (sampleRate, samplesPerBlock);
    fallbackShifters[1].prepare (sampleRate, samplesPerBlock);
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
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
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

    if (processBlockForARA (buffer, isNonRealtime() ? juce::AudioProcessor::Realtime::no : juce::AudioProcessor::Realtime::yes, getPlayHead()))
    {
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
    {
        return;
    }

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
        const float correctionFactor = (correctionAmountParam != nullptr) ? (correctionAmountParam->get() / 100.0f) : 0.85f;
        targetShiftSemitones = rawShift * std::clamp (correctionFactor, 0.0f, 1.0f);
    }
    
    if (std::isnan (targetShiftSemitones) || std::isinf (targetShiftSemitones))
    {
        targetShiftSemitones = 0.0f;
    }

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

    for (int channel = 0; channel < totalNumInputChannels; ++channel)
    {
        if (channel < 2)
        {
            auto* channelData = buffer.getWritePointer (channel);
            fallbackShifters[channel].setPitchShift (smoothedShiftSemitones);
            fallbackShifters[channel].process (channelData, channelData, numSamples);
        }
    }
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
                const float savedCorrection = std::clamp (static_cast<float> (xmlState->getDoubleAttribute ("correctionAmount", 85.0)), 0.0f, 100.0f);
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
