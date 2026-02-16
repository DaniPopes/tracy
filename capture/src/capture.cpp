#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <inttypes.h>
#include <mutex>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#include "../../public/common/TracyProtocol.hpp"
#include "../../public/common/TracyStackFrames.hpp"
#include "../../server/TracyFileRead.hpp"
#include "../../server/TracyFileWrite.hpp"
#include "../../server/TracyMemory.hpp"
#include "../../server/TracyPrint.hpp"
#include "../../server/TracySysUtil.hpp"
#include "../../server/TracyWorker.hpp"

#ifdef _WIN32
#  include "../../getopt/getopt.h"
#endif


// This atomic is written by a signal handler (SigInt). Traditionally that would
// have had to be `volatile sig_atomic_t`, and annoyingly, `bool` was
// technically not allowed there, even though in practice it would work.
// The good thing with C++11 atomics is that we can use atomic<bool> instead
// here and be on the actually supported path.
static std::atomic<bool> s_disconnect { false };

void SigInt( int )
{
    // Relaxed order is closest to a traditional `volatile` write.
    // We don't need stronger ordering since this signal handler doesn't do
    // anything else that would need to be ordered relatively to this.
    s_disconnect.store(true, std::memory_order_relaxed);
}

static bool s_isStdoutATerminal = false;

void InitIsStdoutATerminal() {
#ifdef _WIN32
    s_isStdoutATerminal = _isatty( fileno( stdout ) );
#else
    s_isStdoutATerminal = isatty( fileno( stdout ) );
#endif
}

bool IsStdoutATerminal() { return s_isStdoutATerminal; }

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_BLACK "\033[30m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"
#define ANSI_ERASE_LINE "\033[2K"

// Like printf, but if stdout is a terminal, prepends the output with
// the given `ansiEscape` and appends ANSI_RESET.
void AnsiPrintf( const char* ansiEscape, const char* format, ... ) {
    if( IsStdoutATerminal() )
    {
        // Prepend ansiEscape and append ANSI_RESET.
        char buf[256];
        va_list args;
        va_start( args, format );
        vsnprintf( buf, sizeof buf, format, args );
        va_end( args );
        printf( "%s%s" ANSI_RESET, ansiEscape, buf );
    }
    else
    {
        // Just a normal printf.
        va_list args;
        va_start( args, format );
        vfprintf( stdout, format, args );
        va_end( args );
    }
}

[[noreturn]] void Usage()
{
    printf( "Usage: capture -o output.tracy [-a address] [-p port] [-f] [-s seconds] [-m memlimit]\n" );
    printf( "       capture -A input.tracy [-n top_n]\n" );
    exit( 1 );
}

struct SizeEntry
{
    const char* name;
    uint64_t count;
    uint64_t bytes;
};

struct SrcLocEntry
{
    int16_t srcloc;
    const char* zoneName;
    const char* file;
    uint32_t line;
    uint64_t zoneCount;
    uint64_t lockCount;
};

static void PrintSizeTable( const char* title, std::vector<SizeEntry>& entries )
{
    uint64_t totalBytes = 0;
    for( auto& e : entries ) totalBytes += e.bytes;

    printf( "\n" );
    AnsiPrintf( ANSI_BOLD ANSI_CYAN, "=== %s ===\n", title );
    printf( "%-36s %16s %16s %8s\n", "Category", "Count", "Est. Size", "%" );
    printf( "%-36s %16s %16s %8s\n", "------------------------------------", "----------------", "----------------", "--------" );

    std::sort( entries.begin(), entries.end(), []( const SizeEntry& a, const SizeEntry& b ) { return a.bytes > b.bytes; } );

    for( auto& e : entries )
    {
        if( e.count == 0 && e.bytes == 0 ) continue;
        double pct = totalBytes > 0 ? 100.0 * e.bytes / totalBytes : 0.0;
        printf( "%-36s %16s %16s %7.1f%%\n", e.name, tracy::RealToString( e.count ), tracy::MemSizeToString( e.bytes ), pct );
    }

    printf( "%-36s %16s %16s %8s\n", "------------------------------------", "----------------", "----------------", "--------" );
    printf( "%-36s %16s ", "Total (uncompressed est.)", "" );
    AnsiPrintf( ANSI_BOLD ANSI_YELLOW, "%16s\n", tracy::MemSizeToString( totalBytes ) );
}

