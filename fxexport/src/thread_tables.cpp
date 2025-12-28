#include "thread_tables.hpp"

#include <algorithm>
#include <format>

#include "lib_table.hpp"
#include "string_table.hpp"

#include "common.hpp"

uint32_t ThreadTables::getOrCreateResource(StringTable& st, LibTable& lt, const char* libName)
{
    std::string key(libName ? libName : "");
    auto it = libNameToResource.find(key);
    if (it != libNameToResource.end()) return it->second;

    uint32_t idx = static_cast<uint32_t>(resources.size());
    resources.push_back({
        lt.intern(libName),
        st.intern(libName)
    });
    libNameToResource[key] = idx;
    return idx;
}

uint32_t ThreadTables::getOrCreateNativeSymbol(StringTable& st, LibTable& lt, uint64_t symAddr, const char* name, const char* imageName, uint32_t size)
{
    auto it = symAddrToNativeSymbol.find(symAddr);
    if (it != symAddrToNativeSymbol.end())
    {
        if (imageName && imageName[0])
        {
            lt.intern(imageName, symAddr, size);
        }
        return it->second;
    }

    int32_t libIdx = -1;
    if (imageName && imageName[0])
    {
        lt.intern(imageName, symAddr, size);
        libIdx = static_cast<int32_t>(getOrCreateResource(st, lt, imageName));
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

uint32_t ThreadTables::getOrCreateFunc(StringTable& st, uint64_t symAddr, const char* name, const char* fileName, uint32_t line, int32_t resourceIdx)
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

uint32_t ThreadTables::getOrCreateFrame(StringTable& st, LibTable& lt, uint64_t symAddr, const char* name, const char* fileName,
                           uint32_t line, uint32_t column, uint32_t inlineDepth,
                           const char* imageName, uint32_t symSize, uint32_t category)
{
    uint64_t frameKey = symAddr ^ (static_cast<uint64_t>(inlineDepth) << 48);
    auto it = frameKeyToFrame.find(frameKey);
    if (it != frameKeyToFrame.end()) return it->second;

    int32_t resourceIdx = -1;
    if (imageName && imageName[0])
    {
        resourceIdx = static_cast<int32_t>(getOrCreateResource(st, lt, imageName));
    }

    uint32_t funcIdx = getOrCreateFunc(st, symAddr, name, fileName, line, resourceIdx);
    uint32_t nativeSymbolIdx = getOrCreateNativeSymbol(st, lt, symAddr, name, imageName, symSize);

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

int32_t ThreadTables::getOrCreateStack(int32_t prefix, uint32_t frame)
{
    uint64_t key = (static_cast<uint64_t>(prefix + 1) << 32) | frame;
    auto it = stackKeyToStack.find(key);
    if (it != stackKeyToStack.end()) return it->second;

    int32_t idx = static_cast<int32_t>(stacks.size());
    stacks.push_back({ prefix, frame });
    stackKeyToStack[key] = idx;
    return idx;
}

void ThreadTables::collectZone(
    const tracy::Worker& worker,
    const tracy::ZoneEvent& zone,
    StringTable& st,
    uint32_t category)
{
    if (!zone.IsEndValid()) return;

    const char* name = worker.GetZoneName(zone);
    const char* text = nullptr;
    uint32_t color = 0;
    bool hasColor = false;

    if (worker.HasZoneExtra(zone))
    {
        const auto& extra = worker.GetZoneExtra(zone);
        if (extra.text.Active())
        {
            text = worker.GetString(extra.text);
        }
        if (extra.color.Val() != 0)
        {
            color = extra.color.Val();
            hasColor = true;
        }
    }

    int64_t start = zone.Start();
    int64_t end = zone.End();
    minTime = std::min(minTime, start);
    maxTime = std::max(maxTime, end);

    const auto& srcloc = worker.GetSourceLocation(zone.SrcLoc());
    const char* file = worker.GetString(srcloc.file);
    const char* function = worker.GetString(srcloc.function);
    uint32_t line = srcloc.line;

    json markerData = {
        {"type", "TracyZone"},
        {"name", st.intern(name)}
    };
    if (text)
    {
        markerData["text"] = st.intern(text);
    }
    if (hasColor)
        if (auto graphColor = toGraphColor(color))
            markerData["color"] = graphColor;
    if (file && file[0])
    {
        markerData["file"] = st.intern(file);
        markerData["line"] = line;
    }
    if (function && function[0])
    {
        markerData["function"] = st.intern(function);
    }

    markers.push_back({
        "TracyZone",
        category,
        st.intern("TracyZone"),
        ns_to_ms(start),
        ns_to_ms(end),
        MarkerPhase::Interval,
        std::move(markerData)
    });

    if (zone.HasChildren())
    {
        auto& children = worker.GetZoneChildren(zone.Child());
        collectZones(worker, children, st, category);
    }
}

void ThreadTables::collectZones(
    const tracy::Worker& worker,
    const tracy::Vector<tracy::short_ptr<tracy::ZoneEvent>>& zones,
    StringTable& st,
    uint32_t category)
{
    if (zones.is_magic())
    {
        auto& vec = *(tracy::Vector<tracy::ZoneEvent>*)&zones;
        for (auto& zone : vec)
        {
            collectZone(worker, zone, st, category);
        }
    }
    else
    {
        for (auto& zonePtr : zones)
        {
            if (!zonePtr) continue;
            collectZone(worker, *zonePtr, st, category);
        }
    }
}

void ThreadTables::collectGpuZone(
    const tracy::Worker& worker,
    const tracy::GpuEvent& zone,
    StringTable& st,
    uint32_t category)
{
    if (zone.GpuEnd() < 0) return;

    const char* name = worker.GetZoneName(zone);
    int64_t gpuStart = zone.GpuStart();
    int64_t gpuEnd = zone.GpuEnd();
    int64_t cpuStart = zone.CpuStart();
    int64_t cpuEnd = zone.CpuEnd();

    minTime = std::min(minTime, gpuStart);
    maxTime = std::max(maxTime, gpuEnd);

    const auto& srcloc = worker.GetSourceLocation(zone.SrcLoc());
    const char* file = worker.GetString(srcloc.file);
    const char* function = worker.GetString(srcloc.function);
    uint32_t line = srcloc.line;

    json markerData = {
        {"type", "TracyGpuZone"},
        {"name", st.intern(name)},
        {"gpuStart", ns_to_ms(gpuStart)},
        {"gpuEnd", ns_to_ms(gpuEnd)},
        {"cpuStart", ns_to_ms(cpuStart)},
        {"cpuEnd", ns_to_ms(cpuEnd)}
    };
    if (file && file[0])
    {
        markerData["file"] = st.intern(file);
        markerData["line"] = line;
    }
    if (function && function[0])
    {
        markerData["function"] = st.intern(function);
    }

    markers.push_back({
        "TracyGpuZone",
        category,
        st.intern("TracyGpuZone"),
        ns_to_ms(gpuStart),
        ns_to_ms(gpuEnd),
        MarkerPhase::Interval,
        std::move(markerData)
    });

    if (zone.Child() >= 0)
    {
        auto& children = worker.GetGpuChildren(zone.Child());
        collectGpuZones(worker, children, st, category);
    }
}

void ThreadTables::collectGpuZones(
    const tracy::Worker& worker,
    const tracy::Vector<tracy::short_ptr<tracy::GpuEvent>>& zones,
    StringTable& st,
    uint32_t category)
{
    if (zones.is_magic())
    {
        auto& vec = *(tracy::Vector<tracy::GpuEvent>*)&zones;
        for (auto& zone : vec)
        {
            collectGpuZone(worker, zone, st, category);
        }
    }
    else
    {
        for (auto& zonePtr : zones)
        {
            if (!zonePtr) continue;
            collectGpuZone(worker, *zonePtr, st, category);
        }
    }
}

void ThreadTables::processMessages(
    const tracy::Worker& worker,
    StringTable& st,
    uint32_t category,
    uint64_t threadId)
{
    for (const auto& msgPtr : worker.GetMessages())
    {
        if (!msgPtr) continue;
        const auto& msg = *msgPtr;

        uint64_t msgThread = worker.DecompressThread(msg.thread);
        if (msgThread != threadId) continue;

        int64_t time = msg.time;
        const char* text = worker.GetString(msg.ref);
        uint32_t color = msg.color;

        minTime = std::min(minTime, time);
        maxTime = std::max(maxTime, time);

        json markerData = {
            {"type", "TracyMessage"},
            {"text", st.intern(text)}
        };
        if (color != 0)
            if (auto graphColor = toGraphColor(color))
                markerData["color"] = graphColor;

        markers.push_back({
            "TracyMessage",
            category,
            st.intern("TracyMessage"),
            ns_to_ms(time),
            ns_to_ms(time),
            MarkerPhase::Instant,
            std::move(markerData)
        });
    }
}

void ThreadTables::processLocks(
    const tracy::Worker& worker,
    StringTable& st,
    uint32_t category,
    uint64_t threadId)
{
    for (const auto& [lockId, lockMap] : worker.GetLockMap())
    {
        if (!lockMap || !lockMap->valid) continue;

        auto threadIt = lockMap->threadMap.find(threadId);
        if (threadIt == lockMap->threadMap.end()) continue;
        uint8_t threadBit = threadIt->second;

        const char* lockName = nullptr;
        if (lockMap->customName.Active())
        {
            lockName = worker.GetString(lockMap->customName);
        }
        else
        {
            const auto& srcloc = worker.GetSourceLocation(lockMap->srcloc);
            lockName = worker.GetString(srcloc.function);
        }

        int64_t waitStart = -1;

        for (const auto& lep : lockMap->timeline)
        {
            const auto& ev = *lep.ptr;
            int64_t time = ev.Time();
            auto type = ev.type;
            uint8_t evThread = ev.thread;

            if (evThread != threadBit) continue;

            minTime = std::min(minTime, time);
            maxTime = std::max(maxTime, time);

            switch (type)
            {
            case tracy::LockEvent::Type::Wait:
            case tracy::LockEvent::Type::WaitShared:
            {
                waitStart = time;
                break;
            }
            case tracy::LockEvent::Type::Obtain:
            case tracy::LockEvent::Type::ObtainShared:
            {
                if (waitStart >= 0)
                {
                    bool isShared = (type == tracy::LockEvent::Type::ObtainShared);
                    json markerData = {
                        {"type", "TracyLock"},
                        {"name", st.intern(lockName)},
                        {"lockId", lockId},
                        {"operation", isShared ? "wait_shared" : "wait"}
                    };

                    markers.push_back({
                        "TracyLock",
                        category,
                        st.intern("TracyLock"),
                        ns_to_ms(waitStart),
                        ns_to_ms(time),
                        MarkerPhase::Interval,
                        std::move(markerData)
                    });
                    waitStart = -1;
                }
                break;
            }
            case tracy::LockEvent::Type::Release:
            case tracy::LockEvent::Type::ReleaseShared:
            {
                break;
            }
            }
        }
    }
}

void ThreadTables::processFrames(
    const tracy::Worker& worker,
    StringTable& st,
    uint32_t category)
{
    const auto* framesBase = worker.GetFramesBase();
    if (!framesBase) return;

    const char* frameName = worker.GetString(framesBase->name);

    for (size_t i = 0; i < framesBase->frames.size(); i++)
    {
        const auto& frame = framesBase->frames[i];
        int64_t start = frame.start;
        int64_t end = frame.end;

        if (end < 0) continue;

        minTime = std::min(minTime, start);
        maxTime = std::max(maxTime, end);

        double durationMs = ns_to_ms(end - start);
        double fps = durationMs > 0 ? 1000.0 / durationMs : 0;

        json markerData = {
            {"type", "TracyFrame"},
            {"name", st.intern(frameName)},
            {"frameNumber", static_cast<uint64_t>(i)},
            {"duration", durationMs},
            {"fps", fps}
        };

        markers.push_back({
            "TracyFrame",
            category,
            st.intern("TracyFrame"),
            ns_to_ms(start),
            ns_to_ms(end),
            MarkerPhase::Interval,
            std::move(markerData)
        });
    }
}

void ThreadTables::processSamples(
    const tracy::Worker& worker,
    const tracy::ThreadData& td,
    StringTable& st,
    LibTable& lt,
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

                uint32_t frameIdx = getOrCreateFrame(
                    st, lt, symAddr, funcName, fileName,
                    line, 0, inlineDepth,
                    imageName, symSize, category
                );

                stackIdx = getOrCreateStack(stackIdx, frameIdx);
            }
        }

        samples.push_back({
            ns_to_ms(sampleTime),
            stackIdx,
            1.0
        });
    }
}

