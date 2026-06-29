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

- Shared memory device
- `mmap()` support
- Ping-pong latency benchmark

