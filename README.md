# Facility Drain Coordinator

Physical fleet drain coordination: it decides
which obligations must be evacuated before a physical scope can be removed from
service, records what the systems that own those obligations have actually
proven, and issues removal authority only when the evidence justifies it.

* C++20, CMake, no third-party dependencies.
* Library, command line tool, eight examples and a benchmark.
* 283 tests across 27 suites, plus 8 example programs, 2 CLI scenarios, a
  benchmark smoke test and an installed-package consumer test.
* Apache License 2.0. No telemetry transmission.

## Systems boundary

This repository owns **facility level drain coordination**: the plan, the
evidence, the residual ledger and the removal decision for one physical scope.

It does **not** migrate workloads, reroute traffic, cancel reservations owned
elsewhere, power off equipment, actuate cooling, or decommission hardware. It
never talks to a switch, a scheduler or a BMC. It issues *bounded requests* to
the systems that own those effects and evaluates the evidence they return.

| Owned here | Owned elsewhere |
| --- | --- |
| The drain plan, its bindings and its revision | The workloads and reservations being drained (ASI) |
| The consumer manifest of record per domain | Network paths and attachments (DFI) |
| The residual ledger and its unknowns | Facility reservations and maintenance protections |
| The bounded drain request and its idempotency key | Delivery of that request to the owning system |
| The per domain completeness assessment | Whether a drain actually happened |
| Safe-to-remove authority, its fencing and its explanation | Physical removal, power, cooling, decommissioning |

The consequence worth stating plainly: a granted verdict is **authority to
remove, not removal**. Nothing in this repository removes anything.

## The core question

For this physical scope, which active obligations and consumers must be
evacuated or relinquished, which drains have actually completed, what residual
capacity and authority remains, and when is the scope genuinely safe to remove
from service?

## Doctrines this code enforces

Each of these is a rule with a test behind it, not a slogan:

* **Observation is not authority.** Only a recorded, generation bound report
  from the owning system completes a domain.
* **Acknowledgement is not effect.** An acknowledged request leaves the plan
  draining and never completes anything.
* **Requested state is not observed state.** Lifecycle state is *derived* from
  recorded facts; there is no operation that sets it.
* **Zero known consumers is not proof of no consumers.** An empty manifest is
  proof only under a *complete* enumeration.
* **Drained ASI workloads do not imply drained DFI paths.** Required domains are
  assessed independently, and the default requirement is both.
* **Missing is never zero.** An absent residual count is an unknown, and it
  blocks.
* **Recovered state is not fresh evidence.** A restart advances the control
  epoch, which fences every previously granted answer without erasing the facts
  it was based on.
* **Stale authority is fenced, not inherited.** A new obligation, a revision, a
  changed manifest or an operator fence withdraws an answer and records why.
* **One physical drain, one request.** The idempotency key excludes the plan
  revision, so bookkeeping can never turn one drain into two.

## Building

Requirements: CMake 3.20 or newer, a C++20 compiler, and on Windows an MSVC
toolset with a developer environment (Ninja plus MSVC needs the environment for
the compiler, the resource tools and the SDK library paths).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Options: BUILD_SHARED_LIBS, FDC_BUILD_TESTS, FDC_BUILD_TOOLS,
FDC_BUILD_EXAMPLES, FDC_BUILD_BENCHMARKS, FDC_WARNINGS_AS_ERRORS (default ON),
FDC_SANITIZERS, FDC_ANALYZE.

The project builds with warnings as errors and produces none in Release or
Debug under MSVC /W4 /WX.

## Using the library

