import { describe, expect, it } from 'vitest';
import {
  addDays,
  averageCharsPerDay,
  buildCalendar,
  charsPerMinute,
  currentStreak,
  dayKeyFromDate,
  fastestHour,
  formatDayKey,
  highestDay,
  longestStreak,
  sumDaily,
  type DailyStats,
  type HourlyStats,
} from './statistics-metrics';

function day(dayKey: number, chars: Partial<Omit<DailyStats, 'day'>> = {}): DailyStats {
  return {
    day: dayKey,
    cjk: 0,
    latin: 0,
    digit: 0,
    punct: 0,
    other: 0,
    activeMs: 0,
    ...chars,
  };
}

function cjkDay(dayKey: number, chars: number, activeMs = 0): DailyStats {
  return day(dayKey, { cjk: chars, activeMs });
}

describe('day keys', () => {
  it('formats and walks local dates across month, year and leap boundaries', () => {
    expect(dayKeyFromDate(new Date(2026, 8, 18))).toBe(20260918);
    expect(formatDayKey(20260918)).toBe('2026-09-18');
    expect(formatDayKey(20250101)).toBe('2025-01-01');
    expect(addDays(20251231, 1)).toBe(20260101);
    expect(addDays(20250101, -1)).toBe(20241231);
    expect(addDays(20240228, 1)).toBe(20240229);
    expect(addDays(20250228, 1)).toBe(20250301);
  });
});

describe('sumDaily / averageCharsPerDay', () => {
  it('counts only days with characters in the average denominator', () => {
    const daily = [cjkDay(20260916, 10), day(20260917, { activeMs: 5_000 }), cjkDay(20260918, 20)];
    const totals = sumDaily(daily);
    expect(totals.chars).toBe(30);
    expect(totals.activeMs).toBe(5_000);
    expect(totals.cjk).toBe(30);
    expect(totals.recordedDays).toBe(2);
    expect(averageCharsPerDay(daily)).toBe(15);
  });

  it('handles a single recorded day and empty data', () => {
    expect(averageCharsPerDay([cjkDay(20260918, 42)])).toBe(42);
    expect(averageCharsPerDay([])).toBe(0);
    expect(averageCharsPerDay([day(20260918, { activeMs: 60_000 })])).toBe(0);
  });
});

describe('currentStreak / longestStreak', () => {
  it('counts through today when today has records', () => {
    const daily = [cjkDay(20251229, 1), cjkDay(20251230, 1), cjkDay(20251231, 1)];
    expect(currentStreak(daily, 20251231)).toBe(3);
  });

  it('falls back to yesterday and crosses the new year', () => {
    const daily = [cjkDay(20251230, 1), cjkDay(20251231, 1)];
    expect(currentStreak(daily, 20260101)).toBe(2);
    expect(currentStreak([cjkDay(20251228, 1)], 20260101)).toBe(0);
  });

  it('stops at the first missing day and ignores zero-char rows', () => {
    const daily = [cjkDay(20260914, 5), cjkDay(20260915, 5), day(20260916), cjkDay(20260917, 5), cjkDay(20260918, 5)];
    expect(currentStreak(daily, 20260918)).toBe(2);
    expect(longestStreak(daily)).toBe(2);
  });

  it('finds the longest run across months and years', () => {
    const daily = [cjkDay(20241230, 1), cjkDay(20241231, 1), cjkDay(20250101, 1), cjkDay(20250102, 1), cjkDay(20250203, 1)];
    expect(longestStreak(daily)).toBe(4);
    expect(longestStreak([])).toBe(0);
    expect(longestStreak([cjkDay(20241230, 1), cjkDay(20250101, 1)])).toBe(1);
  });
});

describe('highestDay', () => {
  it('picks the earliest day when several days tie', () => {
    const best = highestDay([cjkDay(20260918, 10), cjkDay(20260916, 10), cjkDay(20260917, 3)]);
    expect(best).toEqual({ day: 20260916, chars: 10 });
  });

  it('returns null without any recorded characters', () => {
    expect(highestDay([])).toBeNull();
    expect(highestDay([day(20260918, { latin: 0, activeMs: 1_000 })])).toBeNull();
  });
});

describe('fastestHour', () => {
  it('skips buckets below the minimum active duration', () => {
    const hourly: HourlyStats[] = [
      { day: 20260918, hour: 9, chars: 100, activeMs: 30_000 },
      { day: 20260918, hour: 10, chars: 60, activeMs: 60_000 },
      { day: 20260918, hour: 11, chars: 300, activeMs: 120_000 },
    ];
    expect(fastestHour(hourly)).toEqual({ day: 20260918, hour: 11, perMinute: 150 });
    expect(charsPerMinute(60, 60_000)).toBe(60);
  });

  it('keeps the earliest bucket on ties and returns null without data', () => {
    const hourly: HourlyStats[] = [
      { day: 20260918, hour: 8, chars: 120, activeMs: 120_000 },
      { day: 20260918, hour: 9, chars: 60, activeMs: 60_000 },
      { day: 20260918, hour: 10, chars: 50, activeMs: 120_000 },
    ];
    expect(fastestHour(hourly)).toEqual({ day: 20260918, hour: 8, perMinute: 60 });
    expect(fastestHour([])).toBeNull();
    expect(fastestHour([{ day: 20260918, hour: 9, chars: 0, activeMs: 0 }])).toBeNull();
  });
});

describe('buildCalendar', () => {
  it('lays out 53 week columns ending today and marks the window', () => {
    const grid = buildCalendar([cjkDay(20251231, 100)], 20251231);
    expect(grid.columns).toBe(53);
    expect(grid.cells).toHaveLength(53 * 7);

    const visible = grid.cells.filter((cell) => cell.inRange);
    expect(visible).toHaveLength(365);
    expect(visible[0].day).toBe(20250101);
    expect(visible.at(-1)!.day).toBe(20251231);
    expect(grid.cells.filter((cell) => !cell.inRange).every((cell) => cell.chars === 0)).toBe(true);
  });

  it('normalizes heat levels against the largest visible day', () => {
    const grid = buildCalendar(
      [cjkDay(20251231, 100), cjkDay(20251230, 50), cjkDay(20251229, 25), cjkDay(20251228, 5)],
      20251231,
    );
    const level = (dayKey: number) => grid.cells.find((cell) => cell.day === dayKey)!.level;
    expect(level(20251231)).toBe(4);
    expect(level(20251230)).toBe(2);
    expect(level(20251229)).toBe(1);
    expect(level(20251228)).toBe(1);
    expect(level(20251227)).toBe(0);
  });

  it('labels every month of the window once, in column order', () => {
    const grid = buildCalendar([], 20251231);
    expect(grid.monthLabels[0]).toEqual({ column: 0, label: '12月' });
    expect(grid.monthLabels[1]).toEqual({ column: 1, label: '1月' });
    expect(grid.monthLabels.map((label) => label.column)).toEqual([...grid.monthLabels.map((label) => label.column)].sort((a, b) => a - b));
    expect(new Set(grid.monthLabels.map((label) => label.label)).size).toBe(12);
  });

  it('starts the first column on a Monday', () => {
    const grid = buildCalendar([], 20251231);
    expect(grid.cells[0].day).toBe(20241230);
  });
});
