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
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include "PitchDetector.h"
#include "PitchShifter.h"

class PitchTunePluginDocumentController;
class PitchTunePluginPlaybackRenderer;

//==============================================================================
/**
    Shared background thread used by BufferingAudioReader instances so the real-time
    audio thread never blocks or creates/destroys ARA host readers during playback.
*/
class SharedARATimeSliceThread final : public juce::TimeSliceThread
{
public:
    SharedARATimeSliceThread()
        : juce::TimeSliceThread ("PitchTune ARA Sample Reader")
    {
        startThread (juce::Thread::Priority::high);
    }

    ~SharedARATimeSliceThread() override
    {
        stopThread (2000);
    }
};

//==============================================================================
/**
    Wrapper holding either a BufferingAudioReader (for realtime playback) or a
    direct ARAAudioSourceReader (for offline rendering).
*/
class PossiblyBufferedReader
{
public:
    PossiblyBufferedReader() = default;

    explicit PossiblyBufferedReader (std::unique_ptr<juce::BufferingAudioReader> readerIn)
        : setTimeoutFn ([ptr = readerIn.get()] (int ms) { ptr->setReadTimeout (ms); }),
          reader (std::move (readerIn))
    {}

    explicit PossiblyBufferedReader (std::unique_ptr<juce::AudioFormatReader> readerIn)
        : setTimeoutFn(),
          reader (std::move (readerIn))
    {}

    void setReadTimeout (int ms) const
    {
        if (setTimeoutFn)
            setTimeoutFn (ms);
    }

    juce::AudioFormatReader* get() const noexcept { return reader.get(); }

private:
    std::function<void (int)> setTimeoutFn;
    std::unique_ptr<juce::AudioFormatReader> reader;
};

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
        float targetCentsOffset = 0.0f; // Fine pitch offset in cents (-50 to +50)
        
        // Detailed pitch tracking (Hz) sampled at fixed intervals inside the note
        std::vector<float> pitchCurve;
    };

    /** Performs pitch analysis on the clip's audio source. */
    void analyzeAudio();

    /** Signals background analysis to stop early if the region is being destroyed. */
    void requestAbortAnalysis() noexcept { abortAnalysis.store (true); }

    /** Clears completion and abort flags prior to a user-triggered re-analysis. */
    void resetForReanalysis() noexcept
    {
        abortAnalysis.store (false);
        analysisCompleted.store (false);
        incrementNotesVersion();
    }

    /** Checks if the background analysis has completed. */
    bool isAnalysisCompleted() const noexcept { return analysisCompleted.load(); }

    /** Monotonic version counter incremented on any note change (allows lock-free UI caching). */
    uint64_t getNotesVersion() const noexcept { return notesVersion.load (std::memory_order_acquire); }
    void incrementNotesVersion() noexcept { notesVersion.fetch_add (1, std::memory_order_release); }

    /** Restores persisted notes from an ARA archive safely under lock. */
    void restoreNotes (const std::vector<Note>& restoredNotes);

    /** Returns a thread-safe snapshot of the region's notes. */
    std::vector<Note> getNotesCopy() const;

    /** Splits a note manually at splitTimeSeconds under lock. Returns true if split succeeded. */
    bool splitNoteAtTime (size_t noteIndex, double splitTimeSeconds);

    /** Merges a note with an adjacent neighbor based on clickedSourceTime. Returns true if merged. */
    bool mergeNoteWithAdjacent (size_t noteIndex, double clickedSourceTime);

    /** Converts DAW playback timeline time (seconds) to AudioSource/Modification time (seconds). */
    double playbackToSourceTime (double playbackTime) const noexcept;

    /** Converts AudioSource/Modification time (seconds) to DAW playback timeline time (seconds). */
    double sourceToPlaybackTime (double sourceTime) const noexcept;

    /** Returns the maximum timeline extent (in seconds) of the region's audio/notes. */
    double getMaxTimelineSeconds() const noexcept;

    /** Computes pitch shifts (in semitones) for an entire block under one non-blocking try-lock. */
    bool computePitchShiftsForBlock (double startSourceTime,
                                     double sourceTimeStepPerSample,
                                     int numSamples,
                                     float correctionAmount,
                                     float* outShifts,
                                     float& lastValidShift) const noexcept;

    /** Computes the required pitch shift (in semitones) at a given source time (seconds). */
    float getPitchShiftSemitones (double timeInSeconds, float correctionAmount = 0.50f) const;

    /** Returns the spin lock protecting `notes`. */
    juce::SpinLock& getNotesLock() const noexcept { return notesLock; }

    std::vector<Note> notes;

private:
    mutable juce::SpinLock notesLock;
    std::atomic<uint64_t> notesVersion { 0 };
    std::atomic<bool> analysisCompleted { false };
    std::atomic<bool> abortAnalysis { false };
    double sourceSampleRate { 44100.0 };
    int64_t sourceLengthSamples { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginPlaybackRegion)
};

//==============================================================================
/**
    ARA Playback Renderer processes audio clips during DAW playback.
    It maintains long-lived buffered audio source readers off the audio thread,
    reads audio with lookahead priming, applies WSOLA pitch shifting, and writes to output.
*/
class PitchTunePluginPlaybackRenderer : public juce::ARAPlaybackRenderer,
                                        private juce::AsyncUpdater
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

    using juce::ARAPlaybackRenderer::processBlock;

    void didAddPlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;
    void willRemovePlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;

    /** Schedules an async reader rebuild (called when host enables audio source sample access). */
    void notifyAudioSourceAccessChanged() noexcept { triggerAsyncUpdate(); }

