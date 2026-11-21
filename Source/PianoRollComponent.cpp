/*
  ==============================================================================

    PianoRollComponent.cpp
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#include "PianoRollComponent.h"
#include <cmath>
#include <algorithm>

PianoRollComponent::PianoRollComponent (PitchTunePluginAudioProcessor& p)
    : processor (p)
{
    startTimerHz (30); // Refresh GUI at 30fps
}

PianoRollComponent::~PianoRollComponent()
{
    stopTimer();
   #if JUCE_WINDOWS
    tonePlayer.stop();
   #endif
}

void PianoRollComponent::triggerAudition (int midiNote)
{
   #if JUCE_WINDOWS
    tonePlayer.playNote (midiNote);
   #endif
}

void PianoRollComponent::endAudition()
{
}

float PianoRollComponent::timeToX (double timeInSeconds) const noexcept
{
    return static_cast<float> (timeInSeconds) * pixelsPerSecond + keyboardWidth;
}

double PianoRollComponent::xToTime (float x) const noexcept
{
    return static_cast<double> (x - keyboardWidth) / pixelsPerSecond;
}

float PianoRollComponent::noteToY (int midiNote) const noexcept
{
    int noteOffset = midiNote - startNote;
    return static_cast<float> (numNotes - 1 - noteOffset) * noteHeight;
}

float PianoRollComponent::noteWithCentsToY (int midiNote, float centsOffset) const noexcept
{
    return noteToY (midiNote) - (centsOffset / 100.0f) * noteHeight;
}

float PianoRollComponent::pitchToY (float midiPitch) const noexcept
{
    float noteOffset = midiPitch - static_cast<float> (startNote);
    return (static_cast<float> (numNotes - 1) - noteOffset + 0.5f) * noteHeight;
}

int PianoRollComponent::yToNote (float y) const noexcept
{
    int row = static_cast<int> (y / noteHeight);
    return startNote + (numNotes - 1 - row);
}

void PianoRollComponent::paint (juce::Graphics& g)
{
    // Draw background
    g.fillAll (juce::Colour (0xff181822));

    // Draw Grid Rows (Notes background)
    for (int note = startNote; note < startNote + numNotes; ++note)
    {
        float y = noteToY (note);
        int noteInOctave = note % 12;
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 || noteInOctave == 8 || noteInOctave == 10);
        
        g.setColour (isBlackKey ? juce::Colour (0xff1f1f2a) : juce::Colour (0xff252535));
        g.fillRect (keyboardWidth, y, static_cast<float> (getWidth()) - keyboardWidth, noteHeight);
        
        g.setColour (juce::Colour (0xff12121a));
        g.drawHorizontalLine (static_cast<int> (y + noteHeight - 1), keyboardWidth, static_cast<float> (getWidth()));
    }

    // Draw time grid (adaptive interval + 0.5s sub-grid when zoomed in)
    double maxTime = xToTime (static_cast<float> (getWidth()));
    if (pixelsPerSecond >= 220.0f)
    {
        g.setColour (juce::Colour (0xff232333));
        for (double halfSec = 0.5; halfSec <= maxTime + 0.5; halfSec += 1.0)
        {
            float hx = timeToX (halfSec);
            g.drawVerticalLine (static_cast<int> (hx), 0.0f, static_cast<float> (getHeight()));
        }
    }

    const int secStep = (pixelsPerSecond < 50.0f) ? 2 : 1;
    g.setColour (juce::Colour (0xff2d2d3d));
    for (int sec = secStep; sec <= static_cast<int> (maxTime) + 1; sec += secStep)
    {
        float x = timeToX (static_cast<double> (sec));
        g.drawVerticalLine (static_cast<int> (x), 0.0f, static_cast<float> (getHeight()));
        
        g.setColour (juce::Colour (0xff5c5c70));
        g.setFont (juce::FontOptions ("Inter", 10.0f, juce::Font::plain));
        g.drawText (juce::String (sec) + "s", static_cast<int> (x + 3), 2, 40, 12, juce::Justification::left);
        g.setColour (juce::Colour (0xff2d2d3d));
    }

    bool anyAnalyzing = false;

    const auto clipBounds = g.getClipBounds();
    const float clipX1 = static_cast<float> (clipBounds.getX());
    const float clipX2 = static_cast<float> (clipBounds.getRight());

    // Fetch and Draw Playback Regions & Notes from Document Controller (lock-free via versioned UI cache)
    if (auto* documentController = processor.getPitchDocumentController())
    {
        const float correctionAmount = documentController->getPitchCorrectionAmount();
        const auto activeRegions = documentController->getActivePitchRegions();

        for (auto cacheIt = regionNoteCache.begin(); cacheIt != regionNoteCache.end(); )
        {
            if (std::find (activeRegions.begin(), activeRegions.end(), cacheIt->first) == activeRegions.end())
                cacheIt = regionNoteCache.erase (cacheIt);
            else
                ++cacheIt;
        }

        for (auto* pitchRegion : activeRegions)
        {
            if (pitchRegion == nullptr)
                continue;

            if (! pitchRegion->isAnalysisCompleted())
            {
                anyAnalyzing = true;
                continue;
            }

            const double modStart = pitchRegion->getStartInAudioModificationTime();
            const double modEnd = pitchRegion->getEndInAudioModificationTime();
            const bool hasValidModRange = (modEnd > modStart + 1e-4);

            auto& cacheEntry = regionNoteCache[pitchRegion];
            const uint64_t currentVersion = pitchRegion->getNotesVersion();
            if (! cacheEntry.initialized || cacheEntry.version != currentVersion)
            {
                cacheEntry.notes = pitchRegion->getNotesCopy();
                cacheEntry.version = currentVersion;
                cacheEntry.initialized = true;
            }

            const auto& regionNotes = cacheEntry.notes;

            for (size_t noteIdx = 0; noteIdx < regionNotes.size(); ++noteIdx)
            {
                const auto& note = regionNotes[noteIdx];

                if (hasValidModRange && (note.endTime <= modStart || note.startTime >= modEnd))
                    continue;

                const double visStartSrc = hasValidModRange ? std::max (note.startTime, modStart) : note.startTime;
                const double visEndSrc   = hasValidModRange ? std::min (note.endTime, modEnd)     : note.endTime;

                float x1 = timeToX (pitchRegion->sourceToPlaybackTime (visStartSrc));
                float x2 = timeToX (pitchRegion->sourceToPlaybackTime (visEndSrc));
                if (x2 < clipX1 || x1 > clipX2)
                    continue;

                float yTarget = noteWithCentsToY (note.targetMidiNote, note.targetCentsOffset);
                float yOrig = noteToY (note.originalMidiNote);
                float w = std::max (4.0f, x2 - x1);

                // Draw original ghost block if note was transposed or fine-tuned
                if (note.targetMidiNote != note.originalMidiNote || std::abs (note.targetCentsOffset) >= 0.5f)
                {
                    g.setColour (juce::Colour (0x33ffffff));
                    g.fillRoundedRectangle (x1, yOrig + 1.0f, w, noteHeight - 2.0f, 4.0f);
                    g.setColour (juce::Colour (0x55ffffff));
                    g.drawRoundedRectangle (x1, yOrig + 1.0f, w, noteHeight - 2.0f, 4.0f, 1.0f);
                }

                // Draw target edited note block
                const bool isDraggingThis = (activeDragRegion == pitchRegion && activeDragNoteIndex == static_cast<int> (noteIdx));
                juce::Colour noteColor = isDraggingThis ? juce::Colour (0xff00e6ff) : juce::Colour (0xff00a6ff);
                g.setColour (noteColor.withAlpha (0.45f));
                g.fillRoundedRectangle (x1, yTarget + 1.0f, w, noteHeight - 2.0f, 4.0f);
                g.setColour (noteColor);
                g.drawRoundedRectangle (x1, yTarget + 1.0f, w, noteHeight - 2.0f, 4.0f, 1.5f);

                // Draw pitch curve inside the note block matching audio attack/release envelope
                if (! note.pitchCurve.empty())
                {
                    juce::Path pitchPath;
                    const double duration = note.endTime - note.startTime;
                    const double timeStep = (note.pitchCurve.size() > 1) ? (duration / static_cast<double> (note.pitchCurve.size() - 1)) : 0.0;
                    const double attackFade = std::min (0.045, std::max (0.0, duration * 0.30));
                    const double releaseFade = std::min (0.035, std::max (0.0, duration * 0.25));

                    g.setColour (juce::Colour (0xffffcc00).withAlpha (0.9f));
                    
                    const float semitoneShift = static_cast<float> (note.targetMidiNote - note.originalMidiNote)
                                                + (note.targetCentsOffset / 100.0f);

                    bool firstPoint = true;
                    for (size_t idx = 0; idx < note.pitchCurve.size(); ++idx)
                    {
                        float freq = note.pitchCurve[idx];
                        if (freq <= 0.0f)
                            continue;

                        double t = note.startTime + (static_cast<double> (idx) * timeStep);
                        if (hasValidModRange && (t < modStart || t > modEnd))
                            continue;

                        float env = 1.0f;
                        if (attackFade > 1e-6 && (t - note.startTime) < attackFade)
                        {
                            const double u = std::clamp ((t - note.startTime) / attackFade, 0.0, 1.0);
                            env *= 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * static_cast<float> (u)));
                        }
                        if (releaseFade > 1e-6 && (note.endTime - t) < releaseFade)
                        {
                            const double u = std::clamp ((note.endTime - t) / releaseFade, 0.0, 1.0);
                            env *= 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi * static_cast<float> (u)));
                        }

                        float px = timeToX (pitchRegion->sourceToPlaybackTime (t));
                        
                        // Calculate instantaneous MIDI note pitch matching computeNoteShiftAtTime
                        float rawMidiPitch = 69.0f + 12.0f * std::log2 (freq / 440.0f);
                        float deviation = std::clamp (rawMidiPitch - static_cast<float> (note.originalMidiNote), -2.0f, 2.0f);
                        float correctedMidiPitch = rawMidiPitch + (semitoneShift - correctionAmount * deviation) * env;
                        float py = pitchToY (correctedMidiPitch);

                        if (firstPoint)
                        {
                            pitchPath.startNewSubPath (px, py);
                            firstPoint = false;
                        }
                        else
                        {
                            pitchPath.lineTo (px, py);
                        }
                    }
                    g.strokePath (pitchPath, juce::PathStrokeType (1.8f));
                }

                // Draw text label of note block
                g.setColour (juce::Colours::white);
                g.setFont (juce::FontOptions ("Inter", 11.0f, juce::Font::bold));
                
                float noteFreq = 440.0f * std::pow (2.0f, static_cast<float> (note.targetMidiNote - 69) / 12.0f);
                juce::String labelText = PitchTunePluginAudioProcessor::getNoteNameFromFrequency (noteFreq);
                const int roundedCents = static_cast<int> (std::round (note.targetCentsOffset));
                if (roundedCents != 0)
                    labelText += (roundedCents > 0 ? " +" : " ") + juce::String (roundedCents) + "c";

                g.drawText (labelText, static_cast<int> (x1 + 5), static_cast<int> (yTarget), static_cast<int> (w - 10), static_cast<int> (noteHeight), juce::Justification::left, true);
            }
        }
    }

    if (anyAnalyzing)
    {
        g.setColour (juce::Colour (0xcc121218));
        g.fillRoundedRectangle (keyboardWidth + 16.0f, 18.0f, 160.0f, 26.0f, 4.0f);
        g.setColour (juce::Colours::cyan);
        g.setFont (juce::FontOptions ("Inter", 12.0f, juce::Font::bold));
        g.drawText ("Analyzing audio...", static_cast<int> (keyboardWidth + 24), 18, 144, 26, juce::Justification::centredLeft);
    }

    // Draw DAW playhead / playback cursor
    double playheadSec = processor.getFallbackPlayheadSeconds();
    if (auto* documentController = processor.getPitchDocumentController())
        playheadSec = documentController->getPlayheadTimeSeconds();

    const float playheadX = timeToX (playheadSec);
    if (playheadX >= keyboardWidth && playheadX <= static_cast<float> (getWidth()))
    {
        g.setColour (juce::Colour (0xffff4455));
        g.drawVerticalLine (static_cast<int> (std::round (playheadX)), 0.0f, static_cast<float> (getHeight()));

        juce::Path marker;
        marker.addTriangle (playheadX - 5.0f, 0.0f, playheadX + 5.0f, 0.0f, playheadX, 8.0f);
        g.fillPath (marker);
    }

    // Draw Keyboard (Left margin overlay)
    g.setColour (juce::Colour (0xff121218));
    g.fillRect (0.0f, 0.0f, keyboardWidth, static_cast<float> (getHeight()));
    g.drawVerticalLine (static_cast<int> (keyboardWidth - 1), 0.0f, static_cast<float> (getHeight()));

    for (int note = startNote; note < startNote + numNotes; ++note)
    {
        float y = noteToY (note);
        int noteInOctave = note % 12;
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 || noteInOctave == 8 || noteInOctave == 10);
        bool isPressed = (note == pressedKeyboardNote);

        // Fill keys
        if (isPressed)
        {
            g.setColour (juce::Colour (0xff00d4ff)); // Bright glowing cyan when pressed
        }
        else
        {
            g.setColour (isBlackKey ? juce::Colours::black : juce::Colours::white);
        }
        g.fillRect (0.0f, y, keyboardWidth - 5.0f, noteHeight);
        
        // Draw key borders
        g.setColour (isPressed ? juce::Colours::white : juce::Colour (0xff32323a));
        g.drawRect (0.0f, y, keyboardWidth - 5.0f, noteHeight, isPressed ? 1.5f : 0.5f);

        // Print key labels (C note or pressed note)
        if (noteInOctave == 0 || isPressed)
        {
            g.setColour (isPressed ? juce::Colours::black : juce::Colours::darkgrey);
            g.setFont (juce::FontOptions ("Inter", 9.0f, juce::Font::bold));
            int octave = (note / 12) - 1;
            const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            juce::String name = juce::String (noteNames[noteInOctave]) + juce::String (octave);
            g.drawText (name, 5, static_cast<int> (y), static_cast<int> (keyboardWidth - 15), static_cast<int> (noteHeight), juce::Justification::right, true);
        }
    }
}

void PianoRollComponent::resized()
{
}

void PianoRollComponent::timerCallback()
{
    double maxDurationSec = 25.0;
    if (auto* documentController = processor.getPitchDocumentController())
    {
        for (auto* pitchRegion : documentController->getActivePitchRegions())
        {
            if (pitchRegion != nullptr)
                maxDurationSec = std::max (maxDurationSec, pitchRegion->getMaxTimelineSeconds() + 2.0);
        }
    }

    auto* vp = findParentComponentOfClass<juce::Viewport>();
    const int minViewportWidth = (vp != nullptr) ? std::max (800, vp->getWidth()) : 1200;
    const int requiredWidth = std::max (minViewportWidth, static_cast<int> (std::ceil (timeToX (maxDurationSec))));
    if (std::abs (requiredWidth - getWidth()) > 10 && getHeight() > 0)
        setSize (requiredWidth, getHeight());

    repaint();
}

void PianoRollComponent::mouseDown (const juce::MouseEvent& event)
{
    if (event.x < keyboardWidth)
    {
        int clickedNote = yToNote (static_cast<float> (event.y));
        if (clickedNote >= startNote && clickedNote < startNote + numNotes)
        {
            isPressingKeyboard = true;
            pressedKeyboardNote = clickedNote;
            triggerAudition (clickedNote);
            repaint();
        }
        return;
    }

    const double clickedPlaybackTime = xToTime (static_cast<float> (event.x));

    activeDragNoteIndex = -1;
    activeDragRegion = nullptr;

    if (auto* documentController = processor.getPitchDocumentController())
    {
        for (auto* pitchRegion : documentController->getActivePitchRegions())
        {
            if (pitchRegion == nullptr || ! pitchRegion->isAnalysisCompleted())
                continue;

            const double regionStart = pitchRegion->getStartInPlaybackTime();
            const double regionEnd = pitchRegion->getEndInPlaybackTime();
            if (regionEnd > regionStart + 1e-4 && (clickedPlaybackTime < regionStart || clickedPlaybackTime > regionEnd))
                continue;

            const double clickedSourceTime = pitchRegion->playbackToSourceTime (clickedPlaybackTime);

            int matchedNoteIdx = -1;
            int matchedNoteMidi = -1;
            float matchedNoteCents = 0.0f;
            {
                const juce::SpinLock::ScopedLockType sl (pitchRegion->getNotesLock());

                for (size_t noteIdx = 0; noteIdx < pitchRegion->notes.size(); ++noteIdx)
                {
                    const auto& note = pitchRegion->notes[noteIdx];
                    if (clickedSourceTime >= note.startTime && clickedSourceTime <= note.endTime)
                    {
                        float yTarget = noteWithCentsToY (note.targetMidiNote, note.targetCentsOffset);
                        if (event.y >= yTarget && event.y <= yTarget + noteHeight)
                        {
                            matchedNoteIdx = static_cast<int> (noteIdx);
                            matchedNoteMidi = note.targetMidiNote;
                            matchedNoteCents = note.targetCentsOffset;
                            break;
                        }
                    }
                }
            }

            if (matchedNoteIdx >= 0)
            {
                if (event.mods.isPopupMenu())
                {
                    const bool changed = event.mods.isShiftDown()
                                             ? pitchRegion->mergeNoteWithAdjacent (static_cast<size_t> (matchedNoteIdx), clickedSourceTime)
                                             : pitchRegion->splitNoteAtTime (static_cast<size_t> (matchedNoteIdx), clickedSourceTime);
                    if (changed)
                    {
                        documentController->cacheRegionNotes (pitchRegion);
                        repaint();
                    }
                    return;
                }

                activeDragNoteIndex = matchedNoteIdx;
                activeDragRegion = pitchRegion;
                initialDragNoteMidi = matchedNoteMidi;
                initialDragNoteCents = matchedNoteCents;
                dragStartPos = event.getPosition();
                triggerAudition (matchedNoteMidi);
                repaint();
                return;
            }
        }
    }
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& event)
{
    if (isPressingKeyboard)
    {
        int draggedNote = yToNote (static_cast<float> (event.y));
        draggedNote = std::clamp (draggedNote, startNote, startNote + numNotes - 1);
        if (draggedNote != pressedKeyboardNote)
        {
            pressedKeyboardNote = draggedNote;
            triggerAudition (draggedNote);
            repaint();
        }
        return;
    }

    if (activeDragRegion == nullptr || activeDragNoteIndex < 0)
        return;

    bool regionStillValid = false;
    if (auto* documentController = processor.getPitchDocumentController())
    {
        for (auto* r : documentController->getActivePitchRegions())
        {
            if (r == activeDragRegion)
            {
                regionStillValid = true;
                break;
            }
        }
    }

    if (! regionStillValid)
    {
        activeDragNoteIndex = -1;
        activeDragRegion = nullptr;
        return;
    }

    // Calculate vertical note displacement
    float deltaY = static_cast<float> (event.y - dragStartPos.y);
    int newMidiNote = initialDragNoteMidi;
    float newCents = initialDragNoteCents;

    if (event.mods.isAltDown())
    {
        const float startTotalSemitones = static_cast<float> (initialDragNoteMidi) + (initialDragNoteCents / 100.0f);
        const float deltaSemitones = -(deltaY / noteHeight) * 0.5f;
        const float clampedSemitones = std::clamp (startTotalSemitones + deltaSemitones,
                                                   static_cast<float> (startNote),
                                                   static_cast<float> (startNote + numNotes - 1));
        newMidiNote = static_cast<int> (std::round (clampedSemitones));
        newCents = std::round ((clampedSemitones - static_cast<float> (newMidiNote)) * 100.0f);
    }
    else
    {
        int semitoneDelta = -static_cast<int> (std::round (deltaY / noteHeight));
        newMidiNote = std::clamp (initialDragNoteMidi + semitoneDelta, startNote, startNote + numNotes - 1);
        newCents = initialDragNoteCents;
    }

    bool changed = false;
    bool midiChanged = false;
    {
        const juce::SpinLock::ScopedLockType sl (activeDragRegion->getNotesLock());
        if (static_cast<size_t> (activeDragNoteIndex) < activeDragRegion->notes.size())
        {
            auto& note = activeDragRegion->notes[static_cast<size_t> (activeDragNoteIndex)];
            if (note.targetMidiNote != newMidiNote || std::abs (note.targetCentsOffset - newCents) >= 0.5f)
            {
                midiChanged = (note.targetMidiNote != newMidiNote);
                note.targetMidiNote = newMidiNote;
                note.targetCentsOffset = newCents;
                activeDragRegion->incrementNotesVersion();
                changed = true;
            }
        }
        else
        {
            activeDragNoteIndex = -1;
            activeDragRegion = nullptr;
        }
    }

    if (changed)
    {
        if (midiChanged)
            triggerAudition (newMidiNote);
        repaint();
    }
}

void PianoRollComponent::mouseUp (const juce::MouseEvent& event)
{
    juce::ignoreUnused (event);

    if (isPressingKeyboard)
    {
        isPressingKeyboard = false;
        pressedKeyboardNote = -1;
        endAudition();
        repaint();
    }

    if (activeDragRegion != nullptr && activeDragNoteIndex >= 0)
    {
        if (auto* documentController = processor.getPitchDocumentController())
        {
            for (auto* r : documentController->getActivePitchRegions())
            {
                if (r == activeDragRegion)
                {
                    documentController->cacheRegionNotes (activeDragRegion);
                    break;
                }
            }
        }

        activeDragNoteIndex = -1;
        activeDragRegion = nullptr;
        endAudition();
        repaint();
    }
}

void PianoRollComponent::mouseExit (const juce::MouseEvent& event)
{
    juce::ignoreUnused (event);

    if (isPressingKeyboard)
    {
        isPressingKeyboard = false;
        pressedKeyboardNote = -1;
        endAudition();
        repaint();
    }
}

void PianoRollComponent::mouseDoubleClick (const juce::MouseEvent& event)
{
    if (event.x < keyboardWidth)
        return;

    const double clickedPlaybackTime = xToTime (static_cast<float> (event.x));

    if (auto* documentController = processor.getPitchDocumentController())
    {
        for (auto* pitchRegion : documentController->getActivePitchRegions())
        {
            if (pitchRegion == nullptr || ! pitchRegion->isAnalysisCompleted())
                continue;

            const double regionStart = pitchRegion->getStartInPlaybackTime();
            const double regionEnd = pitchRegion->getEndInPlaybackTime();
            if (regionEnd > regionStart + 1e-4 && (clickedPlaybackTime < regionStart || clickedPlaybackTime > regionEnd))
                continue;

            const double clickedSourceTime = pitchRegion->playbackToSourceTime (clickedPlaybackTime);

            int resetMidi = -1;
            {
                const juce::SpinLock::ScopedLockType sl (pitchRegion->getNotesLock());

                for (auto& note : pitchRegion->notes)
                {
                    if (clickedSourceTime >= note.startTime && clickedSourceTime <= note.endTime)
                    {
                        const float yTarget = noteWithCentsToY (note.targetMidiNote, note.targetCentsOffset);
                        if (event.y >= yTarget && event.y <= yTarget + noteHeight)
                        {
                            note.targetMidiNote = note.originalMidiNote;
                            note.targetCentsOffset = 0.0f;
                            pitchRegion->incrementNotesVersion();
                            resetMidi = note.originalMidiNote;
                            break;
                        }
                    }
                }
            }

            if (resetMidi >= 0)
            {
                activeDragNoteIndex = -1;
                activeDragRegion = nullptr;
                triggerAudition (resetMidi);
                documentController->cacheRegionNotes (pitchRegion);
                repaint();
                return;
            }
        }
    }
}

void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (event.mods.isCtrlDown() || event.mods.isCommandDown())
    {
        const float deltaY = wheel.deltaY;
        if (std::abs (deltaY) < 1e-4f)
            return;

        const double anchorTimeSec = std::max (0.0, xToTime (static_cast<float> (event.x)));
        auto* vp = findParentComponentOfClass<juce::Viewport>();
        const float mouseOffsetInViewport = static_cast<float> (event.x - (vp != nullptr ? vp->getViewPositionX() : 0));

        const float zoomFactor = std::pow (1.18f, deltaY * 2.5f);
        pixelsPerSecond = std::clamp (pixelsPerSecond * zoomFactor, 30.0f, 600.0f);

        double maxDurationSec = 25.0;
        if (auto* documentController = processor.getPitchDocumentController())
        {
            for (auto* pitchRegion : documentController->getActivePitchRegions())
            {
                if (pitchRegion != nullptr)
                    maxDurationSec = std::max (maxDurationSec, pitchRegion->getMaxTimelineSeconds() + 2.0);
            }
        }

        const int minViewportWidth = (vp != nullptr) ? std::max (800, vp->getWidth()) : 1200;
        const int requiredWidth = std::max (minViewportWidth, static_cast<int> (std::ceil (timeToX (maxDurationSec))));
        if (getHeight() > 0)
            setSize (requiredWidth, getHeight());

        if (vp != nullptr)
        {
            const int newViewX = std::max (0, static_cast<int> (std::round (timeToX (anchorTimeSec) - mouseOffsetInViewport)));
            vp->setViewPosition (newViewX, vp->getViewPositionY());
        }

        repaint();
        return;
    }

    juce::Component::mouseWheelMove (event, wheel);
}
