/*
  ==============================================================================

    PluginProcessor.h
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file is part of PitchTunePlugin, licensed under GNU GPL v3.0 (or AGPL v3.0).

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <juce_audio_formats/format/juce_ARAAudioReaders.h>
#include <map>
#include <string>
#include "PitchDetector.h"
#include "PitchShifter.h"

class PitchTunePluginDocumentController;

//==============================================================================
/**
    Safe preview synthesizer stub.
    Audition playback is handled directly and cleanly by PianoTonePlayer in the UI,
    preventing any feedback loops or continuous drone into the DAW audio bus.
*/
class PreviewSynthesizer
{
public:
    PreviewSynthesizer() = default;
    void prepare (double) {}
    void noteOn (int) {}
    void noteOff() {}
    int getActiveNote() const noexcept { return -1; }
    void render (juce::AudioBuffer<float>&, int = 0, int = -1) {}
};

//==============================================================================
/**
    ARA Playback Region represents a single audio clip in the DAW.
    It manages the captured audio and hosts the pitch analysis (notes and curve).
*/
class PitchTunePluginPlaybackRegion : public juce::ARAPlaybackRegion
{
public:
    PitchTunePluginPlaybackRegion (juce::ARAAudioModification* modification, ARA::ARAPlaybackRegionHostRef hostRef);
    ~PitchTunePluginPlaybackRegion() override;

    // Represents a detected/edited musical note block
    struct Note
    {
        double startTime = 0.0; // In seconds (AudioSource / AudioModification time)
        double endTime = 0.0;   // In seconds (AudioSource / AudioModification time)
        int originalMidiNote = 0;
        int targetMidiNote = 0;
        
        // Detailed pitch tracking (Hz) sampled at fixed intervals inside the note
        std::vector<float> pitchCurve;
    };

    /** Performs pitch analysis on the clip's audio source. */
    void analyzeAudio();

    /** Signals background analysis to stop early if the region is being destroyed. */
    void requestAbortAnalysis() noexcept { abortAnalysis.store (true); }

    /** Checks if the background analysis has completed. */
    bool isAnalysisCompleted() const noexcept { return analysisCompleted.load(); }

    /** Restores persisted notes from an ARA archive safely under lock. */
    void restoreNotes (const std::vector<Note>& restoredNotes);

    /** Returns a thread-safe snapshot of the region's notes. */
    std::vector<Note> getNotesCopy() const;

    /** Converts DAW playback timeline time (seconds) to AudioSource/Modification time (seconds). */
    double playbackToSourceTime (double playbackTime) const noexcept;

    /** Converts AudioSource/Modification time (seconds) to DAW playback timeline time (seconds). */
    double sourceToPlaybackTime (double sourceTime) const noexcept;

    /** Returns the maximum timeline extent (in seconds) of the region's audio/notes. */
    double getMaxTimelineSeconds() const noexcept;

    /** Computes the required pitch shift (in semitones) at a given source time (seconds). */
    float getPitchShiftSemitones (double timeInSeconds) const;

    /** Returns the spin lock protecting `notes`. */
    juce::SpinLock& getNotesLock() const noexcept { return notesLock; }

    std::vector<Note> notes;

private:
    mutable juce::SpinLock notesLock;
    std::atomic<bool> analysisCompleted { false };
    std::atomic<bool> abortAnalysis { false };
    double sourceSampleRate { 44100.0 };
    int64_t sourceLengthSamples { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginPlaybackRegion)
};

//==============================================================================
/**
    ARA Playback Renderer processes audio clips during DAW playback.
    It reads audio from the source, applies pitch shifting, and writes to output.
*/
class PitchTunePluginPlaybackRenderer : public juce::ARAPlaybackRenderer
{
public:
    PitchTunePluginPlaybackRenderer (ARA::PlugIn::DocumentController* documentController);
    ~PitchTunePluginPlaybackRenderer() override;

    void prepareToPlay (double sampleRate, int maximumSamplesPerBlock, int numChannels,
                        juce::AudioProcessor::ProcessingPrecision precision,
                        AlwaysNonRealtime alwaysNonRealtime = AlwaysNonRealtime::no) override;
                        
    void releaseResources() override;
    
    // Replaces processBlock when rendering ARA tracks
    bool processBlock (juce::AudioBuffer<float>& buffer,
                       juce::AudioProcessor::Realtime realtime,
                       const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;

private:
    double currentSampleRate { 44100.0 };
    int currentSamplesPerBlock { 512 };
    float smoothedShiftSemitones { 0.0f };
    
    // Audio processing tools per channel
    std::vector<std::unique_ptr<PitchShifter>> shifters;
    juce::AudioBuffer<float> tempProcessingBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginPlaybackRenderer)
};

//==============================================================================
/**
    ARA Editor Renderer handles audition/preview playback when DAW transport is stopped or editing.
*/
class PitchTunePluginEditorRenderer : public juce::ARAEditorRenderer
{
public:
    PitchTunePluginEditorRenderer (ARA::PlugIn::DocumentController* documentController);
    ~PitchTunePluginEditorRenderer() override;