int AnalyzeTrace( const char* input, int topN )
{
    auto f = std::unique_ptr<tracy::FileRead>( tracy::FileRead::Open( input ) );
    if( !f )
    {
        printf( "Cannot open trace file %s\n", input );
        return 1;
    }

    const auto fileSize = f->GetFileSize();

    printf( "Loading trace %s...", input );
    fflush( stdout );
    auto worker = tracy::Worker( *f, tracy::EventType::All, false );
    printf( " done.\n" );

    const auto decompressedSize = f->GetDecompressedSize();
    const auto actualMemUsage = tracy::memUsage.load( std::memory_order_relaxed );

    const auto firstTime = worker.GetFirstTime();
    const auto lastTime = worker.GetLastTime();

    AnsiPrintf( ANSI_BOLD ANSI_GREEN, "\n=== Trace Overview ===\n" );
    printf( "Program:        %s\n", worker.GetCaptureProgram().c_str() );
    printf( "Time span:      %s\n", tracy::TimeToString( lastTime - firstTime ) );
    printf( "File size:      %s (compressed on disk)\n", tracy::MemSizeToString( fileSize ) );
    printf( "Uncompressed:   %s (%.1fx ratio)\n", tracy::MemSizeToString( decompressedSize ), fileSize > 0 ? (double)decompressedSize / fileSize : 0.0 );
    printf( "Memory usage:   %s (loaded)\n", tracy::MemSizeToString( actualMemUsage ) );
    printf( "Zones:          %s\n", tracy::RealToString( worker.GetZoneCount() ) );
    printf( "GPU zones:      %s\n", tracy::RealToString( worker.GetGpuZoneCount() ) );
    printf( "Source locs:    %s\n", tracy::RealToString( worker.GetSrcLocCount() ) );
    printf( "Threads:        %s\n", tracy::RealToString( worker.GetThreadData().size() ) );
    printf( "Strings:        %s\n", tracy::RealToString( worker.GetStringsCount() ) );

    const auto& threads = worker.GetThreadData();
    const auto& messages = worker.GetMessages();
    const auto& lockMap = worker.GetLockMap();
    const auto& plots = worker.GetPlots();
    const auto& gpuData = worker.GetGpuData();
    const auto& frames = worker.GetFrames();
    const auto& frameImages = worker.GetFrameImages();

    uint64_t zoneCount = worker.GetZoneCount();
    uint64_t zoneExtraCount = worker.GetZoneExtraCount();
    uint64_t gpuZoneCount = worker.GetGpuZoneCount();

    uint64_t totalSamples = 0;
    for( auto& td : threads )
    {
        totalSamples += td->samples.size();
    }

    uint64_t totalCtxSwitch = 0;
    for( auto& td : threads )
    {
        auto cs = worker.GetContextSwitchData( td->id );
        if( cs ) totalCtxSwitch += cs->v.size();
    }

    uint64_t totalLockEvents = 0;
    for( auto& lm : lockMap )
    {
        totalLockEvents += lm.second->timeline.size();
    }

    uint64_t totalMemEvents = 0;
    for( auto& mn : worker.GetMemNameMap() )
    {
        totalMemEvents += mn.second->data.size();
    }

    uint64_t totalPlotItems = 0;
    for( auto& p : plots )
    {
        totalPlotItems += p->data.size();
    }

    uint64_t totalFrameEvents = 0;
    for( auto& fd : frames )
    {
        totalFrameEvents += fd->frames.size();
    }

    uint64_t totalGpuEvents = 0;
    for( auto& g : gpuData )
    {
        for( auto& t : g->threadData )
        {
            totalGpuEvents += t.second.timeline.size();
        }
    }

    uint64_t totalFrameImageBytes = 0;
    for( auto& fi : frameImages )
    {
        totalFrameImageBytes += fi->csz;
    }

    const auto& zoneChildrenVecs = worker.GetZoneChildrenVectors();
    uint64_t zoneChildrenCount = zoneChildrenVecs.size();
    uint64_t totalZoneChildEntries = 0;
    for( size_t i = 0; i < zoneChildrenVecs.size(); i++ )
    {
        totalZoneChildEntries += zoneChildrenVecs[i].size();
    }
    // Each children vector: sizeof(Vector<short_ptr<ZoneEvent>>) header in the outer vector.
    // Each child entry: sizeof(short_ptr<ZoneEvent>) in the heap-allocated inner array.
    uint64_t zoneChildrenBytes =
        zoneChildrenCount * sizeof( tracy::Vector<tracy::short_ptr<tracy::ZoneEvent>> ) +
        totalZoneChildEntries * sizeof( tracy::short_ptr<tracy::ZoneEvent> );

    std::vector<SizeEntry> entries;
    entries.push_back( { "Zones (ZoneEvent)", zoneCount, zoneCount * sizeof( tracy::ZoneEvent ) } );
    entries.push_back( { "Zone extras (ZoneExtra)", zoneExtraCount, zoneExtraCount * sizeof( tracy::ZoneExtra ) } );
    entries.push_back( { "Zone children vectors", zoneChildrenCount, zoneChildrenBytes } );
    entries.push_back( { "GPU zones (GpuEvent)", gpuZoneCount, gpuZoneCount * sizeof( tracy::GpuEvent ) } );
    entries.push_back( { "Context switches", totalCtxSwitch, totalCtxSwitch * sizeof( tracy::ContextSwitchData ) } );
    entries.push_back( { "Lock events (LockEventPtr)", totalLockEvents, totalLockEvents * sizeof( tracy::LockEventPtr ) } );
    entries.push_back( { "Memory events (MemEvent)", totalMemEvents, totalMemEvents * sizeof( tracy::MemEvent ) } );
    entries.push_back( { "Messages", messages.size(), messages.size() * sizeof( tracy::MessageData ) } );
    entries.push_back( { "Plot items", totalPlotItems, totalPlotItems * sizeof( tracy::PlotItem ) } );
    entries.push_back( { "Callstack samples", totalSamples, totalSamples * sizeof( tracy::SampleData ) } );
    entries.push_back( { "Callstack payloads", worker.GetCallstackPayloadCount(), worker.GetCallstackPayloadCount() * 8 * 3 } );
    entries.push_back( { "Callstack frames", worker.GetCallstackFrameCount(), worker.GetCallstackFrameCount() * sizeof( tracy::CallstackFrameData ) } );
    entries.push_back( { "Frame events", totalFrameEvents, totalFrameEvents * sizeof( tracy::FrameEvent ) } );
    entries.push_back( { "Frame images (compressed)", frameImages.size(), totalFrameImageBytes } );
    entries.push_back( { "Source locations", worker.GetSrcLocCount(), worker.GetSrcLocCount() * sizeof( tracy::SourceLocation ) } );
    entries.push_back( { "Symbols", worker.GetSymbolsCount(), worker.GetSymbolsCount() * sizeof( tracy::SymbolData ) } );
    entries.push_back( { "Symbol code", worker.GetSymbolCodeCount(), worker.GetSymbolCodeSize() } );
    entries.push_back( { "Source file cache", worker.GetSourceFileCacheCount(), worker.GetSourceFileCacheSize() } );
    entries.push_back( { "Strings (pointer map est.)", worker.GetStringsCount(), worker.GetStringsCount() * ( sizeof( uint64_t ) + sizeof( char* ) + 32 ) } );

    PrintSizeTable( "Estimated Memory Usage by Category", entries );

    AnsiPrintf( ANSI_BOLD ANSI_CYAN, "\n=== Source Location Analysis ===\n" );
    printf( "Total source locations: %s (int16_t limit: 32,767)\n", tracy::RealToString( worker.GetSrcLocCount() ) );
    double usage = 100.0 * worker.GetSrcLocCount() / 32767.0;
    if( usage > 90.0 )
        AnsiPrintf( ANSI_RED ANSI_BOLD, "WARNING: %.1f%% of source location limit used!\n", usage );
    else if( usage > 70.0 )
        AnsiPrintf( ANSI_YELLOW, "%.1f%% of source location limit used.\n", usage );
    else
        printf( "%.1f%% of source location limit used.\n", usage );

    std::vector<SrcLocEntry> srcLocEntries;

    const auto& srclocCntMap = worker.GetSourceLocationZonesCntMap();
    auto allIds = worker.GetAllSourceLocationIds();

    std::unordered_map<int16_t, uint64_t> lockEventCounts;
    for( auto& [lockId, lm] : lockMap )
    {
        lockEventCounts[lm->srcloc] += lm->timeline.size();
    }

    for( auto id : allIds )
    {
        auto& sl = worker.GetSourceLocation( id );
        const char* zoneName = worker.GetZoneName( sl );
        const char* file = worker.GetString( sl.file );

        uint64_t zoneCount = 0;
        auto it = srclocCntMap.find( id );
        if( it != srclocCntMap.end() ) zoneCount = it->second;

        uint64_t lockCount = 0;
        auto lit = lockEventCounts.find( id );
        if( lit != lockEventCounts.end() ) lockCount = lit->second;

        srcLocEntries.push_back( { id, zoneName, file, sl.line, zoneCount, lockCount } );
    }

    std::sort( srcLocEntries.begin(), srcLocEntries.end(), []( const SrcLocEntry& a, const SrcLocEntry& b )
    {
        return ( a.zoneCount + a.lockCount ) > ( b.zoneCount + b.lockCount );
    } );

    int shown = topN > 0 ? std::min( topN, (int)srcLocEntries.size() ) : (int)srcLocEntries.size();

    printf( "\n%-6s %-40s %-30s %12s %12s\n", "ID", "Name", "File:Line", "Zones", "Locks" );
    printf( "%-6s %-40s %-30s %12s %12s\n", "------", "----------------------------------------", "------------------------------", "------------", "------------" );

    for( int i = 0; i < shown; i++ )
    {
        auto& e = srcLocEntries[i];
        char fileLine[256];
        const char* shortFile = e.file;
        const char* slash = strrchr( e.file, '/' );
        if( !slash ) slash = strrchr( e.file, '\\' );
        if( slash ) shortFile = slash + 1;
        snprintf( fileLine, sizeof( fileLine ), "%s:%" PRIu32, shortFile, e.line );

        char nameBuf[41];
        size_t nameLen = strlen( e.zoneName );
        if( nameLen > 40 )
        {
            memcpy( nameBuf, e.zoneName, 37 );
            memcpy( nameBuf + 37, "...", 4 );
        }
        else
        {
            snprintf( nameBuf, sizeof( nameBuf ), "%s", e.zoneName );
        }

        printf( "%6d %-40s %-30s %12s %12s\n", e.srcloc, nameBuf, fileLine,
            e.zoneCount > 0 ? tracy::RealToString( e.zoneCount ) : "",
            e.lockCount > 0 ? tracy::RealToString( e.lockCount ) : "" );
    }

    if( (int)srcLocEntries.size() > shown )
    {
        printf( "... and %s more source locations\n", tracy::RealToString( srcLocEntries.size() - shown ) );
    }

    std::unordered_map<std::string, uint64_t> fileCounts;
    for( auto& e : srcLocEntries )
    {
        fileCounts[e.file]++;
    }

    std::vector<std::pair<std::string, uint64_t>> fileList( fileCounts.begin(), fileCounts.end() );
    std::sort( fileList.begin(), fileList.end(), []( const auto& a, const auto& b ) { return a.second > b.second; } );

    AnsiPrintf( ANSI_BOLD ANSI_CYAN, "\n=== Source Locations by File ===\n" );
    printf( "%-80s %8s\n", "File", "Src Locs" );
    printf( "%-80s %8s\n", "--------------------------------------------------------------------------------", "--------" );

    int fileShown = std::min( (int)fileList.size(), topN > 0 ? topN : 30 );
    for( int i = 0; i < fileShown; i++ )
    {
        const char* displayFile = fileList[i].first.c_str();
        char truncBuf[81];
        size_t len = strlen( displayFile );
        if( len > 80 )
        {
            snprintf( truncBuf, sizeof( truncBuf ), "...%s", displayFile + len - 77 );
            displayFile = truncBuf;
        }
        printf( "%-80s %8s\n", displayFile, tracy::RealToString( fileList[i].second ) );
    }
    if( (int)fileList.size() > fileShown )
    {
        printf( "... and %s more files\n", tracy::RealToString( fileList.size() - fileShown ) );
    }

    printf( "\n" );
    return 0;
}

