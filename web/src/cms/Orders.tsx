import { useCallback, useEffect, useState } from 'react'
import { useI18n, type StringKey } from '../i18n'
import { CmsError, ORDER_STATUSES, cmsApi, type CmsOrder, type OrderStatus } from './cmsApi'
import './cms.css'

type Filter = 'all' | OrderStatus

const statusKey = (s: OrderStatus) => `cms.status.${s}` as StringKey

function Orders() {
  const { t, formatDate } = useI18n()
  const [orders, setOrders] = useState<CmsOrder[] | null>(null)
  const [loadFailed, setLoadFailed] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [filter, setFilter] = useState<Filter>('all')
  const [savingId, setSavingId] = useState<string | null>(null)

  const load = useCallback(() => {
    cmsApi
      .listOrders()
      .then((list) => {
        setOrders(list)
        setLoadFailed(false)
      })
      .catch(() => setLoadFailed(true))
  }, [])

  useEffect(load, [load])

  const changeStatus = async (order: CmsOrder, status: OrderStatus) => {
    if (status === order.status) return
    if (status === 'cancelled' && !window.confirm(t('cms.orders.confirmCancel', { name: order.customer_name }))) return
    setError(null)
    setSavingId(order.id)
    try {
      const updated = await cmsApi.setOrderStatus(order.id, status)
      setOrders((list) => list?.map((o) => (o.id === updated.id ? updated : o)) ?? null)
    } catch (err) {
      setError(t('cms.orders.updateFailed', { error: err instanceof CmsError ? err.message : String(err) }))
    } finally {
      setSavingId(null)
    }
  }

  const count = (f: Filter) => (orders ? (f === 'all' ? orders.length : orders.filter((o) => o.status === f).length) : 0)
  const visible = orders?.filter((o) => filter === 'all' || o.status === filter) ?? []

  return (
    <section className="cms">
      <div className="page-header">
        <h2>{t('tab.orders')}</h2>
        <span className="page-count">{orders?.length ?? 0}</span>
        <button type="button" className="cms-refresh" onClick={load}>
          {t('cms.refresh')}
        </button>
      </div>

      <div className="cms-filters" role="group" aria-label={t('cms.orders.statusLabel')}>
        {(['all', ...ORDER_STATUSES] as Filter[]).map((f) => (
          <button
            key={f}
            type="button"
            className={filter === f ? 'cms-filter cms-filter--active' : 'cms-filter'}
            aria-pressed={filter === f}
            onClick={() => setFilter(f)}
          >
            {f === 'all' ? t('cms.all') : t(statusKey(f))} <span className="cms-filter-count">{count(f)}</span>
          </button>
        ))}
      </div>

      {loadFailed && <p className="form-error">{t('cms.orders.loadFailed')}</p>}
      {error && <p className="form-error">{error}</p>}
      {orders && visible.length === 0 && <p className="empty-state">{t('cms.orders.empty')}</p>}

      <ul className="cms-list">
        {visible.map((o) => (
          <li key={o.id} className={`card cms-order cms-order--${o.status}`}>
            <div className="cms-order-head">
              <strong className="cms-order-name">{o.customer_name}</strong>
              <span className={`cms-status cms-status--${o.status}`}>{t(statusKey(o.status))}</span>
            </div>
            <div className="cms-order-contact">
              <a href={`tel:${o.phone}`}>{o.phone}</a>
              {o.email && <a href={`mailto:${o.email}`}>{o.email}</a>}
            </div>
            {o.address && <p className="cms-order-address">{o.address}</p>}
            <p className="cms-order-meta">
              {t('cms.orders.qty', { n: o.quantity })} · {formatDate(o.created_at)}
            </p>
            {o.note && <p className="cms-order-note">{o.note}</p>}
            <label className="cms-order-status">
              <span>{t('cms.orders.statusLabel')}</span>
              <select
                value={o.status}
                disabled={savingId === o.id}
                onChange={(e) => void changeStatus(o, e.target.value as OrderStatus)}
              >
                {ORDER_STATUSES.map((s) => (
                  <option key={s} value={s}>
                    {t(statusKey(s))}
                  </option>
                ))}
              </select>
            </label>
          </li>
        ))}
      </ul>
    </section>
  )
}

export default Orders