```cpp
#include <facilitydrain/facility_drain_coordinator.hpp>

using namespace facilitydrain;

// 1. Open a durable store. This is the only writer of that directory.
CoordinatorOpenRequest open;
open.root = "C:/var/lib/fdc/rack-9";
open.writer_label = "drain-controller-1";
auto coordinator = Coordinator::open(open);
if (!coordinator) { /* coordinator.error().to_text() */ }

// 2. Bind a plan to the exact world it was planned against.
CreatePlanRequest create;
create.context.plan = PlanId{11};
create.context.expected_revision = Revision{1};
create.context.incarnation = coordinator.value().incarnation();
create.context.expected_epoch = coordinator.value().control_epoch();
create.context.observation = ObservationSequence{1};
create.context.principal = "operator";
create.id = PlanId{11};
create.scope = DrainScope{ScopeKind::kRack, 9};
create.targets = {DrainScope{ScopeKind::kRack, 9}, DrainScope{ScopeKind::kAsset, 41}};
create.declared_required_domains =
    DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
create.generations = /* the ten generations the plan is bound to */;
create.policy_id = PolicyId{3};
create.policy_digest = digest_text(policy_document);
create.consumers = planned_consumers;   // binds a manifest digest per domain
auto created = coordinator.value().create_plan(create);

// 3. Record what the owning systems report. Evidence is self describing: every
//    record is stamped with the generations and observation it was taken under.
RecordEnumerationRequest enumeration;
enumeration.context = /* plan, revision, incarnation, epoch, observation */;
enumeration.domain = OwnerDomain::kAsi;
enumeration.coverage = CoverageState::kComplete;
enumeration.generation = EvidenceGeneration{1};
enumeration.observed_at = ObservationSequence{2};
enumeration.scope_manifest_digest = created.value().plan.spec.targets.digest();
enumeration.consumers = asi_consumers;
enumeration.generations = create.generations;
enumeration.source = "asi-enumerator";
auto recorded = coordinator.value().record_enumeration(enumeration);

// 4. Issue a bounded request, deliver it, and record the answer.
IssueRequestsRequest issue;
issue.context = /* ... */;
issue.domains = DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
auto issued = coordinator.value().issue_requests(issue);
for (const DrainRequest& request : issued.value().to_deliver) {
  deliver_to_owner(request);                       // yours, not this library's
  ConfirmDeliveryRequest delivered;
  delivered.context = /* ... */;
  delivered.request = request.id;
  coordinator.value().confirm_delivery(delivered);
}

// 5. Ask for the verdict as often as you like. It changes nothing.
auto evaluation = coordinator.value().evaluate_safe_to_remove(PlanId{11});
//    evaluation.value().verdict, .primary_blocking_code, .explanation

// 6. Grant authority explicitly. Denial returns the primary blocking code.
GrantSafeToRemoveRequest grant;
grant.context = /* ... */;
grant.granted_by = "operator";
auto granted = coordinator.value().grant_safe_to_remove(grant);
```

The complete public surface is 20 headers under `include/facilitydrain/`,
with `facility_drain_coordinator.hpp` as the umbrella.

## Authority, generations and fencing

A decision is valid only for the exact combination of generations that
described the world when its evidence was produced. A plan binds ten of them:

| Generation | Describes |
| --- | --- |
| scope | Which physical membership the scope had |
| dependency | The dependency graph the enumeration was taken against |
| reservation | The facility reservations in scope |
| obligation | The obligations in scope |
| policy | The policy generation that permits the decision |
| topology | The network topology DFI reported |
| maintenance | The maintenance protections in scope |
| capacity | The capacity accounting generation |
| hardware | The accelerator hardware generation |
| firmware | The firmware generation |

Two records are compatible when every generation is equal. There is no "close
enough": a report taken under a different generation set is refused with
`generation-incompatible` and recorded, so an operator can see what the
owning system actually said.

Removal authority is a grant bound to the plan revision, the control epoch, the
generation set, the evidence digest and the commit sequence of the publish that
recorded it. It stops being live when any of those moves:

* **A revision** withdraws it (the plan a verdict applied to no longer exists).
* **A new enumeration, completion or residual** withdraws it, because the grant
  named the evidence set it was evaluated against.
* **A restart** withdraws it, because a new session is a new control epoch.
  The facts survive; the authority does not, and re-granting is an explicit act.
* **An operator fence** withdraws it and records why.
* **Cancellation or failure** withdraws it permanently for that plan.

Fencing also moves a **floor**: evidence observed at or before the floor can no
longer support a grant. A revision that changes the physical membership, a new
obligation, or an operator fence advances the floor. A restart does not, because
a restart does not make the evidence false; it makes the authority stale.

