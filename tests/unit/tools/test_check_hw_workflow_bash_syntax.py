"""Hardware CI workflows must be shell-parseable (issue #60, F-37).

Dispatch 26 (run 35935628647) never executed: a comment added inside
the single-quoted `bash -lc '...'` docker payload contained an
apostrophe ("suite's"), which terminated the string early; the stray
quote then swallowed the host section up to another apostrophe in the
F-25 comment, and bash died parsing `(the failure` as code -
`syntax error near '('` at the step script's line 55, exit 2, zero
container work. YAML validation cannot see this: it is a bash-level
fault. Test 1 runs `bash -n` over every workflow `run:` block (the
primary guard - it caught the dispatch-26 block). Test 2 is the
attributable check: inside a `bash -lc '...'` payload, every apostrophe
must belong to the standard `\"'\"'` embedding idiom, never be a raw
quote that truncates the payload.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

REPO_ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = sorted((REPO_ROOT / ".github" / "workflows").glob("*.yml"))
# The POSIX idiom for embedding a literal ' inside a '...' string:
# close-quote + double-quoted quote + re-open. YAML text form: '"'"'
QUOTE_IDIOM = "'\"'\"'"


def _run_blocks():
    """Yield (workflow_job_label, index, script) for every run: block."""
    pytest.importorskip("yaml")
    assert WORKFLOWS, "no workflow files found"
    for wf in WORKFLOWS:
        data = yaml.safe_load(wf.read_text(encoding="utf-8"))
        for job_name, job in (data.get("jobs") or {}).items():
            for index, step in enumerate(job.get("steps") or []):
                script = step.get("run")
                if isinstance(script, str) and script.strip():
                    yield ("%s:%s" % (wf.name, job_name), index, script)


def _strip_gh_expressions(script):
    # ${{ ... }} is GitHub templating, expanded before bash sees it.
    return re.sub(r"\$\{\{.*?\}\}", "GH_EXPR", script, flags=re.S)


@pytest.mark.parametrize(
    "job_label,index,script",
    list(_run_blocks()),
    ids=lambda value: str(value).splitlines()[0][:80]
    if isinstance(value, str) else str(value),
)
def test_workflow_run_block_is_bash_parseable(job_label, index, script):
    """Every run: block parses with bash -n (F-37; dispatch 26 class)."""
    bash = shutil.which("bash")
    if bash is None:
        pytest.skip("bash not available")
    result = subprocess.run(
        [bash, "-n"], input=_strip_gh_expressions(script),
        capture_output=True, text=True)
    assert result.returncode == 0, (
        "bash -n rejected %s step[%d] (would die before executing, as at "
        "dispatch 26). Note: the T2 docker payload is single-quoted - "
        "never put a raw apostrophe inside it.\n%s"
        % (job_label, index, result.stderr))


def _naive_payload_spans(text):
    """Raw spans from bash -lc ' to the next ' 2>&1 (both jobs)."""
    spans, pos = [], 0
    while True:
        start = text.find("bash -lc '", pos)
        if start < 0:
            return spans
        start += len("bash -lc '")
        end = text.find("' 2>&1", start)
        if end < 0:
            end = len(text)
        spans.append(text[start:end])
        pos = end + 1


def test_single_quoted_bash_lc_payloads_have_no_raw_apostrophes():
    """Attributable guard: raw ' inside bash -lc '...' truncates it (F-37)."""
    offenders = []
    for wf in WORKFLOWS:
        text = wf.read_text(encoding="utf-8")
        for span in _naive_payload_spans(text):
            remainder = span.replace(QUOTE_IDIOM, "")
            for line in remainder.splitlines():
                if "'" in line:
                    offenders.append("%s: %s" % (wf.name, line.strip()))
    assert not offenders, (
        "raw apostrophe inside a single-quoted bash -lc payload "
        "(dispatch 26 failure class - it truncates the string and the "
        "step dies with a syntax error before executing): %s" % offenders)
