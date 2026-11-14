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

    // Draw time grid (every 1 second)
    g.setColour (juce::Colour (0xff2d2d3d));
    double maxTime = xToTime (static_cast<float> (getWidth()));
    for (int sec = 1; sec <= static_cast<int> (maxTime) + 1; ++sec)
    {
        float x = timeToX (static_cast<double> (sec));
        g.drawVerticalLine (static_cast<int> (x), 0.0f, static_cast<float> (getHeight()));
        
        g.setColour (juce::Colour (0xff5c5c70));
        g.setFont (juce::FontOptions ("Inter", 10.0f, juce::Font::plain));
        g.drawText (juce::String (sec) + "s", static_cast<int> (x + 3), 2, 40, 12, juce::Justification::left);
        g.setColour (juce::Colour (0xff2d2d3d));
    }

    // Fetch and Draw Playback Regions & Notes from Document Controller
    if (auto* documentController = processor.getPitchDocumentController())
    {
        for (auto* pitchRegion : documentController->getActivePitchRegions())
        {
            if (pitchRegion == nullptr)
                continue;

            for (auto& note : pitchRegion->notes)
            {
                float x1 = timeToX (note.startTime);
                float x2 = timeToX (note.endTime);
                float yTarget = noteToY (note.targetMidiNote);
                float yOrig = noteToY (note.originalMidiNote);
                float w = std::max (4.0f, x2 - x1);

                // Draw original ghost block if note was transposed
                if (note.targetMidiNote != note.originalMidiNote)
                {
                    g.setColour (juce::Colour (0x33ffffff));
                    g.fillRoundedRectangle (x1, yOrig + 1.0f, w, noteHeight - 2.0f, 4.0f);
                    g.setColour (juce::Colour (0x55ffffff));
                    g.drawRoundedRectangle (x1, yOrig + 1.0f, w, noteHeight - 2.0f, 4.0f, 1.0f);
                }

                // Draw target edited note block
                juce::Colour noteColor = (activeDragNote == &note) ? juce::Colour (0xff00e6ff) : juce::Colour (0xff00a6ff);
                g.setColour (noteColor.withAlpha (0.45f));
                g.fillRoundedRectangle (x1, yTarget + 1.0f, w, noteHeight - 2.0f, 4.0f);
                g.setColour (noteColor);
                g.drawRoundedRectangle (x1, yTarget + 1.0f, w, noteHeight - 2.0f, 4.0f, 1.5f);

                // Draw pitch curve inside the note block
                if (!note.pitchCurve.empty())
                {
                    juce::Path pitchPath;
                    double duration = note.endTime - note.startTime;
                    double timeStep = (note.pitchCurve.size() > 1) ? (duration / static_cast<double> (note.pitchCurve.size() - 1)) : 0.0;

                    g.setColour (juce::Colour (0xffffcc00).withAlpha (0.9f));
                    
                    bool firstPoint = true;
                    for (size_t idx = 0; idx < note.pitchCurve.size(); ++idx)
                    {
                        float freq = note.pitchCurve[idx];
                        if (freq <= 0.0f)
                            continue;

                        double t = note.startTime + (static_cast<double> (idx) * timeStep);
                        float px = timeToX (t);
                        
                        // Calculate instantaneous MIDI note pitch
                        float midiPitch = 69.0f + 12.0f * std::log2 (freq / 440.0f);
                        
                        // Shift visual curve by the note transposition offset
                        float transposedMidiPitch = midiPitch + static_cast<float> (note.targetMidiNote - note.originalMidiNote);
                        float py = pitchToY (transposedMidiPitch);

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
                g.drawText (labelText, static_cast<int> (x1 + 5), static_cast<int> (yTarget), static_cast<int> (w - 10), static_cast<int> (noteHeight), juce::Justification::left, true);
            }
        }
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

    double clickedTime = xToTime (static_cast<float> (event.x));

    activeDragNote = nullptr;
    activeDragRegion = nullptr;

    if (auto* documentController = processor.getPitchDocumentController())
    {
        for (auto* pitchRegion : documentController->getActivePitchRegions())
        {
            if (pitchRegion == nullptr)
                continue;

            for (auto& note : pitchRegion->notes)
            {
                if (clickedTime >= note.startTime && clickedTime <= note.endTime)
                {
                    // Match note block boundaries
                    float yTarget = noteToY (note.targetMidiNote);
                    if (event.y >= yTarget && event.y <= yTarget + noteHeight)
                    {
                        activeDragNote = &note;
                        activeDragRegion = pitchRegion;
                        initialDragNoteMidi = note.targetMidiNote;
                        dragStartPos = event.getPosition();
                        triggerAudition (note.targetMidiNote);
                        repaint();
                        return;
                    }
                }
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

    if (activeDragNote == nullptr)
        return;

    // Calculate vertical note displacement
    float deltaY = static_cast<float> (event.y - dragStartPos.y);
    int semitoneDelta = -static_cast<int> (std::round (deltaY / noteHeight));

    // Clamp the dragged target note to reasonable musical boundaries
    int newMidiNote = std::clamp (initialDragNoteMidi + semitoneDelta, startNote, startNote + numNotes - 1);
    
    if (activeDragNote->targetMidiNote != newMidiNote)
    {
        activeDragNote->targetMidiNote = newMidiNote;
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

    if (activeDragNote != nullptr)
    {
        activeDragNote = nullptr;
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
