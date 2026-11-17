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

void PitchTunePluginPlaybackRegion::analyzeAudio()
{
    if (analysisCompleted.load())
        return;

    notes.clear();

    auto* source = getAudioModification()->getAudioSource();
    if (source == nullptr)
        return;

    juce::ARAAudioSourceReader reader (source);
    sourceLengthSamples = reader.lengthInSamples;

    if (sourceLengthSamples <= 0 || sourceSampleRate <= 0.0)
        return;

    const int windowSize = 2048;
    const int hopSize = 256; // High time resolution: ~5.8ms at 44.1kHz

    juce::AudioBuffer<float> windowBuffer (1, windowSize);
    PitchDetector analysisDetector;
    analysisDetector.prepare (sourceSampleRate, windowSize);

    int64_t currentSample = 0;

    bool inNote = false;
    double noteStartTime = 0.0;
    std::vector<float> currentPitchCurve;
    std::vector<float> activeLogPitches;

    while (currentSample + windowSize <= sourceLengthSamples)
    {
        reader.read (&windowBuffer, 0, windowSize, currentSample, true, false);
        float freq = analysisDetector.detectPitchDirect (windowBuffer.getReadPointer (0), windowSize);

        // Timeline position corresponding to the center of the analysis window
        double currentTime = static_cast<double> (currentSample + (windowSize / 2)) / sourceSampleRate;

        if (freq > 0.0f)
        {
            float midiPitch = 69.0f + 12.0f * std::log2 (freq / 440.0f);

            if (!inNote)
            {
                inNote = true;
                noteStartTime = currentTime;
                currentPitchCurve.clear();
                activeLogPitches.clear();
            }
            currentPitchCurve.push_back (freq);
            activeLogPitches.push_back (midiPitch);
        }
        else
        {
            if (inNote)
            {
                inNote = false;
                double noteEndTime = currentTime;

                // Minimum note duration: require at least 8 frames (~46ms) to reject consonants/clicks
                if (currentPitchCurve.size() >= 8)
                {
                    float sumMidi = 0.0f;
                    for (float m : activeLogPitches)
                        sumMidi += m;
                    float avgMidi = sumMidi / static_cast<float> (activeLogPitches.size());

                    int midiNote = static_cast<int> (std::round (avgMidi));

                    Note newNote;
                    newNote.startTime = noteStartTime;
                    newNote.endTime = noteEndTime;
                    newNote.originalMidiNote = midiNote;
                    newNote.targetMidiNote = midiNote; // Default: no pitch change
                    newNote.pitchCurve = currentPitchCurve;

                    notes.push_back (newNote);
                }
            }
        }

        currentSample += hopSize;
    }

    // Process trailing note if active at audio end
    if (inNote && currentPitchCurve.size() >= 8)
    {
        double noteEndTime = static_cast<double> (sourceLengthSamples) / sourceSampleRate;
        float sumMidi = 0.0f;
        for (float m : activeLogPitches)
            sumMidi += m;
        float avgMidi = sumMidi / static_cast<float> (activeLogPitches.size());
        int midiNote = static_cast<int> (std::round (avgMidi));

        Note newNote;
        newNote.startTime = noteStartTime;
        newNote.endTime = noteEndTime;
        newNote.originalMidiNote = midiNote;
        newNote.targetMidiNote = midiNote;
        newNote.pitchCurve = currentPitchCurve;

        notes.push_back (newNote);
    }

    analysisCompleted.store (true);
}

