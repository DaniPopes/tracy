#ifdef _WIN32
#  include <windows.h>
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../getopt/getopt.h"

using json = nlohmann::json;

static void print_usage_exit(int e)
{
    fprintf(stderr, "Export a Tracy trace to Firefox Profiler JSON format\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  fxexport [OPTION...] <trace file>\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -h, --help           Print usage\n");
    fprintf(stderr, "  -o, --output FILE    Output file (default: stdout)\n");
    exit(e);
}

struct Args {
    const char* trace_file;
    const char* output;
};

static Args parse_args(int argc, char** argv)
{
    if (argc == 1)
    {
        print_usage_exit(1);
    }

    Args args = { nullptr, nullptr };

    struct option long_opts[] = {
        { "help", no_argument, NULL, 'h' },
        { "output", required_argument, NULL, 'o' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "ho:", long_opts, NULL)) != -1)
    {
        switch (c)
        {
        case 'h':
            print_usage_exit(0);
            break;
        case 'o':
            args.output = optarg;
            break;
        default:
            print_usage_exit(1);
            break;
        }
    }

    if (argc != optind + 1)
    {
        print_usage_exit(1);
    }

    args.trace_file = argv[optind];

    return args;
}

class StringTable
{
public:
    uint32_t intern(const std::string& s)
    {
        auto it = m_map.find(s);
        if (it != m_map.end()) return it->second;
        uint32_t idx = static_cast<uint32_t>(m_strings.size());
        m_strings.push_back(s);
        m_map[s] = idx;
        return idx;
    }

    uint32_t intern(const char* s)
    {
        return intern(std::string(s ? s : ""));
    }

    json to_json() const
    {
        return json(m_strings);
    }

private:
    std::vector<std::string> m_strings;
    std::unordered_map<std::string, uint32_t> m_map;
};

struct MarkerData
{
    const char* name;
    const char* text;
    int64_t startNs;
    int64_t endNs;
};

enum class MarkerPhase {
    Instant = 0,
    Interval = 1,
    IntervalStart = 2,
    IntervalEnd = 3,
};

static double ns_to_ms(int64_t ns)
{
    return static_cast<double>(ns) / 1e6;
}

static bool is_kernel_address(uint64_t addr)
{
    return (addr >> 63) != 0;
}

static void collect_zone(
    const tracy::Worker& worker,
    const tracy::ZoneEvent& zone,
    std::vector<MarkerData>& markers,
    int64_t& minTime,
    int64_t& maxTime);

static void collect_zones_recursive(
    const tracy::Worker& worker,
    const tracy::Vector<tracy::short_ptr<tracy::ZoneEvent>>& zones,
    std::vector<MarkerData>& markers,
    int64_t& minTime,
    int64_t& maxTime)
{
    if (zones.is_magic())
    {
        auto& vec = *(tracy::Vector<tracy::ZoneEvent>*)&zones;
        for (auto& zone : vec)
        {
            collect_zone(worker, zone, markers, minTime, maxTime);
        }
    }
    else
    {
        for (auto& zonePtr : zones)
        {
            if (!zonePtr) continue;
            collect_zone(worker, *zonePtr, markers, minTime, maxTime);
        }
    }
}

static void collect_zone(
    const tracy::Worker& worker,
    const tracy::ZoneEvent& zone,
    std::vector<MarkerData>& markers,
    int64_t& minTime,
    int64_t& maxTime)
{
    if (!zone.IsEndValid()) return;

    const char* name = worker.GetZoneName(zone);
    const char* text = nullptr;
    if (worker.HasZoneExtra(zone))
    {
        const auto& extra = worker.GetZoneExtra(zone);
        if (extra.text.Active())
        {
            text = worker.GetString(extra.text);
        }
    }
    int64_t start = zone.Start();
    int64_t end = zone.End();

    markers.push_back({ name, text, start, end });
    minTime = std::min(minTime, start);
    maxTime = std::max(maxTime, end);

    if (zone.HasChildren())
    {
        auto& children = worker.GetZoneChildren(zone.Child());
        collect_zones_recursive(worker, children, markers, minTime, maxTime);
    }
}

struct ThreadTables
{
    struct FrameEntry
    {
        uint32_t funcIdx;
        uint32_t nativeSymbolIdx;
        uint32_t category;
        int64_t address;
        uint32_t line;
        uint32_t column;
        uint32_t inlineDepth;
    };