    void prepareToPlay (double sampleRate, int maximumSamplesPerBlock, int numChannels,
                        juce::AudioProcessor::ProcessingPrecision precision,
                        AlwaysNonRealtime alwaysNonRealtime = AlwaysNonRealtime::no) override;
                        
    void releaseResources() override;

    bool processBlock (juce::AudioBuffer<float>& buffer,
                       juce::AudioProcessor::Realtime realtime,
                       const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;

private:
    double currentSampleRate { 44100.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginEditorRenderer)
};

//==============================================================================
/**
    ARA Editor View binding for the editor UI.
*/
class PitchTunePluginEditorView : public juce::ARAEditorView
{
public:
    using juce::ARAEditorView::ARAEditorView;
};

//==============================================================================
/**
    ARA Document Controller coordinates instances of playbacks, regions, and resources.
*/
class PitchTunePluginDocumentController : public juce::ARADocumentControllerSpecialisation
{
public:
    using juce::ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;
    ~PitchTunePluginDocumentController() override;

    /** Triggers asynchronous pitch analysis on all regions. */
    void triggerAsyncAnalysis();

    /** Returns a thread-safe snapshot of active playback regions. */
    std::vector<PitchTunePluginPlaybackRegion*> getActivePitchRegions() const;
    void unregisterRegion (PitchTunePluginPlaybackRegion* region);

    /** Caches region note edits for project persistence and notifies the ARA host. */
    void cacheRegionNotes (PitchTunePluginPlaybackRegion* region);

    /** Stores / retrieves current playhead position in seconds for the UI cursor. */
    void setPlayheadTimeSeconds (double t) noexcept { playheadTimeSeconds.store (t, std::memory_order_relaxed); }
    double getPlayheadTimeSeconds() const noexcept { return playheadTimeSeconds.load (std::memory_order_relaxed); }

    void playPreviewNote (int) {}
    void stopPreviewNote() {}
    void renderPreview (juce::AudioBuffer<float>&) {}
    void preparePreview (double) {}

protected:
    bool doRestoreObjectsFromStream (juce::ARAInputStream& input, const juce::ARARestoreObjectsFilter* filter) override;
    bool doStoreObjectsToStream (juce::ARAOutputStream& output, const juce::ARAStoreObjectsFilter* filter) override;

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() override;
    juce::ARAEditorRenderer* doCreateEditorRenderer() override;
    juce::ARAEditorView* doCreateEditorView() override;
    juce::ARAPlaybackRegion* doCreatePlaybackRegion (juce::ARAAudioModification* modification,
                                                     ARA::ARAPlaybackRegionHostRef hostRef) override;

private:
    juce::ThreadPool threadPool { 1 };
    mutable juce::CriticalSection regionLock;
    std::vector<PitchTunePluginPlaybackRegion*> activeRegions;
    PitchTunePluginPlaybackRegion* currentlyAnalyzingRegion { nullptr };
    std::map<std::string, std::vector<PitchTunePluginPlaybackRegion::Note>> archivedModificationNotes;
    std::map<std::string, std::vector<PitchTunePluginPlaybackRegion::Note>> archivedSourceNotes;
    std::map<std::string, std::string> modToSourceId;
    std::atomic<double> playheadTimeSeconds { 0.0 };
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginDocumentController)
};

//==============================================================================
/**
    The core AudioProcessor. In ARA plugins, this inherits from juce::AudioProcessor and juce::AudioProcessorARAExtension.
*/
class PitchTunePluginAudioProcessor  : public juce::AudioProcessor,
                                       public juce::AudioProcessorARAExtension
{
public:
    //==============================================================================
    PitchTunePluginAudioProcessor();
    ~PitchTunePluginAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Parameter accessors
    juce::RangedAudioParameter* getRetuneSpeedParameter() const { return retuneSpeedParam; }
    juce::RangedAudioParameter* getScaleParameter() const { return scaleParam; }

    // Frequency to musical note utility
    static juce::String getNoteNameFromFrequency (float freq);

    // Dynamic pitch mapping helper
    static float getTargetFrequency (float detectedFreq, int scaleIndex);

    PitchTunePluginDocumentController* getPitchDocumentController() const noexcept;

    /** Fallback playhead position in seconds (when running outside ARA). */
    double getFallbackPlayheadSeconds() const noexcept { return fallbackPlayheadSeconds.load (std::memory_order_relaxed); }

    // Preview tone playback control stubs (handled by UI directly)
    void playPreviewNote (int) {}
    void stopPreviewNote() {}
    int getActivePreviewNote() const noexcept { return -1; }

private:
    // Fallback processors for when ARA is bypassed/not supported by DAW
    PitchDetector fallbackDetector;
    PitchShifter fallbackShifters[2];
    float smoothedShiftSemitones { 0.0f };
    double currentSampleRate { 44100.0 };
    std::atomic<double> fallbackPlayheadSeconds { 0.0 };

    // Parameters
    juce::AudioParameterFloat* retuneSpeedParam { nullptr };
    juce::AudioParameterChoice* scaleParam { nullptr };

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginAudioProcessor)
};
