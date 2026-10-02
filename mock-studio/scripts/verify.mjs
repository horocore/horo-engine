import { readdir, readFile, realpath, stat } from 'node:fs/promises';
import { dirname, join, resolve, sep } from 'node:path';

const studio = new URL('../', import.meta.url).pathname;
const repository = resolve(studio, '..');
// The catalog is a fixed path relative to this script.
// eslint-disable-next-line security/detect-non-literal-fs-filename
const catalog = JSON.parse(await readFile(join(studio, 'src/catalog.json'), 'utf8'));
const errors = [];

async function filesUnder(root) {
  const files = [];
  // Every recursive root is derived from a fixed repository directory.
  // eslint-disable-next-line security/detect-non-literal-fs-filename
  for (const entry of await readdir(root, { withFileTypes: true })) {
    if (entry.isSymbolicLink()) continue;
    const path = join(root, entry.name);
    if (entry.isDirectory()) files.push(...await filesUnder(path));
    else files.push(path);
  }
  return files;
}

async function exists(path) {
  try {
    // Reject links that resolve outside the repository before checking a document target.
    // eslint-disable-next-line security/detect-non-literal-fs-filename
    const resolved = await realpath(path);
    if (resolved !== repository && !resolved.startsWith(repository + sep)) return false;
    // eslint-disable-next-line security/detect-non-literal-fs-filename
    return (await stat(resolved)).isFile();
  } catch (error) {
    if (error && typeof error === 'object' && 'code' in error && error.code === 'ENOENT') return false;
    throw error;
  }
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

// The design index is a fixed path relative to this script.
// eslint-disable-next-line security/detect-non-literal-fs-filename
const designIndex = await readFile(join(studio, 'designs.md'), 'utf8');

for (const path of await filesUnder(join(repository, 'docs'))) {
  if (!path.endsWith('.md')) continue;
  // filesUnder excludes symlinks and only traverses the repository docs tree.
  // eslint-disable-next-line security/detect-non-literal-fs-filename
  const source = await readFile(path, 'utf8');
  for (const match of source.matchAll(/\]\(([^)]*)\)/g)) {
    const reference = match[1];
    const [target, anchor] = reference.split('#', 2);
    if (target.endsWith('.html') && !target.includes('://') && !await exists(resolve(dirname(path), target))) {
      errors.push(`Broken HTML reference: ${path} -> ${target}`);
    }
    if (target.endsWith('mock-studio/designs.md') && anchor) {
      if (!await exists(resolve(dirname(path), target))) errors.push(`Broken design index path: ${path} -> ${reference}`);
      if (!designIndex.includes(`### ${anchor}\n`)) errors.push(`Broken design anchor: ${path} -> ${anchor}`);
    }
  }
}

if (errors.length) {
  console.error(errors.join('\n'));
  process.exitCode = 1;
} else {
  console.log(`Verified ${catalog.length} native design entries, Markdown-only architecture docs, and local design links.`);
}
