# F4SE Menu Framework 3 — Plugin Development Guide

This guide covers everything you need to build an F4SE plugin that adds in-game ImGui menus via the **F4SE Menu Framework 3**. The framework handles all DirectX 11 hooking, ImGui rendering, input capture, and window management. Your plugin just registers callbacks and draws widgets.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Project Setup from Scratch](#2-project-setup-from-scratch)
3. [The Consumer Header — F4SEMenuFramework.h](#3-the-consumer-header--f4semenufreameworkh)
4. [Plugin Entry Point](#4-plugin-entry-point)
5. [Registering Menus](#5-registering-menus)
6. [Drawing UI with ImGuiMCP](#6-drawing-ui-with-imguimcp)
7. [Windows (Popup Panels)](#7-windows-popup-panels)
8. [HUD Overlays](#8-hud-overlays)
9. [Input Events](#9-input-events)
10. [Loading Textures and Images](#10-loading-textures-and-images)
11. [Font Awesome Icons](#11-font-awesome-icons)
12. [Working with Game Forms (CommonLibF4)](#12-working-with-game-forms-commonlibf4)
13. [Build and Deploy](#13-build-and-deploy)
14. [Gotchas and F4SE-Specific Differences](#14-gotchas-and-f4se-specific-differences)
15. [Complete Minimal Plugin](#15-complete-minimal-plugin)

---

## 1. Prerequisites

| Tool | Version | Notes |
|------|---------|-------|
| Visual Studio 2022 | 17.x | With "Desktop development with C++" workload |
| CMake | 3.21+ | Bundled with VS, or install separately |
| vcpkg | Latest | Set `VCPKG_ROOT` environment variable |
| CommonLibF4 | Latest | Cloned into `PluginTemplate/CommonLibF4/` |
| F4SE Menu Framework 3 | This project | `F4SEMenuFramework.dll` must be built or obtained |

**Environment variables** (optional but recommended):

```
VCPKG_ROOT       = C:\vcpkg               (or wherever your vcpkg is)
FALLOUT4_FOLDER  = C:\...\Fallout 4       (game install, for auto-copy on build)
FALLOUT4_MODS_FOLDER = C:\...\mods        (MO2/Vortex output, for auto-copy on build)
```

---

## 2. Project Setup from Scratch

### Directory Layout

Your plugin lives alongside CommonLibF4 in a workspace like this:

```
YourWorkspace/
├── PluginTemplate/
│   └── CommonLibF4/          ← the reverse-engineered Fallout 4 library
├── F4SE-Menu-Framework-3/    ← the framework (builds F4SEMenuFramework.dll)
└── MyF4SEPlugin/             ← YOUR plugin
    ├── cmake/
    │   ├── lib/
    │   │   ├── autoCollectSources.cmake
    │   │   ├── automaticGameFolderOutput.cmake
    │   │   └── copyOutputs.cmake
    │   └── version.rc.in
    ├── include/
    │   ├── PCH.h
    │   ├── logger.h
    │   ├── Plugin.h
    │   ├── F4SEMenuFramework.h    ← consumer header (copy from framework)
    │   └── UI.h
    ├── src/
    │   ├── plugin.cpp
    │   └── UI.cpp
    ├── CMakeLists.txt
    ├── CMakePresets.json
    └── vcpkg.json
```

### vcpkg.json

```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",
  "name": "my-f4se-plugin",
  "version-string": "1.0.0",
  "dependencies": [
    "boost-stl-interfaces",
    "boost-iostreams",
    "boost-iterator",
    "boost-predef",
    "fmt",
    "frozen",
    "spdlog",
    "xbyak",
    "rsm-mmio"
  ]
}
```

These are the dependencies required by CommonLibF4. Your plugin doesn't need to use them directly — they're pulled in transitively.

### CMakePresets.json

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "generator": "Visual Studio 17 2022",
      "architecture": "x64",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "installDir": "${sourceDir}/install/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release"
      }
    },
    {
      "name": "debug",
      "inherits": ["base"],
      "displayName": "Debug",
      "binaryDir": "${sourceDir}/build/debug",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" }
    },
    {
      "name": "release",
      "inherits": ["base"],
      "displayName": "Release",
      "binaryDir": "${sourceDir}/build/release",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" }
    }
  ]
}
```

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.21)

# ---------- vcpkg setup ----------
macro(set_from_environment VARIABLE)
    if (NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
        set(${VARIABLE} $ENV{${VARIABLE}})
    endif ()
endmacro()

set_from_environment(VCPKG_ROOT)
set_from_environment(Fallout4Path)

if (DEFINED VCPKG_ROOT)
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
else()
    message(WARNING "VCPKG_ROOT is not set.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "")

# ---------- Sources ----------
include(cmake/lib/copyOutputs.cmake)
include(cmake/lib/automaticGameFolderOutput.cmake)
include(cmake/lib/autoCollectSources.cmake)

add_src_folder(src)
add_include_folder(include)

set(AUTHOR_NAME "YourName")
set(PRODUCT_NAME "MyF4SEPlugin")
set(BEAUTIFUL_NAME "My F4SE Plugin")

project(${PRODUCT_NAME} VERSION 1.0.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

# Boost MODULE finder from CommonLibF4
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../PluginTemplate/CommonLibF4/cmake")

# ---------- Resources ----------
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
set(RESOURCE_FILE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
set(RESOURCE_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/version.res)
add_custom_command(OUTPUT ${RESOURCE_OUTPUT}
    COMMAND rc /fo ${RESOURCE_OUTPUT} ${RESOURCE_FILE}
    DEPENDS ${RESOURCE_FILE})
add_custom_target(Resource ALL DEPENDS ${RESOURCE_OUTPUT})

# ---------- CommonLibF4 ----------
if (NOT TARGET CommonLibF4)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../PluginTemplate/CommonLibF4/CommonLibF4" CommonLibF4)
endif()

# ---------- Build ----------
find_package(spdlog REQUIRED CONFIG)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${headers} ${sources})

add_library(${PROJECT_NAME} SHARED ${headers} ${sources})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    BEAUTIFUL_NAME="${BEAUTIFUL_NAME}"
    MOD_NAME="${BEAUTIFUL_NAME}"
    _UNICODE UNICODE NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibF4::CommonLibF4 spdlog::spdlog)

if (MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /sdl /utf-8 /Zi /permissive- /Zc:preprocessor /EHsc /W4 /WX-
        /wd4100 /wd4189 /wd4244)
endif()

# ---------- Output ----------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/Compile/F4SE/Plugins")

option(COPY_BUILD "Copy output to Fallout 4 directory." OFF)
if (COPY_BUILD AND DEFINED Fallout4Path)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${Fallout4Path}/Data/F4SE/Plugins/"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> "${Fallout4Path}/Data/F4SE/Plugins/")
endif()

set(fallout4_mods_output true)
automaticGameFolderOutput(fallout4_mods_output)
```

> **Important**: The `_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING` definition is required because the consumer header uses `std::codecvt_utf8` internally (for Font Awesome icon conversion). Without it, MSVC treats the deprecation warning as an error.

### PCH.h (Precompiled Header)

```cpp
#pragma once

#include <fstream>
#include <spdlog/sinks/basic_file_sink.h>

#include "RE/Fallout.h"
#include "F4SE/F4SE.h"

#define DLLEXPORT __declspec(dllexport)

namespace logger = F4SE::log;
using namespace std::literals;
```

The `DLLEXPORT` macro is required for the F4SE entry point functions. Without it you'll get `F4SEAPI` syntax errors.

### logger.h

```cpp
#pragma once
#include <spdlog/sinks/basic_file_sink.h>

namespace logger = F4SE::log;

void SetupLog() {
    auto logsFolder = F4SE::log::log_directory();
    if (!logsFolder)
        F4SE::stl::report_and_fail("F4SE log_directory not provided, logs disabled.");

    auto logFilePath = *logsFolder / "MyF4SEPlugin.log";
    auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
    spdlog::set_default_logger(std::move(loggerPtr));

#ifndef NDEBUG
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);
#else
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
#endif
}
```

Change `"MyF4SEPlugin.log"` to match your plugin name.

---

## 3. The Consumer Header — F4SEMenuFramework.h

Copy `F4SEMenuFramework.h` from the framework's `resources/` folder into your `include/` directory. This is the **only file you need** from the framework — you do NOT link against `F4SEMenuFramework.lib`.

The header works via **runtime dynamic linking**: it calls `GetModuleHandle(L"F4SEMenuFramework")` and then `GetProcAddress(...)` for every ImGui function and framework API. This means:

- Your plugin has **zero compile-time dependency** on the framework DLL.
- The framework DLL must be loaded by F4SE before your plugin's UI functions are called.
- You should always check `F4SEMenuFramework::IsInstalled()` before calling any framework functions.

The header provides two namespaces:

| Namespace | Purpose |
|-----------|---------|
| `F4SEMenuFramework::` | Framework API (register menus, windows, input, textures) |
| `ImGuiMCP::` | Full Dear ImGui API (buttons, text, tables, draw lists, etc.) |
| `FontAwesome::` | Icon font helpers |

---

## 4. Plugin Entry Point

F4SE plugins use a two-function entry pattern:

```cpp
#include "Plugin.h"  // includes logger.h and UI.h

namespace Plugin {
    static constexpr auto NAME = "MyF4SEPlugin"sv;
    static constexpr auto VERSION = REL::Version{ 1, 0, 0 };
}

// Called first — validate environment, reject editor/wrong runtime
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* a_f4se,
    F4SE::PluginInfo* a_info)
{
    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = Plugin::NAME.data();
    a_info->version = 1;

    if (a_f4se->IsEditor()) return false;
    if (a_f4se->RuntimeVersion() < F4SE::RUNTIME_1_10_162) return false;

    return true;
}

// Called second — do all initialization here
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(
    const F4SE::LoadInterface* a_f4se)
{
    SetupLog();
    logger::info("{} v{} loading", Plugin::NAME, Plugin::VERSION.string());

    F4SE::Init(a_f4se);

    // Register for messaging if you need game-data-ready events
    F4SE::GetMessagingInterface()->RegisterListener(
        [](F4SE::MessagingInterface::Message* msg) {
            if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
                // Safe to look up game forms here
            }
        });

    // Register your menus (framework functions are safe to call here)
    UI::Register();

    logger::info("{} loaded successfully", Plugin::NAME);
    return true;
}
```

### Key differences from SKSE

| SKSE | F4SE |
|------|------|
| `SKSEPluginLoad(const SKSE::LoadInterface*)` | Split into `F4SEPlugin_Query` + `F4SEPlugin_Load` |
| `SKSE::Init(skse)` | `F4SE::Init(f4se)` |
| `SKSE::MessagingInterface::kDataLoaded` | `F4SE::MessagingInterface::kGameDataReady` |
| `namespace logger = SKSE::log` | `namespace logger = F4SE::log` |

---

## 5. Registering Menus

All registration happens in a single setup function called from `F4SEPlugin_Load`:

```cpp
// UI.h
#pragma once
#include "F4SEMenuFramework.h"

namespace UI {
    void Register();

    namespace MyPage {
        void __stdcall Render();
    }
}
```

```cpp
// UI.cpp
#include "UI.h"

void UI::Register() {
    if (!F4SEMenuFramework::IsInstalled()) {
        return;  // framework not present — fail gracefully
    }

    // Set the section name (shown as a top-level group in the Mod Control Panel)
    F4SEMenuFramework::SetSection(MOD_NAME);

    // Add a menu page under your section
    F4SEMenuFramework::AddSectionItem("Settings", MyPage::Render);
}

void __stdcall UI::MyPage::Render() {
    ImGuiMCP::Text("Hello from my plugin!");
    
    static int value = 50;
    ImGuiMCP::SliderInt("My Slider", &value, 0, 100);
    
    if (ImGuiMCP::Button("Click Me")) {
        // do something
    }
}
```

**Important details:**
- `SetSection()` sets the group name. Call it once before any `AddSectionItem` calls.
- `MOD_NAME` is a compile definition set in CMakeLists.txt (from `BEAUTIFUL_NAME`).
- Render callbacks must be `void __stdcall` with no parameters.
- You can use folder separators in item names: `"Folder/Subfolder/Item"`.

---

## 6. Drawing UI with ImGuiMCP

All ImGui functions are accessed through the `ImGuiMCP::` namespace. This is a **full ImGui API** — every widget, layout, and drawing function is available.

### Common Widgets

```cpp
void __stdcall MyRender() {
    // Text
    ImGuiMCP::Text("Plain text");
    ImGuiMCP::TextColored(ImGuiMCP::ImVec4(1, 0, 0, 1), "Red text");
    ImGuiMCP::TextWrapped("Long text that wraps...");

    // Input
    static char buf[256] = "";
    ImGuiMCP::InputText("Name", buf, sizeof(buf));

    static int count = 1;
    ImGuiMCP::InputInt("Count", &count);
    ImGuiMCP::SliderInt("Slider", &count, 1, 100);

    static float color[4] = {1, 1, 1, 1};
    ImGuiMCP::ColorEdit4("Color", color);

    // Buttons
    if (ImGuiMCP::Button("OK")) {
        // clicked
    }
    ImGuiMCP::SameLine();
    if (ImGuiMCP::Button("Cancel")) {
        // clicked
    }

    // Checkbox
    static bool enabled = true;
    ImGuiMCP::Checkbox("Enable feature", &enabled);

    // Combo / Dropdown
    static int current = 0;
    const char* items[] = { "Low", "Medium", "High" };
    ImGuiMCP::Combo("Quality", &current, items, 3);
}
```

### Tables

```cpp
void __stdcall RenderTable() {
    ImGuiMCP::ImGuiTableFlags flags =
        ImGuiMCP::ImGuiTableFlags_Resizable |
        ImGuiMCP::ImGuiTableFlags_RowBg |
        ImGuiMCP::ImGuiTableFlags_BordersOuter;

    if (ImGuiMCP::BeginTable("myTable", 3, flags)) {
        ImGuiMCP::TableSetupColumn("Name");
        ImGuiMCP::TableSetupColumn("Value");
        ImGuiMCP::TableSetupColumn("Action");
        ImGuiMCP::TableHeadersRow();

        for (int i = 0; i < 10; i++) {
            ImGuiMCP::TableNextRow();
            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::Text("Item %d", i);
            ImGuiMCP::TableSetColumnIndex(1);
            ImGuiMCP::Text("%d", i * 10);
            ImGuiMCP::TableSetColumnIndex(2);
            char label[32];
            sprintf(label, "Select##%d", i);
            ImGuiMCP::Button(label);
        }
        ImGuiMCP::EndTable();
    }
}
```

### Scrollable Regions

```cpp
ImGuiMCP::BeginChild("ScrollRegion");
for (int i = 0; i < 100; i++) {
    ImGuiMCP::Text("Line %d", i);
}
ImGuiMCP::EndChild();
```

### Using IM_COL32

The `IM_COL32(r, g, b, a)` macro is defined in the consumer header. However, it expands to use `ImU32` which lives inside the `ImGuiMCP` namespace. If you use `IM_COL32` in your `.cpp` files, you need to bring `ImU32` into scope:

```cpp
#include "UI.h"

using ImGuiMCP::ImU32;  // Required for IM_COL32 macro to resolve

void __stdcall MyRender() {
    auto drawList = ImGuiMCP::GetForegroundDrawList();
    ImGuiMCP::ImDrawListManager::AddText(
        drawList,
        ImGuiMCP::ImVec2(10, 10),
        IM_COL32(255, 255, 255, 255),  // now compiles
        "Hello World");
}
```

---

## 7. Windows (Popup Panels)

Windows are independent floating panels that can be opened/closed. They can optionally pause game input.

```cpp
// UI.h
namespace UI {
    void Register();
    namespace MyWindow {
        inline MENU_WINDOW Handle;
        void __stdcall Render();
        void __stdcall RenderWindow();
    }
}
```

```cpp
// UI.cpp
void UI::Register() {
    if (!F4SEMenuFramework::IsInstalled()) return;
    F4SEMenuFramework::SetSection(MOD_NAME);
    F4SEMenuFramework::AddSectionItem("Open My Window", MyWindow::Render);

    // true = window pauses game input; false = non-blocking overlay
    MyWindow::Handle = F4SEMenuFramework::AddWindow(MyWindow::RenderWindow, true);
}

void __stdcall UI::MyWindow::Render() {
    if (ImGuiMCP::Button("Open Window")) {
        Handle->IsOpen = true;
    }
}

void __stdcall UI::MyWindow::RenderWindow() {
    auto viewport = ImGuiMCP::GetMainViewport();

    // Center the window on first appearance
    auto center = ImGuiMCP::ImVec2Manager::Create();
    ImGuiMCP::ImGuiViewportManager::GetCenter(center, viewport);
    ImGuiMCP::SetNextWindowPos(*center, ImGuiMCP::ImGuiCond_Appearing, ImGuiMCP::ImVec2{0.5f, 0.5f});
    ImGuiMCP::ImVec2Manager::Destroy(center);

    // Size: 40% of screen
    ImGuiMCP::SetNextWindowSize(
        ImGuiMCP::ImVec2{viewport->Size.x * 0.4f, viewport->Size.y * 0.4f},
        ImGuiMCP::ImGuiCond_Appearing);

    // Always add ##YourModName to window titles to avoid collisions with other mods
    ImGuiMCP::Begin("My Window##MyF4SEPlugin", nullptr, ImGuiMCP::ImGuiWindowFlags_MenuBar);

    if (ImGuiMCP::BeginMenuBar()) {
        if (ImGuiMCP::BeginMenu("File")) {
            if (ImGuiMCP::MenuItem("Close", "Ctrl+W")) {
                Handle->IsOpen = false;
            }
            ImGuiMCP::EndMenu();
        }
        ImGuiMCP::EndMenuBar();
    }

    ImGuiMCP::Text("Window content goes here");

    if (ImGuiMCP::Button("Close")) {
        Handle->IsOpen = false;
    }

    ImGuiMCP::End();
}
```

### Window Properties

The `MENU_WINDOW` handle (which is `F4SEMenuFramework::Model::WindowInterface*`) has two atomic fields:

```cpp
Handle->IsOpen;          // std::atomic<bool> — set to true/false to open/close
Handle->BlockUserInput;  // std::atomic<bool> — set at creation via AddWindow's 2nd arg
```

---

## 8. HUD Overlays

HUD elements render every frame **over the game**, even when the Mod Control Panel is closed. They are ideal for persistent on-screen information.

```cpp
void UI::Register() {
    // ...
    F4SEMenuFramework::AddHudElement(MyOverlay::Render);
}

void __stdcall MyOverlay::Render() {
    // Don't draw overlay when a blocking menu is open
    if (F4SEMenuFramework::IsAnyBlockingWindowOpened()) return;

    auto drawList = ImGuiMCP::GetForegroundDrawList();

    const char* text = "My HUD Text";
    ImGuiMCP::ImVec2 textSize;
    ImGuiMCP::CalcTextSize(&textSize, text, 0, false, 0);

    // Position at top-right corner
    ImGuiMCP::ImVec2 pos = ImGuiMCP::ImVec2(
        ImGuiMCP::GetIO()->DisplaySize.x - textSize.x - 20,
        20);

    ImGuiMCP::ImDrawListManager::AddText(
        drawList, pos, IM_COL32(255, 255, 255, 255), text);
}
```

---

## 9. Input Events

Input event callbacks let your plugin intercept keyboard/gamepad input, even outside the Mod Control Panel. Use them to toggle custom windows with hotkeys.

```cpp
void UI::Register() {
    // ...
    F4SEMenuFramework::AddInputEvent(MyInput::OnInput);
}

// Return true to consume the input (prevent game from seeing it)
// Return false to let the input pass through
bool __stdcall MyInput::OnInput(RE::InputEvent* event) {
    if (*event->device == RE::INPUT_DEVICE::kKeyboard) {
        if (auto button = event->As<RE::ButtonEvent>()) {
            // 0x30 = DirectInput scan code for 'B'
            if (button->idCode == 0x30 && button->JustPressed()) {
                MyWindow::Handle->IsOpen = !MyWindow::Handle->IsOpen;
                return true;  // consume the input
            }
        }
    }
    return false;
}
```

### CommonLibF4 Input API Reference

In CommonLibF4, input events are accessed via **direct member access**, not getter methods:

| What | How |
|------|-----|
| Event type | `*event->eventType` (dereference the stl::enumeration) |
| Device type | `*event->device` |
| Cast to ButtonEvent | `event->As<RE::ButtonEvent>()` |
| Cast to CharacterEvent | `event->As<RE::CharacterEvent>()` |
| Key/button code | `button->idCode` (DirectInput scan code) |
| Is pressed | `button->JustPressed()` |
| Held duration | `button->heldDownSecs` |
| Press value | `button->value` (0.0 = released, non-zero = pressed) |
| Character code | `charEvent->charCode` |

### Common DirectInput Keyboard Scan Codes

```
0x01 = Escape        0x0F = Tab           0x1C = Enter
0x02 = 1             0x10 = Q             0x1D = Left Ctrl
0x03 = 2             0x11 = W             0x2A = Left Shift
0x04 = 3             0x12 = E             0x38 = Left Alt
0x0E = Backspace     0x13 = R             0x39 = Space
0x1E = A             0x20 = D             0x3B = F1
0x1F = S             0x21 = F             0x3C = F2
0x30 = B             0x31 = N             0x3D = F3
```

Full list: search for "DirectInput keyboard scan codes" or DIK constants.

### Device Types

```cpp
RE::INPUT_DEVICE::kKeyboard
RE::INPUT_DEVICE::kMouse
RE::INPUT_DEVICE::kGamepad
```

---

## 10. Loading Textures and Images

The framework can load PNG, JPG, and SVG images for use as ImGui textures. Results are cached internally.

```cpp
void __stdcall MyRender() {
    // SVG with explicit size (rasterized at the given dimensions)
    auto icon = F4SEMenuFramework::LoadTexture("Data\\F4SE\\Plugins\\MyMod\\icon.svg", {64, 64});

    // PNG/JPG (size determined from the image file)
    auto photo = F4SEMenuFramework::LoadTexture("Data\\F4SE\\Plugins\\MyMod\\photo.png");

    // Display as image
    ImGuiMCP::Image(icon, ImGuiMCP::ImVec2(64, 64));

    // Display as clickable image button
    if (ImGuiMCP::ImageButton("myButton##id", icon, {64, 64})) {
        // clicked
    }
}
```

**Paths** are relative to the Fallout 4 game directory (where `Fallout4.exe` lives). Use backslashes in paths.

---

## 11. Font Awesome Icons

The framework bundles Font Awesome 6. Use the helper functions to push icon fonts:

```cpp
void __stdcall MyRender() {
    // Solid icons (filled)
    FontAwesome::PushSolid();
    ImGuiMCP::Text(FontAwesome::UnicodeToUtf8(0xf00c).c_str());  // checkmark
    FontAwesome::Pop();

    // Regular icons (outline)
    FontAwesome::PushRegular();
    ImGuiMCP::Button(FontAwesome::UnicodeToUtf8(0xf06e).c_str());  // eye
    FontAwesome::Pop();

    // Brand icons
    FontAwesome::PushBrands();
    ImGuiMCP::Text(FontAwesome::UnicodeToUtf8(0xf2b4).c_str());
    FontAwesome::Pop();

    // Mix icon + text in a button
    FontAwesome::PushSolid();
    std::string label = FontAwesome::UnicodeToUtf8(0xf0e9) + " Umbrella";
    ImGuiMCP::Button(label.c_str());
    FontAwesome::Pop();
}
```

Find icon Unicode codepoints at [fontawesome.com/icons](https://fontawesome.com/icons).

---

## 12. Working with Game Forms (CommonLibF4)

### Looking Up Forms

```cpp
// By form ID
auto form = RE::TESForm::GetFormByID(0x00064B33);

// Typed lookup
auto weapon = RE::TESForm::GetFormByID<RE::TESObjectWEAP>(0x00064B33);

// Cast to specific type
auto boundObj = form->As<RE::TESBoundObject>();
```

### Getting a Form's Name

Unlike Skyrim's CommonLibSSE, `TESBoundObject` does **not** have a `GetName()` method in CommonLibF4. The name comes from the `TESFullName` component, which is mixed in via multiple inheritance on specific subclasses (weapons, armor, misc items, etc.).

Use `dynamic_cast` to check if the form has a name:

```cpp
const char* GetFormName(RE::TESBoundObject* obj) {
    if (auto* fullName = dynamic_cast<RE::TESFullName*>(obj)) {
        return fullName->GetFullName();
    }
    return "Unknown";
}
```

### Adding Items to the Player

```cpp
auto player = RE::PlayerCharacter::GetSingleton();
if (player && boundObj) {
    // ITEM_REMOVE_REASON is forward-declared but not defined in CommonLibF4.
    // Use static_cast<RE::ITEM_REMOVE_REASON>(0) for the "kNone" reason.
    player->AddObjectToContainer(
        boundObj,
        nullptr,  // extra data (BSTSmartPointer<ExtraDataList>)
        count,    // std::int32_t
        nullptr,  // old container (TESObjectREFR*)
        static_cast<RE::ITEM_REMOVE_REASON>(0));
}
```

### Delayed Form Lookup

Forms are not available until game data loads. Register a messaging callback and do lookups in `kGameDataReady`:

```cpp
F4SE::GetMessagingInterface()->RegisterListener(
    [](F4SE::MessagingInterface::Message* msg) {
        if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
            // Safe to call RE::TESForm::GetFormByID here
            MyData::LookupForms();
        }
    });
```

---

## 13. Build and Deploy

### Configure and Build

```powershell
cd MyF4SEPlugin
cmake --preset release
cmake --build build/release --config Release
```

Output: `Compile/F4SE/Plugins/MyF4SEPlugin.dll`

### Deploy

Copy to your Fallout 4 data folder:

```
Fallout 4/Data/F4SE/Plugins/MyF4SEPlugin.dll
```

Also ensure the framework is installed:

```
Fallout 4/Data/F4SE/Plugins/F4SEMenuFramework.dll
Fallout 4/Data/F4SE/Plugins/F4SEMenuFramework.ini
Fallout 4/Data/F4SE/Plugins/F4SEMenuFrameworkStrings.json
Fallout 4/Data/F4SE/Plugins/Fonts/               (font files)
Fallout 4/Data/F4SE/Plugins/F4SEMenuFrameworkThemes/  (theme files)
```

### Auto-Copy on Build

Set the `FALLOUT4_FOLDER` environment variable to your game install path and the build system will auto-copy the DLL after each build.

---

## 14. Gotchas and F4SE-Specific Differences

### vs. SKSE Menu Framework

If you're porting a plugin from the SKSE version, here are the key changes:

| Area | SKSE | F4SE |
|------|------|------|
| Consumer header | `SKSEMenuFramework.h` | `F4SEMenuFramework.h` |
| Framework namespace | `SKSEMenuFramework::` | `F4SEMenuFramework::` |
| DLL check path | `Data/SKSE/Plugins/SKSEMenuFramework.dll` | `Data/F4SE/Plugins/F4SEMenuFramework.dll` |
| Entry point | `SKSEPluginLoad()` | `F4SEPlugin_Query()` + `F4SEPlugin_Load()` |
| Data loaded event | `SKSE::MessagingInterface::kDataLoaded` | `F4SE::MessagingInterface::kGameDataReady` |
| Input event cast | `event->AsButtonEvent()` | `event->As<RE::ButtonEvent>()` |
| Key code access | `button->GetIDCode()` | `button->idCode` |
| Is pressed | `button->IsDown()` | `button->JustPressed()` |
| Device type | `event->GetDevice()` | `*event->device` |
| Held duration | `button->HeldDuration()` | `button->heldDownSecs` |
| Keyboard keys | `RE::BSWin32KeyboardDevice::Key::kB` | Raw DIK code `0x30` |
| Form lookup | `RE::TESForm::LookupByID(...)` | `RE::TESForm::GetFormByID(...)` |
| Get form name | `boundObj->GetName()` | `dynamic_cast<RE::TESFullName*>(obj)->GetFullName()` |
| Macro | `DLLEXPORT` defined by CommonLibSSE | Must define `#define DLLEXPORT __declspec(dllexport)` in PCH.h |

### Common Runtime Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| ImGui overlay never appears, no crash | D3D11 `Present` hook installed on a dummy device VTable instead of the game's real swap chain | Hook `D3D11CreateDeviceAndSwapChain` via `write_call<5>` on call site `REL::ID(224250)+0x419` and patch the real swap chain's VTable inside (see §16) |
| D3D11 IAT hook installed but thunk never called | Game caches the `D3D11CreateDeviceAndSwapChain` function pointer from the IAT during early init, before F4SE plugins load. Patching the IAT entry afterward has no effect | Use `write_call<5>` on the call site (`REL::ID(224250)+0x419`) instead of IAT replacement (`REL::ID(254484)`) — this patches the CALL instruction itself, which cannot be bypassed |
| Crash on startup in `keyboardThunk` or `mouseThunk` (RCX=0) | `ImGui::GetIO()` called before `ImGui::CreateContext()` — input device poll hooks run before first `Present` call | Guard: `if (!initialized.load()) return;` in all input thunks |
| Menu renders but mouse can't click anything | Game's `ClipCursor()` confines cursor to a narrow area | Hook `ClipCursor` via IAT (`REL::ID(641385)`) and pass full window rect when menu is open |
| Menu renders but player still walks/shoots | Game still processing keyboard/mouse input | Set `RE::ControlMap::GetSingleton()->ignoreKeyboardMouse = true` when menu opens |
| Toggle key (F1) doesn't open menu, no crash | Hotkey detection was in `BSInputDevice::Poll` hooks or `RE::InputEvent` processing, but those never receive keypress data usable for toggling | Move toggle key detection to `WndProcHook` via `WM_KEYDOWN` — extract scan code from `lParam` bits 16–23 and compare with DIK code. Both Shadow-Boost and GunMover use this pattern (see §16) |
| Menu opens but game still processes input alongside ImGui | WndProc always calls the original `WndProc` even when menu is active | When menu is active, forward to `ImGui_ImplWin32_WndProcHandler` and `return true` to block the game. Only call original WndProc when menu is closed |

### Common Compile Errors and Fixes

| Error | Fix |
|-------|-----|
| `'F4SEAPI': undeclared` or `'bool' should be preceded by ';'` | Add `#define DLLEXPORT __declspec(dllexport)` to PCH.h |
| `'ImU32': undeclared identifier` (when using `IM_COL32`) | Add `using ImGuiMCP::ImU32;` at top of your .cpp file |
| `codecvt deprecation error C4996` | Add `_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING` to compile definitions |
| `'InputScalar' is not a member of 'ImGuiMCP'` | Ensure `F4SEMenuFramework.h` does NOT have `namespace ImGui {}` wrapping the function definitions (around line 3709). The functions must be directly inside `ImGuiMCP`. |
| `'std::basic_ifstream' undefined` | Add `#include <fstream>` to PCH.h |
| `'GetName' is not a member of 'RE::TESBoundObject'` | Use `dynamic_cast<RE::TESFullName*>(obj)->GetFullName()` instead |
| `ITEM_REMOVE_REASON::kNone` not found | Use `static_cast<RE::ITEM_REMOVE_REASON>(0)` — the enum is only forward-declared |

---

## 15. Complete Minimal Plugin

Here is the absolute minimum for a working plugin with one menu page:

**include/PCH.h**
```cpp
#pragma once
#include <fstream>
#include <spdlog/sinks/basic_file_sink.h>
#include "RE/Fallout.h"
#include "F4SE/F4SE.h"
#define DLLEXPORT __declspec(dllexport)
namespace logger = F4SE::log;
using namespace std::literals;
```

**include/logger.h**
```cpp
#pragma once
#include <spdlog/sinks/basic_file_sink.h>
namespace logger = F4SE::log;
void SetupLog() {
    auto logsFolder = F4SE::log::log_directory();
    if (!logsFolder) F4SE::stl::report_and_fail("F4SE log_directory not provided.");
    auto logFilePath = *logsFolder / "MyPlugin.log";
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto log = std::make_shared<spdlog::logger>("log", std::move(sink));
    spdlog::set_default_logger(std::move(log));
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
}
```

**include/UI.h**
```cpp
#pragma once
#include "F4SEMenuFramework.h"
namespace UI {
    void Register();
    namespace Settings {
        void __stdcall Render();
    }
}
```

**src/UI.cpp**
```cpp
#include "UI.h"

void UI::Register() {
    if (!F4SEMenuFramework::IsInstalled()) return;
    F4SEMenuFramework::SetSection(MOD_NAME);
    F4SEMenuFramework::AddSectionItem("Settings", Settings::Render);
}

void __stdcall UI::Settings::Render() {
    ImGuiMCP::Text("Welcome to my plugin!");

    static bool feature = true;
    ImGuiMCP::Checkbox("Enable Feature", &feature);

    static int power = 50;
    ImGuiMCP::SliderInt("Power Level", &power, 0, 100);

    if (ImGuiMCP::Button("Apply")) {
        // Apply settings to game...
    }
}
```

**src/plugin.cpp**
```cpp
#include "logger.h"
#include "UI.h"

namespace Plugin {
    static constexpr auto NAME = "MyPlugin"sv;
    static constexpr auto VERSION = REL::Version{1, 0, 0};
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info) {
    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = Plugin::NAME.data();
    a_info->version = 1;
    if (a_f4se->IsEditor()) return false;
    if (a_f4se->RuntimeVersion() < F4SE::RUNTIME_1_10_162) return false;
    return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(
    const F4SE::LoadInterface* a_f4se) {
    SetupLog();
    logger::info("{} v{} loading", Plugin::NAME, Plugin::VERSION.string());
    F4SE::Init(a_f4se);
    UI::Register();
    logger::info("{} loaded", Plugin::NAME);
    return true;
}
```

Build it, drop the DLL into `Data/F4SE/Plugins/`, launch the game with F4SE, and press the framework's toggle key (default: **F1**) to see your menu in the Mod Control Panel.

---

## 16. Framework Internals — How the Hooks Work

This section documents how the framework hooks into the game engine. You don't need to do any of this in your consumer plugin — the framework handles it all. This is reference material for understanding what happens behind the scenes and for anyone maintaining or forking the framework.

### D3D11 Present Hook via `write_call<5>`

The framework hooks `D3D11CreateDeviceAndSwapChain` to intercept the game's real device creation, then patches the real swap chain's `Present` VTable entry.

**Three approaches were tried — only the third works reliably:**

1. **Dummy null device trick** — fundamentally broken. The dummy device's VTable is a separate instance; patching and releasing it accomplishes nothing.

2. **IAT pointer replacement** (`REL::ID(254484)`) — the hook installs successfully but **never gets called**. The game resolves the `D3D11CreateDeviceAndSwapChain` function pointer from the IAT once during early initialization (before F4SE plugins load) and caches it. Patching the IAT entry after that point has no effect — the game calls through the cached pointer, not the IAT.

3. **`write_call<5>` on the call site** (`REL::ID(224250) + 0x419`) — **this is what works.** It patches the actual CALL machine instruction at the point where the game invokes `D3D11CreateDeviceAndSwapChain`. This cannot be bypassed by pointer caching. GunMover uses this exact approach.

```cpp
// Allocate trampoline space (must be done in F4SEPlugin_Load before hook install)
F4SE::AllocTrampoline(128);

// Hook the CALL instruction at the call site
REL::Relocation<std::uintptr_t> callSite{ REL::ID(224250), 0x419 };
auto& trampoline = F4SE::GetTrampoline();
originalFunc = reinterpret_cast<FnType>(
    trampoline.write_call<5>(callSite.address(), &CreateDeviceHook));
```

Inside the hook, after the original function succeeds, we patch the **real** swap chain's VTable:

```cpp
auto* vtbl = reinterpret_cast<std::uintptr_t*>(*reinterpret_cast<std::uintptr_t*>(*ppSwapChain));
originalPresent = reinterpret_cast<PresentFn>(vtbl[8]);
REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtbl[8]),
    reinterpret_cast<std::uintptr_t>(&PresentHook));
```

### ClipCursor Hook (REL::ID 641385)

Fallout 4 calls `ClipCursor()` every frame to lock the mouse cursor to the game window. Without hooking this, the cursor can't move freely to interact with ImGui widgets even when the menu renders. The framework hooks `ClipCursor` via its IAT entry and passes the full window rect instead when a blocking menu is open.

### ControlMap::ignoreKeyboardMouse

When a blocking menu is open, the framework sets `RE::ControlMap::GetSingleton()->ignoreKeyboardMouse = true`. This tells the game engine to stop processing keyboard and mouse input, preventing the player from walking, looking around, or firing while interacting with ImGui. It is reset to `false` when the menu closes.

### WndProc Hook — Hotkey Detection and Input Forwarding

The framework replaces the game window's `WndProc` via `SetWindowLongPtrA` on the first `Present` call (after ImGui is initialized). The WndProc hook serves **two critical purposes**:

**1. Toggle key detection via `WM_KEYDOWN`**

Both Shadow-Boost-FO4 and GunMover detect their hotkeys in WndProc, not through Bethesda's `RE::InputEvent` pipeline. This is the only reliable approach because:

- `BSInputDevice::Poll` VTable hooks run during device polling, **before** the game builds the `InputEvent` chain — there are no events to read yet
- Bethesda's `InputEvent` processing pipeline is internal and has no clean F4SE hook point equivalent to SKSE's `ProcessInputQueueHook`
- WndProc receives all Windows keyboard messages regardless of game state, making it reliable for hotkey detection

The toggle key is detected by extracting the **scan code** from `WM_KEYDOWN`'s `lParam` (bits 16–23). DirectInput scan codes and Windows hardware scan codes are identical, so `Config::ToggleKey` (a DIK code like `0x3B` for F1) can be compared directly:

```cpp
if (uMsg == WM_KEYDOWN) {
    UINT scanCode = (lParam >> 16) & 0xFF;
    bool isFirstPress = (lParam & 0x40000000) == 0;  // bit 30 = was previously down

    if (scanCode == Config::ToggleKey && isFirstPress) {
        // Toggle menu open/close
    }
}
```

All three toggle modes (single press, hold, double press) are supported using `WM_KEYDOWN` repeat detection and timing.

**2. ImGui input forwarding with game input blocking**

When a blocking menu is open, the hook forwards all messages to `ImGui_ImplWin32_WndProcHandler` and **returns `true`** to prevent the game from processing the input. This matches the pattern used by both reference projects:

```cpp
if (menuIsActive) {
    ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
    return true;  // block game from seeing this message
}
return CallWindowProcA(originalWndProc, hWnd, uMsg, wParam, lParam);
```

Returning `true` when the menu is active is critical — without it, both the game and ImGui process the same input simultaneously (the player moves/shoots while clicking menu widgets).

### ImGui Initialization Order

1. `F4SEPlugin_Load` → `F4SE::AllocTrampoline(128)` → `Hooks::Install()` → patches `D3D11CreateDeviceAndSwapChain` call site via `write_call<5>`, patches `ClipCursor` IAT
2. Game calls `D3D11CreateDeviceAndSwapChain` → our hook patches the real swap chain's `Present`
3. First `Present` call → `IMGUI_CHECKVERSION()`, `ImGui::CreateContext()`, `ImGui_ImplWin32_Init()`, `ImGui_ImplDX11_Init()`, WndProc replacement, font loading
4. `kGameDataReady` message → `DevicePollHook::install()` patches keyboard/mouse `BSInputDevice::Poll` VTables

### Key Address Library IDs Used

| REL::ID | Function | Hook Type | Notes |
|---------|----------|-----------|-------|
| `224250` + `0x419` | `D3D11CreateDeviceAndSwapChain` call site | `write_call<5>` (trampoline) | **Use this one.** Patches the CALL instruction directly |
| `254484` | `D3D11CreateDeviceAndSwapChain` IAT entry | IAT pointer replacement | **Does NOT work** — game caches the pointer before plugins load |
| `641385` | `ClipCursor` IAT entry | IAT pointer replacement | Works (called per-frame, not cached) |
| `325206` | `ControlMap` singleton pointer | Singleton access (read) | |
| `IDXGISwapChain VTable[8]` | `Present` | VTable pointer replacement | Patched inside CreateDevice hook |
| `BSInputDevice VTable[1]` | `Poll` | VTable pointer replacement | |

### Reference Projects for ImGui in F4SE

| Project | URL | Key Pattern |
|---------|-----|-------------|
| Shadow-Boost-FO4 | [GitHub](https://github.com/P-K-0/Shadow-Boost-FO4) | IAT hook for D3D11+ClipCursor, ControlMap input blocking, WndProc via RegisterClassEx hook |
| GunMover | [GitHub](https://github.com/jarari/GunMover) | `write_call<5>` for D3D11, ComPtr for device lifetime, SetWindowLongPtr for WndProc |
