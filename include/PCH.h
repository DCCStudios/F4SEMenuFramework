#pragma once

#include <fstream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include "RE/Fallout.h"
#include "Scaleform/Scaleform.h"  // full GFx definitions (ASMovieRootBase etc.)
#include "F4SE/F4SE.h"

#define DLLEXPORT __declspec(dllexport)

// The multi-runtime CommonLibF4 fork has no F4SE::log wrapper; our logger::
// call sites map 1:1 onto spdlog's free functions (default logger installed
// by SetupLog()).
namespace logger = spdlog;

// The fork moved Scaleform out of RE into a top-level namespace; alias it
// back so existing RE::Scaleform::GFx::... code compiles unchanged.
namespace RE {
    namespace Scaleform = ::Scaleform;
}

using namespace std::literals;

#define IF_FIND(array, value, it) \
    auto it = array.find(value);  \
    if (it != array.end())
