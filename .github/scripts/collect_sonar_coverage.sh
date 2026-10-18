#!/usr/bin/env bash
set -euo pipefail

workspace=${GITHUB_WORKSPACE:-$(git rev-parse --show-toplevel)}
workspace=$(cd -- "$workspace" && pwd -P)
build_dir="$workspace/build/sonar"
fastcov=${FASTCOV:-/tmp/horo-coverage/bin/fastcov}
python=${COVERAGE_PYTHON:-/tmp/horo-coverage/bin/python}

started=$SECONDS
stage=$(mktemp -d -- "$build_dir/coverage-batches.XXXXXX")
cleanup() {
  # Only the exact mktemp-owned build child is eligible, never the build/root.
  if [[ "$stage" == "$build_dir"/coverage-batches.* && -d "$stage" && ! -L "$stage" ]]; then
    rm -rf -- "$stage"
  fi
}
trap cleanup EXIT

"$python" "$workspace/scripts/collect_sonar_coverage.py" plan \
  --root "$workspace" --build-directory "$build_dir" --stage "$stage"
mapfile -d '' -t manifests < "$stage/batches.manifest"
includes=(
    "$workspace/include/"
    "$workspace/src/"
    "$workspace/apps/common/"
    "$workspace/apps/horo-engine/"
    "$workspace/apps/horo-package/"
    "$workspace/apps/HoroEditor/app/ConfiguredEditorUpdateManifestSource.cpp"
    "$workspace/apps/HoroEditor/app/EditorUserStateMigration.cpp"
    "$workspace/apps/HoroEditor/app/ConfiguredEditorUpdateBackend.cpp"
    "$workspace/apps/HoroEditor/app/RenderGraphInspectionPane.cpp"
    "$workspace/apps/HoroEditor/app/EditorSurfaceLifecycleQualification.cpp"
    "$workspace/apps/HoroEditor/app/EditorAudioPreviewHost.cpp"
    "$workspace/apps/HoroEditor/app/EditorAudioPreviewDecode.cpp"
    "$workspace/apps/HoroEditor/app/EditorAudioPreviewPreparation.cpp"
)
reports=()
for manifest in "${manifests[@]}"; do
  mapfile -d '' -t inputs < "$manifest"
  report="${manifest%.files}.json"
  echo "Fastcov batch ${#reports[@]}: ${#inputs[@]} inputs, at most two workers."
  "$fastcov" --gcov gcov --jobs 2 --process-gcno --search-directory "$build_dir" \
    --gcda-files "${inputs[@]}" --include "${includes[@]}" --output "$report"
  [[ -s "$report" ]] || { echo "Missing fastcov batch report" >&2; exit 2; }
  reports+=("$report")
done
# Execution authority stays in the existing CI host. The data helper receives
# neither an executable argument nor a process-launch capability.
"$fastcov" --jobs 2 --add-tracefile "${reports[@]}" --output "$stage/fastcov-coverage.json"
"$python" "$workspace/scripts/collect_sonar_coverage.py" publish \
  --root "$workspace" --build-directory "$build_dir" --stage "$stage"

elapsed=$((SECONDS - started))
echo "Fastcov collection and conversion: ${elapsed}s."
if [[ -n ${GITHUB_OUTPUT:-} ]]; then
  echo "seconds=$elapsed" >> "$GITHUB_OUTPUT"
fi
if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
  echo "## C++ line coverage" >> "$GITHUB_STEP_SUMMARY"
  echo "Fastcov collection and conversion: ${elapsed}s." >> "$GITHUB_STEP_SUMMARY"
fi