## Lifecycle

A plan's state is derived from its recorded facts by a fixed, first-match-wins
rule. There is no setter, because a caller that could write "safe to remove"
could authorise a removal by assertion.

| Order | Condition | State |
| --- | --- | --- |
| 1 | the plan is cancelled | `cancelled` |
| 2 | the plan is marked failed | `failed` |
| 3 | a live grant exists | `safe-to-remove` |
| 4 | every required domain is proven complete and no residual blocks | `drained` |
| 5 | a residual blocks | `residuals-present` |
| 6 | a request is acknowledged or completed | `draining` |
| 7 | a request is staged or issued | `requested` |
| 8 | an enumeration is recorded | `enumerating` |
| 9 | otherwise | `proposed` |

A *blocking* residual is an open entry in a required domain, or an open entry of
an unknown kind in any domain: the plan may not require that domain, but nobody
can call a scope safe while an identified factor is unmeasured.

## The residual ledger

The ledger preserves exactly what is unresolved, including what could not be
measured. Ten kinds are recorded: an obligation still active, an obligation the
owner cannot determine, a refusal, incomplete enumeration, missing evidence, an
unknown residual count, an unacknowledged request, a failed domain, evidence
that is stale, and a protected obligation that policy forbids relinquishing.

Entries are keyed by identity (domain, obligation, kind), so recording the same
one twice replaces it. Re-opening a relinquished entry means the obligation came
back, which is treated as a new obligation appearing and withdraws authority.

A completion report that states a residual count is a *claim about how many
things remain*. The claim is only usable when the ledger identifies that many
obligations and every one of them is relinquished with evidence. A count nobody
can break down is an unknown:

```text
the owning system reports 3 obligations remaining but only 1 are identified in the ledger
```

## The verdict

```text
verdict granted plan=11 revision=2 epoch=3 required-domains=2 primary=ok
domains asi=proven-complete dfi=proven-complete facility=not-required monitoring=not-required
```

Each required domain is assessed in a fixed order, and the first blocking
condition is the one reported:

| Blocking code | Meaning |
| --- | --- |
| incomplete-enumeration | No enumeration, or coverage that is not complete |
| consumer-digest-mismatch | The enumerated consumers differ from the plan binding |
| scope-manifest-mismatch | The owner looked at a different physical membership |
| stale-evidence | Observed at or before the plan's fence floor |
| evidence-incomplete | No completion report recorded |
| generation-incompatible | The report was produced under a different generation set |
| evidence-mismatch | The report acted on a different consumer manifest |
| acknowledgement-is-not-effect | Received, requested or acknowledged, but not an effect |
| not-drained | The owner reports the drain still in progress |
| domain-failed | The owner refused, or reported a failure |
| protected-obligation | Policy forbids relinquishing it |
| unknown-residual-count | Drained, but the owner did not say how many remain |
| residuals-present | Known obligations remain |
| unknown-obligation | A count nobody can break down, or an unmeasurable factor |
| plan-cancelled / plan-failed | The plan's own terminal state |

`fdc explain` prints the same reasoning in text, deterministically.

## Idempotent drain requests

A bounded request is externally consequential: the owning system will act on it.
The library therefore makes one physical drain produce exactly one request.

The idempotency key is a digest over the plan, the domain, the scope, the
obligation set being drained, the policy generation and an attempt counter. It
**deliberately excludes the plan revision**. Only an explicit `supersede`,
which records who decided and why, advances the attempt and therefore the key.

Delivery is a separate, durable step:

1. `issue_requests` durably stages a request and returns it for delivery.
2. The caller hands it to the owning system (the library performs no I/O).
3. `confirm_delivery` records that it was handed over.

A process that dies between 1 and 3 leaves a *staged* request. After a restart,
the next `issue_requests` re-offers that same request with the same
idempotency key, so the owning system sees a replay rather than a second drain.
Once delivery is confirmed, further issues report a duplicate and deliver
nothing. This is proven with a real second process that is killed between the
staging and the confirmation.

## Persistence and recovery

A durable store is a directory:

