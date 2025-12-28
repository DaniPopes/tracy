#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "../../server/TracyWorker.hpp"

using json = nlohmann::json;

class LibTable;
class StringTable;

enum class MarkerPhase
{
    Instant = 0,
    Interval = 1,
    IntervalStart = 2,
    IntervalEnd = 3,
};

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

    struct AllocationEntry
    {
        double time;
        int64_t weight;
        int32_t stackIdx;
        uint64_t memoryAddress;
        uint64_t threadId;
    };

    struct MarkerEntry
    {
        std::string type;
        uint32_t category;
        uint32_t nameIdx;
        double startTime;
        double endTime;
        MarkerPhase phase;
        json data;
    };

    std::vector<FrameEntry> frames;
    std::vector<FuncEntry> funcs;
    std::vector<NativeSymbolEntry> nativeSymbols;
    std::vector<ResourceEntry> resources;
    std::vector<StackEntry> stacks;
    std::vector<SampleEntry> samples;
    std::vector<AllocationEntry> allocations;
    std::vector<MarkerEntry> markers;

    int64_t minTime = INT64_MAX;
    int64_t maxTime = 0;

    std::unordered_map<uint64_t, uint32_t> symAddrToNativeSymbol;
    std::unordered_map<uint64_t, uint32_t> symAddrToFunc;
    std::unordered_map<std::string, uint32_t> libNameToResource;
    std::unordered_map<uint64_t, uint32_t> frameKeyToFrame;
    std::unordered_map<uint64_t, int32_t> stackKeyToStack;

    uint32_t getOrCreateResource(StringTable& st, LibTable& lt, const char* libName);
    uint32_t getOrCreateNativeSymbol(StringTable& st, LibTable& lt, uint64_t symAddr, const char* name, const char* imageName, uint32_t size);
    uint32_t getOrCreateFunc(StringTable& st, uint64_t symAddr, const char* name, const char* fileName, uint32_t line, int32_t resourceIdx);
    uint32_t getOrCreateFrame(StringTable& st, LibTable& lt, uint64_t symAddr, const char* name, const char* fileName,
                               uint32_t line, uint32_t column, uint32_t inlineDepth,
                               const char* imageName, uint32_t symSize, uint32_t category);
    int32_t getOrCreateStack(int32_t prefix, uint32_t frame);

    void collectZones(
        const tracy::Worker& worker,
        const tracy::Vector<tracy::short_ptr<tracy::ZoneEvent>>& zones,
        StringTable& st,
        uint32_t category);

    void collectGpuZones(
        const tracy::Worker& worker,
        const tracy::Vector<tracy::short_ptr<tracy::GpuEvent>>& zones,
        StringTable& st,
        uint32_t category);

    void processMessages(
        const tracy::Worker& worker,
        StringTable& st,
        uint32_t category,
        uint64_t threadId);

    void processLocks(
        const tracy::Worker& worker,
        StringTable& st,
        uint32_t category,
        uint64_t threadId);

    void processFrames(
        const tracy::Worker& worker,
        StringTable& st,
        uint32_t category);

    void processSamples(
        const tracy::Worker& worker,
        const tracy::ThreadData& td,
        StringTable& st,
        LibTable& lt,
        uint32_t userCategory,
        uint32_t kernelCategory);

    void processAllocations(
        const tracy::Worker& worker,
        StringTable& st,
        LibTable& lt,
        uint32_t category,
        uint64_t threadId);

    json frameTableToJson() const;
    json funcTableToJson() const;
    json nativeSymbolsToJson() const;
    json resourceTableToJson() const;
    json stackTableToJson() const;
    json samplesToJson() const;
    json nativeAllocationsToJson() const;
    json markersToJson() const;
    json threadToJson() const;

    static json buildMarkerSchemas();
    static json buildCounters(const tracy::Worker& worker, StringTable& st, uint64_t mainThreadIndex);

private:
    void collectZone(
        const tracy::Worker& worker,
        const tracy::ZoneEvent& zone,
        StringTable& st,
        uint32_t category);

    void collectGpuZone(
        const tracy::Worker& worker,
        const tracy::GpuEvent& zone,
        StringTable& st,
        uint32_t category);
};
