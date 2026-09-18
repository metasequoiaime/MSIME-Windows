#include "stats_collector.h"

#include <Windows.h>
#include <cstring>
#include <mutex>
#include <vector>

#include "Globals.h"
#include "Ipc.h"

namespace
{
std::mutex statsMutex;
MsimeStats::PendingStatsQueue statsQueue;
// Guarded by statsMutex. Zero means "no previous commit in the current
// enabled session", which the first commit reports as zero active time.
uint64_t statsLastCommitMs = 0;
bool statsFlushScheduled = false;

void CALLBACK FlushStatisticsRecords(PTP_CALLBACK_INSTANCE, PVOID);

void ScheduleStatsFlushLocked()
{
    if (statsFlushScheduled)
    {
        return;
    }
    statsFlushScheduled = true;
    DllAddRef();
    if (!TrySubmitThreadpoolCallback(FlushStatisticsRecords, nullptr, nullptr))
    {
        statsFlushScheduled = false;
        DllRelease();
    }
}

uint16_t SaturateToUint16(uint32_t value)
{
    return value > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(value);
}

bool SendStatsBatch(const FanyImeStatsBatchHeader &header, const std::vector<FanyImeStatsRecord> &records)
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        pipe = CreateFileW(FANY_IME_STATS_NAMED_PIPE, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            break;
        }
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(FANY_IME_STATS_NAMED_PIPE, 20))
        {
            break;
        }
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    std::vector<unsigned char> frame(sizeof(header) + records.size() * sizeof(FanyImeStatsRecord));
    memcpy(frame.data(), &header, sizeof(header));
    if (!records.empty())
    {
        memcpy(frame.data() + sizeof(header), records.data(), records.size() * sizeof(FanyImeStatsRecord));
    }
    DWORD bytesWritten = 0;
    const BOOL writeResult = WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &bytesWritten, nullptr);
    CloseHandle(pipe);
    return writeResult && bytesWritten == frame.size();
}

void CALLBACK FlushStatisticsRecords(PTP_CALLBACK_INSTANCE, PVOID)
{
    Sleep(MsimeStats::kFlushDelayMs);

    FanyImeStatsBatchHeader header;
    std::vector<FanyImeStatsRecord> records;
    bool statisticsDisabled = false;
    {
        std::lock_guard lock(statsMutex);
        if (!Global::StatisticsEnabled.load(std::memory_order_relaxed))
        {
            // Turning the switch off discards what was queued: the user asked
            // for no further collection, not for a delayed flush.
            statsQueue.Reset();
            statsFlushScheduled = false;
            statisticsDisabled = true;
        }
        else
        {
            records.reserve(MsimeStats::kMaxRecordsPerBatch);
            header.record_count = static_cast<uint32_t>(statsQueue.TakeBatch(records, MsimeStats::kMaxRecordsPerBatch));
            header.dropped_count = statsQueue.TakeDropped();
            header.source_process_id = GetCurrentProcessId();
        }
    }
    if (statisticsDisabled)
    {
        DllRelease();
        return;
    }

    bool sent = false;
    if (header.record_count != 0)
    {
        header.payload_bytes = static_cast<uint32_t>(records.size() * sizeof(FanyImeStatsRecord));
        sent = SendStatsBatch(header, records);
    }
    {
        std::lock_guard lock(statsMutex);
        if (header.record_count != 0 && !sent)
        {
            // Statistics are lossy by design: a Server that is not listening
            // must never make the TSF side retry or buffer to disk.
            statsQueue.AddDropped(header.record_count);
        }
        statsFlushScheduled = false;
        if (!statsQueue.Empty())
        {
            ScheduleStatsFlushLocked();
        }
    }
    DllRelease();
}
} // namespace

namespace MsimeStats
{
void QueueStatisticsCommit(const CharClassCounts &counts)
{
    if (!Global::StatisticsEnabled.load(std::memory_order_relaxed) || counts.Total() == 0)
    {
        return;
    }

    // Bucket at commit time: a batch may only be flushed 250 ms later, and the
    // day/hour key has to reflect when the text was actually typed.
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    const uint32_t dayKey = static_cast<uint32_t>(localTime.wYear) * 10000u +
                            static_cast<uint32_t>(localTime.wMonth) * 100u + static_cast<uint32_t>(localTime.wDay);

    std::lock_guard lock(statsMutex);
    FanyImeStatsRecord record;
    record.day_key = dayKey;
    record.hour = localTime.wHour;
    record.cjk = SaturateToUint16(counts.cjk);
    record.latin = SaturateToUint16(counts.latin);
    record.digit = SaturateToUint16(counts.digit);
    record.punct = SaturateToUint16(counts.punct);
    record.other = SaturateToUint16(counts.other);
    record.active_ms = ComputeActiveDeltaMs(GetTickCount64(), statsLastCommitMs);
    statsQueue.Accumulate(record);
    ScheduleStatsFlushLocked();
}

void ResetStatisticsActiveTimer()
{
    std::lock_guard lock(statsMutex);
    statsLastCommitMs = 0;
}
} // namespace MsimeStats
