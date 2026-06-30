# Shared Memory IPC Kernel Module

A Linux kernel module exposing a shared memory region through `mmap()` for
inter-process communication.

Design inspired by [ivshmem](https://github.com/siemens/jailhouse/blob/master/Documentation/ivshmem-v2-specification.md).

## Shared Memory Region Layout
```
+-----------------------------+   -
|                             |   :
| Output Section for peer n-1 |   : Output Section Size
|     (n = Maximum Peers)     |   :
+-----------------------------+   -
:                             :
:                             :
:                             :
+-----------------------------+   -
|                             |   :
|  Output Section for peer 1  |   : Output Section Size
|                             |   :
+-----------------------------+   -
|                             |   :
|  Output Section for peer 0  |   : Output Section Size
|                             |   :
+-----------------------------+   -
|                             |   :
|     Read/Write Section      |   : R/W Section Size
|                             |   :
+-----------------------------+   -
|                             |   :
|         State Table         |   : State Table Size
|                             |   :
+-----------------------------+   <-- Shared memory base address
```
The first section consists of the mandatory State Table. Its size is defined by the State Table Size register and cannot be zero. This section is read-only for all peers. It contains a data structure for peers-discovery

The second section consists of shared memory that is read/writable for all peers. Its size is defined by the R/W Section Size register. A size of zero is permitted.

The third and following sections are output sections, one for each peer. Their sizes are all identical. An output section is read/writable for the corresponding peer and read-only for all other peers.

All sizes have to be rounded up to multiples of a mappable page in order to allow access control according to the section restrictions.

## Features

The kernel module implements a shared-memory IPC mechanism exposed through the character device `/dev/shmipc`.

Each process that opens the device becomes a **peer** in the communication system. During `open()`, the module assigns the process a unique peer identifier that remains valid until the device is closed.

The module allocates a single shared memory region that is mapped into every peer's address space. Although every peer maps the same underlying memory, different protection permissions are applied to different parts of the mapping, allowing the kernel to enforce ownership of private output buffers.

The module implements three main file operations:

- `open()` / `release()` for peer registration and cleanup
- `mmap()` for mapping the shared memory
- `ioctl()` for querying the shared-memory layout


## Benchmarking

The kernel module performance have been evaluated by measuring the RTT between two processes. We compare the perfornce against the `MMapShared` module studied during the lectures.

The performances over **100000** samples are the following:

```
=== shmipc ping-pong RTT (rounds=100000, warmup=1000) ===
min:     1445265 ns
p50:     1999334 ns
mean:    2015477 ns
p90:     2020130 ns
p99:     2977351 ns
p99.9:   3020894 ns
max:     7761558 ns
rounds/sec (1/mean): 496


=== mmap-test ping-pong RTT (rounds=100000, warmup=1000) ===
min:     1712224 ns
p50:     1999420 ns
mean:    2017991 ns
p90:     2014717 ns
p99:     2984858 ns
p99.9:   3045232 ns
max:     4864800 ns
rounds/sec (1/mean): 496

```

### Contributors
+ **Tommaso Baldi**, *SSSA*, [email](mailto:tommaso.baldi@santannapisa.it)