```text
writer.lock                         exclusive operating system lock, held for the session
CURRENT                             the pointer record, 128 bytes
gen-00000000000000000007.fdcdrain   a committed generation
tmp-<token>                         a transient publish file, never authoritative
```

A generation is a 128 byte header followed by the canonical payload. The header
carries the format and payload versions, the commit sequence, the payload length,
a CRC-32 of the payload, a CRC-32 of the header itself, the SHA-256 of the
payload, a zero flags field and zeroed reserved bytes. Every one of those is
verified on read; any mismatch, any trailing byte, any unknown version, any
non-zero reserved field and any unknown enumeration byte is refused rather than
repaired.

Publishing is a seven step sequence: encode and validate, write the staged file,
flush it, **read it back and verify it**, atomically replace the generation,
write and flush the pointer, then prune. The in-memory state is adopted only
after the pointer is durable, so a mutation that did not publish did not happen.

Recovery adopts exactly one generation: the one CURRENT names, verified against
the digest CURRENT carries. Nothing is merged. A generation file that CURRENT
does not name is never interpreted, and a generation *above* the published one
makes the store refuse to open (`store-recovery-failed`) because whether that
publish happened is not knowable from the bytes. Removing the unpublished
generation is the documented operator action; the previous generation is intact.

Limits are part of the durable record. A store may be reopened with bounds at
least as generous as the ones it was written under; tighter bounds are refused
(`limit-exceeded`) rather than silently rescaling the data.

## Error model

Every failure is one code from a documented taxonomy, rendered by
`to_token(ErrorCode)`: `invalid-argument`, `field-too-long`,
`invalid-text`, `duplicate-identifier` for shape; `limit-exceeded`,
`payload-too-large`, `too-many-entries` for bounds;
`revision-conflict`, `epoch-mismatch`, `stale-evidence`,
`stale-authority`, `generation-incompatible`, `consumer-digest-mismatch`,
`unknown-obligation` for authority; `invalid-state-transition`,
`plan-cancelled`, `request-superseded` for lifecycle;
`store-locked`, `store-corrupt`, `store-checksum-mismatch`,
`store-truncated`, `store-trailing-bytes`, `store-recovery-failed`,
`missing-generation-file`, `commit-failed` for durability.

Validation order is fixed and documented per operation, so the same invalid
request always resolves to the same primary code regardless of map ordering or
thread scheduling. That is asserted by tests that submit the same invalid input
in two different orders.

## Concurrency model

One writer incarnation per store, enforced by an operating system file lock held
for the whole session. A second process is refused with `store-locked`, and
because the lock is the operating system's, a holder that is killed releases it
without running any cleanup. A read-only session takes no lock.

In process, every operation takes one mutex for its whole duration, no callback
is ever invoked while it is held (the caller receives the requests it must
deliver and delivers them afterwards), and snapshots are copied under the lock
and rendered outside it. There are no background threads and therefore no
shutdown wait that could deadlock. A mutation is built as a whole next state and
adopted only after it publishes, so a concurrent reader sees one complete
generation and never a half applied mutation.

## Command line tool

```sh
fdc --help
fdc --self-check                 # the whole scenario in memory, one summary line
fdc scenario --root DIR          # the same scenario against a real durable store
fdc plan create --root DIR --plan 11 --scope rack:9 --target rack:9 --target asset:41 \
    --required asi,dfi --scope-generation 4 --dependency-generation 3 \
    --reservation-generation 5 --obligation-generation 7 --policy-generation 2 \
    --topology-generation 6 --maintenance-generation 1 --capacity-generation 8 \
    --hardware-generation 9 --firmware-generation 10 --policy-id 3 --policy-digest <hex> \
    --consumer-file consumers.txt
fdc enumerate --root DIR --plan 11 --domain asi --coverage complete --generation 1 \
    --observed 2 --source asi-enumerator --consumer-file asi.txt
fdc issue --root DIR --plan 11 --domain asi,dfi
fdc deliver --root DIR --plan 11 --request 1234
fdc acknowledge --root DIR --plan 11 --request 1234 --system asi
fdc ingest --root DIR --plan 11 --domain asi --state drained --generation 3 --observed 4 \
    --payload-digest <hex> --residual-count 0 --source asi
fdc residuals --root DIR --plan 11
fdc evaluate --root DIR --plan 11
fdc explain --root DIR --plan 11
fdc grant --root DIR --plan 11 --by operator
fdc fence --root DIR --plan 11 --reason operator-fence --detail "window withdrawn"
fdc cancel --root DIR --plan 11 --reason "maintenance cancelled"
fdc export --root DIR --json
fdc store inspect --root DIR
```