    struct FuncEntry
    {
        uint32_t nameIdx;
        int32_t resourceIdx;
        uint32_t fileNameIdx;
        uint32_t lineNumber;
        uint32_t columnNumber;
    };

    struct NativeSymbolEntry
    {
        int32_t libIndex;
        uint64_t address;
        uint32_t nameIdx;
        uint32_t functionSize;
    };

    struct ResourceEntry
    {
        int32_t libIdx;
        uint32_t nameIdx;
    };

    struct StackEntry
    {
        int32_t prefix;
        uint32_t frame;
    };

    struct SampleEntry
    {
        double time;
        int32_t stackIdx;
        double weight;
    };

    struct MarkerEntry
    {
        uint32_t category;
        uint32_t nameIdx;
        uint32_t textIdx;
        bool hasText;
        uint32_t markerNameIdx;
        double startTime;
        double endTime;
        MarkerPhase phase;
    };

    std::vector<FrameEntry> frames;
    std::vector<FuncEntry> funcs;
    std::vector<NativeSymbolEntry> nativeSymbols;
    std::vector<ResourceEntry> resources;
    std::vector<StackEntry> stacks;
    std::vector<SampleEntry> samples;
    std::vector<MarkerEntry> markers;

    std::unordered_map<uint64_t, uint32_t> symAddrToNativeSymbol;
    std::unordered_map<uint64_t, uint32_t> symAddrToFunc;
    std::unordered_map<std::string, uint32_t> libNameToResource;
    std::unordered_map<uint64_t, uint32_t> frameKeyToFrame;
    std::unordered_map<uint64_t, int32_t> stackKeyToStack;

    uint32_t getOrCreateResource(StringTable& st, const char* libName)
    {
        std::string key(libName ? libName : "");
        auto it = libNameToResource.find(key);
        if (it != libNameToResource.end()) return it->second;

        uint32_t idx = static_cast<uint32_t>(resources.size());
        resources.push_back({
            -1,
            st.intern(libName)
        });
        libNameToResource[key] = idx;
        return idx;
    }

    uint32_t getOrCreateNativeSymbol(StringTable& st, uint64_t symAddr, const char* name, const char* imageName, uint32_t size)
    {
        auto it = symAddrToNativeSymbol.find(symAddr);
        if (it != symAddrToNativeSymbol.end()) return it->second;

        int32_t libIdx = -1;
        if (imageName && imageName[0])
        {
            libIdx = static_cast<int32_t>(getOrCreateResource(st, imageName));
        }

        uint32_t idx = static_cast<uint32_t>(nativeSymbols.size());
        nativeSymbols.push_back({
            libIdx,
            symAddr,
            st.intern(name),
            size
        });
        symAddrToNativeSymbol[symAddr] = idx;
        return idx;
    }

    uint32_t getOrCreateFunc(StringTable& st, uint64_t symAddr, const char* name, const char* fileName, uint32_t line, int32_t resourceIdx)
    {
        auto it = symAddrToFunc.find(symAddr);
        if (it != symAddrToFunc.end()) return it->second;

        uint32_t idx = static_cast<uint32_t>(funcs.size());
        funcs.push_back({
            st.intern(name),
            resourceIdx,
            st.intern(fileName),
            line,
            0
        });
        symAddrToFunc[symAddr] = idx;
        return idx;
    }

    uint32_t getOrCreateFrame(StringTable& st, uint64_t symAddr, const char* name, const char* fileName,
                               uint32_t line, uint32_t column, uint32_t inlineDepth,
                               const char* imageName, uint32_t symSize, uint32_t category)
    {
        uint64_t frameKey = symAddr ^ (static_cast<uint64_t>(inlineDepth) << 48);
        auto it = frameKeyToFrame.find(frameKey);
        if (it != frameKeyToFrame.end()) return it->second;

        int32_t resourceIdx = -1;
        if (imageName && imageName[0])
        {
            resourceIdx = static_cast<int32_t>(getOrCreateResource(st, imageName));
        }

        uint32_t funcIdx = getOrCreateFunc(st, symAddr, name, fileName, line, resourceIdx);
        uint32_t nativeSymbolIdx = getOrCreateNativeSymbol(st, symAddr, name, imageName, symSize);

        uint32_t idx = static_cast<uint32_t>(frames.size());
        frames.push_back({
            funcIdx,
            nativeSymbolIdx,
            category,
            static_cast<int64_t>(symAddr),
            line,
            column,
            inlineDepth
        });
        frameKeyToFrame[frameKey] = idx;
        return idx;
    }

