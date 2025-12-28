#ifdef _WIN32
#  include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <thread>

#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyWorker.hpp"
#include "../../getopt/getopt.h"

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
    const std::string& hostInfo = worker.GetHostInfo();

    uint32_t userCategory = 1;
    uint32_t kernelCategory = 2;
    uint32_t gpuCategory = 3;
    uint32_t lockCategory = 4;
    uint32_t messageCategory = 5;
    uint32_t frameCategory = 6;

    profile["meta"] = {
        {"categories", json::array({
            {{"name", "Other"}, {"color", "grey"}, {"subcategories", json::array({"Other"})}},
            {{"name", "User"}, {"color", "yellow"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Kernel"}, {"color", "orange"}, {"subcategories", json::array({"Other"})}},
            {{"name", "GPU"}, {"color", "magenta"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Lock"}, {"color", "red"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Message"}, {"color", "blue"}, {"subcategories", json::array({"Other"})}},
            {{"name", "Frame"}, {"color", "green"}, {"subcategories", json::array({"Other"})}}
        })},
        {"debug", false},
        {"interval", ns_to_ms(worker.GetSamplingPeriod())},
        {"markerSchema", ThreadTables::buildMarkerSchemas()},
        {"pausedRanges", json::array()},
        {"platform", hostInfo},
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
        {"sourceCodeIsNotOnSearchfox", true}
    };
    if (!captureName.empty())
    {
        profile["meta"]["importedFrom"] = captureName;
    }

    bool firstThread = true;

    for (const auto* td : worker.GetThreadData())
    {
        ThreadTables tables;

        tables.collectZones(worker, td->timeline, st, userCategory);
        tables.processMessages(worker, st, messageCategory, td->id);
        tables.processLocks(worker, st, lockCategory, td->id);
        tables.processSamples(worker, *td, st, lt, userCategory, kernelCategory);

        if (firstThread)
        {
            tables.processFrames(worker, st, frameCategory);
            firstThread = false;
        }

        const char* threadName_ = worker.GetThreadName(td->id);
        std::string threadName = threadName_ ? threadName_ : std::format("Thread <{}>", td->id);
        auto pid_ = worker.GetPidFromTid(td->id);
        auto pid = pid_ != 0 ? pid_ : worker.GetPid();

        auto& thread = profile["threads"].emplace_back();
        thread["name"] = threadName;
        thread["isMainThread"] = threadName == "Main thread" || pid == td->id;
        thread["processType"] = "default";
        thread["processName"] = captureProgram.empty() ? "Tracy" : captureProgram;
        thread["processStartupTime"] = 0.0;
        thread["processShutdownTime"] = nullptr;
        thread["pid"] = std::to_string(pid);
        thread["tid"] = td->id;
        thread["showMarkersInTimeline"] = true;
        thread.merge_patch(tables.threadToJson());
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

    profile["counters"] = ThreadTables::buildCounters(worker, st);

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
