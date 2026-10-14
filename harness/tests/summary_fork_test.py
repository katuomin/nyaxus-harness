#!/usr/bin/env python3
"""e2e coverage for auxiliary summary forks and thinking input policy.

The test starts independent local primary and auxiliary OpenAI-compatible
servers, so no live model or external network is involved.
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DAEMON = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    ROOT, "harness", "nyaxus_daemon.exe")

# Built from fragments so the literal tag survives tooling that strips
# well-formed tags from generated text.
THINK = "<" + "think>"
UNTHINK = "</" + "think>"


class MockProvider:
    def __init__(self, kind, delay=0.0):
        self.kind = kind
        self.delay = delay
        self.requests = []
        self.lock = threading.Lock()
        self.fail_next = 0
        self.aux_mode = "valid"
        self.port = reserve_port()
        self.server = ThreadingHTTPServer(
            ("127.0.0.1", self.port), make_handler(self))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return "http://127.0.0.1:%d/v1" % self.port

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)

    def snapshot_requests(self):
        with self.lock:
            return list(self.requests)


def reserve_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def make_handler(provider):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, _format, *_args):
            return

        def do_GET(self):
            if self.path != "/v1/models":
                self.send_error(404)
                return
            payload = json.dumps({
                "data": [{"id": "mock-%s" % provider.kind,
                          "meta": {"n_ctx": 4096}}]
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)

        def do_POST(self):
            if self.path != "/v1/chat/completions":
                self.send_error(404)
                return
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length)
            try:
                parsed = json.loads(raw.decode())
            except (UnicodeDecodeError, ValueError):
                self._json_error(400, "invalid json")
                return

            with provider.lock:
                provider.requests.append(parsed)
                fail = provider.fail_next > 0
                if fail:
                    provider.fail_next -= 1
            if fail:
                self._json_error(503, "auxiliary mock failure")
                return
            if provider.delay > 0:
                time.sleep(provider.delay)

            messages = parsed.get("messages", [])
            if provider.kind == "auxiliary":
                text = auxiliary_text(provider, messages)
                reasoning = "auxiliary reasoning must not become summary text"
            else:
                users = [m for m in messages if m.get("role") == "user"]
                last_user = users[-1].get("content", "") if users else ""
                if last_user == "after compact":
                    text = "primary saw the injected compact context"
                else:
                    text = (THINK + "inline hidden reasoning" + UNTHINK
                            + " visible source answer with reference "
                            + "https://example.com/docs/notes and file "
                            + "notes/plan.md, see fork/branch for details")
                reasoning = "primary separated diagnostic reasoning"
            self._sse(reasoning, text)

        def _json_error(self, status, message):
            payload = json.dumps({"error": {"message": message}}).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)

        def _sse(self, reasoning, text):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()

            def send(payload):
                data = ("data: " + json.dumps(payload) + "\n\n").encode()
                self.wfile.write(data)
                self.wfile.flush()

            send({
                "id": "mock-reasoning",
                "object": "chat.completion.chunk",
                "choices": [{"index": 0,
                             "delta": {"reasoning_content": reasoning}}],
            })
            send({
                "id": "mock-text",
                "object": "chat.completion.chunk",
                "choices": [{"index": 0, "delta": {"content": text}}],
            })
            send({
                "id": "mock-usage",
                "object": "chat.completion.chunk",
                "choices": [],
                "usage": {"prompt_tokens": 20, "completion_tokens": 8,
                           "total_tokens": 28},
            })
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()

    return Handler


CITATION_RE = re.compile(r"source message id: (\d+)")


def auxiliary_text(provider, messages):
    """Deterministic auxiliary output for the mock.

    The default mode emits the primary contract: a sectioned markdown
    checkpoint (wrapped in a fence, as small models often do). The JSON-based
    modes exercise the tolerant fallback path, the string-field relaxation,
    the bounded repair retry, and the provenance checks.
    """
    users = [m for m in messages if m.get("role") == "user"]
    last_user = users[-1].get("content", "") if users else ""
    first_user = users[0].get("content", "") if users else ""
    # A repair call carries the rejected output as an assistant message; the
    # source ids then live in the original source document, not the last user
    # message.
    has_assistant = any(m.get("role") == "assistant" for m in messages)
    if "BEGIN PRE-VALIDATED CHECKPOINT" in last_user:
        # Re-compression: the checkpoint's citations come first, then the new
        # turns' ids in order. Cite the first checkpoint id and the last new
        # turn id - both are valid provenance for the merge.
        ids = CITATION_RE.findall(last_user)
        if ids:
            old_id, new_id = ids[0], ids[-1]
            return (
                "## Goal\n"
                "Continue the selected conversation [source message id: %s]\n"
                "\n"
                "## Decisions\n"
                "- retain the selected conversation [source message id: %s]\n"
                "- acknowledged the new turns [source message id: %s]\n"
                "\n"
                "## Constraints\n"
                "- cite source ids [source message id: %s]\n"
                "\n"
                "## Unresolved Tasks\n"
                "- (none)\n"
                "\n"
                "## Relevant Artifacts\n"
                "- (none)\n"
                "\n"
                "## Next Steps\n"
                "- respond to the next user turn\n"
            ) % (old_id, old_id, new_id, old_id)
    merge_request = ""
    for m in reversed(messages):
        if m.get("role") == "user" and \
                "BEGIN PRE-VALIDATED COMPACT CONTEXT SEGMENTS" in m.get("content", ""):
            merge_request = m.get("content", "")
            break
    if merge_request:
        # Merge: the contract preserves every citation the input segments
        # carried, so cite each distinct id once.
        distinct = []
        for cid in CITATION_RE.findall(merge_request):
            if cid not in distinct:
                distinct.append(cid)
        if provider.aux_mode in ("merge_drops_citation", "merge_always_drops"):
            # The first merge attempt drops the most recent segment's
            # citation; merge_always_drops keeps dropping it on the repair
            # call, forcing the mechanical segment merge.
            if (not has_assistant) or provider.aux_mode == "merge_always_drops":
                if len(distinct) > 1:
                    distinct = distinct[:-1]
        if not distinct:
            return "no source message ids were provided"
        cites = " ".join("[source message id: %s]" % cid for cid in distinct)
        return (
            "## Goal\n"
            "Continue the selected conversation %s\n"
            "\n"
            "## Decisions\n"
            "- retain the selected conversation %s\n"
            "\n"
            "## Constraints\n"
            "- cite source ids %s\n"
            "\n"
            "## Unresolved Tasks\n"
            "- continue the requested work %s\n"
            "\n"
            "## Relevant Artifacts\n"
            "- (none)\n"
            "\n"
            "## Next Steps\n"
            "- respond to the next user turn\n"
        ) % (cites, cites, cites, cites)
    if provider.aux_mode == "garbage":
        return "I cannot reduce this conversation to the required JSON object."
    if provider.aux_mode == "bad_citation":
        return json.dumps({
            "decisions": ["keep the plan [source message id: 999999]"],
            "constraints": [],
            "unresolved_tasks": [],
            "relevant_artifacts": [],
        })
    ids = CITATION_RE.findall(last_user) or CITATION_RE.findall(first_user)
    if not ids:
        return "no source message ids were provided"
    first = ids[0]

    def valid_doc():
        return {
            "decisions": ["retain the selected conversation [source message id: %s]" % first],
            "constraints": ["cite source ids [source message id: %s]" % first],
            "unresolved_tasks": ["continue the requested work [source message id: %s]" % first],
            "relevant_artifacts": [],
        }

    def valid_checkpoint():
        return (
            "## Goal\n"
            "Continue the selected conversation [source message id: %s]\n"
            "\n"
            "## Decisions\n"
            "- retain the selected conversation [source message id: %s]\n"
            "\n"
            "## Constraints\n"
            "- cite source ids [source message id: %s]\n"
            "\n"
            "## Unresolved Tasks\n"
            "- continue the requested work [source message id: %s]\n"
            "\n"
            "## Relevant Artifacts\n"
            "- (none)\n"
            "\n"
            "## Next Steps\n"
            "- respond to the next user turn\n"
        ) % (first, first, first, first)

    if provider.aux_mode == "truncated_then_valid":
        if not has_assistant:
            # First attempt: a truncated object that fails to parse.
            return ('{"decisions": ["retain the selected conversation '
                    '[source message id: %s]' % first)
        # Repair attempt: a complete, valid document.
        return json.dumps(valid_doc())
    if provider.aux_mode == "string_field":
        doc = valid_doc()
        doc["decisions"] = doc["decisions"][0]
        return json.dumps(doc)
    if provider.aux_mode == "uncited":
        # The 2B omits inline citations; the details are still present.
        return json.dumps({
            "decisions": ["retain the selected conversation"],
            "constraints": ["keep the summary small"],
            "unresolved_tasks": [],
            "relevant_artifacts": [],
        })
    if provider.aux_mode == "object_claims":
        # The 2B returns a nested array of objects carrying the text and a
        # source id as separate keys.
        return json.dumps({
            "decisions": ["retain the selected conversation"],
            "constraints": [],
            "unresolved_tasks": [],
            "relevant_artifacts": [[
                {"source message id": first, "role": "user",
                 "text": "the selected conversation"},
            ]],
        })
    if provider.aux_mode == "object_bad_id":
        return json.dumps({
            "decisions": [],
            "constraints": [],
            "unresolved_tasks": [],
            "relevant_artifacts": [[
                {"source message id": 999999, "role": "user",
                 "text": "a fabricated artifact"},
            ]],
        })
    return "```markdown\n" + valid_checkpoint() + "\n```"


def call(base, method, path, body=None, timeout=5):
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(
        base + path, method=method, data=data,
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = response.read().decode() or "{}"
            return response.status, json.loads(payload)
    except urllib.error.HTTPError as error:
        try:
            return error.code, json.loads(error.read().decode() or "{}")
        except ValueError:
            return error.code, {}


def snapshot(base, sid):
    status, body = call(base, "GET", "/sessions/" + sid)
    if status != 200:
        raise AssertionError("snapshot failed: %s %s" % (status, body))
    return body


def wait_for_snapshot(base, sid, predicate, description, timeout=15):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = snapshot(base, sid)
        if predicate(last):
            return last
        time.sleep(0.1)
    raise AssertionError("timed out waiting for %s: %s" % (description, last))


def wait_run(base, sid, run_id, timeout=15):
    def done(snap):
        return any(event.get("run_id") == run_id and
                   event.get("event_type") in ("run.completed", "run.failed",
                                                 "run.cancelled")
                   for event in snap.get("events", []))

    return wait_for_snapshot(base, sid, done, "run " + run_id, timeout)


def start_daemon(daemon, port, primary, auxiliary, include_reasoning,
                 extra_args=None, extra_env=None):
    workdir = tempfile.mkdtemp(prefix="nyaxus-summary-fork-")
    log = open(os.path.join(workdir, "daemon.log"), "wb")
    env = os.environ.copy()
    env["NYAXUS_SUMMARY_INCLUDE_REASONING"] = "1" if include_reasoning else "0"
    if extra_env:
        env.update(extra_env)
    command = [
        os.path.abspath(daemon), "--port", str(port), "--provider", "openai",
        "--llama", primary.url,
        "--model", "mock-primary", "--auxiliary", auxiliary.url,
        "--auxiliary-model", "mock-auxiliary",
    ]
    if extra_args:
        command.extend(extra_args)
    process = subprocess.Popen(command, cwd=workdir, env=env,
                               stdout=log, stderr=subprocess.STDOUT)
    base = "http://127.0.0.1:%d/api/v1" % port
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            status, _ = call(base, "GET", "/health", timeout=1)
            if status == 200:
                return process, base, workdir, log
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(0.1)
    log.flush()
    log.close()
    try:
        output = open(os.path.join(workdir, "daemon.log"), "rb").read().decode(errors="replace")
    except OSError:
        output = ""
    process.terminate()
    process.wait(timeout=5)
    shutil.rmtree(workdir, ignore_errors=True)
    raise AssertionError("daemon did not start: %s" % output)


def stop_daemon(process, workdir, log):
    try:
        process.terminate()
        process.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        try:
            process.kill()
            process.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            pass
    try:
        log.close()
    except OSError:
        pass
    shutil.rmtree(workdir, ignore_errors=True)


def create_source(base, message):
    status, created = call(base, "POST", "/sessions", {})
    assert status == 200, (status, created)
    sid = created["id"]
    status, started = call(
        base, "POST", "/sessions/%s/runs" % sid,
        {"session_id": sid, "message": message})
    assert status == 200 and started.get("run_id"), (status, started)
    wait_run(base, sid, started["run_id"])
    return sid, started["run_id"]


def main():
    daemon = os.path.abspath(DAEMON)
    if not os.path.exists(daemon):
        print("daemon binary not found: %s" % daemon, file=sys.stderr)
        return 1

    primary = MockProvider("primary")
    auxiliary = MockProvider("auxiliary", delay=1.0)
    first = None
    second = None
    third = None
    fourth = None
    try:
        first = start_daemon(daemon, reserve_port(), primary, auxiliary, False)
        process, base, workdir, log = first

        source_sid, _ = create_source(base, "source turn")
        source = snapshot(base, source_sid)
        assistant = [m for m in source["messages"] if m["role"] == "assistant"][0]
        assert "<think>inline hidden reasoning</think>" in assistant["content"], assistant
        assert assistant.get("reasoning") == "primary separated diagnostic reasoning", assistant

        status, followup = call(
            base, "POST", "/sessions/%s/runs" % source_sid,
            {"session_id": source_sid, "message": "followup"})
        assert status == 200, (status, followup)
        wait_run(base, source_sid, followup["run_id"])
        followup_requests = primary.snapshot_requests()
        followup_request = next(
            request for request in reversed(followup_requests)
            if any(m.get("role") == "user" and m.get("content") == "followup"
                   for m in request.get("messages", [])))
        serialized_followup = json.dumps(followup_request)
        assert "inline hidden reasoning" not in serialized_followup, serialized_followup
        assert "visible source answer" in serialized_followup, serialized_followup
        source = snapshot(base, source_sid)
        assert "<think>inline hidden reasoning</think>" in \
            [m for m in source["messages"] if m["role"] == "assistant"][0]["content"]

        before_aux = len(auxiliary.snapshot_requests())
        started_at = time.time()
        status, forked = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        elapsed = time.time() - started_at
        assert status == 200, (status, forked)
        assert elapsed < 0.8, elapsed
        assert forked["context_mode"] == "summary", forked
        assert forked["context_status"] == "preparing", forked
        assert forked["context_error"] == "", forked
        fork_sid = forked["id"]
        assert forked["message_count"] == 0, forked

        status, queued = call(
            base, "POST", "/sessions/%s/runs" % fork_sid,
            {"session_id": fork_sid, "message": "after compact"})
        assert status == 200 and queued.get("run_id"), (status, queued)
        ready = wait_for_snapshot(
            base, fork_sid,
            lambda snap: snap.get("context_status") == "ready",
            "summary context readiness")
        assert ready["context_mode"] == "summary", ready
        ready_summary = json.loads(ready["context_summary"])
        assert set(ready_summary) == {"goal", "decisions", "constraints",
                                      "unresolved_tasks",
                                      "relevant_artifacts",
                                      "next_steps",
                                      "latest_user_request", "anchors"}, ready
        first_source_id = source["messages"][0]["id"]
        assert ("[source message id: %s]" % first_source_id) in \
            ready_summary["decisions"][0], ready_summary
        # Harness-populated provenance: verbatim anchors and the latest user
        # request, independent of the auxiliary model's output.
        assert "https://example.com/docs/notes" in ready_summary["anchors"], \
            ready_summary
        assert "notes/plan.md" in ready_summary["anchors"], ready_summary
        # Slash-words without a file extension are not paths and must not
        # become anchors.
        assert "fork/branch" not in ready_summary["anchors"], ready_summary
        assert ready_summary["latest_user_request"] == "followup", \
            ready_summary
        assert any(event.get("event_type") == "context.preparing" and
                   event.get("run_id") == "context-" + fork_sid
                   for event in ready["events"]), ready["events"]
        assert any(event.get("event_type") == "context.completed" and
                   event.get("run_id") == "context-" + fork_sid
                   for event in ready["events"]), ready["events"]
        completed = wait_run(base, fork_sid, queued["run_id"])
        assert any(event.get("event_type") == "run.completed" and
                   event.get("run_id") == queued["run_id"]
                   for event in completed["events"]), completed["events"]
        summary_requests = auxiliary.snapshot_requests()[before_aux:]
        assert summary_requests, "auxiliary endpoint received no summary request"
        summary_request = summary_requests[0]
        summary_text = json.dumps(summary_request)
        assert "source message id" in summary_text, summary_text
        assert "primary separated diagnostic reasoning" not in summary_text, summary_text
        assert "inline hidden reasoning" not in summary_text, summary_text
        assert "auxiliary reasoning must not become summary text" not in \
            ready["context_summary"], ready["context_summary"]
        primary_after_summary = primary.snapshot_requests()
        queued_request = next(
            request for request in reversed(primary_after_summary)
            if any(m.get("role") == "user" and m.get("content") == "after compact"
                   for m in request.get("messages", [])))
        queued_messages = queued_request["messages"]
        compact = [m for m in queued_messages
                   if m.get("role") == "system" and
                   "[COMPACT CONTEXT]" in m.get("content", "")]
        assert compact and "retain the selected conversation" in \
            compact[0]["content"], queued_messages
        assert "## Goal" in compact[0]["content"], queued_messages
        assert "https://example.com/docs/notes" in compact[0]["content"], \
            queued_messages
        assert "followup" in compact[0]["content"], queued_messages

        # Provenance: a claim citing an unknown source id is rejected by the
        # auxiliary path, and the fork falls back to the deterministic
        # mechanical summary instead of hard-failing.
        auxiliary.aux_mode = "bad_citation"
        status, cite_fork = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, cite_fork)
        cite_sid = cite_fork["id"]
        status, cite_run = call(
            base, "POST", "/sessions/%s/runs" % cite_sid,
            {"session_id": cite_sid, "message": "citation fallback run"})
        assert status == 200, (status, cite_run)
        cite_ready = wait_for_snapshot(
            base, cite_sid,
            lambda snap: snap.get("context_status") == "ready",
            "provenance fallback readiness")
        cite_summary = json.loads(cite_ready["context_summary"])
        assert cite_summary["relevant_artifacts"], cite_ready
        assert "source message id" in json.dumps(
            cite_summary["relevant_artifacts"]), cite_ready
        assert cite_summary["latest_user_request"] == "followup", cite_ready
        assert any(event.get("event_type") == "context.fallback" and
                   event.get("run_id") == "context-" + cite_sid
                   for event in cite_ready["events"]), cite_ready["events"]
        cite_completed = wait_run(base, cite_sid, cite_run["run_id"])
        assert any(event.get("event_type") == "run.completed" and
                   event.get("run_id") == cite_run["run_id"]
                   for event in cite_completed["events"]), cite_completed["events"]

        # Schema: non-JSON output is rejected by the auxiliary path and the
        # fork falls back to the deterministic mechanical summary.
        auxiliary.aux_mode = "garbage"
        status, garbage_fork = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, garbage_fork)
        garbage_ready = wait_for_snapshot(
            base, garbage_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "schema fallback readiness")
        garbage_summary = json.loads(garbage_ready["context_summary"])
        assert garbage_summary["relevant_artifacts"], garbage_ready
        assert any(event.get("event_type") == "context.fallback" and
                   event.get("run_id") == "context-" + garbage_fork["id"]
                   for event in garbage_ready["events"]), garbage_ready["events"]

        # Relaxation: a bare string where an array is expected is accepted as
        # one claim, with no repair call.
        auxiliary.aux_mode = "string_field"
        string_before = len(auxiliary.snapshot_requests())
        status, string_fork = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, string_fork)
        string_ready = wait_for_snapshot(
            base, string_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "string-field summary readiness")
        string_summary = json.loads(string_ready["context_summary"])
        assert isinstance(string_summary["decisions"], list), string_ready
        assert string_summary["decisions"], string_ready
        assert len(auxiliary.snapshot_requests()) - string_before == 1, \
            len(auxiliary.snapshot_requests())

        # Repair: a truncated first attempt is fed back with the specific
        # error and corrected on a single bounded retry.
        auxiliary.aux_mode = "truncated_then_valid"
        repair_before = len(auxiliary.snapshot_requests())
        status, repair_fork = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, repair_fork)
        repair_ready = wait_for_snapshot(
            base, repair_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "repaired summary readiness")
        repair_summary = json.loads(repair_ready["context_summary"])
        assert repair_summary["decisions"], repair_ready
        repair_aux = auxiliary.snapshot_requests()[repair_before:]
        assert len(repair_aux) == 2, len(repair_aux)
        assert "rejected" in json.dumps(repair_aux[1]), repair_aux

        # Uncited claims: the 2B omits inline citations; the summary must
        # still be accepted when the details are present.
        auxiliary.aux_mode = "uncited"
        status, uncited_fork = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, uncited_fork)
        uncited_ready = wait_for_snapshot(
            base, uncited_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "uncited summary readiness")
        uncited_summary = json.loads(uncited_ready["context_summary"])
        assert uncited_summary["decisions"], uncited_ready

        # Object claims: the 2B returns a nested array of objects carrying the
        # text and a source id; the normalizer must accept it and keep the
        # citation valid.
        auxiliary.aux_mode = "object_claims"
        status, obj_fork = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, obj_fork)
        obj_ready = wait_for_snapshot(
            base, obj_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "object-claim summary readiness")
        obj_summary = json.loads(obj_ready["context_summary"])
        assert obj_summary["relevant_artifacts"], obj_ready
        assert "[source message id:" in obj_summary["relevant_artifacts"][0], \
            obj_ready

        # Object claims citing an unknown source id are rejected by the
        # auxiliary path (anti-hallucination preserved); the fork falls back
        # to the mechanical summary.
        auxiliary.aux_mode = "object_bad_id"
        status, obj_bad_fork = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, obj_bad_fork)
        obj_bad_ready = wait_for_snapshot(
            base, obj_bad_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "object-claim fallback readiness")
        obj_bad_summary = json.loads(obj_bad_ready["context_summary"])
        assert obj_bad_summary["relevant_artifacts"], obj_bad_ready
        assert any(event.get("event_type") == "context.fallback" and
                   event.get("run_id") == "context-" + obj_bad_fork["id"]
                   for event in obj_bad_ready["events"]), obj_bad_ready["events"]
        auxiliary.aux_mode = "valid"

        # ── re-compression: cooldown reuses the summary for a small delta ──
        status, rc_fork = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, rc_fork)
        rc_sid = rc_fork["id"]
        assert rc_fork["context_watermark"] == source["messages"][-1]["id"], rc_fork
        rc_ready = wait_for_snapshot(
            base, rc_sid,
            lambda snap: snap.get("context_status") == "ready",
            "re-compression fork readiness")
        first_summary = rc_ready["context_summary"]
        aux_before = len(auxiliary.snapshot_requests())
        status, rc_turn = call(
            base, "POST", "/sessions/%s/runs" % rc_sid,
            {"session_id": rc_sid, "message": "small delta turn"})
        assert status == 200, (status, rc_turn)
        wait_run(base, rc_sid, rc_turn["run_id"])
        status, reused = call(
            base, "POST", "/sessions/%s/context/recompress" % rc_sid, {})
        assert status == 200 and reused.get("status") == "reused", (status, reused)
        assert reused["tail_messages"] == 2, reused
        assert len(auxiliary.snapshot_requests()) == aux_before, \
            "cooldown must not call the auxiliary"
        reused_snap = snapshot(base, rc_sid)
        assert reused_snap["context_summary"] == first_summary, reused_snap
        assert any(event.get("event_type") == "context.reused" and
                   event.get("run_id") == "context-" + rc_sid
                   for event in reused_snap["events"]), reused_snap["events"]

        # ── re-compression: a large delta merges checkpoint + new turns ──
        for i in range(4):
            status, big_turn = call(
                base, "POST", "/sessions/%s/runs" % rc_sid,
                {"session_id": rc_sid,
                 "message": "recompression delta marker-%d " % i + "x" * 2500})
            assert status == 200, (status, big_turn)
            wait_run(base, rc_sid, big_turn["run_id"])
        aux_before = len(auxiliary.snapshot_requests())
        status, merging = call(
            base, "POST", "/sessions/%s/context/recompress" % rc_sid, {})
        assert status == 200 and merging.get("status") == "recompressing", \
            (status, merging)
        assert merging["tail_messages"] >= 6, merging
        rc_merged = wait_for_snapshot(
            base, rc_sid,
            lambda snap: snap.get("context_status") == "ready",
            "re-compression readiness")
        merged_aux = auxiliary.snapshot_requests()[aux_before:]
        assert merged_aux, "auxiliary endpoint received no re-compression request"
        merged_text = json.dumps(merged_aux[0])
        assert "BEGIN PRE-VALIDATED CHECKPOINT" in merged_text, merged_text
        assert "recompression delta marker-3" in merged_text, merged_text
        assert "retain the selected conversation" in merged_text, merged_text
        merged_summary = json.loads(rc_merged["context_summary"])
        assert any("acknowledged the new turns" in item for item in
                   merged_summary["decisions"]), merged_summary
        last_fork_message = rc_merged["messages"][-1]
        assert ("[source message id: %s]" % last_fork_message["id"]) in \
            json.dumps(merged_summary["decisions"]), merged_summary
        assert rc_merged["context_watermark"] == last_fork_message["id"], rc_merged
        assert any(event.get("event_type") == "context.recompressed" and
                   event.get("run_id") == "context-" + rc_sid
                   for event in rc_merged["events"]), rc_merged["events"]

        # Truncation: absorbed turns must not be re-sent to the primary.
        status, post_turn = call(
            base, "POST", "/sessions/%s/runs" % rc_sid,
            {"session_id": rc_sid, "message": "post merge turn"})
        assert status == 200, (status, post_turn)
        wait_run(base, rc_sid, post_turn["run_id"])
        post_request = next(
            request for request in reversed(primary.snapshot_requests())
            if any(m.get("role") == "user" and m.get("content") == "post merge turn"
                   for m in request.get("messages", [])))
        post_messages = post_request["messages"]
        compact = [m for m in post_messages
                   if m.get("role") == "system" and
                   "[COMPACT CONTEXT]" in m.get("content", "")]
        assert compact, post_messages
        # Absorbed turns must not be re-sent as conversation messages...
        conversation_text = json.dumps(
            [m for m in post_messages if m.get("role") in ("user", "assistant")])
        assert "recompression delta marker-3" not in conversation_text, \
            conversation_text
        assert "small delta turn" not in conversation_text, conversation_text
        # ...but the compact context still carries the verbatim latest user
        # request from the absorbed tail.
        assert "recompression delta marker-3" in compact[0]["content"], \
            compact[0]["content"]

        # ── re-compression: auxiliary down → mechanical merge fallback ──
        # The provider retries one 503 per call, so fail both attempts of the
        # initial call and both attempts of the bounded repair call.
        auxiliary.fail_next = 4
        for i in range(4):
            status, fb_turn = call(
                base, "POST", "/sessions/%s/runs" % rc_sid,
                {"session_id": rc_sid,
                 "message": "fallback delta marker-%d " % i + "y" * 2500})
            assert status == 200, (status, fb_turn)
            wait_run(base, rc_sid, fb_turn["run_id"])
        status, fb_start = call(
            base, "POST", "/sessions/%s/context/recompress" % rc_sid, {})
        assert status == 200 and fb_start.get("status") == "recompressing", \
            (status, fb_start)
        fb_ready = wait_for_snapshot(
            base, rc_sid,
            lambda snap: snap.get("context_status") == "ready",
            "mechanical re-compression readiness")
        fb_summary = json.loads(fb_ready["context_summary"])
        assert fb_summary["goal"], fb_summary
        assert any("retain the selected conversation" in item for item in
                   fb_summary["decisions"]), fb_summary
        assert any("source message id" in item for item in
                   fb_summary["relevant_artifacts"]), fb_summary
        assert any("fallback delta marker-3" in item for item in
                   fb_summary["relevant_artifacts"]), fb_summary
        assert fb_summary["latest_user_request"].startswith(
            "fallback delta marker-3"), fb_summary
        assert fb_ready["context_watermark"] == fb_ready["messages"][-1]["id"], \
            fb_ready
        assert any(event.get("event_type") == "context.fallback" and
                   event.get("run_id") == "context-" + rc_sid and
                   event.get("payload", {}).get("recompressed") == "true"
                   for event in fb_ready["events"]), fb_ready["events"]

        # ── session search: recover verbatim turns from the original ──
        status, found = call(base, "GET", "/sessions/%s/search?q=followup" % rc_sid)
        assert status == 200 and found.get("ok"), (status, found)
        assert found["scope"] == "source", found
        assert found["session_id"] == source_sid, found
        assert found["matches"], found
        assert found["matches"][0]["session_id"] == source_sid, found
        assert "followup" in found["matches"][0]["snippet"], found
        status, no_match = call(
            base, "GET", "/sessions/%s/search?q=zzzznotthere" % rc_sid)
        assert status == 200 and no_match.get("matches") == [], (status, no_match)
        status, bad_q = call(base, "GET", "/sessions/%s/search" % rc_sid)
        assert status == 400 and "q" in bad_q.get("error", ""), (status, bad_q)
        # A selected session searches its own messages.
        status, self_found = call(
            base, "GET", "/sessions/%s/search?q=source" % source_sid)
        assert status == 200 and self_found.get("scope") == "self", \
            (status, self_found)
        assert self_found["matches"], self_found

        # OpenAICompatibleProvider retries one empty 503 response per call, so
        # fail both attempts of the initial call and both attempts of the
        # bounded repair call: the auxiliary is down, and the fork must still
        # reach ready with the deterministic mechanical summary.
        auxiliary.fail_next = 4
        status, failing_fork = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, failing_fork)
        failing_sid = failing_fork["id"]
        status, recovered = call(
            base, "POST", "/sessions/%s/runs" % failing_sid,
            {"session_id": failing_sid, "message": "fallback run"})
        assert status == 200, (status, recovered)
        fallback_ready = wait_for_snapshot(
            base, failing_sid,
            lambda snap: snap.get("context_status") == "ready",
            "auxiliary-down fallback readiness")
        fallback_summary = json.loads(fallback_ready["context_summary"])
        assert fallback_summary["relevant_artifacts"], fallback_ready
        assert "source message id" in json.dumps(
            fallback_summary["relevant_artifacts"]), fallback_ready
        assert fallback_summary["latest_user_request"] == "followup", \
            fallback_ready
        assert any(event.get("event_type") == "context.fallback" and
                   event.get("run_id") == "context-" + failing_sid
                   for event in fallback_ready["events"]), fallback_ready["events"]
        fallback_completed = wait_run(base, failing_sid, recovered["run_id"])
        assert any(event.get("event_type") == "run.completed" and
                   event.get("run_id") == recovered["run_id"]
                   for event in fallback_completed["events"]), \
            fallback_completed["events"]
        status, recovery = call(
            base, "POST", "/sessions/%s/context" % failing_sid,
            {"mode": "selected"})
        assert status == 409 and "copied selected context" in \
            recovery.get("error", ""), (status, recovery)

        status, selected = call(
            base, "POST", "/sessions",
            {"fork_from": source_sid, "path": "active"})
        assert status == 200, (status, selected)
        selected_snapshot = snapshot(base, selected["id"])
        assert selected["context_mode"] == "selected", selected
        assert selected["context_status"] == "ready", selected
        assert len(selected_snapshot["messages"]) == len(source["messages"]), selected_snapshot
        assert selected_snapshot["title"].startswith("fork of"), selected_snapshot

        second = start_daemon(daemon, reserve_port(), primary, auxiliary, True)
        process2, base2, workdir2, log2 = second
        include_source, _ = create_source(base2, "include source")
        include_before = len(auxiliary.snapshot_requests())
        status, include_fork = call(
            base2, "POST", "/sessions",
            {"fork_from": include_source, "path": "active", "mode": "summary"})
        assert status == 200, (status, include_fork)
        include_ready = wait_for_snapshot(
            base2, include_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "opt-in summary readiness")
        include_request = auxiliary.snapshot_requests()[include_before:][0]
        include_text = json.dumps(include_request)
        assert "INTERNAL REASONING SOURCE MATERIAL" in include_text, include_text
        assert "INLINE <think> SOURCE MATERIAL" in include_text, include_text
        assert "primary separated diagnostic reasoning" in include_text, include_text
        assert "inline hidden reasoning" in include_text, include_text
        include_summary = json.loads(include_ready["context_summary"])
        assert include_summary["decisions"], include_ready

        # Hierarchical chunking: a small budget forces per-chunk calls and a
        # merge call, all validated against the same source ids.
        third = start_daemon(
            daemon, reserve_port(), primary, auxiliary, False,
            extra_env={"NYAXUS_SUMMARY_SOURCE_BUDGET_BYTES": "200"})
        process3, base3, workdir3, log3 = third
        chunk_sid, _ = create_source(base3, "chunk source turn with a bit more text")
        chunk_before = len(auxiliary.snapshot_requests())
        status, chunk_fork = call(
            base3, "POST", "/sessions",
            {"fork_from": chunk_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, chunk_fork)
        chunk_ready = wait_for_snapshot(
            base3, chunk_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "chunked summary readiness")
        chunk_summary = json.loads(chunk_ready["context_summary"])
        assert chunk_summary["unresolved_tasks"], chunk_ready
        chunk_aux_requests = auxiliary.snapshot_requests()[chunk_before:]
        assert len(chunk_aux_requests) >= 3, len(chunk_aux_requests)

        # Audit: every validated segment checkpoint is retained in the
        # event ledger, so the merge can be inspected against its inputs.
        segment_events = [e for e in chunk_ready["events"]
                          if e.get("event_type") == "context.segment"]
        assert len(segment_events) >= 2, chunk_ready["events"]
        assert all(e.get("payload", {}).get("document")
                   for e in segment_events), chunk_ready["events"]

        # Merge coverage: a merge that drops a segment's citation is
        # rejected and repaired with the specific missing id; the final
        # checkpoint carries every segment's provenance.
        chunk_source = snapshot(base3, chunk_sid)
        chunk_ids = [m["id"] for m in chunk_source["messages"]]
        assert len(chunk_ids) >= 2, chunk_ids
        first_id, second_id = chunk_ids[0], chunk_ids[-1]
        auxiliary.aux_mode = "merge_drops_citation"
        drop_before = len(auxiliary.snapshot_requests())
        status, drop_fork = call(
            base3, "POST", "/sessions",
            {"fork_from": chunk_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, drop_fork)
        drop_ready = wait_for_snapshot(
            base3, drop_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "merge repair readiness")
        drop_summary = json.loads(drop_ready["context_summary"])
        drop_text = json.dumps(drop_summary)
        assert ("[source message id: %s]" % first_id) in drop_text, drop_summary
        assert ("[source message id: %s]" % second_id) in drop_text, drop_summary
        drop_aux = auxiliary.snapshot_requests()[drop_before:]
        # two segment calls + the dropped merge + the bounded repair
        assert len(drop_aux) == 4, len(drop_aux)

        # Merge coverage: a merge that keeps dropping the citation falls
        # back to the deterministic segment merge, which loses nothing.
        auxiliary.aux_mode = "merge_always_drops"
        mech_before = len(auxiliary.snapshot_requests())
        status, mech_fork = call(
            base3, "POST", "/sessions",
            {"fork_from": chunk_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, mech_fork)
        mech_ready = wait_for_snapshot(
            base3, mech_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "mechanical merge readiness")
        mech_summary = json.loads(mech_ready["context_summary"])
        mech_text = json.dumps(mech_summary)
        assert ("[source message id: %s]" % first_id) in mech_text, mech_summary
        assert ("[source message id: %s]" % second_id) in mech_text, mech_summary
        mech_aux = auxiliary.snapshot_requests()[mech_before:]
        assert len(mech_aux) == 4, len(mech_aux)
        assert any(
            event.get("event_type") == "context.completed" and
            event.get("run_id") == "context-" + mech_fork["id"] and
            event.get("payload", {}).get("mechanical_merge") == "true"
            for event in mech_ready["events"]), mech_ready["events"]
        auxiliary.aux_mode = "valid"

        # No-thinking route: the auxiliary request must carry the
        # chat_template_kwargs payload.
        fourth = start_daemon(
            daemon, reserve_port(), primary, auxiliary, False,
            extra_args=["--auxiliary-no-thinking"])
        process4, base4, workdir4, log4 = fourth
        nt_sid, _ = create_source(base4, "no thinking source")
        nt_before = len(auxiliary.snapshot_requests())
        status, nt_fork = call(
            base4, "POST", "/sessions",
            {"fork_from": nt_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, nt_fork)
        wait_for_snapshot(
            base4, nt_fork["id"],
            lambda snap: snap.get("context_status") == "ready",
            "no-thinking summary readiness")
        nt_request = auxiliary.snapshot_requests()[nt_before:][0]
        assert nt_request.get("chat_template_kwargs") == {
            "enable_thinking": False}, nt_request

        # Vision reference: an image-bearing source must not hard-fail a
        # text-only auxiliary (vision off); the image is referenced by
        # message id and the summary still becomes ready.
        status, vision_profile = call(
            base, "POST", "/profiles",
            {"id": "vision-test", "name": "vision test", "vision": True})
        assert status == 200, (status, vision_profile)
        status, vision_source = call(base, "POST", "/sessions", {})
        assert status == 200, vision_source
        vision_sid = vision_source["id"]
        status, _ = call(
            base, "POST", "/sessions/%s/profile" % vision_sid,
            {"profile": "vision-test"})
        assert status == 200
        status, vision_run = call(
            base, "POST", "/sessions/%s/runs" % vision_sid,
            {"session_id": vision_sid, "message": "what is in this image",
             "attachments": [{"name": "pixel.png", "kind": "image",
                              "content": "data:image/png;base64,iVBORw0KGgo="}]})
        assert status == 200 and vision_run.get("run_id"), (status, vision_run)
        wait_run(base, vision_sid, vision_run["run_id"])
        vision_before = len(auxiliary.snapshot_requests())
        status, vision_fork = call(
            base, "POST", "/sessions",
            {"fork_from": vision_sid, "path": "active", "mode": "summary"})
        assert status == 200, (status, vision_fork)
        vision_ready = wait_for_snapshot(
            base, vision_fork["id"],
            lambda snap: snap.get("context_status") in ("ready", "failed"),
            "vision-reference summary readiness")
        assert vision_ready["context_status"] == "ready", vision_ready
        vision_aux = auxiliary.snapshot_requests()[vision_before:]
        assert vision_aux, "no auxiliary request for the image-bearing source"
        vision_text = json.dumps(vision_aux[0])
        assert "not inlined (auxiliary vision disabled)" in vision_text, vision_text
        assert "iVBORw0KGgo" not in vision_text, vision_text

        print("SUMMARY_FORK_TEST_PASSED")
        return 0
    finally:
        if fourth is not None:
            stop_daemon(fourth[0], fourth[2], fourth[3])
        if third is not None:
            stop_daemon(third[0], third[2], third[3])
        if second is not None:
            stop_daemon(second[0], second[2], second[3])
        if first is not None:
            stop_daemon(first[0], first[2], first[3])
        auxiliary.stop()
        primary.stop()


if __name__ == "__main__":
    sys.exit(main())
