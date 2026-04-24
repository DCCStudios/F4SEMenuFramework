#pragma once

#include <fstream>
#include <spdlog/sinks/basic_file_sink.h>
#include "RE/Fallout.h"
#include "F4SE/F4SE.h"

#define DLLEXPORT __declspec(dllexport)

namespace logger = F4SE::log;
using namespace std::literals;

#define IF_FIND(array, value, it) \
    auto it = array.find(value);  \
    if (it != array.end())
