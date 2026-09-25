type IconName = 'panel' | 'scene' | 'search' | 'settings' | 'terminal' | 'plus' | 'filter' | 'sort' | 'more' | 'box' | 'eye' | 'lock' | 'grid' | 'select' | 'move' | 'rotate' | 'scale' | 'frame' | 'chevron' | 'folder';

export function WorkspaceIcon({ name, size = 18 }: { name: IconName; size?: number }) {
  const common = { fill: 'none', stroke: 'currentColor', strokeWidth: 1.8, strokeLinecap: 'round' as const, strokeLinejoin: 'round' as const };
  const shapes: Record<IconName, React.ReactNode> = {
    panel: <><rect x="4" y="4" width="16" height="16" rx="1" /><path d="M9 4v16" /></>,
    scene: <><rect x="4" y="4" width="16" height="16" rx="1" /><path d="M7 7h10v10H7z" /></>,
    search: <><circle cx="10.5" cy="10.5" r="5.5" /><path d="m15 15 4.5 4.5" /></>,
    settings: <><circle cx="12" cy="12" r="3" /><path d="M12 2v2m0 16v2M2 12h2m16 0h2M4.9 4.9l1.4 1.4m11.4 11.4 1.4 1.4M19.1 4.9l-1.4 1.4M6.3 17.7l-1.4 1.4" /></>,
    terminal: <><rect x="2.5" y="3" width="19" height="18" rx="2" /><path d="m6 9 3 3-3 3m5 0h6" /></>,
    plus: <path d="M12 5v14M5 12h14" />,
    filter: <path d="M4 5h16l-6.4 7v5l-3.2 2v-7z" />,
    sort: <><path d="M5 6h10M5 11h7M5 16h4m9-7v10m-3-3 3 3 3-3" /></>,
    more: <><circle cx="12" cy="5" r="1" fill="currentColor" stroke="none" /><circle cx="12" cy="12" r="1" fill="currentColor" stroke="none" /><circle cx="12" cy="19" r="1" fill="currentColor" stroke="none" /></>,
    box: <><path d="M5 5h14v14H5zM8 8h8v8H8z" /></>,
    eye: <><path d="M2 12s3.5-5 10-5 10 5 10 5-3.5 5-10 5-10-5-10-5z" /><circle cx="12" cy="12" r="2" /></>,
    lock: <><rect x="5" y="10" width="14" height="11" rx="2" /><path d="M8 10V7a4 4 0 0 1 8 0v3" /></>,
    grid: <><rect x="4" y="4" width="6" height="6" /><rect x="14" y="4" width="6" height="6" /><rect x="4" y="14" width="6" height="6" /><rect x="14" y="14" width="6" height="6" /></>,
    select: <path d="M5 3v17l5-5 4 6 2-1-4-6 7-1z" fill="currentColor" stroke="none" />,
    move: <><path d="M12 2v20M2 12h20m-13-7 3-3 3 3m-6 14 3 3 3-3M5 9l-3 3 3 3m14-6 3 3-3 3" /></>,
    rotate: <><path d="M19 8V3m0 5h-5M19 8a8 8 0 1 0 1 7" /></>,
    scale: <><path d="M5 19h14V5M10 14 20 4m-6 0h6v6" /></>,
    frame: <path d="M9 4H4v5m11-5h5v5M4 15v5h5m11-5v5h-5M9 9h6v6H9z" />,
    chevron: <path d="m6 9 6 6 6-6" />,
    folder: <path d="M3 6.5a2 2 0 0 1 2-2h5l2 2.5h7a2 2 0 0 1 2 2v9.5a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />,
  };
  return <svg aria-hidden="true" width={size} height={size} viewBox="0 0 24 24" {...common}>{shapes[name]}</svg>;
}
