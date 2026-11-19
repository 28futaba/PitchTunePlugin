/*
  ==============================================================================

    PluginEditor.cpp
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>

//==============================================================================
PitchTunePluginAudioProcessorEditor::PitchTunePluginAudioProcessorEditor (PitchTunePluginAudioProcessor& p)
    : juce::AudioProcessorEditor (&p),
      juce::AudioProcessorEditorARAExtension (&p),
      audioProcessor (p),
      pianoRoll (p)
{
    // Setup title label
    titleLabel.setText ("PitchTune ARA 2.0 Editor", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions ("Inter", 16.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::cyan);
    addAndMakeVisible (titleLabel);

    // Setup editing hint label
    hintLabel.setText ("Drag: Semitone | Alt+Drag: Cents | Right-Click: Split | Double-Click: Reset", juce::dontSendNotification);
    hintLabel.setFont (juce::FontOptions ("Inter", 11.5f, juce::Font::plain));
    hintLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9ab0));
    hintLabel.setJustificationType (juce::Justification::centredLeft);
    hintLabel.setMinimumHorizontalScale (0.7f);
    addAndMakeVisible (hintLabel);

    // Setup analyze/trigger button
    analyzeButton.setButtonText ("Analyze Track");
    analyzeButton.onClick = [this] 
    {
        if (auto* docController = audioProcessor.getPitchDocumentController())
        {
            docController->triggerAsyncAnalysis();
        }
    };
    addAndMakeVisible (analyzeButton);

    // Configure scroll view with custom piano roll
    pianoRoll.setSize (3000, 61 * 18);
    viewport.setViewedComponent (&pianoRoll, false);
    viewport.setScrollBarsShown (true, true);
    addAndMakeVisible (viewport);

    // Enable plugin window resizing and center initial scroll around vocal middle C (C4)
    setResizable (true, true);
    setResizeLimits (560, 340, 2560, 1440);
    setSize (760, 500);

    const int c4Y = (61 - 1 - (60 - 36)) * 18;
    viewport.setViewPosition (0, std::max (0, c4Y - ((getHeight() - 40) / 2)));

    startTimerHz (30);
}

PitchTunePluginAudioProcessorEditor::~PitchTunePluginAudioProcessorEditor()
{
    stopTimer();
}

//==============================================================================
void PitchTunePluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    // Top bar header background
    g.fillAll (juce::Colour (0xff0d0d12));

    // Divider line between header and editor viewport
    g.setColour (juce::Colour (0xff2d2d3d));
    g.drawHorizontalLine (40, 0.0f, static_cast<float> (getWidth()));
}

void PitchTunePluginAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();
    
    // Header layout
    auto headerArea = area.removeFromTop (40).reduced (10, 5);
    titleLabel.setBounds (headerArea.removeFromLeft (200));
    analyzeButton.setBounds (headerArea.removeFromRight (120));
    headerArea.removeFromLeft (8);
    headerArea.removeFromRight (8);
    hintLabel.setBounds (headerArea);

    // Viewport layout (everything else)
    viewport.setBounds (area);
}

void PitchTunePluginAudioProcessorEditor::timerCallback()
{
    repaint();
}