void ThreadTables::processAllocations(
    const tracy::Worker& worker,
    StringTable& st,
    LibTable& lt,
    uint32_t category)
{
    for (const auto& [memName, memData] : worker.GetMemNameMap())
    {
        if (!memData) continue;

        for (const auto& ev : memData->data)
        {
            int64_t allocTime = ev.TimeAlloc();
            int64_t freeTime = ev.TimeFree();
            int64_t size = static_cast<int64_t>(ev.Size());
            uint64_t ptr = ev.Ptr();
            uint32_t csAllocIdx = ev.CsAlloc();
            uint32_t csFreeIdx = ev.csFree.Val();

            uint64_t allocThreadId = worker.DecompressThread(ev.ThreadAlloc());
            uint64_t freeThreadId = worker.DecompressThread(ev.ThreadFree());

            auto buildStack = [&](uint32_t csIdx) -> int32_t {
                if (csIdx == 0) return -1;

                const auto& callstack = worker.GetCallstack(csIdx);
                if (callstack.empty()) return -1;

                int32_t stackIdx = -1;
                for (size_t i = callstack.size(); i > 0; i--)
                {
                    auto frameId = callstack[i - 1];
                    auto frameData = worker.GetCallstackFrame(frameId);
                    if (!frameData) continue;

                    uint64_t canonicalAddr = worker.GetCanonicalPointer(frameId);
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
                        if (symData) symSize = symData->size.Val();

                        uint32_t inlineDepth = frameData->size - j;

                        uint32_t frameIdx = getOrCreateFrame(
                            st, lt, symAddr, funcName, fileName,
                            line, 0, inlineDepth,
                            imageName, symSize, category
                        );

                        stackIdx = getOrCreateStack(stackIdx, frameIdx);
                    }
                }
                return stackIdx;
            };

            minTime = std::min(minTime, allocTime);
            maxTime = std::max(maxTime, allocTime);

            allocations.push_back({
                ns_to_ms(allocTime),
                size,
                buildStack(csAllocIdx),
                ptr,
                allocThreadId
            });

            if (freeTime >= 0)
            {
                minTime = std::min(minTime, freeTime);
                maxTime = std::max(maxTime, freeTime);

                allocations.push_back({
                    ns_to_ms(freeTime),
                    -size,
                    buildStack(csFreeIdx),
                    ptr,
                    freeThreadId
                });
            }
        }
    }

    std::stable_sort(allocations.begin(), allocations.end(),
        [](const AllocationEntry& a, const AllocationEntry& b) {
            return a.time < b.time;
        });
}

