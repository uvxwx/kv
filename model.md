# KV Store Model (v2)

This document specifies the **conceptual model** (not implementation) for this project's key-value store.

Based on:
- Current API shape in `include/kv/kv.hpp` (`kv::Store`, `kv::Tx`, `runInTx`, `runInTxWithRetry`)
- Goals discussed in chat: pedagogical, not a toy; in-memory; process-local; serializable SS2PL; callback transactions; TTL; waits

## Design Goals

- Simple but real: enough semantics to build something useful (leases and locks, rate limits, coordination within one process).
- Strong correctness by default: serializable transactions (no "best effort" fallbacks).
- Small surface area: avoid scans and ranges, MVCC, persistence, replication.

## Core Data Model

### Keys and Values

- Key: opaque string (treat as bytes).
- Value: opaque bytes.

### Separate Internal Tables

The model uses two internal tables with distinct roles:

- Lock and metadata table:
  - keyed by key identity
  - stores the per-key synchronization state used to serialize access
  - stores `revision`, a monotonically increasing integer for that key
  - stores the notification mechanism that allows waiting for `revision` to change
- Data table:
  - keyed by key identity
  - stores only committed logical value state for keys that are present
  - stores `value`
  - stores `expiresAt`, the optional expiration timestamp (TTL)

A key is logically present iff it has a row in the data table that is not expired.
A key may still have a lock and metadata table entry even when it is logically missing.

### Expiration (TTL)

- A key is logically missing if `expiresAt <= now`.
- TTL enforcement is lazy on access:
  - reads, writes, and waits treat expired keys as missing
  - expiry is modeled as removing the data-table row, incrementing `revision`, and notifying waiters

### Revision Rule (Wakeups and Lost-wakeup Avoidance)

Any change to a key's logical state increments its `revision` and notifies waiters:
- create, update, delete, expire-cleanup

## Transaction Model (Serializable SS2PL, Callback)

### API Shape

Transactions are executed via `Store::runInTx(func)` where:
- `func(Tx&) -> std::expected<T, E>`

The callback's result defines the outcome:
- `ok` result => transaction commits
- `error` result => transaction rolls back

`runInTxWithRetry` retries only on internal aborts.

### Write Visibility and Commit Point

- Transaction writes are buffered in a per-transaction write-set (puts, erases, and TTL updates) during the callback.
- Reads inside the transaction observe the transaction view (base store and write-set), i.e. read-your-writes.
- Reads and writes first resolve the key's lock and metadata entry, creating it if needed.
- The commit point is when the store applies the write-set to the shared state:
  - the data table is updated for each logically changed key
  - per-key `revision` in the lock and metadata table is incremented for each logically changed key
  - waiters are notified for those keys
- On rollback or abort, the write-set is discarded and produces no externally visible changes.

### Isolation and Locking

- Isolation level: serializable
- Concurrency control: strict 2PL (SS2PL)
  - locks acquired during a transaction are held until after the store finalizes the transaction outcome (commit or rollback) when the callback returns
- Serialization is by key identity via the lock and metadata table, not by the presence of a data-table row.
- Transactions provide read-your-writes semantics.

### Abort and Retry

- A transaction may abort for internal reasons (e.g., contention management, deadlock avoidance, or cancellation).
- Abort is reported as `Tx::Error::kAborted`.
- `runInTxWithRetry` retries only when the store reports `kAborted` (never retries application-level logic errors).

### Contention Policy

- Deadlock avoidance may use transaction age (for example, `wait-die`) as an implementation detail.
- When choosing which already-waiting operation to wake next, the primary goal is to strive to minimize induced aborts among remaining waiters.
- Reader and writer ordering is therefore not a hard semantic rule in the model; it is subordinate to the abort-minimization goal.
- Fairness between waiters is an implementation detail unless it conflicts with correctness.

### Commit and Rollback Visibility

For v2's callback-transaction model:
- "Commit" and "rollback" are outcomes of `runInTx`, not public `Tx::commit()` and `Tx::rollback()` calls.
- Resource-management mechanisms (like destructors unlocking) are not part of the model; externally visible effects are defined by the commit point above.
- Rollback discards buffered logical data changes. It does not need to undo creation of a lock and metadata entry that was created only to coordinate access to a previously absent key.

## Operations (Minimal, Useful)

Inside a transaction (`Tx`), all operations are serializable:

- `get(key) -> optional<Value>`
  - returns missing if key absent or expired
- `put(key, value)` and `erase(key)`
- `compareAndSwap(key, expected, desired)`
  - `expected` and `desired` can be "missing" or a concrete value
  - `compareAndSwap(key, missing, desired)` is the create-if-absent primitive
- `add(key, delta)` (counter primitive)
  - defines a value encoding for integers (e.g., decimal int64 stored in value bytes)

Outside a transaction (optional conveniences), single-key ops may be modeled as a single-key transaction internally.

## Waiting and Watching Model (Revision-based)

### Revision waits are not allowed inside a transaction

Blocking during lock acquisition is allowed in the model.
What is intentionally disallowed is waiting for revision-change notifications or other external state changes while a transaction is open, because that risks deadlocks and complicates reasoning.
Waiting APIs are used outside `runInTx`, then callers re-enter a transaction to re-check and update.

### Revision-based Wait API

Model a wait primitive:
- `waitForChange(key, afterRevision, deadline) -> (newRevision, optional<Value>)`

Rules:
- If the key's current `revision` is already `> afterRevision`, return immediately.
- Otherwise, block until:
  - the key's `revision` changes due to a committed write or delete or due to expire-cleanup, or
  - deadline expires (timeout), or
  - the waiting task is cancelled (if the runtime supports cancellation)

`revision` and wait and wakeup state live in the lock and metadata table, so waiting on a currently absent key is valid.
This revision-based design avoids lost wakeups and works with spurious notifications.

## Anchor Workload: Lease and Lock Service (Not a Toy)

Implement a lease and lock service on top of the KV store:

- Key: `lease/<name>`
- Value: `owner_id` (and optionally metadata)
- TTL: stored via the key's `expiresAt`

### Acquire or Renew (in a transaction)

`acquireOrRenew(name, owner, ttl)`:
- If key is missing or expired:
  - set value = owner
  - set TTL
  - succeed
- Else if value == owner:
  - renew TTL
  - succeed
- Else:
  - fail (return current owner)

### Release (in a transaction)

`release(name, owner)`:
- delete only if current owner matches

### Waiters

Clients that fail to acquire can:
- call `waitForChange("lease/<name>", lastRevision, deadline)`
- then retry `acquireOrRenew` in a new transaction

This workload exercises:
- contention and concurrency
- TTL and lazy expiry semantics
- revision-based waiting
- conditional updates (CAS-like behavior)
- transaction atomicity (multi-step decisions inside a tx)

## Non-goals (v2)

- Persistence and recovery after restart
- Range scans, prefix iteration, ordered keys
- MVCC and snapshot isolation
- Cross-process consistency and replication

## Assumptions

- "Now" is provided by the embedding environment (no specific clock API mandated in the model).
- Keys and values are treated as opaque bytes; higher-level encoding is workload-specific.
- Cleanup of idle lock and metadata table entries is an implementation detail, as long as it does not change externally visible revision or wait behavior for active users.
