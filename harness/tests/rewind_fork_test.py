#!/usr/bin/env python3
"""End-to-end check for rewind, edit, and fork against the daemon (fake provider).

Verifies:
  1. rewind moves the active leaf; the tail stays as an inactive branch and
     can be re-entered
  2. rewind to 0 clears the active path
  3. explicit trim rewind permanently removes a checkpoint's descendants and
     their branches while preserving the checkpoint and earlier siblings
  4. editing a user message branches on the user side (sibling message +
     immediate run) and the original stays as an inactive branch
  5. user-side branches are navigable via select {message_id}
  6. fork copies a continuous path (explicit id list or "active") into a new
     standalone session with re-mapped parents, copied attachments, and no
     run ids (runs belong to the source session)
  7. invalid fork paths are rejected (400) and unknown sources (404)

Usage: python harness/tests/rewind_fork_test.py [path-to-nyaxus_daemon]
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DAEMON = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    ROOT, "harness", "nyaxus_daemon.exe")
PORT = 8899
BASE = "http://127.0.0.1:%d/api/v1" % PORT

SID = None


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        BASE + path, method=method, data=data,
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as resp:
            return resp.status, json.loads(resp.read().decode() or "{}")
    except urllib.error.HTTPError as err:
        try:
            return err.code, json.loads(err.read().decode() or "{}")
        except ValueError:
            return err.code, {}


def snapshot():
    status, body = call("GET", "/sessions/" + SID)
    if status != 200:
        raise AssertionError("snapshot failed: %s %s" % (status, body))
    return body


def wait_for(predicate, what, timeout=15):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = snapshot()
        if predicate(last):
            return last
        time.sleep(0.3)
    raise AssertionError(
        "timed out waiting for %s; last messages: %s"
        % (what, json.dumps([
            {"id": m["id"], "role": m["role"], "run_id": m.get("run_id"),
             "active": m["is_active"]} for m in last.get("messages", [])
        ])))


def find_run(snap, run_id):
    for message in snap["messages"]:
        if message.get("run_id") == run_id and message["role"] == "assistant":
            return message
    return None


def by_run(snap, run_id):
    message = find_run(snap, run_id)
    if message is None:
        raise AssertionError("assistant message for run %s not found" % run_id)
    return message


def active_ids(snap):
    return [m["id"] for m in snap["messages"] if m["is_active"]]


def main():
    global SID
    if not os.path.exists(DAEMON):
        print("daemon binary not found: %s" % DAEMON, file=sys.stderr)
        return 1
    workdir = tempfile.mkdtemp(prefix="nyaxus-rewind-fork-")
    proc = subprocess.Popen(
        [DAEMON, "--port", str(PORT), "--provider", "fake",
         "--workspace", workdir],
        cwd=workdir,
        stdout=open(os.path.join(workdir, "daemon.log"), "wb"),
        stderr=subprocess.STDOUT)
    try:
        for _ in range(50):
            try:
                with urllib.request.urlopen(BASE + "/health", timeout=1) as resp:
                    if resp.status == 200:
                        break
            except Exception:
                time.sleep(0.2)
        else:
            raise AssertionError("daemon did not come up (see %s)"
                                 % os.path.join(workdir, "daemon.log"))

        _, created = call("POST", "/sessions", {})
        SID = created["id"]
        print("session:", SID)

        # Build: u1 -> a1, a2 (regenerate) -> u2 -> a3
        _, r1 = call("POST", "/sessions/%s/runs" % SID,
                     {"session_id": SID, "message": "hello"})
        run1 = r1["run_id"]
        wait_for(lambda s: any(m.get("run_id") == run1
                               for m in s["messages"] if m["role"] == "assistant"),
                 "run 1 answer")

        _, regen = call("POST", "/runs/%s/commands" % run1,
                        {"command": "regenerate", "session_id": SID})
        run2 = regen["run_id"]
        snap = wait_for(lambda s: find_run(s, run2) is not None, "run 2 answer")

        _, r3 = call("POST", "/sessions/%s/runs" % SID,
                     {"session_id": SID, "message": "second"})
        run3 = r3["run_id"]
        snap = wait_for(lambda s: find_run(s, run3) is not None, "run 3 answer")

        messages = {m["id"]: m for m in snap["messages"]}
        u1 = [m for m in snap["messages"]
              if m["role"] == "user" and m["content"] == "hello"][0]["id"]
        a1 = by_run(snap, run1)["id"]
        a2 = by_run(snap, run2)["id"]
        u2 = [m for m in snap["messages"]
              if m["role"] == "user" and m["content"] == "second"][0]["id"]
        a3 = by_run(snap, run3)["id"]
        assert active_ids(snap) == [u1, a1, a2, u2, a3] or \
            active_ids(snap) == [u1, a2, u2, a3], active_ids(snap)
        a_active = by_run(snap, run1) if by_run(snap, run1)["is_active"] else by_run(snap, run2)
        print("tree built:", [u1, a1, a2, u2, a3])

        # 1. rewind to u1: only the first user turn stays active.
        status, body = call("POST", "/sessions/%s/rewind" % SID,
                            {"message_id": u1})
        assert status == 200 and body.get("ok"), (status, body)
        snap = snapshot()
        assert active_ids(snap) == [u1], active_ids(snap)
        for now_inactive in (a1, a2, u2, a3):
            assert not [m for m in snap["messages"] if m["id"] == now_inactive][0]["is_active"], now_inactive
        print("rewind to first turn: ok")

        # 2. rewind to 0 clears the active path entirely.
        status, body = call("POST", "/sessions/%s/rewind" % SID,
                            {"message_id": 0})
        assert status == 200 and body.get("ok"), (status, body)
        assert active_ids(snapshot()) == [], "expected empty active path"
        print("rewind to root: ok")

        # 3. restore the full path, then edit u1: a sibling user message with
        #    the new text becomes the active leaf and a run answers it; the
        #    original message and its tail stay as an inactive branch.
        status, body = call("POST", "/sessions/%s/select" % SID,
                            {"run_id": run3})
        assert status == 200, (status, body)
        assert active_ids(snapshot())[-1] == a3
        status, body = call("POST", "/sessions/%s/messages/%d" % (SID, u1),
                            {"content": "hello (edited)"})
        assert status == 200 and body.get("run_id"), (status, body)
        edit_run = body["run_id"]
        edit_user_id = body.get("message_id")
        assert edit_user_id and edit_user_id != u1, body
        snap = wait_for(lambda s: find_run(s, edit_run) is not None,
                        "edit run answer")
        u1e = [m for m in snap["messages"] if m["id"] == edit_user_id][0]
        assert u1e["content"] == "hello (edited)" and u1e["parent_id"] == 0, u1e
        edit_answer = by_run(snap, edit_run)
        assert edit_answer["parent_id"] == edit_user_id, edit_answer
        assert active_ids(snap) == [edit_user_id, edit_answer["id"]], active_ids(snap)
        original = [m for m in snap["messages"] if m["id"] == u1][0]
        assert original["content"] == "hello" and not original["is_active"], original
        print("edit branches on the user side and sends: ok")

        # Editing an assistant message is rejected.
        status, body = call("POST", "/sessions/%s/messages/%d" % (SID, a3),
                            {"content": "nope"})
        assert status == 400, (status, body)
        print("assistant edit rejected: ok")

        # 4. user-side branch navigation: selecting the original u1 by
        #    message id re-enters its latest continuation (the old tail), and
        #    the edited branch can be selected back.
        status, body = call("POST", "/sessions/%s/select" % SID,
                            {"message_id": u1})
        assert status == 200, (status, body)
        snap = snapshot()
        assert active_ids(snap)[0] == u1 and active_ids(snap)[-1] == a3, active_ids(snap)
        status, body = call("POST", "/sessions/%s/select" % SID,
                            {"message_id": edit_user_id})
        assert status == 200, (status, body)
        snap = snapshot()
        assert active_ids(snap) == [edit_user_id, by_run(snap, edit_run)["id"]], active_ids(snap)
        print("user branch select: ok")

        # 5. fork the active path (the edited branch) into a new session.
        status, forked = call("POST", "/sessions",
                              {"fork_from": SID, "path": "active"})
        assert status == 200, (status, forked)
        fork1 = forked["id"]
        status, snap = call("GET", "/sessions/" + fork1)
        assert status == 200
        copied = snap["messages"]
        assert [m["content"] for m in copied] == [
            "hello (edited)", edit_answer["content"]], copied
        assert copied[0]["parent_id"] == 0, copied
        assert all(not m.get("run_id") for m in copied), copied
        assert forked["title"].startswith("fork of"), forked
        print("fork active path: ok (%s)" % fork1)

        # 6. fork an explicit mixed branch: u1 -> a2 -> u2 -> a3.
        status, forked = call("POST", "/sessions",
                              {"fork_from": SID, "path": [u1, a2, u2, a3]})
        assert status == 200, (status, forked)
        fork2 = forked["id"]
        status, snap = call("GET", "/sessions/" + fork2)
        copied = snap["messages"]
        assert [m["content"] for m in copied] == [
            "hello", messages[a2]["content"], "second",
            messages[a3]["content"]], copied
        assert [m["parent_id"] for m in copied] == [0, copied[0]["id"],
                                                    copied[1]["id"],
                                                    copied[2]["id"]], copied
        assert all(not m.get("run_id") for m in copied), copied
        print("fork explicit branch: ok (%s)" % fork2)

        # 7. invalid forks are rejected.
        status, body = call("POST", "/sessions",
                            {"fork_from": SID, "path": [a3]})
        assert status == 400, (status, body)
        status, body = call("POST", "/sessions",
                            {"fork_from": SID, "path": [u1, a3]})
        assert status == 400, (status, body)
        status, body = call("POST", "/sessions", {"fork_from": SID})
        assert status == 400, (status, body)
        status, body = call("POST", "/sessions",
                            {"fork_from": "session-does-not-exist",
                             "path": "active"})
        assert status == 404, (status, body)
        print("invalid forks rejected: ok")

        # 8. the source session keeps every branch: the original five plus
        #    the edited user message and its answer.
        snap = snapshot()
        assert len(snap["messages"]) == 7, len(snap["messages"])
        print("source session intact: ok")

        # 9. explicit trim rewind removes all descendants of the checkpoint,
        #    including an inactive continuation, while preserving a sibling
        #    branch that diverged before the checkpoint.
        _, trim_created = call("POST", "/sessions", {})
        trim_sid = trim_created["id"]
        SID = trim_sid
        _, trim_r1 = call("POST", "/sessions/%s/runs" % SID,
                          {"session_id": SID, "message": "trim root"})
        trim_run1 = trim_r1["run_id"]
        snap = wait_for(lambda s: find_run(s, trim_run1) is not None,
                        "trim root answer")
        trim_u1 = [m for m in snap["messages"] if m["role"] == "user" and
                   m["content"] == "trim root"][0]["id"]
        trim_a1 = by_run(snap, trim_run1)["id"]

        _, trim_regen = call("POST", "/runs/%s/commands" % trim_run1,
                             {"command": "regenerate", "session_id": SID})
        trim_run2 = trim_regen["run_id"]
        snap = wait_for(lambda s: find_run(s, trim_run2) is not None,
                        "trim sibling answer")
        trim_a2 = by_run(snap, trim_run2)["id"]

        # Create one continuation under a1, then another under its sibling a2.
        status, _ = call("POST", "/sessions/%s/select" % SID,
                         {"run_id": trim_run1})
        assert status == 200
        _, trim_down1 = call("POST", "/sessions/%s/runs" % SID,
                             {"session_id": SID, "message": "descendant to remove"})
        trim_run3 = trim_down1["run_id"]
        snap = wait_for(lambda s: find_run(s, trim_run3) is not None,
                        "trim removable descendant")
        trim_u2 = [m for m in snap["messages"] if m["role"] == "user" and
                   m["content"] == "descendant to remove"][0]["id"]
        trim_a3 = by_run(snap, trim_run3)["id"]

        status, _ = call("POST", "/sessions/%s/select" % SID,
                         {"run_id": trim_run2})
        assert status == 200
        _, trim_down2 = call("POST", "/sessions/%s/runs" % SID,
                             {"session_id": SID, "message": "sibling to keep"})
        trim_run4 = trim_down2["run_id"]
        snap = wait_for(lambda s: find_run(s, trim_run4) is not None,
                        "trim sibling continuation")
        trim_u3 = [m for m in snap["messages"] if m["role"] == "user" and
                   m["content"] == "sibling to keep"][0]["id"]
        trim_a4 = by_run(snap, trim_run4)["id"]

        # Return to a1's continuation and permanently trim after a1.
        status, _ = call("POST", "/sessions/%s/select" % SID,
                         {"run_id": trim_run1})
        assert status == 200
        status, body = call("POST", "/sessions/%s/rewind" % SID,
                            {"message_id": trim_a1, "trim": True})
        assert status == 200 and body.get("trimmed") is True, (status, body)
        snap = snapshot()
        ids = {m["id"] for m in snap["messages"]}
        assert trim_u1 in ids and trim_a1 in ids, ids
        assert trim_a2 in ids and trim_u3 in ids and trim_a4 in ids, ids
        assert trim_u2 not in ids and trim_a3 not in ids, ids
        assert active_ids(snap) == [trim_u1, trim_a1], active_ids(snap)
        print("trim rewind removes descendant branches: ok")

        print()
        print("REWIND_FORK_TEST_PASSED")
        return 0
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, OSError, ValueError):
            proc.kill()
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
