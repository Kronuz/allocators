# Retirement wakeups

The retirement worker sleeps in `Wake::tick`: `poll` on a pipe, 20 ms when a pass reports
pending work, unbounded otherwise. Three producers hand a domain to the worker, and each one
writes its wake before its final handoff, because once a domain can drain the runtime may be
destroyed and the producer must not touch it again:

| Producer | Marker | Wake | Final handoff |
|---|---|---|---|
| `RetirementDomain::close` | `closing = true` | `signal()` | `admission \|= closed` |
| `undo_resident` (a failed `make`) | `rolling_back += 1` | `signal()` | `admission -= 1`, then `rolling_back -= 1` |
| `release` (last reference) | node in `incoming` | `signal()` | `ready_ = true` |

The worker can consume a wake before the handoff it announces lands. The marker covers that
window: a pass that sees the marker without the handoff reports pending and polls again in 20 ms.

## The ordering

`RetirementState::step` (`retirement.cc`) loads `rolling_back` and `closing`, then `admission` once, and decides
both drainage and pending from those values. If a pass misses a handoff, either it saw the
marker, so the pass is pending, or the wake is still unread, so the next `poll` returns:

- `closing` only rises. A pass that consumed close's wake loads `closing` after it, so it sees
  `true`. Its one `admission` value either holds the closed bit, or it lacks it and the pass is
  pending.
- `rolling_back` falls back to zero after the handoff. Loading it before `admission` makes a zero
  safe: a zero that follows a rollback's decrement synchronizes with it, so the later `admission`
  load sees that rollback's `admission -= 1`.
- A node not yet `ready_` stays in `waiting`, which keeps the pass pending.

A pass that loads `admission` twice, deciding drainage from one value and pending from another,
loses the wakeup. The closed bit or the rollback lands between the loads, the second load reports
nothing pending, and the worker polls with no deadline on a domain that could drain.

## Tests

`test/retirement.cc` (`drains_across`) uses
`RetirementTesting::interleave`. It holds close or rollback between its wake and its final
handoff until the worker is parked at `worker_admission` (before the `admission` load) or
`worker_admitted` (after it). The worker parks on the second pass that sees the wake, which
begins after the wake was consumed. The test then lands the handoff and requires the domain to
drain within 5 s. All four combinations drain. Each of these mutants fails one of them:

1. Deciding pending from a second `admission` load fails close at `worker_admitted`.
2. Loading `admission` before `rolling_back` fails rollback at `worker_admission`.
3. Dropping the `closing` term fails close at `worker_admitted`.