Exit codes: 0 success, 2 usage, 3 rejected by the coordinator (the token and
detail are printed), 4 durable or filesystem failure. A rejection is never
printed as a success line.

The consumer file is line oriented: `#` starts a comment, blank lines are
ignored, and every other line is
`<category> <obligation-id> <generation> <reservation> <mandatory|advisory> [label] [source]`.

One operational note that follows from the authority model: the tool opens a
session per invocation, and every read-write open is a restart, so a grant made
by one invocation is not live in the next read-write invocation. Read-only
commands still report the recorded grant and its liveness from the durable facts.

## Examples

Eight programs under `examples/`, each deterministic and each demonstrating
real behaviour: plan creation and binding; enumeration and bounded issuing;
ingesting completion evidence; the residual ledger counting unknowns separately;
a granted verdict; cancellation and fencing; restart, recovery and replay of a
staged request against a real temporary store; and a denied verdict explained.

## Benchmarks

```sh
fdc_benchmark --quick        # the smoke run CTest uses
fdc_benchmark                # the full run
```

The benchmark measures **completed operations**, never submission latency.
Durable numbers include the whole publish cost: staging, flush, read back
verification and pointer replacement. The inputs are generated plans and
generated evidence -- **SYNTHETIC**: no fleet, no network, no hardware. The
library itself is not a simulation of anything, but the *workload* is generated.

One measured run on this development machine (Release, MSVC 19.44.35222.0,
x64, Windows 11 26100, local SSD; wall clock, single process):

| Case | Completed | Wall ms | Ops/s |
| --- | --- | --- | --- |
| (a) ephemeral plan creation | 2048 | 2344.144 | 873.7 |
| (b) durable plan creation | 256 | 2877.420 | 89.0 |
| (c) durable enumeration ingestion | 256 | 3062.280 | 83.6 |
| (d) durable completion ingestion | 256 | 4374.472 | 58.5 |
| (e) durable restart and recovery, 256 plans in the store | 32 | 171.600 | 186.5 |

These are measurements of one configuration on one machine, published as a
description of the implementation's cost profile. No before/after comparison is
claimed and no improvement is claimed, because no controlled pair of runs was
performed.

## Validation performed

Everything below was run on the machine described above. Claims about processes,
filesystems, locking, durability and packaging are **REAL**: they were proven
with real operating system processes and real files.

| Configuration | Result |
| --- | --- |
| Release (Ninja, MSVC, /W4 /WX) | 0 warnings, 13/13 CTest entries, 283/283 tests |
| Debug (Ninja, MSVC, /W4 /WX) | 0 warnings, 13/13 CTest entries, 283/283 tests |
| AddressSanitizer (/fsanitize=address, RelWithDebInfo) | 13/13 CTest entries, 283/283 tests |
| Installed package consumer (out of tree, find_package) | 1/1 |

What the 283 tests cover:

* **Unit and integration** -- identities, digests, limits, scopes, consumers,
  plans, lifecycle, enumeration, requests, evidence, residuals, the verdict, the
  canonical report and the internal codec.
* **Property tests with a deterministic seed** (0xFDCD1234) -- a randomized state
  machine over the real API that checks the documented invariants after every
  accepted step, proves that the same seed reproduces the same trace byte for
  byte, and proves that a durable state survives a restart after any random
  prefix.
* **Adversarial input** -- the largest representable identifiers, hostile text
  (overlong and truncated UTF-8, stray continuation bytes, surrogates, embedded
  NUL, C0 controls, DEL), duplicated identities, a file where a store directory
  belongs, unrelated files next to a store, alternate data stream paths and
  bounds too small to hold the state.
