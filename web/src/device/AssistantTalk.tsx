import { useI18n, type StringKey } from '../i18n'
import { useAssistantTalk, type TalkState } from './useAssistantTalk'

const STATE_LABEL: Record<TalkState, StringKey> = {
  idle: 'talk.idle',
  listening: 'talk.listening',
  processing: 'talk.processing',
  speaking: 'talk.speaking',
}

export default function AssistantTalk({ deviceId, onClose }: { deviceId: string; onClose: () => void }) {
  const { t } = useI18n()
  const talk = useAssistantTalk(deviceId)

  const close = () => {
    talk.stop()
    onClose()
  }

  return (
    <section className="card dp-talk" aria-live="polite">
      <div className="dp-section-head">
        <h3>{t('talk.open')}</h3>
        <button type="button" className="dp-icon-btn" onClick={close} aria-label={t('talk.close')} title={t('talk.close')}>
          ✕
        </button>
      </div>

      <div className={`dp-talk-ball dp-talk-ball--${talk.state}`} style={{ transform: `scale(${talk.state === 'listening' ? talk.scale : 1})` }}>
        <MicGlyph />
      </div>
      <p className="dp-talk-state" data-state={talk.state}>
        {t(STATE_LABEL[talk.state])}
      </p>
      {!talk.micAvailable && <p className="form-hint">{t('talk.insecure')}</p>}
      {talk.error && <p className="form-error">{talk.error}</p>}

      {talk.turns.length > 0 && (
        <ul className="discussions dp-talk-log">
          {talk.turns.slice(-4).map((turn, i) => (
            <li key={i} className="discussion-turn">
              {turn.you && (
                <p className="discussion-user">
                  <span className="sr-only">{t('talk.you')}: </span>
                  {turn.you}
                </p>
              )}
              <p className="discussion-assistant">{turn.synora}</p>
            </li>
          ))}
        </ul>
      )}

      {talk.state === 'idle' ? (
        <button type="button" className="btn-primary dp-talk-btn" onClick={talk.start} disabled={!talk.micAvailable}>
          {t('talk.start')}
        </button>
      ) : (
        <button type="button" className="dp-talk-btn" onClick={talk.stop}>
          {t('talk.stop')}
        </button>
      )}
    </section>
  )
}

function MicGlyph() {
  return (
    <svg viewBox="0 0 24 24" width="40" height="40" fill="currentColor" aria-hidden="true">
      <path d="M12 14a3 3 0 0 0 3-3V5a3 3 0 1 0-6 0v6a3 3 0 0 0 3 3zm5-3a5 5 0 0 1-10 0H5a7 7 0 0 0 6 6.92V21h2v-3.08A7 7 0 0 0 19 11h-2z" />
    </svg>
  )
}
