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

static const char* get_name(int32_t id, const tracy::Worker& worker)
{
    auto& srcloc = worker.GetSourceLocation(id);
    return worker.GetString(srcloc.name.active ? srcloc.name : srcloc.function);
}

static const char* cpu_arch_string(tracy::CpuArchitecture arch)
{
    switch (arch)
    {
    case tracy::CpuArchX86: return "x86";
    case tracy::CpuArchX64: return "x86_64";
    case tracy::CpuArchArm32: return "arm";
    case tracy::CpuArchArm64: return "aarch64";
    default: return "";
    }
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

struct ThreadData
{
    uint64_t tid = 0;
    std::string name;
    std::vector<std::tuple<const char*, int64_t, int64_t>> markers; // name, startNs, endNs
    int64_t minTime = INT64_MAX;
    int64_t maxTime = 0;
};

static double ns_to_ms(int64_t ns)
{
    return static_cast<double>(ns) / 1e6;
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

    std::unordered_map<uint64_t, ThreadData> threads;

    auto& slz = worker.GetSourceLocationZones();
    for (auto it = slz.begin(); it != slz.end(); ++it)
    {
        const auto srclocId = it->first;
        const auto& zoneData = it->second;
        const char* zoneName = get_name(srclocId, worker);

        for (const auto& ztd : zoneData.zones)
        {
            auto* zone = ztd.Zone();
            if (!zone->IsEndValid()) continue;

            uint16_t tCompressed = ztd.Thread();
            uint64_t tFull = worker.DecompressThread(tCompressed);

            auto& td = threads[tFull];
            if (td.tid == 0)
            {
                td.tid = tFull;
                const char* tname = worker.GetThreadName(tFull);
                td.name = tname ? tname : "Thread";
            }

            int64_t start = zone->Start();
            int64_t end = zone->End();

            td.markers.emplace_back(zoneName, start, end);
            td.minTime = std::min(td.minTime, start);
            td.maxTime = std::max(td.maxTime, end);
        }
    }

    // Build JSON
    json profile;

    // meta
    const std::string& captureName = worker.GetCaptureName();
    const std::string& captureProgram = worker.GetCaptureProgram();
    const std::string& hostInfo = worker.GetHostInfo();
    const char* cpuArch = cpu_arch_string(worker.GetCpuArch());
    const char* cpuManufacturer = worker.GetCpuManufacturer();

    profile["meta"] = {
        {"version", 28},
        {"interval", 1.0},
        {"startTime", 0.0},
        {"processType", 0},
        {"product", captureProgram.empty() ? "Tracy" : captureProgram},
        {"stackwalk", 0},
        {"debug", 0},
        {"platform", hostInfo},
        {"oscpu", cpuArch},
        {"misc", cpuManufacturer ? cpuManufacturer : ""},
        {"abi", cpuArch},
        {"toolkit", ""},
        {"categories", json::array({
            {{"name", "Tracy"}, {"color", "#0074D9"}, {"subcategories", json::array({"Zone"})}}
        })},
        {"markerSchema", json::array()},
        {"sampleUnits", {
            {"time", "ms"},
            {"eventDelay", "ms"},
            {"threadCPUDelta", "µs"}
        }}
    };

    if (!captureName.empty())
    {
        profile["meta"]["importedFrom"] = captureName;
    }

    // libs (empty)
    profile["libs"] = json::array();

    // threads
    profile["threads"] = json::array();
    for (auto& kv : threads)
    {
        auto& td = kv.second;
        if (td.markers.empty()) continue;

        StringTable stringTable;

        json thread;
        thread["name"] = td.name;
        thread["isMainThread"] = false;
        thread["processType"] = "default";
        thread["processName"] = captureProgram.empty() ? "Tracy" : captureProgram;
        thread["processStartupTime"] = 0.0;
        thread["processShutdownTime"] = nullptr;
        thread["registerTime"] = ns_to_ms(td.minTime);
        thread["unregisterTime"] = nullptr;
        thread["pid"] = std::to_string(worker.GetPid());
        thread["tid"] = td.tid;

        // samples (minimal)
        thread["samples"] = {
            {"schema", {{"stack", 0}, {"time", 1}, {"responsiveness", 2}}},
            {"data", json::array({
                json::array({nullptr, ns_to_ms(td.minTime), 0.0}),
                json::array({nullptr, ns_to_ms(td.maxTime), 0.0})
            })}
        };

        // markers - intern on demand
        json markersData = json::array();
        for (auto& m : td.markers)
        {
            uint32_t nameIdx = stringTable.intern(std::get<0>(m));
            markersData.push_back(json::array({
                nameIdx,
                ns_to_ms(std::get<1>(m)),
                ns_to_ms(std::get<2>(m)),
                1,  // phase: Interval
                0,  // category: Tracy
                nullptr
            }));
        }
        thread["markers"] = {
            {"schema", {{"name", 0}, {"startTime", 1}, {"endTime", 2}, {"phase", 3}, {"category", 4}, {"data", 5}}},
            {"data", markersData}
        };

        // stackTable (empty)
        thread["stackTable"] = {
            {"schema", {{"prefix", 0}, {"frame", 1}}},
            {"data", json::array()}
        };

        // frameTable (empty)
        thread["frameTable"] = {
            {"schema", {{"location", 0}, {"relevantForJS", 1}, {"innerWindowID", 2}, {"implementation", 3}, {"line", 4}, {"column", 5}, {"category", 6}, {"subcategory", 7}}},
            {"data", json::array()}
        };

        // stringTable (per-thread)
        thread["stringTable"] = stringTable.to_json();

        profile["threads"].push_back(thread);
    }

    // pausedRanges (empty)
    profile["pausedRanges"] = json::array();

    // processes (empty)
    profile["processes"] = json::array();

    // Output
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