int main( int argc, char** argv )
{
#ifdef _WIN32
    if( !AttachConsole( ATTACH_PARENT_PROCESS ) )
    {
        AllocConsole();
        SetConsoleMode( GetStdHandle( STD_OUTPUT_HANDLE ), 0x07 );
    }
#endif

    InitIsStdoutATerminal();

    bool overwrite = false;
    const char* address = "127.0.0.1";
    const char* output = nullptr;
    const char* analyzeInput = nullptr;
    int port = 8086;
    int seconds = -1;
    int64_t memoryLimit = -1;
    int analyzeTopN = 25;

    int c;
    while( ( c = getopt( argc, argv, "a:o:p:fs:m:A:n:" ) ) != -1 )
    {
        switch( c )
        {
        case 'a':
            address = optarg;
            break;
        case 'o':
            output = optarg;
            break;
        case 'p':
            port = atoi( optarg );
            break;
        case 'f':
            overwrite = true;
            break;
        case 's':
            seconds = atoi(optarg);
            break;
        case 'm':
            memoryLimit = std::clamp( atoll( optarg ), 1ll, 999ll ) * tracy::GetPhysicalMemorySize() / 100;
            break;
        case 'A':
            analyzeInput = optarg;
            break;
        case 'n':
            analyzeTopN = atoi( optarg );
            break;
        default:
            Usage();
            break;
        }
    }

    if( analyzeInput )
    {
        return AnalyzeTrace( analyzeInput, analyzeTopN );
    }

    if( !address || !output ) Usage();

    struct stat st;
    if( stat( output, &st ) == 0 && !overwrite )
    {
        printf( "Output file %s already exists! Use -f to force overwrite.\n", output );
        return 4;
    }

    FILE* test = fopen( output, "wb" );
    if( !test )
    {
        printf( "Cannot open output file %s for writing!\n", output );
        return 5;
    }
    fclose( test );
    unlink( output );

    printf( "Connecting to %s:%i...", address, port );
    fflush( stdout );
    tracy::Worker worker( address, port, memoryLimit );
    while( !worker.HasData() )
    {
        const auto handshake = worker.GetHandshakeStatus();
        if( handshake == tracy::HandshakeProtocolMismatch )
        {
            printf( "\nThe client you are trying to connect to uses incompatible protocol version.\nMake sure you are using the same Tracy version on both client and server.\n" );
            return 1;
        }
        if( handshake == tracy::HandshakeNotAvailable )
        {
            printf( "\nThe client you are trying to connect to is no longer able to sent profiling data,\nbecause another server was already connected to it.\nYou can do the following:\n\n  1. Restart the client application.\n  2. Rebuild the client application with on-demand mode enabled.\n" );
            return 2;
        }
        if( handshake == tracy::HandshakeDropped )
        {
            printf( "\nThe client you are trying to connect to has disconnected during the initial\nconnection handshake. Please check your network configuration.\n" );
            return 3;
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }
    printf( "\nTimer resolution: %s\n", tracy::TimeToString( worker.GetResolution() ) );

#ifdef _WIN32
    signal( SIGINT, SigInt );
#else
    struct sigaction sigint, oldsigint;
    memset( &sigint, 0, sizeof( sigint ) );
    sigint.sa_handler = SigInt;
    sigaction( SIGINT, &sigint, &oldsigint );
#endif

    const auto firstTime = worker.GetFirstTime();
    auto& lock = worker.GetMbpsDataLock();

    const auto t0 = std::chrono::high_resolution_clock::now();
    while( worker.IsConnected() )
    {
        // Relaxed order is sufficient here because `s_disconnect` is only ever
        // set by this thread or by the SigInt handler, and that handler does
        // nothing else than storing `s_disconnect`.
        if( s_disconnect.load( std::memory_order_relaxed ) )
        {
            worker.Disconnect();
            // Relaxed order is sufficient because only this thread ever reads
            // this value.
            s_disconnect.store(false, std::memory_order_relaxed );
            break;
        }

        lock.lock();
        const auto mbps = worker.GetMbpsData().back();
        const auto compRatio = worker.GetCompRatio();
        const auto netTotal = worker.GetDataTransferred();
        const auto queueSize = worker.GetSendQueueSize();
        lock.unlock();

        // Output progress info only if destination is a TTY to avoid bloating
        // log files (so this is not just about usage of ANSI color codes).
        if( IsStdoutATerminal() )
        {
            const char* unit = "Mbps";
            float unitsPerMbps = 1.f;
            if( mbps < 0.1f )
            {
                unit = "Kbps";
                unitsPerMbps = 1000.f;
            }
            AnsiPrintf( ANSI_ERASE_LINE ANSI_CYAN ANSI_BOLD, "\r%7.2f %s", mbps * unitsPerMbps, unit );
            printf( " /");
            AnsiPrintf( ANSI_CYAN ANSI_BOLD, "%5.1f%%", compRatio * 100.f );
            printf( " =");
            AnsiPrintf( ANSI_YELLOW ANSI_BOLD, "%7.2f Mbps", mbps / compRatio );
            printf( " | ");
            AnsiPrintf( ANSI_YELLOW, "Tx: ");
            AnsiPrintf( ANSI_GREEN, "%s", tracy::MemSizeToString( netTotal ) );
            printf( " | ");
            AnsiPrintf( ANSI_RED ANSI_BOLD, "%s", tracy::MemSizeToString( tracy::memUsage.load( std::memory_order_relaxed ) ) );
            if( memoryLimit > 0 )
            {
                printf( " / " );
                AnsiPrintf( ANSI_BLUE ANSI_BOLD, "%s", tracy::MemSizeToString( memoryLimit ) );
            }
            printf( " | ");
            AnsiPrintf( ANSI_RED, "%s", tracy::TimeToString( worker.GetLastTime() - firstTime ) );
            printf( " | ");
            AnsiPrintf( ANSI_RED ANSI_BOLD, "%s query backlog", tracy::RealToString( queueSize ) );
            fflush( stdout );
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        if( seconds != -1 )
        {
            const auto dur = std::chrono::high_resolution_clock::now() - t0;
            if( std::chrono::duration_cast<std::chrono::seconds>(dur).count() >= seconds )
            {
                // Relaxed order is sufficient because only this thread ever reads
                // this value.
                s_disconnect.store(true, std::memory_order_relaxed );
            }
        }
    }
    const auto t1 = std::chrono::high_resolution_clock::now();

    const auto& failure = worker.GetFailureType();
    if( failure != tracy::Worker::Failure::None )
    {
        AnsiPrintf( ANSI_RED ANSI_BOLD, "\nInstrumentation failure: %s", tracy::Worker::GetFailureString( failure ) );
        auto& fd = worker.GetFailureData();
        if( !fd.message.empty() )
        {
            printf( "\nContext: %s", fd.message.c_str() );
        }
        if( fd.callstack != 0 )
        {
            AnsiPrintf( ANSI_BOLD, "\nFailure callstack:\n" );
            auto& cs = worker.GetCallstack( fd.callstack );
            int fidx = 0;
            for( auto& entry : cs )
            {
                auto frameData = worker.GetCallstackFrame( entry );
                if( !frameData )
                {
                    printf( "%3i. %p\n", fidx++, (void*)worker.GetCanonicalPointer( entry ) );
                }
                else
                {
                    const auto fsz = frameData->size;
                    for( uint8_t f=0; f<fsz; f++ )
                    {
                        const auto& frame = frameData->data[f];
                        auto txt = worker.GetString( frame.name );

                        if( fidx == 0 && f != fsz-1 )
                        {
                            auto test = tracy::s_tracyStackFrames;
                            bool match = false;
                            do
                            {
                                if( strcmp( txt, *test ) == 0 )
                                {
                                    match = true;
                                    break;
                                }
                            }
                            while( *++test );
                            if( match ) continue;
                        }

                        if( f == fsz-1 )
                        {
                            printf( "%3i. ", fidx++ );
                        }
                        else
                        {
                            AnsiPrintf( ANSI_BLACK ANSI_BOLD, "inl. " );
                        }
                        AnsiPrintf( ANSI_CYAN, "%s  ", txt );
                        txt = worker.GetString( frame.file );
                        if( frame.line == 0 )
                        {
                            AnsiPrintf( ANSI_YELLOW, "(%s)", txt );
                        }
                        else
                        {
                            AnsiPrintf( ANSI_YELLOW, "(%s:%" PRIu32 ")", txt, frame.line );
                        }
                        if( frameData->imageName.Active() )
                        {
                            AnsiPrintf( ANSI_MAGENTA, " %s\n", worker.GetString( frameData->imageName ) );
                        }
                        else
                        {
                            printf( "\n" );
                        }
                    }
                }
            }
        }
    }

    printf( "\nFrames: %" PRIu64 "\nTime span: %s\nZones: %s\nElapsed time: %s\nSaving trace...",
        worker.GetFrameCount( *worker.GetFramesBase() ), tracy::TimeToString( worker.GetLastTime() - firstTime ), tracy::RealToString( worker.GetZoneCount() ),
        tracy::TimeToString( std::chrono::duration_cast<std::chrono::nanoseconds>( t1 - t0 ).count() ) );
    fflush( stdout );
    auto f = std::unique_ptr<tracy::FileWrite>( tracy::FileWrite::Open( output, tracy::FileCompression::Zstd, 3, 4 ) );
    if( f )
    {
        worker.Write( *f, false );
        AnsiPrintf( ANSI_GREEN ANSI_BOLD, " done!\n" );
        f->Finish();
        const auto stats = f->GetCompressionStatistics();
        printf( "Trace size %s (%.2f%% ratio)\n", tracy::MemSizeToString( stats.second ), 100.f * stats.second / stats.first );
    }
    else
    {
        AnsiPrintf( ANSI_RED ANSI_BOLD, " failed!\n");
    }

    return 0;
}
