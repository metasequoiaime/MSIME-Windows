// 统计面板的派生指标：全部是纯函数，口径见 prd.md R3。
// 放在前端算的理由见 design.md §3.3：纯函数，省一次契约往返，也不把口径固化进协议。

export type DailyStats = {
  /** 本地日期 YYYYMMDD。 */
  day: number;
  cjk: number;
  latin: number;
  digit: number;
  punct: number;
  other: number;
  activeMs: number;
};

export type HourlyStats = {
  day: number;
  /** 0–23。 */
  hour: number;
  chars: number;
  activeMs: number;
};

export type CharacterBreakdown = Pick<DailyStats, 'cjk' | 'latin' | 'digit' | 'punct' | 'other'>;

export type DailyTotals = CharacterBreakdown & {
  chars: number;
  activeMs: number;
  /** 有输入记录的天数，即日均的分母；零记录日不计入。 */
  recordedDays: number;
};

export type DaySummary = { day: number; chars: number };
export type HourSpeed = { day: number; hour: number; perMinute: number };

/**
 * 最快速度只采信活跃时长至少这么久的时段：小样本（一两个字符）除出来会虚高。
 * 与 DLL 侧「相邻两次上屏间隔 ≤ 5 秒才计入活跃」共同决定速度口径。
 */
export const FASTEST_HOUR_MIN_ACTIVE_MS = 60_000;

const MONTH_LABELS = [
  '1月',
  '2月',
  '3月',
  '4月',
  '5月',
  '6月',
  '7月',
  '8月',
  '9月',
  '10月',
  '11月',
  '12月',
];

export function totalChars(row: CharacterBreakdown): number {
  return row.cjk + row.latin + row.digit + row.punct + row.other;
}

export function dayKeyFromDate(date: Date): number {
  return date.getFullYear() * 10_000 + (date.getMonth() + 1) * 100 + date.getDate();
}

export function dateFromDayKey(day: number): Date {
  return new Date(Math.floor(day / 10_000), (Math.floor(day / 100) % 100) - 1, day % 100);
}

/** 按本地日历加减天数，跨月、跨年、闰年与夏令时都交给 Date 处理。 */
export function addDays(day: number, delta: number): number {
  const date = dateFromDayKey(day);
  date.setDate(date.getDate() + delta);
  return dayKeyFromDate(date);
}

export function formatDayKey(day: number): string {
  const date = dateFromDayKey(day);
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const dayOfMonth = String(date.getDate()).padStart(2, '0');
  return `${date.getFullYear()}-${month}-${dayOfMonth}`;
}

export function sumDaily(daily: DailyStats[]): DailyTotals {
  const totals: DailyTotals = {
    cjk: 0,
    latin: 0,
    digit: 0,
    punct: 0,
    other: 0,
    chars: 0,
    activeMs: 0,
    recordedDays: 0,
  };
  for (const row of daily) {
    const chars = totalChars(row);
    totals.cjk += row.cjk;
    totals.latin += row.latin;
    totals.digit += row.digit;
    totals.punct += row.punct;
    totals.other += row.other;
    totals.chars += chars;
    totals.activeMs += row.activeMs;
    if (chars > 0) totals.recordedDays += 1;
  }
  return totals;
}

/** 日均分母是「有输入记录的天数」，不是自然日跨度，也不是行数。 */
export function averageCharsPerDay(daily: DailyStats[]): number {
  const totals = sumDaily(daily);
  return totals.recordedDays > 0 ? totals.chars / totals.recordedDays : 0;
}

/** 速度 = 字数 ÷ 活跃分钟数；活跃时长已由 DLL 按停顿切分，这里只做除法。 */
export function charsPerMinute(chars: number, activeMs: number): number {
  return activeMs > 0 ? chars / (activeMs / 60_000) : 0;
}

export function findDay(daily: DailyStats[], day: number): DailyStats | undefined {
  return daily.find((row) => row.day === day);
}

function recordedDayKeys(daily: DailyStats[]): number[] {
  return daily
    .filter((row) => totalChars(row) > 0)
    .map((row) => row.day)
    .sort((left, right) => left - right);
}

