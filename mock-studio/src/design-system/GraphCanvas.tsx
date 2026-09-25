import './graphCanvas.css';

export type GraphNode = { title: string; summary: string; x: number; y: number };

export function GraphCanvas({ nodes, edges, selected, onSelect, width = 800, height = 340 }: { nodes: GraphNode[]; edges: string[]; selected?: string; onSelect?: (name: string) => void; width?: number; height?: number }) {
  return <div className="graph-canvas-scroll"><div className="graph-canvas-design" style={{ width, height }}>
    <svg viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" aria-hidden="true">{edges.map((path, index) => <path key={index} d={path} fill="none" stroke="var(--mock-accent)" strokeWidth="1.5" opacity=".6" />)}</svg>
    {nodes.map(node => <button key={node.title} type="button" className={selected === node.title ? 'graph-canvas-node selected' : 'graph-canvas-node'} style={{ left: node.x, top: node.y }} onClick={() => onSelect?.(node.title)}><strong>{node.title}</strong><small>{node.summary}</small></button>)}
  </div></div>;
}
