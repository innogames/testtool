#!/usr/bin/env python3
"""Mock pfctl for testtool tests.

Simulates FreeBSD's pfctl by storing table state in a JSON file.
State file path is read from MOCK_PFCTL_STATE environment variable.

Uses argparse so arguments can appear in any order, just like real pfctl.
"""

import argparse
import json
import os
import sys


def load_state(path):
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    return {"tables": {}, "kill_log": []}


def save_state(path, state):
    with open(path, "w") as f:
        json.dump(state, f, indent=2)


def parse_kv_pairs(values):
    """Parse a list of alternating key-value tokens into a dict.

    Example: ["table", "pool_0", "dsthost", "2001:db8::1", "kill", "rststates"]
    Returns: {"table": "pool_0", "dsthost": "2001:db8::1", "kill": "rststates"}
    """
    result = {}
    if values:
        it = iter(values)
        for key in it:
            result[key] = next(it, None)
    return result


def main():
    state_file = os.environ.get("MOCK_PFCTL_STATE")
    if not state_file:
        print("MOCK_PFCTL_STATE not set", file=sys.stderr)
        sys.exit(1)

    parser = argparse.ArgumentParser(description="Mock pfctl")
    parser.add_argument("-q", action="store_true", help="Quiet mode")
    parser.add_argument("-t", metavar="TABLE", help="Table name")
    parser.add_argument("-T", metavar="COMMAND", help="Table command (show/add/del)")
    # -K (uppercase) for kill src_nodes — can appear multiple times
    parser.add_argument("-K", action="append", metavar="VALUE",
                        help="Kill src_nodes key-value pairs")
    # -k (lowercase) for kill states — can appear multiple times
    parser.add_argument("-k", action="append", metavar="VALUE",
                        help="Kill states key-value pairs")

    args, remaining = parser.parse_known_args()

    state = load_state(state_file)

    # Table operations: -t TABLE -T {show|add|del} [remaining IPs...]
    if args.t is not None and args.T is not None:
        table = args.t
        operation = args.T

        if operation == "show":
            if table not in state["tables"]:
                sys.exit(1)
            for addr in state["tables"][table]:
                # pfctl show output has leading whitespace
                print(f"   {addr}")
            sys.exit(0)

        elif operation == "add":
            if table not in state["tables"]:
                state["tables"][table] = []
            for addr in remaining:
                if addr not in state["tables"][table]:
                    state["tables"][table].append(addr)
            save_state(state_file, state)
            sys.exit(0)

        elif operation == "del":
            if table in state["tables"]:
                state["tables"][table] = [
                    a for a in state["tables"][table] if a not in remaining
                ]
            save_state(state_file, state)
            sys.exit(0)

    # Kill src_nodes: -K table -K TABLE -K dsthost -K ADDR [-K kill -K rststates]
    if args.K:
        kv = parse_kv_pairs(args.K)
        table = kv.get("table", "")
        address = kv.get("dsthost", "")
        with_states = "kill" in kv
        state["kill_log"].append({
            "type": "src_nodes",
            "table": table,
            "address": address,
            "with_states": with_states,
        })
        save_state(state_file, state)
        sys.exit(0)

    # Kill states rdr: -k table -k TABLE -k rdrhost -k ADDR -k kill -k rststates
    if args.k:
        kv = parse_kv_pairs(args.k)
        table = kv.get("table", "")
        address = kv.get("rdrhost", "")
        state["kill_log"].append({
            "type": "states_rdr",
            "table": table,
            "address": address,
        })
        save_state(state_file, state)
        sys.exit(0)

    print(f"mock_pfctl: unrecognized args: {sys.argv[1:]}", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    main()