/** 截至今天（今天没记录就从昨天起算）的连续有记录天数。 */
export function currentStreak(daily: DailyStats[], today: number): number {
  const recorded = new Set(recordedDayKeys(daily));
  let cursor = recorded.has(today) ? today : addDays(today, -1);
  let streak = 0;
  while (recorded.has(cursor)) {
    streak += 1;
    cursor = addDays(cursor, -1);
  }
  return streak;
}

export function longestStreak(daily: DailyStats[]): number {
  let best = 0;
  let run = 0;
  let previous: number | null = null;
  for (const day of recordedDayKeys(daily)) {
    run = previous !== null && addDays(previous, 1) === day ? run + 1 : 1;
    if (run > best) best = run;
    previous = day;
  }
  return best;
}

/** 并列最大时取更早的一天：按日期升序扫描，严格大于才替换。 */
export function highestDay(daily: DailyStats[]): DaySummary | null {
  let best: DaySummary | null = null;
  for (const row of [...daily].sort((left, right) => left.day - right.day)) {
    const chars = totalChars(row);
    if (chars > 0 && (best === null || chars > best.chars)) {
      best = { day: row.day, chars };
    }
  }
  return best;
}

/** 历史最快速度：只看活跃满 FASTEST_HOUR_MIN_ACTIVE_MS 的小时桶；并列时取更早的桶。 */
export function fastestHour(hourly: HourlyStats[]): HourSpeed | null {
  let best: HourSpeed | null = null;
  const rows = [...hourly].sort((left, right) => left.day - right.day || left.hour - right.hour);
  for (const row of rows) {
    if (row.activeMs < FASTEST_HOUR_MIN_ACTIVE_MS || row.chars <= 0) continue;
    const perMinute = charsPerMinute(row.chars, row.activeMs);
    if (best === null || perMinute > best.perMinute) {
      best = { day: row.day, hour: row.hour, perMinute };
    }
  }
  return best;
}

export type CalendarCell = {
  day: number;
  chars: number;
  /** 0 = 无输入；1–4 为相对窗口内最大值的热力档位。 */
  level: number;
  /** 落在统计窗口内的日期才渲染；窗口外的只是占位格。 */
  inRange: boolean;
};

export type CalendarGrid = {
  /** 列数，每列一周（周一 → 周日）。 */
  columns: number;
  /** 按列优先排列：第 0 列是周一…周日，第 1 列是下一周。 */
  cells: CalendarCell[];
  monthLabels: Array<{ column: number; label: string }>;
};

function weekdayIndexMondayFirst(date: Date): number {
  return (date.getDay() + 6) % 7;
}

/** 构造最近 days 天的日历网格：365 天最少占 53 个自然周列，对齐参考实现的输入日历。 */
export function buildCalendar(daily: DailyStats[], today: number, days = 365): CalendarGrid {
  const charsByDay = new Map<number, number>();
  for (const row of daily) {
    const chars = totalChars(row);
    if (chars > 0) charsByDay.set(row.day, (charsByDay.get(row.day) ?? 0) + chars);
  }

  const end = today;
  const start = addDays(today, -(days - 1));
  const gridStart = addDays(start, -weekdayIndexMondayFirst(dateFromDayKey(start)));

  const cells: CalendarCell[] = [];
  const monthLabels: Array<{ column: number; label: string }> = [];
  let maxChars = 0;
  let column = 0;
  let cursor = gridStart;
  let lastMonth = -1;
  while (cursor <= end) {
    const month = dateFromDayKey(cursor).getMonth();
    if (month !== lastMonth) {
      monthLabels.push({ column, label: MONTH_LABELS[month] });
      lastMonth = month;
    }
    for (let row = 0; row < 7; row += 1) {
      const day = addDays(cursor, row);
      const inRange = day >= start && day <= end;
      const chars = inRange ? (charsByDay.get(day) ?? 0) : 0;
      if (chars > maxChars) maxChars = chars;
      cells.push({ day, chars, level: 0, inRange });
    }
    cursor = addDays(cursor, 7);
    column += 1;
  }

  if (maxChars > 0) {
    for (const cell of cells) {
      cell.level = cell.chars === 0 ? 0 : Math.max(1, Math.ceil((cell.chars / maxChars) * 4));
    }
  }
  return { columns: column, cells, monthLabels };
}