float PitchTunePluginPlaybackRegion::getPitchShiftSemitones (double timeInSeconds) const
{
    // Search for a note encompassing the current timeline position
    for (const auto& note : notes)
    {
        if (timeInSeconds >= note.startTime && timeInSeconds <= note.endTime)
        {
            // Only shift when user manually transposes the note (target != original)
            return static_cast<float> (note.targetMidiNote - note.originalMidiNote);
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

    currentSampleRate = sampleRate;
    currentSamplesPerBlock = maximumSamplesPerBlock;

    shifters.clear();
    for (int i = 0; i < numChannels; ++i)
    {
        auto shifter = std::make_unique<PitchShifter>();
        shifter->prepare (sampleRate, maximumSamplesPerBlock);
        shifters.push_back (std::move (shifter));
    }

    tempProcessingBuffer.setSize (numChannels, maximumSamplesPerBlock);
}

void PitchTunePluginPlaybackRenderer::releaseResources()
{
    shifters.clear();
    tempProcessingBuffer.setSize (0, 0);
}

bool PitchTunePluginPlaybackRenderer::processBlock (juce::AudioBuffer<float>& buffer,
                                                   juce::AudioProcessor::Realtime realtime,
                                                   const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    juce::ignoreUnused (realtime);
    
    int numSamples = buffer.getNumSamples();
    int numChannels = buffer.getNumChannels();

    auto regions = getPlaybackRegions();
    if (regions.empty())
    {
        buffer.clear();
        return true;
    }

    buffer.clear();

    auto* region = regions[0];
    auto* source = region->getAudioModification()->getAudioSource();
    if (source == nullptr)
    {
        return true;
    }

    juce::ARAAudioSourceReader reader (source);

    double blockStartTime = positionInfo.getTimeInSeconds().orFallback (0.0);
    double sampleDuration = 1.0 / currentSampleRate;

    auto* pitchRegion = dynamic_cast<PitchTunePluginPlaybackRegion*> (region);
    if (pitchRegion == nullptr || !pitchRegion->isAnalysisCompleted())
    {
        // Direct read when analysis isn't finished yet
        reader.read (&buffer, 0, numSamples, static_cast<int64_t> (blockStartTime * currentSampleRate), true, false);
        return true;
    }

    tempProcessingBuffer.setSize (numChannels, numSamples);
    tempProcessingBuffer.clear();
    int64_t sourceSampleOffset = static_cast<int64_t> (blockStartTime * currentSampleRate);
    reader.read (&tempProcessingBuffer, 0, numSamples, sourceSampleOffset, true, false);

    // Apply pitch shifting per sample based on note transposition offset
    for (int i = 0; i < numSamples; ++i)
    {
        double currentTime = blockStartTime + (static_cast<double> (i) * sampleDuration);
        float shiftAmount = pitchRegion->getPitchShiftSemitones (currentTime);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            if (ch < static_cast<int> (shifters.size()))
            {
                shifters[ch]->setPitchShift (shiftAmount);
                
                float sampleInput = tempProcessingBuffer.getSample (ch, i);
                float sampleOutput = 0.0f;
                shifters[ch]->process (&sampleInput, &sampleOutput, 1);
                
                buffer.setSample (ch, i, sampleOutput);
            }
        }
    }

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
}

bool PitchTunePluginDocumentController::doRestoreObjectsFromStream (juce::ARAInputStream& input, const juce::ARARestoreObjectsFilter* filter)
{
    juce::ignoreUnused (input, filter);
    return true;
}

bool PitchTunePluginDocumentController::doStoreObjectsToStream (juce::ARAOutputStream& output, const juce::ARAStoreObjectsFilter* filter)
{
    juce::ignoreUnused (output, filter);
    return true;
}

void PitchTunePluginDocumentController::unregisterRegion (PitchTunePluginPlaybackRegion* region)
{
    activeRegions.erase (std::remove (activeRegions.begin(), activeRegions.end(), region), activeRegions.end());
}

void PitchTunePluginDocumentController::triggerAsyncAnalysis()
{
    for (auto* pitchRegion : activeRegions)
    {
        if (pitchRegion != nullptr && !pitchRegion->isAnalysisCompleted())
        {
            threadPool.addJob ([pitchRegion]
            {
                pitchRegion->analyzeAudio();
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
    activeRegions.push_back (region);
    
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
    
    prepareToPlayForARA (sampleRate, samplesPerBlock, getTotalNumOutputChannels(), getProcessingPrecision());

    fallbackDetector.prepare (sampleRate, samplesPerBlock);
    fallbackShifters[0].prepare (sampleRate, samplesPerBlock);
    fallbackShifters[1].prepare (sampleRate, samplesPerBlock);
    smoothedShiftSemitones = 0.0f;
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

    if (processBlockForARA (buffer, isNonRealtime() ? juce::AudioProcessor::Realtime::no : juce::AudioProcessor::Realtime::yes, getPlayHead()))
    {
        return;
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
        targetShiftSemitones = 12.0f * std::log2 (targetFreq / detectedFreq);
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
