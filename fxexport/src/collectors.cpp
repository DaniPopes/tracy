#include "collectors.hpp"

json build_counters(const tracy::Worker& worker, StringTable& st)
{
    json counters = json::array();

    for (const auto* plot : worker.GetPlots())
    {
        if (!plot) continue;
        if (plot->data.empty()) continue;
        if (plot->type == tracy::PlotType::SysTime) continue;

        const char* plotName = worker.GetString(plot->name);

        json time = json::array();
        json count = json::array();

        for (const auto& item : plot->data)
        {
            time.push_back(ns_to_ms(item.time.Val()));
            count.push_back(item.val);
        }

        std::string category;
        std::string description;
        std::string color = "grey";

        switch (plot->type)
        {
        case tracy::PlotType::User:
            category = "User";
            description = "User-defined plot";
            color = "blue";
            break;
        case tracy::PlotType::Memory:
            category = "Memory";
            description = "Memory usage";
            color = "purple";
            break;
        case tracy::PlotType::Power:
            category = "Power";
            description = "Power consumption";
            color = "orange";
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
            {"color", color},
            {"pid", std::to_string(worker.GetPid())},
            {"mainThreadIndex", 0},
            {"samples", {
                {"time", time},
                {"count", count},
                {"length", plot->data.size()}
            }}
        });
    }

    return counters;
}

json build_marker_schemas()
{
    return json::array({
        {
            {"name", "TracyZone"},
            {"display", json::array({"marker-chart", "marker-table", "timeline-overview"})},
            {"chartLabel", "{marker.data.name}"},
            {"tooltipLabel", "{marker.data.name}"},
            {"tableLabel", "{marker.data.name}"},
            {"description", "Tracy instrumentation zone"},
            {"fields", json::array({
                {{"key", "name"}, {"label", "Name"}, {"format", "unique-string"}},
                {{"key", "text"}, {"label", "Text"}, {"format", "unique-string"}},
                {{"key", "color"}, {"label", "Color"}, {"format", "string"}},
                {{"key", "file"}, {"label", "File"}, {"format", "unique-string"}},
                {{"key", "line"}, {"label", "Line"}, {"format", "integer"}},
                {{"key", "function"}, {"label", "Function"}, {"format", "unique-string"}}
            })}
        },
        {
            {"name", "TracyMessage"},
            {"display", json::array({"marker-chart", "marker-table"})},
            {"chartLabel", "{marker.data.text}"},
            {"tooltipLabel", "Message: {marker.data.text}"},
            {"tableLabel", "{marker.data.text}"},
            {"description", "Tracy log message"},
            {"fields", json::array({
                {{"key", "text"}, {"label", "Message"}, {"format", "unique-string"}},
                {{"key", "color"}, {"label", "Color"}, {"format", "string"}}
            })}
        },
        {
            {"name", "TracyLock"},
            {"display", json::array({"marker-chart", "marker-table"})},
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
            {"display", json::array({"marker-chart", "marker-table", "timeline-overview"})},
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
            {"display", json::array({"marker-chart", "marker-table", "timeline-overview"})},
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
