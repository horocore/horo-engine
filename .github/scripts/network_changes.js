const buildFiles = new Set([
  'CMakeLists.txt', 'CMakePresets.json', '.github/workflows/ci.yml',
  '.github/scripts/network_changes.js', 'tests/cmake/HoroCiSuites.cmake',
]);
const networkDirectories = [
  'cmake/', '.github/actions/', 'include/Horo/Network/',
  'src/runtime/networking/', 'tests/unit/runtime/networking/', 'tests/integration/network/',
];

function affectsNetwork(path) {
  return typeof path === 'string' && (buildFiles.has(path) || path.endsWith('/CMakeLists.txt')
    || networkDirectories.some(directory => path.startsWith(directory)));
}

/** Select optional checks from the event's commits, including renames. */
module.exports = async function networkChanged({ github, context, core }) {
  if (context.eventName === 'workflow_dispatch') return true;
  const pr = context.payload.pull_request;
  const base = pr ? pr.base.sha : context.payload.before;
  const head = pr ? pr.head.sha : context.sha;
  if (!base || /^0+$/.test(base)) return true;

  try {
    const { data } = await github.request('GET /repos/{owner}/{repo}/compare/{basehead}', {
      ...context.repo, basehead: `${base}...${head}`, per_page: 1,
    });
    // GitHub returns at most 300 files. Incomplete input must run the checks.
    if (!Array.isArray(data.files) || data.files.length >= 300) return true;
    return data.files.some(file => affectsNetwork(file.filename) || affectsNetwork(file.previous_filename));
  } catch {
    core.warning('Change comparison unavailable; running network checks.');
    return true;
  }
};