json ThreadTables::frameTableToJson() const
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

json ThreadTables::funcTableToJson() const
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

json ThreadTables::nativeSymbolsToJson() const
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

json ThreadTables::resourceTableToJson() const
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

json ThreadTables::stackTableToJson() const
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

json ThreadTables::samplesToJson() const
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

json ThreadTables::nativeAllocationsToJson() const
{
    json time = json::array();
    json weight = json::array();
    json stack = json::array();
    json memoryAddress = json::array();
    json threadId = json::array();

    for (const auto& a : allocations)
    {
        time.push_back(a.time);
        weight.push_back(a.weight);
        stack.push_back(a.stackIdx >= 0 ? json(a.stackIdx) : json(nullptr));
        memoryAddress.push_back(a.memoryAddress);
        threadId.push_back(a.threadId);
    }

    return {
        {"time", time},
        {"weight", weight},
        {"weightType", "bytes"},
        {"stack", stack},
        {"memoryAddress", memoryAddress},
        {"threadId", threadId},
        {"length", allocations.size()}
    };
}

json ThreadTables::markersToJson() const
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
        data.push_back(m.data);
        name.push_back(m.nameIdx);
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

json ThreadTables::threadToJson() const
{
    return {
        {"frameTable", frameTableToJson()},
        {"funcTable", funcTableToJson()},
        {"markers", markersToJson()},
        {"nativeSymbols", nativeSymbolsToJson()},
        {"registerTime", ns_to_ms(minTime == INT64_MAX ? 0 : minTime)},
        {"resourceTable", resourceTableToJson()},
        {"samples", samplesToJson()},
        {"stackTable", stackTableToJson()},
        {"unregisterTime", ns_to_ms(maxTime)}
    };
}

