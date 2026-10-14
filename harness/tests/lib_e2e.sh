# Shared helpers for the e2e test scripts (sourced, not executed).
#
# e2e_port_free <port> - fail fast when something is already listening on a
# test port. A daemon leaked by an interrupted previous run (the EXIT trap
# does not fire when its shell is SIGKILLed) owns its port silently: the
# test's own daemon fails to bind and dies, every curl then lands on the
# stray (old workdir, old DB), and the test fails in a way that looks like
# a product bug ("the approval never appeared"). Proven on agent-host
# 2026-10-04 (web_cache_test vs a 13-hour-old stray on :9086).
e2e_port_free() {
    # LISTEN-only: a TIME_WAIT/FIN_WAIT row quoting the port in its foreign
    # column is stale traffic, not a live listener.
    if netstat -ano 2>/dev/null | grep -q ":$1 .*LISTEN"; then
        echo "FAIL: port $1 is already in use before the test daemon started" >&2
        echo "  (stray daemon from an interrupted previous run - kill it and re-run)" >&2
        exit 1
    fi
}

# e2e_daemon_up <pid> - fail when the test's own daemon process is gone.
# A health 200 from a stray process on the same port would pass silently.
e2e_daemon_up() {
    if ! kill -0 "$1" 2>/dev/null; then
        echo "FAIL: the test daemon died at startup (see daemon.log)" >&2
        exit 1
    fi
}
