import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { createElement } from 'react';
import { renderToString } from 'react-dom/server';
import { createServer } from 'vite';

const root = new URL('../', import.meta.url).pathname;
const catalog = JSON.parse(await readFile(join(root, 'src/catalog.json'), 'utf8'));
const server = await createServer({ root, server: { middlewareMode: true }, appType: 'custom', logLevel: 'error' });
try {
  const { NativeScreen, nativeScreenIds } = await server.ssrLoadModule('/src/screens/registry.tsx');
  const known = new Set(catalog.map(item => item.id));
  if (nativeScreenIds.size !== known.size) throw new Error(`Catalog has ${known.size} designs but ${nativeScreenIds.size} native screens are registered`);
  for (const id of known) if (!nativeScreenIds.has(id)) throw new Error(`Catalog design has no native screen: ${id}`);
  for (const id of nativeScreenIds) {
    if (!known.has(id)) throw new Error(`Native screen has no catalog entry: ${id}`);
    const markup = renderToString(createElement(NativeScreen, { designId: id, openDesign() {}, navigate() {}, closeModal() {} }));
    if (!markup.trim()) throw new Error(`Native screen rendered no markup: ${id}`);
  }
  console.log(`Rendered ${nativeScreenIds.size} native designs without runtime errors.`);
} finally {
  await server.close();
}
