# Prodsim — Produktionsumgebung, so nah wie dieser Code sie wirklich kann

> Stand: 09/2026. Kein Multi-DC-Kubernetes, kein Jepsen-Clojure-Harness.
> Ziel: deterministisch testbare Fehler, die echte Cluster haben
> (Partition, Drop, Leader-Crash, Clock-Skew, Catch-up nach Heal).

## Schichten

| Schicht | Was | Was nicht |
|---|---|---|
| **1. In-process Cluster** | `RaftGroup` RF=3, 3×3 Partition/Drop-Matrix, Default fully meshed. `isolate`/`heal`/`set_drop_rate`/`can_send`. Votes und Replikation zählen nur erreichbare Lebende. | Kein asynchrones Netz, keine RTT, keine NIC. Replikation bleibt ein Methodenaufruf unter Mutex. |
| **2. TCP-Loopback-Hosts** | `tools/prodsim_cluster.cpp`: drei `TcpLoopbackPair` auf 127.0.0.1 plus `fork()`-Roundtrip. Partition = `close(fd)`, Heal = reconnect. Wire = `EncodeEntryWire`/`SendWire`/`RecvWire`. | Kein Raft-over-TCP, keine zweite Consensus-Implementierung. Die Matrix in Schicht 1 ist die Wahrheit. |
| **3. Echtes Prod** | bräuchte `netem`/`tc`, mehrere Maschinen, Jepsen, Disk-full, langsames `fsync`, Membership-Change, WAN-RTT, echte Kill -9 gegen OS-Prozesse. | Diese Repo-Stufe liefert das **nicht**. k8s StatefulSet bleibt `replicas: 1`. |

`alive` und Partition sind getrennt: `killNode` = Prozess tot (`alive=false`); Partition = Prozess lebt, Pakete kommen nicht an.

## Laufen

```bash
cmake --build build -j24
ctest --test-dir build -R prodsim --output-on-failure
# oder gezielt:
./build/test_prodsim
./build/prodsim_cluster
```

`test_prodsim` ist sleep-frei (Fake-Clock via `tick(now_ms)`). Laufzeit: Sekunden, nicht Minuten.

## Invarianten (müssen laut schreien, wenn sie fallen)

- Höchstens ein **committeter** Leader pro Majority-Term. Isolierte Minderheit darf wählen, aber ohne 2/3-Votes nicht gewinnen.
- `append` auf einem von beiden Followern getrennten Leader gibt `0` zurück und darf `commit_index` nicht vorschieben (uncommitted Tail ist erlaubt, committed Daten nicht).
- Committed Log-Prefix überlebt SaveLog/LoadLog (Kill-9-Stand-in; echtes SIGKILL ist im Unit-Test nicht zuverlässig).
- Nach Heal: Catch-up, angewandte Maps der Majority stimmen; kein gegabelter committed Verlauf.
- Drop (PRNG Seed 42) verliert einzelne Calls, kein Dauer-Cut. Keys mit `append()!=0` gehen im committed Prefix nicht verloren.

Szenarien in `tests/test_prodsim.cpp`: A Majority-Commit, B Split-Brain, C 1+1+1, D Log-Persistenz, E HLC-Skew, F Drop, G Catch-up, H Linearisierbarkeitsskizze (ein Key, nur Ack=`append()!=0`).

## Non-Goals

- Macht **kein** k8s-HA real.
- Ersetzt **kein** Jepsen auf echten Hosts.
- WAL-Torn-Tail bleibt `tests/test_chaos.cpp` — hier nicht verdoppelt.
- Graph/SQL/HNSW sind absichtlich außen vor.