    int32_t getOrCreateStack(int32_t prefix, uint32_t frame)
    {
        uint64_t key = (static_cast<uint64_t>(prefix + 1) << 32) | frame;
        auto it = stackKeyToStack.find(key);
        if (it != stackKeyToStack.end()) return it->second;

        int32_t idx = static_cast<int32_t>(stacks.size());
        stacks.push_back({ prefix, frame });
        stackKeyToStack[key] = idx;
        return idx;
    }

    json frameTableToJson() const
    {
        json address = json::array();
        json category = json::array();
        json subcategory = json::array();
        json func = json::array();
        json nativeSymbol = json::array();
        json innerWindowID = json::array();
        json line = json::array();
        json column = json::array();
        json inlineDepth = json::array();

        for (const auto& f : frames)
        {
            address.push_back(f.address);
            category.push_back(f.category);
            subcategory.push_back(nullptr);
            func.push_back(f.funcIdx);
            nativeSymbol.push_back(f.nativeSymbolIdx);
            innerWindowID.push_back(nullptr);
            line.push_back(f.line > 0 ? json(f.line) : json(nullptr));
            column.push_back(f.column > 0 ? json(f.column) : json(nullptr));
            inlineDepth.push_back(f.inlineDepth);
        }

        return {
            {"length", frames.size()},
            {"address", address},
            {"category", category},
            {"subcategory", subcategory},
            {"func", func},
            {"nativeSymbol", nativeSymbol},
            {"innerWindowID", innerWindowID},
            {"line", line},
            {"column", column},
            {"inlineDepth", inlineDepth}
        };
    }

    json funcTableToJson() const
    {
        json name = json::array();
        json isJS = json::array();
        json relevantForJS = json::array();
        json resource = json::array();
        json fileName = json::array();
        json lineNumber = json::array();
        json columnNumber = json::array();

        for (const auto& f : funcs)
        {
            name.push_back(f.nameIdx);
            isJS.push_back(false);
            relevantForJS.push_back(false);
            resource.push_back(f.resourceIdx);
            fileName.push_back(f.fileNameIdx);
            lineNumber.push_back(f.lineNumber > 0 ? json(f.lineNumber) : json(nullptr));
            columnNumber.push_back(f.columnNumber > 0 ? json(f.columnNumber) : json(nullptr));
        }

        return {
            {"length", funcs.size()},
            {"name", name},
            {"isJS", isJS},
            {"relevantForJS", relevantForJS},
            {"resource", resource},
            {"fileName", fileName},
            {"lineNumber", lineNumber},
            {"columnNumber", columnNumber}
        };
    }

    json nativeSymbolsToJson() const
    {
        json libIndex = json::array();
        json address = json::array();
        json name = json::array();
        json functionSize = json::array();

        for (const auto& ns : nativeSymbols)
        {
            libIndex.push_back(ns.libIndex);
            address.push_back(ns.address);
            name.push_back(ns.nameIdx);
            functionSize.push_back(ns.functionSize > 0 ? json(ns.functionSize) : json(nullptr));
        }

        return {
            {"length", nativeSymbols.size()},
            {"libIndex", libIndex},
            {"address", address},
            {"name", name},
            {"functionSize", functionSize}
        };
    }

    json resourceTableToJson() const
    {
        json lib = json::array();
        json name = json::array();
        json host = json::array();
        json type = json::array();

        for (const auto& r : resources)
        {
            lib.push_back(r.libIdx);
            name.push_back(r.nameIdx);
            host.push_back(nullptr);
            type.push_back(1);
        }

        return {
            {"length", resources.size()},
            {"lib", lib},
            {"name", name},
            {"host", host},
            {"type", type}
        };
    }

    json stackTableToJson() const
    {
        json prefix = json::array();
        json frame = json::array();

        for (const auto& s : stacks)
        {
            prefix.push_back(s.prefix >= 0 ? json(s.prefix) : json(nullptr));
            frame.push_back(s.frame);
        }

        return {
            {"length", stacks.size()},
            {"prefix", prefix},
            {"frame", frame}
        };
    }

    json samplesToJson() const
    {
        json stack = json::array();
        json timeDeltas = json::array();
        json weight = json::array();
        json threadCPUDelta = json::array();

        double prevTime = 0.0;
        for (const auto& s : samples)
        {
            stack.push_back(s.stackIdx >= 0 ? json(s.stackIdx) : json(nullptr));
            timeDeltas.push_back(s.time - prevTime);
            weight.push_back(s.weight);
            threadCPUDelta.push_back(nullptr);
            prevTime = s.time;
        }

        return {
            {"length", samples.size()},
            {"stack", stack},
            {"timeDeltas", timeDeltas},
            {"weight", weight},
            {"weightType", "samples"},
            {"threadCPUDelta", threadCPUDelta}
        };
    }

