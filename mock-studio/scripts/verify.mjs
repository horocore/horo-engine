import { readdir, readFile, stat } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';

const studio = new URL('../', import.meta.url).pathname;
const repository = resolve(studio, '..');
const catalog = JSON.parse(await readFile(join(studio, 'src/catalog.json'), 'utf8'));
const errors = [];

async function filesUnder(root) {
  const files = [];
  for (const entry of await readdir(root, { withFileTypes: true })) {
    const path = join(root, entry.name);
    if (entry.isDirectory()) files.push(...await filesUnder(path));
    else files.push(path);
  }
  return files;
}

async function exists(path) {
  try { return (await stat(path)).isFile(); } catch { return false; }
}

for (const path of await filesUnder(join(repository, 'docs/architecture'))) {
  if (!path.endsWith('.md')) errors.push(`Non-architecture file remains in docs/architecture: ${path}`);
}

const ids = new Set();
for (const item of catalog) {
  if (!item.id || !item.title || !item.group) errors.push(`Incomplete catalog entry: ${JSON.stringify(item)}`);
  if (ids.has(item.id)) errors.push(`Duplicate design ID: ${item.id}`);
  ids.add(item.id);
  if ('archive' in item) errors.push(`Archive field remains in catalog: ${item.id}`);
}

for (const path of await filesUnder(join(studio, 'public'))) {
  if (path.endsWith('.html')) errors.push(`HTML mock remains in public: ${path}`);
}

const designIndex = await readFile(join(studio, 'designs.md'), 'utf8');

for (const path of await filesUnder(join(repository, 'docs'))) {
  if (!path.endsWith('.md')) continue;
  const source = await readFile(path, 'utf8');
  for (const match of source.matchAll(/\]\(([^)]+\.html(?:#[^)]*)?)\)/g)) {
    const target = match[1].split('#')[0];
    if (!target.includes('://') && !await exists(resolve(dirname(path), target))) errors.push(`Broken HTML reference: ${path} -> ${target}`);
  }
  for (const match of source.matchAll(/\]\(([^)]*mock-studio\/designs\.md#([^)]*))\)/g)) {
    const [reference, anchor] = [match[1], match[2]];
    if (!await exists(resolve(dirname(path), reference.split('#')[0]))) errors.push(`Broken design index path: ${path} -> ${reference}`);
    if (!designIndex.includes(`### ${anchor}\n`)) errors.push(`Broken design anchor: ${path} -> ${anchor}`);
  }
}

if (errors.length) {
  console.error(errors.join('\n'));
  process.exitCode = 1;
} else {
  console.log(`Verified ${catalog.length} native design entries, Markdown-only architecture docs, and local design links.`);
}
