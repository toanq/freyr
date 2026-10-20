import { useI18n, type StringKey } from '../i18n'
import type { DeviceDetailDto, MetricStatus } from './types'
import { metricStatuses } from './types'

type Tile = {
  key: string
  label: StringKey
  value: string
  status: MetricStatus
  accent: string
  warning?: boolean
}

export default function Metrics({ device }: { device: DeviceDetailDto }) {
  const { t, formatDate } = useI18n()
  const r = device.latestReading
  const statuses = metricStatuses(device)
  const fmt = (v: number | null | undefined, unit: string) => (v != null ? `${v.toFixed(0)}${unit}` : '—')
  const warning = device.lowWaterWarningThresholdPercent
  const waterLevel = r?.waterLevelPercent ?? null

  const tiles: Tile[] = [
    { key: 'soil', label: 'telemetry.soil', value: fmt(r?.soilMoisturePercent, '%'), status: statuses.soil, accent: 'var(--high)' },
    { key: 'temp', label: 'telemetry.temperature', value: fmt(r?.temperatureCelsius, '°C'), status: statuses.temperature, accent: '#ffa726' },
    { key: 'hum', label: 'telemetry.humidity', value: fmt(r?.humidityPercent, '%'), status: statuses.humidity, accent: '#81d4fa' },
    {
      key: 'water',
      label: 'telemetry.water',
      value: fmt(waterLevel, '%'),
      status: statuses.water,
      accent: '#4dd0e1',
      // In-app-only warning tier, same rule as the app's _MetricTile.isWarning.
      warning: warning != null && waterLevel != null && waterLevel <= warning,
    },
  ]

  return (
    <section className="dp-metrics">
      <div className="dp-metric-grid">
        {tiles.map((tile) => (
          <div
            key={tile.key}
            className={tile.warning ? 'dp-metric dp-metric--warning' : 'dp-metric'}
            style={{ borderLeftColor: tile.accent }}
          >
            <span className="dp-metric-label">
              {t(tile.label)}
              {tile.warning && <span className="dp-warning-icon" aria-hidden="true">⚠</span>}
            </span>
            <span className="dp-metric-value">{tile.value}</span>
            <span className={`dp-metric-status dp-tone-${tile.status.tone}`}>
              {tile.status.label ? t(tile.status.label) : '—'}
            </span>
          </div>
        ))}
      </div>
      {r ? (
        <p className="telemetry-timestamp">{t('telemetry.updated', { date: formatDate(r.recordedAt) })}</p>
      ) : (
        <p className="telemetry--empty">{t('telemetry.empty')}</p>
      )}
      <div className="card dp-suggestion">
        <h3>{t('dp.suggestion')}</h3>
        <p>{device.careSuggestion || t('dp.suggestionPending')}</p>
      </div>
    </section>
  )
}
