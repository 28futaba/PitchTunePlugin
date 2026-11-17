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

    // Set default plugin UI window size
    setSize (700, 480);
    
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
    titleLabel.setBounds (headerArea.removeFromLeft (250));
    analyzeButton.setBounds (headerArea.removeFromRight (120));

    // Viewport layout (everything else)
    viewport.setBounds (area);
}

void PitchTunePluginAudioProcessorEditor::timerCallback()
{
    repaint();
}
