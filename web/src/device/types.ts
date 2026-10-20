import type { StringKey } from '../i18n'

export type Reading = {
  temperatureCelsius: number | null
  humidityPercent: number | null
  soilMoisturePercent: number | null
  waterLevelPercent: number | null
  recordedAt: string
}

// GET /devices/{id} (server DeviceSummaryDto).
export type DeviceDetailDto = {
  id: string
  name: string
  macAddress: string | null
  pairedAt: string | null
  lowWaterWarningThresholdPercent: number | null
  lowWaterCriticalThresholdPercent: number | null
  lowWaterAlertIntervalMinutes: number | null
  avatarUrl: string | null
  owner: string | null
  latestReading: Reading | null
  careSuggestion: string | null
  isOnline: boolean
  xiaozhiActivationCode: string | null
  muted: boolean
  firmwareVersion: string | null
}

export type WateringSchedule = {
  id: string
  hour: number
  minute: number
  durationSeconds: number
  enabled: boolean
}

export type MetricStatus = { label: StringKey | null; tone: 'good' | 'low' | 'high' | 'none' }

// Same thresholds as the app's DeviceDetailScreen._statusFor calls.
export const METRIC_RANGES = {
  soil: { low: 30, high: 60 },
  temperature: { low: 18, high: 30 },
  humidity: { low: 40, high: 80 },
  water: { low: 20, high: 100 },
} as const

export function statusFor(value: number | null, range: { low: number; high: number }): MetricStatus {
  if (value == null) return { label: null, tone: 'none' }
  if (value < range.low) return { label: 'dp.metricLow', tone: 'low' }
  if (value > range.high) return { label: 'dp.metricHigh', tone: 'high' }
  return { label: 'dp.metricGood', tone: 'good' }
}

export const MIN_WATER_SECONDS = 1
export const MAX_WATER_SECONDS = 300
export const DEFAULT_WATER_SECONDS = 10

export function isValidDuration(seconds: number): boolean {
  return Number.isInteger(seconds) && seconds >= MIN_WATER_SECONDS && seconds <= MAX_WATER_SECONDS
}

export function formatTime(hour: number, minute: number): string {
  return `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}`
}

// "HTTP 400: <server text>" -- the server returns plain-text problem bodies for
// validation errors, which are more useful to show than the status alone.
export async function describeError(res: Response): Promise<string> {
  const body = (await res.text().catch(() => '')).trim()
  return body && body.length < 200 ? `HTTP ${res.status}: ${body}` : `HTTP ${res.status}`
}

export function metricStatuses(device: DeviceDetailDto) {
  const r = device.latestReading
  return {
    soil: statusFor(r?.soilMoisturePercent ?? null, METRIC_RANGES.soil),
    temperature: statusFor(r?.temperatureCelsius ?? null, METRIC_RANGES.temperature),
    humidity: statusFor(r?.humidityPercent ?? null, METRIC_RANGES.humidity),
    water: statusFor(r?.waterLevelPercent ?? null, METRIC_RANGES.water),
  }
}
