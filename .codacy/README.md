# Codacy project configuration

`codacy.config.json` is a snapshot of the repository's Codacy settings for local
analysis. It preserves the existing tools, security patterns, complexity
thresholds, and cloud exclusions. Clang-Tidy is a separate client-side tool and
is not represented by this Analysis CLI snapshot.

The project-specific adjustments are:

- Disable Agentlinter's `clarity_escape-hatch-missing`: the repository contract
  deliberately contains mandatory ownership, dependency, and delivery rules.
- Disable Stylelint's `color-no-hex`: the documentation UI uses hexadecimal theme
  colors and fallback values throughout.
- Disable Stylelint's `rule-empty-line-before`, `comment-empty-line-before`, and
  `at-rule-empty-line-before`: the documentation stylesheets group related rules
  compactly. Keep the other CSS checks enabled.
- Use the root `bandit.yml` to exempt `tests/python/test_*.py` from B101 only.
  Production assertions and all other security checks in tests remain enabled.

The Bandit exception uses its documented
[per-file assert configuration](https://bandit.readthedocs.io/en/latest/plugins/b101_assert_used.html).
It does not exclude test files from analysis.

## Local checks

```bash
codacy-analysis analyze --tool Stylelint --tool Agentlinter --tool markdownlint --tool ESLint8
codacy-analysis analyze --tool Bandit --files 'scripts/**/*.py' 'tests/python/test_*.py'
```

An exit status of 1 can indicate reported findings; inspect the tool execution
status and errors before treating it as an analyzer failure.

## Cloud settings

The five managed pattern changes were submitted individually to Codacy. The
snapshot is not automatically synchronized with cloud settings and should be
refreshed before any future bulk import. No tools were switched or disabled.

Codacy already uses configuration-file mode for Bandit. The `bandit.yml` change
takes effect in cloud analysis when the analyzed branch includes it. Merge this
configuration through the normal delivery process to apply it to the default branch.


Keep generated analysis summaries and raw output outside the source tree. Record
validation results and limitations in the pull request description.