    json markersToJson() const
    {
        json category = json::array();
        json data = json::array();
        json name = json::array();
        json startTime = json::array();
        json endTime = json::array();
        json phase = json::array();

        for (const auto& m : markers)
        {
            category.push_back(m.category);
            json markerData = {
                {"type", "TracyZone"},
                {"name", m.nameIdx}
            };
            if (m.hasText)
            {
                markerData["text"] = m.textIdx;
            }
            data.push_back(std::move(markerData));
            name.push_back(m.markerNameIdx);
            startTime.push_back(m.startTime);
            endTime.push_back(m.endTime);
            phase.push_back(static_cast<int>(m.phase));
        }

        return {
            {"length", markers.size()},
            {"category", category},
            {"data", data},
            {"name", name},
            {"startTime", startTime},
            {"endTime", endTime},
            {"phase", phase}
        };
    }
};

static void process_markers(
    const std::vector<MarkerData>& marker_data,
    ThreadTables& tables,
    StringTable& st,
    uint32_t userCategory)
{
    uint32_t markerTypeIdx = st.intern("TracyZone");
    for (const auto& m : marker_data)
    {
        bool hasText = m.text != nullptr;
        tables.markers.push_back({
            userCategory,
            st.intern(m.name),
            hasText ? st.intern(m.text) : 0u,
            hasText,
            markerTypeIdx,
            ns_to_ms(m.startNs),
            ns_to_ms(m.endNs),
            MarkerPhase::Interval
        });
    }
}

static void process_samples(
    const tracy::Worker& worker,
    const tracy::ThreadData& td,
    ThreadTables& tables,
    StringTable& st,
    int64_t& minTime,
    int64_t& maxTime,
    uint32_t userCategory,
    uint32_t kernelCategory)
{
    for (const auto& sample : td.samples)
    {
        int64_t sampleTime = sample.time.Val();
        uint32_t csIdx = sample.callstack.Val();

        if (csIdx == 0) continue;

        const auto& callstack = worker.GetCallstack(csIdx);
        if (callstack.empty()) continue;

        minTime = std::min(minTime, sampleTime);
        maxTime = std::max(maxTime, sampleTime);

        int32_t stackIdx = -1;

        for (size_t i = callstack.size(); i > 0; i--)
        {
            auto frameId = callstack[i - 1];
            auto frameData = worker.GetCallstackFrame(frameId);
            if (!frameData) continue;

            uint64_t canonicalAddr = worker.GetCanonicalPointer(frameId);
            uint32_t category = is_kernel_address(canonicalAddr) ? kernelCategory : userCategory;

            const char* imageName = frameData->imageName.Active() ? worker.GetString(frameData->imageName) : "";

            for (uint8_t j = frameData->size; j > 0; j--)
            {
                const auto& frame = frameData->data[j - 1];
                const char* funcName = worker.GetString(frame.name);
                const char* fileName = worker.GetString(frame.file);
                uint32_t line = frame.line;
                uint64_t symAddr = frame.symAddr;

                uint32_t symSize = 0;
                auto symData = worker.GetSymbolData(symAddr);
                if (symData)
                {
                    symSize = symData->size.Val();
                }

                uint32_t inlineDepth = frameData->size - j;

                uint32_t frameIdx = tables.getOrCreateFrame(
                    st, symAddr, funcName, fileName,
                    line, 0, inlineDepth,
                    imageName, symSize, category
                );

                stackIdx = tables.getOrCreateStack(stackIdx, frameIdx);
            }
        }

        tables.samples.push_back({
            ns_to_ms(sampleTime),
            stackIdx,
            1.0
        });
    }
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
    {
        AllocConsole();
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), 0x07);
    }
#endif

    Args args = parse_args(argc, argv);

    auto f = std::unique_ptr<tracy::FileRead>(tracy::FileRead::Open(args.trace_file));
    if (!f)
    {
        fprintf(stderr, "Could not open file %s\n", args.trace_file);
        return 1;
    }

    tracy::Worker worker(*f);

    while (!worker.AreSourceLocationZonesReady())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

