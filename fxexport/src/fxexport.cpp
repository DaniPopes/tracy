#ifdef _WIN32
#  include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <sstream>
#include <string>
#include <thread>

#include "../../getopt/getopt.h"
#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyWorker.hpp"

#include "common.hpp"
#include "lib_table.hpp"
#include "string_table.hpp"
#include "thread_tables.hpp"

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

struct HostInfo {
    std::string os;
    std::string compiler;
    std::string user;
    std::string arch;
    std::string cpu;
    uint64_t cpu_cores;
    uint64_t ram;
};

HostInfo parseHostInfo(const std::string& input) {
    /*
OS: Linux 6.0.0-1-MANJARO
Compiler: gcc 12.2.0
User: wolf@mimir
Arch: x64
CPU: 11th Gen Intel(R) Core(TM) i7-1185G7 @ 3.00GHz
CPU cores: 8
RAM: 15707 MB
    */

    HostInfo info;
    std::istringstream stream(input);
    std::string line;

    while (std::getline(stream, line)) {
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);

            // Trim leading whitespace from value
            size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) {
                value = value.substr(start);
            }

            if (key == "OS") info.os = value;
            else if (key == "Compiler") info.compiler = value;
            else if (key == "User") info.user = value;
            else if (key == "Arch") info.arch = value;
            else if (key == "CPU") info.cpu = value;
            else if (key == "CPU cores" && value != "unknown") info.cpu_cores = std::stoull(value);
            else if (key == "RAM" && value != "unknown") {
                // MB -> bytes
                size_t end = value.find(' ');
                if (end != std::string::npos) {
                    info.ram = std::stoull(value.substr(0, end)) * 1024 * 1024;
                }
            }
        }
    }

    return info;
}

