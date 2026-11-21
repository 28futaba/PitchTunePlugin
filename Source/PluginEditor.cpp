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
#include <cmath>

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
    hintLabel.setText ("Drag: Pitch | Alt+Drag: Cents | R-Click: Split | Shift+R-Click: Merge | Dbl-Click: Reset | Ctrl+Wheel: Zoom",
                       juce::dontSendNotification);
    hintLabel.setFont (juce::FontOptions ("Inter", 11.0f, juce::Font::plain));
    hintLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9ab0));
    hintLabel.setJustificationType (juce::Justification::centredLeft);
    hintLabel.setMinimumHorizontalScale (0.5f);
    addAndMakeVisible (hintLabel);

    // Setup intra-note pitch centering slider
    correctionLabel.setText ("Center:", juce::dontSendNotification);
    correctionLabel.setFont (juce::FontOptions ("Inter", 12.0f, juce::Font::bold));
    correctionLabel.setColour (juce::Label::textColourId, juce::Colour (0xffccccdd));
    correctionLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (correctionLabel);

    correctionSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    correctionSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 46, 22);
    correctionSlider.setRange (0.0, 100.0, 1.0);
    correctionSlider.setTextValueSuffix ("%");

    const float initialPct = (audioProcessor.getCorrectionAmountParameter() != nullptr)
                                 ? audioProcessor.getCorrectionAmountParameter()->get()
                                 : 50.0f;
    correctionSlider.setValue (initialPct, juce::dontSendNotification);
    if (auto* docController = audioProcessor.getPitchDocumentController())
        docController->setPitchCorrectionAmount (initialPct / 100.0f);

    correctionSlider.onValueChange = [this]
    {
        const float pct = static_cast<float> (correctionSlider.getValue());
        if (auto* param = audioProcessor.getCorrectionAmountParameter())
            *param = pct;
        if (auto* docController = audioProcessor.getPitchDocumentController())
            docController->setPitchCorrectionAmount (pct / 100.0f);
        pianoRoll.repaint();
    };
    addAndMakeVisible (correctionSlider);

    // Setup force re-analyze button (discards edits and resets track analysis)
    analyzeButton.setButtonText ("Re-Analyze");
    analyzeButton.onClick = [this] 
    {
        if (auto* docController = audioProcessor.getPitchDocumentController())
        {
            docController->triggerAsyncAnalysis (true);
            pianoRoll.repaint();
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
    setSize (840, 520);

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
    titleLabel.setBounds (headerArea.removeFromLeft (185));
    analyzeButton.setBounds (headerArea.removeFromRight (105));
    headerArea.removeFromRight (8);
    correctionSlider.setBounds (headerArea.removeFromRight (150));
    correctionLabel.setBounds (headerArea.removeFromRight (52));
    headerArea.removeFromLeft (8);
    headerArea.removeFromRight (8);
    hintLabel.setVisible (headerArea.getWidth() >= 110);
    hintLabel.setBounds (headerArea);

    // Viewport layout (everything else)
    viewport.setBounds (area);
}

void PitchTunePluginAudioProcessorEditor::timerCallback()
{
    if (auto* param = audioProcessor.getCorrectionAmountParameter())
    {
        const float paramPct = param->get();
        if (! correctionSlider.isMouseButtonDown() && std::abs (correctionSlider.getValue() - paramPct) >= 0.5)
            correctionSlider.setValue (paramPct, juce::dontSendNotification);

        if (auto* docController = audioProcessor.getPitchDocumentController())
            docController->setPitchCorrectionAmount (paramPct / 100.0f);
    }
}