private:
    void handleAsyncUpdate() override;
    void rebuildReaders();

    double currentSampleRate { 44100.0 };
    int currentSamplesPerBlock { 512 };
    int preparedNumChannels { 2 };
    bool useBufferedReaders { true };
    bool isPreparedFlag { false };

    float smoothedShiftSemitones { 0.0f };
    float lastValidTargetShift { 0.0f };
    int64_t lastNextBlockSample { -1 };
    bool wasPlaying { false };
    bool needsReprime { true };
    int fadeInSamplesRemaining { 0 };
    int fadeInTotalSamples { 192 };
    PitchTunePluginPlaybackRegion* lastActiveRegion { nullptr };
    
    // Audio processing tools per channel
    std::vector<std::unique_ptr<PitchShifter>> shifters;
    juce::AudioBuffer<float> tempProcessingBuffer;
    juce::AudioBuffer<float> resampleScratchBuffer;
    juce::AudioBuffer<float> primeBuffer;
    std::vector<float> shiftScratchBuffer;

    // Persistent source readers managed off the real-time audio thread
    juce::SharedResourcePointer<SharedARATimeSliceThread> sharedTimesliceThread;
    juce::SpinLock readerLock;
    std::map<juce::ARAAudioSource*, PossiblyBufferedReader> audioSourceReaders;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginPlaybackRenderer)
};

//==============================================================================
/**
    ARA Editor Renderer handles audition/preview playback when DAW transport is stopped or editing.
    Leaves the shared buffer untouched so hosts binding PlaybackRenderer + EditorRenderer on the
    same plugin instance retain PlaybackRenderer's rendered audio.
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

    using juce::ARAEditorRenderer::processBlock;

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

    /** Triggers asynchronous pitch analysis on regions (pass true to discard edits and re-analyze). */
    void triggerAsyncAnalysis (bool forceReanalyze = false);

    /** Returns a thread-safe snapshot of active playback regions. */
    std::vector<PitchTunePluginPlaybackRegion*> getActivePitchRegions() const;
    void unregisterRegion (PitchTunePluginPlaybackRegion* region);

    void registerPlaybackRenderer (PitchTunePluginPlaybackRenderer* renderer);
    void unregisterPlaybackRenderer (PitchTunePluginPlaybackRenderer* renderer);
    void notifyRenderersToRebuildReaders();

    /** Non-blocking lock used by audio threads during ARA model edits. */
    juce::ScopedTryReadLock getProcessingTryReadLock() const noexcept { return juce::ScopedTryReadLock (processBlockLock, true); }

    /** Caches region note edits for project persistence and notifies the ARA host. */
    void cacheRegionNotes (PitchTunePluginPlaybackRegion* region);

    /** Stores / retrieves current playhead position in seconds for the UI cursor. */
    void setPlayheadTimeSeconds (double t) noexcept { playheadTimeSeconds.store (t, std::memory_order_relaxed); }
    double getPlayheadTimeSeconds() const noexcept { return playheadTimeSeconds.load (std::memory_order_relaxed); }

    /** Stores / retrieves intra-note pitch centering strength (0.0 to 1.0). */
    void setPitchCorrectionAmount (float amount) noexcept { pitchCorrectionAmount.store (std::clamp (amount, 0.0f, 1.0f), std::memory_order_relaxed); }
    float getPitchCorrectionAmount() const noexcept { return pitchCorrectionAmount.load (std::memory_order_relaxed); }

    void playPreviewNote (int) {}
    void stopPreviewNote() {}
    void renderPreview (juce::AudioBuffer<float>&) {}
    void preparePreview (double) {}

protected:
    void willBeginEditing (juce::ARADocument* document) override;
    void didEndEditing (juce::ARADocument* document) override;
    void didEnableAudioSourceSamplesAccess (juce::ARAAudioSource* audioSource, bool enable) override;

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
    mutable juce::ReadWriteLock processBlockLock;
    std::vector<PitchTunePluginPlaybackRegion*> activeRegions;
    std::vector<PitchTunePluginPlaybackRenderer*> playbackRenderers;
    PitchTunePluginPlaybackRegion* currentlyAnalyzingRegion { nullptr };
    std::map<std::string, std::vector<PitchTunePluginPlaybackRegion::Note>> archivedModificationNotes;
    std::map<std::string, std::vector<PitchTunePluginPlaybackRegion::Note>> archivedSourceNotes;
    std::map<std::string, std::string> modToSourceId;
    std::atomic<double> playheadTimeSeconds { 0.0 };
    std::atomic<float> pitchCorrectionAmount { 0.50f };
    std::shared_ptr<std::atomic<bool>> controllerAlive { std::make_shared<std::atomic<bool>> (true) };
    
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
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorARAExtension* getARAClientExtensions() override { return this; }
    void didBindToARA() noexcept override;

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
    juce::AudioParameterFloat* getCorrectionAmountParameter() const noexcept { return correctionAmountParam; }

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
    juce::AudioParameterFloat* correctionAmountParam { nullptr };

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchTunePluginAudioProcessor)
};
