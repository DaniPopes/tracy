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
    int64_t start = zone.Start();
    int64_t end = zone.End();

    markers.push_back({ name, start, end });
    minTime = std::min(minTime, start);
    maxTime = std::max(maxTime, end);

    if (zone.HasChildren())
    {
        auto& children = worker.GetZoneChildren(zone.Child());
        collect_zones_recursive(worker, children, markers, minTime, maxTime);
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

    json profile;
    StringTable st;

    const std::string& captureName = worker.GetCaptureName();
    const std::string& captureProgram = worker.GetCaptureProgram();
    const std::string& hostInfo = worker.GetHostInfo();
    uint64_t userCategory = 1;
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
                // {"description", "Emitted for marker spans in a markers text file."},
                {"fields", json::array({
                    {
                        {"key", "name"}, {"label", "Name"}, {"format", "unique-string"}
                    }
                })}
            }
        })},
        {"pausedRanges", json::array()},
        {"platform", hostInfo},
        {"preprocessedProfileVersion", 57},
        {"processType", 0},
        {"product", captureProgram.empty() ? "Tracy" : captureProgram},
        {"startTime", worker.GetCaptureTime() * 1000},
        {"startTimeAsClockMonotonicNanosecondsSinceBoot", 0}, // TODO: platform specific
        {"symbolicated", false}, // TODO
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

    profile["threads"] = json::array();

    for (const auto* td : worker.GetThreadData())
    {
        if (!td) continue;

        std::vector<MarkerData> marker_data;
        int64_t minTime = INT64_MAX;
        int64_t maxTime = 0;

        collect_zones_recursive(worker, td->timeline, marker_data, minTime, maxTime);

        if (marker_data.empty())
        {
            minTime = 0;
            maxTime = 0;
        }

        const char* threadName = worker.GetThreadName(td->id);

        auto& thread = profile["threads"].emplace_back();
        thread["name"] = threadName ? threadName : "Thread";
        thread["isMainThread"] = false;
        thread["processType"] = "default";
        thread["processName"] = captureProgram.empty() ? "Tracy" : captureProgram;
        thread["processStartupTime"] = 0.0;
        thread["processShutdownTime"] = nullptr;
        thread["registerTime"] = ns_to_ms(minTime);
        thread["unregisterTime"] = ns_to_ms(maxTime);
        auto pid = worker.GetPidFromTid(td->id);
        thread["pid"] = std::to_string(pid != 0 ? pid : worker.GetPid());
        thread["tid"] = td->id;

        thread["frameTable"] = {
            {"length", 0},
            {"func", json::array()},
            {"category", json::array()},
            {"subcategory", json::array()},
            {"line", json::array()},
            {"column", json::array()},
            {"address", json::array()},
            {"nativeSymbol", json::array()},
            {"inlineDepth", json::array()},
            {"innerWindowID", json::array()}
        };

        thread["funcTable"] = {
            {"length", 0},
            {"name", json::array()},
            {"isJS", json::array()},
            {"relevantForJS", json::array()},
            {"resource", json::array()},
            {"fileName", json::array()},
            {"lineNumber", json::array()},
            {"columnNumber", json::array()}
        };

        thread["markers"] = {
            {"length", marker_data.size()},
            {"category", json::array()},
            {"data", json::array()},
            {"endTime", json::array()},
            {"name", json::array()},
            {"phase", json::array()},
            {"startTime", json::array()}
        };
        auto& markers = thread["markers"];
        auto markerType = "TracyZone";
        uint32_t markerTypeIdx = st.intern(markerType);
        for (const auto& m : marker_data)
        {
            markers["category"].push_back(userCategory);
            markers["data"].push_back({
                {"type", markerType},
                {"name", st.intern(m.name)}
            });
            markers["name"].push_back(markerTypeIdx);
            markers["startTime"].push_back(ns_to_ms(m.startNs));
            markers["endTime"].push_back(ns_to_ms(m.endNs));
            markers["phase"].push_back(static_cast<int>(MarkerPhase::Interval));
        }

        thread["nativeSymbols"] = {
            {"length", 0},
            {"address", json::array()},
            {"functionSize", json::array()},
            {"libIndex", json::array()},
            {"name", json::array()}
        };

        thread["resourceTable"] = {
            {"length", 0},
            {"lib", json::array()},
            {"name", json::array()},
            {"host", json::array()},
            {"type", json::array()}
        };

        thread["samples"] = {
            {"length", 0},
            {"weightType", json::array()},
            {"stack", json::array()},
            {"timeDeltas", json::array()},
            {"weight", json::array()},
            {"threadCPUDelta", json::array()}
        };

        thread["stackTable"] = {
            {"length", 0},
            {"prefix", json::array()},
            {"frame", json::array()}
        };
    }

    profile["shared"]["stringArray"] = st.to_json();

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
