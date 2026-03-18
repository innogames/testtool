# Testtool

Testtool is a daemon running on InnoGames Hardware Load Balancers (HWLBs). It maintains FreeBSD's `pf` (packet filter) configuration for forwarding packets toward LB Nodes (backend servers). It loads a JSON config with LB Pools, their LB Nodes, and Health Checks (http, https, tcp, ping, dns, postgres), runs checks in a loop, and adds/removes nodes from pf tables based on check results.

## Build & Test

After implementing any code changes, always do a clean build and run the tests:

```sh
rm -rf obj
cmake -B obj -S .
/usr/bin/make -j4 -C obj check
```

Always delete `obj/` and re-run cmake before building — never reuse a stale build directory.

- A clean test run with no failures is required before a task is done.
- Fix all compile errors and test failures before finishing.

## Tech Stack

- **Language:** C++17
- **Build:** CMake (min 3.31) + Makefile wrapper; build dir is `obj/`
- **Key deps:** libevent (async I/O), OpenSSL (HTTPS), Boost (IPC/string/asio), nlohmann/json, fmt, libpq (postgres HC), GTest
- **Platforms:** FreeBSD (production), macOS/Linux (dev)
- **Run tests:** `make check`

## Architecture

```
JSON config → LbPools → LbNodes → Healthchecks
                                       ↓ (libevent async loop)
                             HC result → node_logic() → state_changed
                                                             ↓
                                                       pool_logic()
                                                             ↓
                                                   pfctl_msg via IPC queue
                                                             ↓
                                                   pfctl_worker child proc → /sbin/pfctl
                                                             ↓
                                                     pf table updated
```

### Key Classes

- **TestTool** (`src/testtool.h`) — Main orchestrator. Owns `map<string, LbPool*> lb_pools`, libevent base, pfctl message queue.
- **LbPool** (`src/lb_pool.h`) — A virtual service (VIP). Has pf table name, set of all nodes, `up_nodes`, fault policy (FORCE_DOWN/FORCE_UP/BACKUP_POOL), min/max node constraints. `pool_logic()` is the core algorithm.
- **LbNode** (`src/lb_node.h`) — A backend server. Has `state` (from healthchecks: UP/DOWN), `admin_state` (ENABLED/DRAIN_SOFT/DRAIN_HARD/DOWNTIME). `is_up()` = state AND admin_state.
- **Healthcheck** (`src/healthcheck.h`) — Abstract base with factory method `healthcheck_factory()`. Subclasses: `Healthcheck_http`, `Healthcheck_https`, `Healthcheck_tcp`, `Healthcheck_ping`, `Healthcheck_dns`, `Healthcheck_postgres`, `Healthcheck_dummy`.
- **pfctl_worker** (`src/pfctl_worker.h`) — Child process that receives `pfctl_msg` IPC messages and executes pf operations asynchronously, so the main event loop doesn't block.

### pool_logic() — Core Algorithm

1. Only runs if startup or a node's `state_changed == true`
2. Respect `max_nodes`: preserve previously wanted nodes first, then add new up nodes
3. Satisfy `min_nodes` based on fault policy:
   - **FORCE_DOWN:** pool goes fully down if not enough up nodes
   - **FORCE_UP:** force-keep minimum nodes even if checks fail (priority: recently changed → previously kept → any → from downtime)
   - **BACKUP_POOL:** activate backup pool
4. Clear flags for removed nodes
5. Sync desired state to pf via IPC message

### Node/Check State

- `LbNode::state` — result of healthchecks (UP/DOWN)
- `LbNode::admin_state` — operational directive (enabled, draining, downtime)
- `LbNode::is_up()` — state AND admin_state both must allow it
- `Healthcheck::hard_state` — transitions after `max_failed_checks` (default 3) consecutive failures
- Healthcheck defaults: interval 2s, timeout 1500ms, max_failed 3

### IPC / pfctl_msg

Struct with pool_name, table_name, array of `SyncedLbNode` (desired state, admin_state, IPv4/IPv6). Limits: MAX_NODES=100 per pool, QUEUE_LEN=10.

### Signals

- **SIGTERM** — graceful shutdown
- **SIGHUP** — terminates process (watchdog re-launches; no hot reload)
- **SIGUSR1** — debug dump

## Config Format

Top-level keys are pool names:

```json
{
  "pool_name": {
    "ip4": "192.0.2.0",
    "ip6": "2001:db8::0",
    "pf_name": "pool_0",
    "protocol_port": ["tcp80", "tcp443"],
    "min_nodes": 1,
    "max_nodes": 0,
    "min_nodes_action": "force_down|force_up|backup_pool",
    "backup_pool": "other_pool_name",
    "health_checks": [{
      "hc_type": "http|https|tcp|ping|dns|postgres",
      "hc_interval": 2,
      "hc_timeout": 1500,
      "hc_max_failed": 3,
      "hc_port": 80,
      "hc_query": "HEAD /check?pool={POOL_NAME}",
      "hc_host": "example.com",
      "hc_ok_codes": [200],
      "hc_drain_codes": [503]
    }],
    "nodes": {
      "node1": {
        "ip4": "10.0.0.1",
        "ip6": "2001:db8:1000::1",
        "state": "online|deploy_online|deploy_offline|maintenance|cold_standby|retired"
      }
    }
  }
}
```

**Node state → admin_state mapping:**
- `online`, `deploy_online` → ENABLED
- `deploy_offline` → DRAIN_HARD
- `maintenance`, `cold_standby`, `retired` → DOWNTIME

**Query macros (HTTP/DNS):** `{POOL_NAME}`, `{POOL_ADDRESS}`, `{NODE_NAME}`, `{NODE_ADDRESS}`, `{ACTIVE_NODES_NAMES}`, `{ACTIVE_NODES_ADDRESSES}`

## Testing

- **Framework:** Google Test
- **Main suite:** `tests/lb_pool_test.cpp` (35+ tests)
- **Fixture:** `tests/lb_pool_test.json` — 1 pool, 3 nodes, dummy HC
- **Base class:** `TesttoolTest` in `tests/testtool_test.h`
  - `SetUp(bool init_state)` — loads pools from JSON, optionally brings all nodes UP
  - `EndDummyHC(pool, node, result, all_nodes)` — manually fires a HC result
  - `UpNodesNames()` — returns set of currently up node names
- **Mocks:** `send_message()` (tracks `sent_up_lb_nodes`), `pf_is_in_table()` (via `_pf_is_in_table`), `log()` (writes to cerr)
- Tests modify `base_config` JSON before calling `SetUp()` for per-test config variations

## Conventions

- **Naming:** Classes PascalCase, methods/members snake_case, constants UPPER_CASE
- **Error handling:** Custom exceptions (`NotLbPoolException`, `HealthcheckSchedulingException`); boolean returns with `log(MSG_CRIT, ...)` on failure
- **Logging:** `log()` overloaded for string/LbPool*/LbNode*/Healthcheck*; syslog facility LOG_LOCAL3
- **JSON parsing:** Always use `safe_get<T>()` and `key_present()` from `src/config.h`
- **Memory:** Raw `new`/`delete` (no smart pointers) — consistent with existing code
