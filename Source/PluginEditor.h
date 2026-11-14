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

private:
    PitchTunePluginAudioProcessor& audioProcessor;

    // GUI scroll layout
    juce::Viewport viewport;
    
    // Custom piano roll grid
    PianoRollComponent pianoRoll;

    // Header buttons
    juce::TextButton analyzeButton;
    juce::Label titleLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginAudioProcessorEditor)
};