json ThreadTables::buildMarkerSchemas()
{
    json display = json::array({"marker-chart", "marker-table", "timeline-overview"});
    return json::array({
        {
            {"name", "TracyZone"},
            {"display", display},
            {"chartLabel", "{marker.data.name}"},
            {"tooltipLabel", "{marker.data.name}"},
            {"tableLabel", "{marker.data.name}"},
            {"description", "Tracy instrumentation zone"},
            {"colorField", "color"},
            {"fields", json::array({
                {{"key", "name"}, {"label", "Name"}, {"format", "unique-string"}},
                {{"key", "text"}, {"label", "Text"}, {"format", "unique-string"}},
                {{"key", "color"}, {"label", "Color"}, {"format", "string"}, {"hide", true}},
                {{"key", "file"}, {"label", "File"}, {"format", "unique-string"}},
                {{"key", "line"}, {"label", "Line"}, {"format", "integer"}},
                {{"key", "function"}, {"label", "Function"}, {"format", "unique-string"}}
            })}
        },
        {
            {"name", "TracyMessage"},
            {"display", display},
            {"chartLabel", "{marker.data.text}"},
            {"tooltipLabel", "{marker.data.text}"},
            {"tableLabel", "{marker.data.text}"},
            {"description", "Tracy log message"},
            {"colorField", "color"},
            {"fields", json::array({
                {{"key", "text"}, {"label", "Message"}, {"format", "unique-string"}},
                {{"key", "color"}, {"label", "Color"}, {"format", "string"}}
            })}
        },
        {
            {"name", "TracyLock"},
            {"display", display},
            {"chartLabel", "{marker.data.name}"},
            {"tooltipLabel", "Lock: {marker.data.name} ({marker.data.operation})"},
            {"tableLabel", "{marker.data.name}"},
            {"description", "Tracy lock contention"},
            {"fields", json::array({
                {{"key", "name"}, {"label", "Lock Name"}, {"format", "unique-string"}},
                {{"key", "lockId"}, {"label", "Lock ID"}, {"format", "integer"}},
                {{"key", "operation"}, {"label", "Operation"}, {"format", "string"}}
            })}
        },
        {
            {"name", "TracyGpuZone"},
            {"display", display},
            {"chartLabel", "{marker.data.name}"},
            {"tooltipLabel", "GPU: {marker.data.name}"},
            {"tableLabel", "{marker.data.name}"},
            {"description", "Tracy GPU zone"},
            {"fields", json::array({
                {{"key", "name"}, {"label", "Name"}, {"format", "unique-string"}},
                {{"key", "gpuStart"}, {"label", "GPU Start"}, {"format", "time"}},
                {{"key", "gpuEnd"}, {"label", "GPU End"}, {"format", "time"}},
                {{"key", "cpuStart"}, {"label", "CPU Start"}, {"format", "time"}},
                {{"key", "cpuEnd"}, {"label", "CPU End"}, {"format", "time"}},
                {{"key", "file"}, {"label", "File"}, {"format", "unique-string"}},
                {{"key", "line"}, {"label", "Line"}, {"format", "integer"}},
                {{"key", "function"}, {"label", "Function"}, {"format", "unique-string"}}
            })}
        },
        {
            {"name", "TracyFrame"},
            {"display", display},
            {"chartLabel", "Frame {marker.data.frameNumber}"},
            {"tooltipLabel", "Frame {marker.data.frameNumber} ({marker.data.fps} FPS)"},
            {"tableLabel", "Frame {marker.data.frameNumber}"},
            {"description", "Tracy frame marker"},
            {"fields", json::array({
                {{"key", "name"}, {"label", "Name"}, {"format", "unique-string"}},
                {{"key", "frameNumber"}, {"label", "Frame"}, {"format", "integer"}},
                {{"key", "duration"}, {"label", "Duration (ms)"}, {"format", "duration"}},
                {{"key", "fps"}, {"label", "FPS"}, {"format", "number"}}
            })}
        }
    });
}

