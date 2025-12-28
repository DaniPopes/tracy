#pragma once

#include "../../server/TracyWorker.hpp"

#include "common.hpp"
#include "string_table.hpp"

json build_counters(const tracy::Worker& worker, StringTable& st);

json build_marker_schemas();