* **Corruption** -- a flipped bit in the payload, a flipped bit in the pointer, a
  truncated generation, appended trailing bytes, a broken magic, a pointer that
  names a missing generation, an unpublished generation, and malformed
  generation file names that must be ignored rather than interpreted.
* **Crash consistency, in process** -- fault injection at every commit point,
  proving that a publish either becomes the next whole generation or leaves the
  previous one exactly as it was, that a failed publish consumes no commit
  sequence, and that a truncated or corrupted staged file is caught by the read
  back verification.
* **Crash consistency and locking, across real processes** -- a live writer
  excludes a second process and losing the holder releases the lock; a process
  killed between staging a request and confirming its delivery is re-offered the
  same idempotency key; killing a writer mid-publish never yields a hybrid store;
  and a process killed at each of five commit points leaves exactly one whole
  generation or refuses to guess.
* **Concurrency** -- eight threads creating plans lose no mutation, eight threads
  issuing the same request stage exactly one, readers never observe a half
  applied mutation, a second writer thread is locked out, and repeated
  open/close releases the lock every time.

Defects found and fixed during hardening, rather than asserted away:

1. **A caller could publish a state the reader would refuse.** Unknown
   enumeration values reached an array index, and a bad coverage state was
   recorded rather than refused. Every enumeration field is now checked
   exhaustively at the API boundary, and the encoder refuses such a state as
   well, so a payload the same build cannot read can never be published.
2. **A store directory holding only its own lock file became unusable** after a
   session that never published, because the lock was treated as evidence of a
   lost generation. It is now recognised as the store's own file.
3. **A Windows byte-range lock over the whole file made the holder's own record
   unreadable**, because a Windows range lock is mandatory for I/O. The lock now
   covers one reserved byte far beyond anything the store writes, so a second
   writer is still excluded while the holder record stays readable.
4. **The Error constructor was ambiguous for string literals** (two overloads of
   equal rank), which would have made every diagnostic a compile error.
5. **An over-strict limits rule** rejected a store reopened with bounds larger
   than the ones it was written under. The rule is now one directional, which is
   what the durability model actually requires.
6. **AddressSanitizer found two memory safety defects in the test fixtures** --
   an explicit string length one byte longer than its literal, and a reference
   bound into a temporary returned by value. Both were fixed; the library itself
   reported no sanitizer finding.

Not validated here: **plant and hardware behaviour is not implemented at all**,
so there is nothing to validate. There is no accelerator, switch, PDU or cooling
actuator in this repository, and no claim is made about any. The obligations,
manifests and reports used by the tests and the benchmark are generated inputs
standing in for owning systems that are not part of this repository.

## Installation and downstream consumption

```sh
cmake --install build --prefix /some/prefix
```

The install exports a namespaced target, the 20 public headers, the command line
tool and the Apache licence and notice:

```cmake
find_package(FacilityDrainCoordinator 1.0 REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE FacilityDrainCoordinator::facility_drain_coordinator)
```

`tests/package_consumer/` is an independent project that configures only
against an installed prefix, links the exported target, and drives the installed
library end to end: plan, both enumerations, bounded requests, delivery
confirmation, acknowledgements, evidence from one domain, a denied verdict that
names the other domain, evidence from that domain, and a granted verdict. It is
registered as the `fdc.package.consumer` test and passes.

## Limits and honest caveats

* The durable payload is a whole state snapshot: a mutation serialises the whole
  store. That is simple and it is what makes crash consistency easy to argue, but
  it bounds practical store sizes to the thousands of plans, which is the scale
  this repository targets.
* Evidence is retained for the newest N records per domain (default 4096) and
  older ones are dropped, because evidence is an observation stream rather than
  user data. Drain requests and residuals are never dropped: discarding either
  could hide a duplicate drain or an unresolved obligation.
* A caller that supplies an observation sequence at its maximum freezes that
  plan: the counter cannot advance, and the mutation is refused rather than
  wrapping.
* POSIX builds compile from the same sources, but the file and lock layer was
  exercised on Windows only. The POSIX branches are written against documented
  POSIX semantics and were reviewed by hand; they have not been run here.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
