/*
  ==============================================================================

    PluginEditor.h
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PianoRollComponent.h"

//==============================================================================
/**
    The ARA-enabled plugin editor. It hosts the scrolling viewport with the piano roll.
*/
class PitchTunePluginAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                             public juce::AudioProcessorEditorARAExtension,
                                             public juce::Timer
{
public:
    PitchTunePluginAudioProcessorEditor (PitchTunePluginAudioProcessor&);
    ~PitchTunePluginAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;
    
    // Timer callback to refresh visual elements
    void timerCallback() override;

    juce::AudioProcessorEditorARAExtension* getARAClientExtensions() override { return this; }

private:
    PitchTunePluginAudioProcessor& audioProcessor;

    // GUI scroll layout
    juce::Viewport viewport;
    
    // Custom piano roll grid
    PianoRollComponent pianoRoll;

    // Header controls and labels
    juce::TextButton analyzeButton;
    juce::Label titleLabel;
    juce::Label hintLabel;
    juce::Label correctionLabel;
    juce::Slider correctionSlider;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginAudioProcessorEditor)
};