#ifndef TRACY_NO_STATISTICS
    while (!worker.AreCallstackSamplesReady())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif

    json profile;
    StringTable st;

    const std::string& captureName = worker.GetCaptureName();
    const std::string& captureProgram = worker.GetCaptureProgram();
    const std::string& hostInfo = worker.GetHostInfo();
    // Indexes into the `categories` array.
    uint32_t userCategory = 1;
    uint32_t kernelCategory = 2;
    profile["meta"] = {
        {"categories", json::array({
            {{"name", "Other"}, {"color", "grey"}, {"subcategories", json::array({"Other"})}},
            {{"name", "User"}, {"color", "yellow"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Kernel"}, {"color", "orange"}, {"subcategories", json::array({"Other"})}}
        })},
        {"debug", false},
        {"interval", ns_to_ms(worker.GetSamplingPeriod())},
        {"markerSchema", json::array({
            {
                {"name", "TracyZone"},
                {"display", json::array({"marker-chart", "marker-table"})},
                {"chartLabel", "{marker.data.name}"},
                {"tooltipLabel", "{marker.data.name}"},
                {"tableLabel", "{marker.data.name}"},
                {"description", "Emitted for Tracy zones"},
                {"fields", json::array({
                    {{"key", "name"}, {"label", "Name"}, {"format", "unique-string"}},
                    {{"key", "text"}, {"label", "Text"}, {"description", "User text"}, {"format", "unique-string"}}
                })}
            }
        })},
        {"pausedRanges", json::array()},
        {"platform", hostInfo},
        // https://github.com/firefox-devtools/profiler/blob/main/docs-developer/CHANGELOG-formats.md
        {"preprocessedProfileVersion", 57},
        {"processType", 0},
        {"product", captureProgram.empty() ? "Tracy" : captureProgram},
        {"startTime", worker.GetCaptureTime() * 1000},
        {"startTimeAsClockMonotonicNanosecondsSinceBoot", 0}, // TODO: platform specific
        {"symbolicated", true},
        {"version", 28},
        {"sampleUnits", {
            {"time", "ms"},
            {"eventDelay", "ms"},
            {"threadCPUDelta", "µs"}
        }},

        {"usesOnlyOneStackType", true},
        {"sourceCodeIsNotOnSearchfox", true}
    };
    if (!captureName.empty())
    {
        profile["meta"]["importedFrom"] = captureName;
    }

    // TODO
    profile["libs"] = json::array();

    for (const auto* td : worker.GetThreadData())
    {
        ThreadTables tables;
        std::vector<MarkerData> marker_data;
        int64_t minTime = INT64_MAX;
        int64_t maxTime = 0;

        collect_zones_recursive(worker, td->timeline, marker_data, minTime, maxTime);

        process_markers(marker_data, tables, st, userCategory);
        process_samples(worker, *td, tables, st, minTime, maxTime, userCategory, kernelCategory);

        if (minTime == INT64_MAX) minTime = 0;

        const char* threadName = worker.GetThreadName(td->id);
        auto pid_ = worker.GetPidFromTid(td->id);
        auto pid = pid_ != 0 ? pid_ : worker.GetPid();

        auto& thread = profile["threads"].emplace_back();
        thread["name"] = threadName ? threadName : "Thread";
        thread["isMainThread"] = pid == td->id;
        thread["processType"] = "default";
        thread["processName"] = captureProgram.empty() ? "Tracy" : captureProgram;
        thread["processStartupTime"] = 0.0;
        thread["processShutdownTime"] = nullptr;
        thread["registerTime"] = ns_to_ms(minTime);
        thread["unregisterTime"] = ns_to_ms(maxTime);
        thread["pid"] = std::to_string(pid);
        thread["tid"] = td->id;

        thread["frameTable"] = tables.frameTableToJson();
        thread["funcTable"] = tables.funcTableToJson();
        thread["markers"] = tables.markersToJson();
        thread["nativeSymbols"] = tables.nativeSymbolsToJson();
        thread["resourceTable"] = tables.resourceTableToJson();
        thread["samples"] = tables.samplesToJson();
        thread["stackTable"] = tables.stackTableToJson();
    }

    profile["shared"] = {
        {"stringArray", st.to_json()}
    };

    FILE* out = args.output ? fopen(args.output, "wb") : stdout;
    if (!out)
    {
        fprintf(stderr, "Could not open output file %s\n", args.output);
        return 1;
    }

    std::string output = profile.dump();
    fwrite(output.data(), 1, output.size(), out);
    fputc('\n', out);

    if (args.output) fclose(out);

    return 0;
}
