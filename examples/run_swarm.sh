#!/usr/bin/env bash
# Convenience launcher for example2_search_and_deliver: spawns N agents as
# real separate OS processes (that's the whole point of the example - see
# examples/example2_search_and_deliver.c) and interleaves their output with
# an [agent N] prefix so a single command demonstrates the full swarm.
#
# Usage: examples/run_swarm.sh [num_agents] [duration_seconds]
#   examples/run_swarm.sh          # 3 agents, runs until Ctrl+C
#   examples/run_swarm.sh 5        # 5 agents, runs until Ctrl+C
#   examples/run_swarm.sh 3 10     # 3 agents, stops automatically after 10s

set -u

NUM_AGENTS="${1:-3}"
DURATION="${2:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY=""
for candidate in \
    "$SCRIPT_DIR/../build/example2_search_and_deliver" \
    "$SCRIPT_DIR/build/example2_search_and_deliver" \
    "$(command -v example2_search_and_deliver 2>/dev/null)"; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
        BINARY="$candidate"
        break
    fi
done

if [ -z "$BINARY" ]; then
    echo "error: could not find example2_search_and_deliver (build it first: cmake --build build)" >&2
    exit 1
fi

echo "Launching $NUM_AGENTS agents from: $BINARY"
[ -n "$DURATION" ] && echo "Will stop automatically after ${DURATION}s"
echo "Press Ctrl+C to stop early"
echo

TMPDIR="$(mktemp -d)"
AGENT_PIDS=()
WATCHER_PIDS=()
STOPPED=0

# Polls a log file for new bytes and prefixes each new line - deliberately
# NOT `tail -f | while read`: killing one side of that pipe leaves the other
# side (usually tail itself) as a permanent orphan still watching the file,
# since the two sides are separate sibling processes, not parent/child. A
# plain polling loop is exactly one process, so its PID is unambiguous to kill.
watch_log() {
    local prefix="$1" log="$2" pos=0 size=0
    while :; do
        if [ -f "$log" ]; then
            size=$(wc -c < "$log" 2>/dev/null || echo 0)
            if [ "$size" -gt "$pos" ]; then
                tail -c "+$((pos + 1))" "$log" 2>/dev/null | while IFS= read -r line; do
                    echo "$prefix $line"
                done
                pos="$size"
            fi
        fi
        sleep 0.2
    done
}

stop_all() {
    [ "$STOPPED" -eq 1 ] && return
    STOPPED=1
    echo
    echo "Stopping agents..."
    # Stop the agents FIRST and wait for them to actually exit (they run a
    # destroy() lifecycle hook that prints a final summary line - see
    # examples/example2_search_and_deliver.c) before touching the watchers,
    # otherwise a watcher can be killed before it ever polls that final line.
    for pid in "${AGENT_PIDS[@]:-}"; do kill "$pid" 2>/dev/null; done
    for pid in "${AGENT_PIDS[@]:-}"; do wait "$pid" 2>/dev/null; done
    sleep 0.5  # give each watcher one more poll cycle (see watch_log's 0.2s interval) to catch up
    for pid in "${WATCHER_PIDS[@]:-}"; do kill "$pid" 2>/dev/null; done
    for pid in "${WATCHER_PIDS[@]:-}"; do wait "$pid" 2>/dev/null; done
    rm -rf "$TMPDIR"
}
trap stop_all EXIT INT TERM

for i in $(seq 1 "$NUM_AGENTS"); do
    log="$TMPDIR/agent$i.log"
    : > "$log"
    "$BINARY" "$i" > "$log" 2>&1 &
    AGENT_PIDS+=("$!")

    watch_log "[agent $i]" "$log" &
    WATCHER_PIDS+=("$!")
done

if [ -n "$DURATION" ]; then
    sleep "$DURATION"
else
    wait "${AGENT_PIDS[@]}" 2>/dev/null
fi
