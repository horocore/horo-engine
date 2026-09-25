import { useEffect, useState } from 'react';
import './loading.css';

const steps = [
  { threshold: 0, label: 'Initializing asset database…' },
  { threshold: 15, label: 'Parsing project manifests…' },
  { threshold: 30, label: 'Resolving dependencies…' },
  { threshold: 55, label: 'Compiling shaders…' },
  { threshold: 85, label: 'Loading default scene…' },
  { threshold: 98, label: 'Finalizing workspace…' },
];

export function LoadingDesign({ onOpenWorkspace }: { onOpenWorkspace: () => void }) {
  const [progress, setProgress] = useState(0);
  const [state, setState] = useState<'loading' | 'cancelled' | 'ready'>('loading');

  useEffect(() => {
    if (state !== 'loading') return;
    const timer = window.setInterval(() => {
      setProgress(value => {
        const next = Math.min(100, value + 2);
        if (next === 100) window.setTimeout(() => setState('ready'), 0);
        return next;
      });
    }, 140);
    return () => window.clearInterval(timer);
  }, [state]);

  const current = [...steps].reverse().find(step => progress >= step.threshold);
  const status = state === 'cancelled' ? 'Opening cancelled' : state === 'ready' ? 'Ready' : current?.label;

  return <div className="loading-design"><section className="loading-card" aria-label="Project loading progress">
    <h1>Opening ‘SecondGame’</h1>
    <div className="loading-meta"><span role="status" aria-live="polite">{status}</span><strong>{Math.floor(progress)}%</strong></div>
    <div className="loading-track" role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={progress} aria-label="Opening project"><i style={{ width: `${progress}%` }} /></div>
    <footer>
      {state === 'loading' && <button type="button" onClick={() => setState('cancelled')}>Cancel</button>}
      {state === 'cancelled' && <button type="button" onClick={() => { setProgress(0); setState('loading'); }}>Try again</button>}
      {state === 'ready' && <button type="button" className="primary" onClick={onOpenWorkspace}>Open workspace</button>}
    </footer>
  </section></div>;
}
