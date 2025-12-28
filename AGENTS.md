# Tracy Profiler - Agent Guidelines

## Build Commands
- **fxexport**: `cmake -B fxexport/build -S fxexport && cmake --build fxexport/build --parallel`
- **profiler**: `cmake --preset ninja-vcpkg && cmake --build build`
- **capture/csvexport**: similar cmake pattern in respective directories

## Project Structure
- `public/` - Client instrumentation API (TracyClient.cpp, tracy/ headers)
- `server/` - Core trace parsing/analysis (TracyWorker, TracyFileRead)
- `profiler/` - GUI profiler application (ImGui-based)
- `capture/` - Headless trace capture tool
- `csvexport/` - Export traces to CSV
- `fxexport/` - Export traces to Firefox Profiler JSON format
- `import/` - Import from other formats (Chrome, Fuchsia, etc.)

## Code Style
- **Formatting**: Microsoft-based style, 4-space indent, Allman braces, no column limit
- **Pointers**: Left-aligned (`int* ptr`)
- **Naming**: PascalCase for types/methods, camelCase for variables
- **Headers**: Use `#pragma once`, group includes: system, then tracy, then local
- **Inline**: Functions in headers must be `inline`
- Do NOT reformat entire files - match surrounding style