std::string formatAppInfo(tracy::Worker& worker) {
    auto& appInfos = worker.GetAppInfo();
    if (appInfos.empty()) return "<empty>";

    std::string appInfo;
    bool first = true;
    for (auto& infoRef : appInfos) {
        auto info = worker.GetString(infoRef);
        if (!first) appInfo += " | ";
        appInfo += info;
        first = false;
    }
    return appInfo;
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

    // Docs:     https://github.com/firefox-devtools/profiler/blob/0d72df877672802eae9e48da1a40511b74b33010/docs-developer/processed-profile-format.md
    // Versions: https://github.com/firefox-devtools/profiler/blob/0d72df877672802eae9e48da1a40511b74b33010/docs-developer/CHANGELOG-formats.md
    // Schema:   https://github.com/firefox-devtools/profiler/blob/0d72df877672802eae9e48da1a40511b74b33010/src/types/profile.ts

    json profile;
    StringTable st;
    LibTable lt;

    const std::string& captureName = worker.GetCaptureName();
    const std::string& captureProgram = worker.GetCaptureProgram();
    auto hostInfo = parseHostInfo(worker.GetHostInfo());
    auto appInfo = formatAppInfo(worker);

    uint32_t userCategory = 1;
    uint32_t kernelCategory = 2;
    uint32_t gpuCategory = 3;
    uint32_t lockCategory = 4;
    uint32_t messageCategory = 5;
    uint32_t frameCategory = 6;
    uint32_t memoryCategory = 7;

    profile["meta"] = {
        {"categories", json::array({
            {{"name", "Other"}, {"color", "grey"}, {"subcategories", json::array({"Other"})}},
            {{"name", "User"}, {"color", "yellow"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Kernel"}, {"color", "orange"}, {"subcategories", json::array({"Other"})}},
            {{"name", "GPU"}, {"color", "magenta"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Lock"}, {"color", "red"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Message"}, {"color", "blue"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Frame"}, {"color", "green"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Memory"}, {"color", "purple"}, {"subcategories", json::array({"Other"})}}
        })},
        {"debug", false},
        {"interval", ns_to_ms(worker.GetSamplingPeriod())},
        {"markerSchema", ThreadTables::buildMarkerSchemas()},
        {"pausedRanges", json::array()},
        {"abi", hostInfo.arch + "-" + hostInfo.compiler},
        {"oscpu", hostInfo.os},
        {"mainMemory", hostInfo.ram},
        {"CPUName", hostInfo.cpu},
        {"physicalCPUs", hostInfo.cpu_cores},
        {"preprocessedProfileVersion", 57},
        {"processType", 0},
        {"product", captureProgram.empty() ? "Tracy" : captureProgram},
        {"startTime", worker.GetCaptureTime() * 1000},
        {"startTimeAsClockMonotonicNanosecondsSinceBoot", 0},
        {"symbolicated", true},
        {"version", 28},
        {"sampleUnits", {
            {"time", "ms"},
            {"eventDelay", "ms"},
            {"threadCPUDelta", "µs"}
        }},
        {"usesOnlyOneStackType", true},
        {"sourceCodeIsNotOnSearchfox", true},
        {"extra", {
            {
                {"label", "Tracy info"},
                {"entries", json::array({
                    {{"label", "User"}, {"format", "string"}, {"value", hostInfo.user}},
                    {{"label", "Compiler"}, {"format", "string"}, {"value", hostInfo.compiler}},
                    {{"label", "Application info"}, {"format", "string"}, {"value", appInfo}},
                })}
            }
        }},
    };
    if (!captureName.empty())
    {
        profile["meta"]["importedFrom"] = captureName;
    }

    uint64_t mainThreadIndex = 0;
    uint64_t threadIndex = 0;
    for (const auto* td : worker.GetThreadData())
    {
        ThreadTables tables;

        tables.collectZones(worker, td->timeline, st, userCategory);
        tables.processMessages(worker, st, messageCategory, td->id);
        tables.processLocks(worker, st, lockCategory, td->id);
        tables.processSamples(worker, *td, st, lt, userCategory, kernelCategory);
        tables.processAllocations(worker, st, lt, memoryCategory, td->id);

        if (threadIndex == 0)
        {
            tables.processFrames(worker, st, frameCategory);
        }

        const char* threadName_ = worker.GetThreadName(td->id);
        std::string threadName = threadName_ ? threadName_ : std::format("Thread <{}>", td->id);
        auto pid_ = worker.GetPidFromTid(td->id);
        auto pid = pid_ != 0 ? pid_ : worker.GetPid();
        bool isMainThread = threadName == "Main thread" || pid == td->id;

        auto& thread = profile["threads"].emplace_back();

        if ((isMainThread && pid == worker.GetPid()) || (mainThreadIndex == 0 && isMainThread)) {
            mainThreadIndex = threadIndex;
        }

        thread["name"] = threadName;
        thread["isMainThread"] = isMainThread;
        thread["processType"] = "default";
        thread["processName"] = captureProgram.empty() ? "Tracy" : captureProgram;
        thread["processStartupTime"] = 0.0;
        thread["processShutdownTime"] = nullptr;
        thread["pid"] = std::to_string(pid);
        thread["tid"] = td->id;
        thread["showMarkersInTimeline"] = true;
        thread.merge_patch(tables.threadToJson());

        threadIndex++;
    }

    for (const auto* gpuCtx : worker.GetGpuData())
    {
        if (!gpuCtx) continue;

        for (const auto& [tid, gpuThread] : gpuCtx->threadData)
        {
            if (gpuThread.timeline.empty()) continue;

            ThreadTables tables;

            tables.collectGpuZones(worker, gpuThread.timeline, st, gpuCategory);

            if (tables.markers.empty()) continue;

            std::string gpuName;
            if (gpuCtx->name.Active())
            {
                gpuName = worker.GetString(gpuCtx->name);
            }
            else
            {
                gpuName = std::format("GPU Context {}", static_cast<int>(gpuCtx->type));
            }

            auto& thread = profile["threads"].emplace_back();
            thread["name"] = gpuName;
            thread["isMainThread"] = false;
            thread["processType"] = "gpu";
            thread["processName"] = captureProgram.empty() ? "Tracy" : captureProgram;
            thread["processStartupTime"] = 0.0;
            thread["processShutdownTime"] = nullptr;
            thread["pid"] = std::to_string(worker.GetPid());
            thread["tid"] = std::format("gpu-{}", tid);
            thread["showMarkersInTimeline"] = true;
            thread.merge_patch(tables.threadToJson());
        }
    }

    profile["counters"] = ThreadTables::buildCounters(worker, st, mainThreadIndex);
    profile["meta"]["initialSelectedThreads"] = json::array({mainThreadIndex});

    profile["libs"] = lt.to_json();
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
