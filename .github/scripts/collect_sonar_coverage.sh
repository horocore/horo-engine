#!/usr/bin/env bash
set -euo pipefail

workspace=${GITHUB_WORKSPACE:-$(git rev-parse --show-toplevel)}
build_dir="$workspace/build/sonar"
fastcov=${FASTCOV:-/tmp/horo-coverage/bin/fastcov}
python=${COVERAGE_PYTHON:-/tmp/horo-coverage/bin/python}

started=$SECONDS
"$fastcov" \
  --gcov gcov \
  --jobs "$(nproc)" \
  --process-gcno \
  --search-directory "$build_dir" \
  --include \
    "$workspace/include/" \
    "$workspace/src/" \
    "$workspace/apps/common/" \
    "$workspace/apps/horo-engine/" \
    "$workspace/apps/horo-package/" \
    "$workspace/apps/HoroEditor/app/ConfiguredEditorUpdateManifestSource.cpp" \
    "$workspace/apps/HoroEditor/app/EditorUserStateMigration.cpp" \
    "$workspace/apps/HoroEditor/app/ConfiguredEditorUpdateBackend.cpp" \
    "$workspace/apps/HoroEditor/app/RenderGraphInspectionPane.cpp" \
  --output "$build_dir/fastcov-coverage.json"

"$python" "$workspace/scripts/fastcov_sonar_coverage.py" \
  --input "$build_dir/fastcov-coverage.json" \
  --output "$build_dir/coverage.xml"

elapsed=$((SECONDS - started))
echo "Fastcov collection and conversion: ${elapsed}s."
if [[ -n ${GITHUB_OUTPUT:-} ]]; then
  echo "seconds=$elapsed" >> "$GITHUB_OUTPUT"
fi
if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
  echo "## C++ line coverage" >> "$GITHUB_STEP_SUMMARY"
  echo "Fastcov collection and conversion: ${elapsed}s." >> "$GITHUB_STEP_SUMMARY"
fi