json ThreadTables::buildCounters(const tracy::Worker& worker, StringTable& st, uint64_t mainThreadIndex)
{
    json counters = json::array();

    for (const auto* plot : worker.GetPlots())
    {
        if (!plot) continue;
        if (plot->data.empty()) continue;
        if (plot->type == tracy::PlotType::SysTime) continue;

        const char* plotName = worker.GetString(plot->name);

        // Convert absolute value to delta counts.
        json time = json::array();
        json count = json::array();
        time.push_back(ns_to_ms(plot->data[0].time.Val()));
        count.push_back(plot->data[0].val);
        for (int i = 1; i < plot->data.size(); ++i)
        {
            time.push_back(ns_to_ms(plot->data[i].time.Val()));
            count.push_back(plot->data[i].val - plot->data[i - 1].val);
        }

        std::string category;
        std::string description;
        switch (plot->type)
        {
        case tracy::PlotType::User:
            category = "User";
            description = "User-defined plot";
            break;
        case tracy::PlotType::Memory:
            category = "Memory";
            description = "Memory usage";
            break;
        case tracy::PlotType::Power:
            category = "Power";
            description = "Power consumption";
            break;
        default:
            category = "Other";
            description = "Plot data";
            break;
        }

        counters.push_back({
            {"name", plotName},
            {"category", category},
            {"description", description},
            {"pid", std::to_string(worker.GetPid())},
            {"mainThreadIndex", mainThreadIndex},
            {"samples", {
                {"time", time},
                {"count", count},
                {"length", plot->data.size()}
            }}
        });
    }

    return counters;
}
