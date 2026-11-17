/*
  ==============================================================================

    ARALibraryIncludes.cpp
    PitchTunePlugin
    Copyright (c) 2026 28futaba

    This file includes implementation sources from the Celemony ARA SDK.
    ARA SDK: Copyright (c) Celemony Software GmbH. All rights reserved.
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at:
        http://www.apache.org/licenses/LICENSE-2.0

  ==============================================================================
*/

#ifndef NOMINMAX
 #define NOMINMAX 1
#endif

#include <JuceHeader.h>

#if JucePlugin_Enable_ARA

#ifdef min
 #undef min
#endif
#ifdef max
 #undef max
#endif

#if defined (_MSC_VER)
 #pragma warning (push)
 #pragma warning (disable : 4100 4127 4244 4458)
#endif

// ARA Debug implementations
#include <ARA_Library/Debug/ARADebug.c>

// ARA Library implementations
#include <ARA_Library/Dispatch/ARAPlugInDispatch.cpp>
#include <ARA_Library/Dispatch/ARAHostDispatch.cpp>
#include <ARA_Library/PlugIn/ARAPlug.cpp>
#include <ARA_Library/Utilities/ARAChannelFormat.cpp>
#include <ARA_Library/Utilities/ARAPitchInterpretation.cpp>

#if defined (_MSC_VER)
 #pragma warning (pop)
#endif

#endif
