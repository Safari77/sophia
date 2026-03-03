<p align="center">
  <img src="https://github.com/pmwkaa/sophia/blob/master/documentation/sophia.png"/>
</p>

Sophia is advanced transactional [MVCC](http://en.wikipedia.org/wiki/Multiversion_concurrency_control)
key-value/row storage library.

Sophia is RAM-Disk hybrid storage. It is designed to provide best possible on-disk performance without degradation
in time. It has guaranteed *O(1)* worst case complexity for read, write and range scan operations.

**Features**

* Full ACID compliancy
* MVCC engine
* Optimistic, non-blocking concurrency with N-writers and M-readers
* Pure Append-Only
* Unique data storage architecture
* Fast: O(1) worst for read, write and range scan operations
* Multi-threaded compaction
* Multi-databases support (sharing a single write-ahead log)
* Multi-Statement and Single-Statement Transactions (cross-database)
* Serialized Snapshot Isolation (SSI)
* Optimized storage schema (numeric types has zero-cost storage)
* Can be used to build Secondary Indexes
* Upsert (fast write-only 'update or insert' operation)
* Consistent Cursors
* Prefix search
* Automatic garbage-collection
* Automatic key-expire
* Hot Backup
* Compression (no fixed-size blocks, no-holes, supported: lz4, zstd)
* Direct IO support
* Use mmap or pread access methods
* Simple and easy to use (minimalistic API, FFI-friendly, amalgamated)
* Implemented as small *C-written* library with zero dependencies
* Carefully tested
* Open Source Software, BSD

---

The project is no longer developed, but it is still interesting in terms of the modern storage engine design evolution and architectural choices.

Storage engines of [Amelia](https://amelielabs.io) and [Monotone](https://monotone.studio) are heavily inspired by Sophia.

---

Version 1.1 (first design approach)
-----------

Original Sophia's architecture combines a region in-memory index with an in-memory key index.

A region index is represented by an ordered range of regions with their min and max
keys and latest on-disk reference. Regions never overlap.

These regions have the same semantical meaning as the B-Tree pages, but designed differently.
They do not have a tree structure or any internal page-to-page relationships, thus no meta-data
overhead (specifically to append-only B-Tree).

A single region on-disk holds keys with values. As a B-tree page, region has its maximum
key count. Regions are uniquely identified by region id number that makes them trackable in future.

Key index is very similar to LSM zero-level (memtable), but has a different key lifecycle.
All modifications first get inserted into the index and then hold up until they are
explicitly removed by merger.

<p align="center">
  <img src="https://github.com/pmwkaa/sophia/blob/master/documentation/arch/v11.png"/>
</p>

Lifecycle
---------

The database update lifecycle is organized in terms of epochs. Epoch lifetime is set in
terms of key updates. When the update counter reaches an epoch's watermark number then
the Rotation event happen.

Each epoch, depending on its state, is associated with a single log file or database file.
Before getting added to the in-memory index, modifications are first written to the
epoch's write-ahead log.

On each rotation event:

1. current epoch, so called 'live', is marked as 'transfer' which leads to a new 'live' epoch creation (new log file)
2. create new in-memory key index and swap it with current one
3. merger thread is being woken up

The merger thread is the core part that is responsible for region merging and garbage
collecting of the old regions and older epochs. On the wakeup, the merger thread
iterates through list of epochs marked as 'transfer' and starts the
merge procedure.

The merge procedure has the following steps:

1. create new database file for the latest 'transfer' epoch
2. fetch any keys from in-memory index that associated with a single destination region
3. start the merge for each fetched key and origin region, then write a new region to the database file
4. on each completed region (current merged key count is less or equal to max region key count):
  1. allocate new split region for region index, set min and max
  2. first region always has id of origin destination region
  3. link region and schedule for future commit
5. on origin region update completion:
  1. update destination region index file reference to the current epoch and insert split regions
  2. remove keys from key index
6. start step (2) until there is no updates left
7. start garbage collector
8. sync database with a disk drive, then, if everything went well, remove all 'transfer' epochs (log files) and gc'ed databases
9. free index memory

Garbage Collection
------------------

The garbage collector has a simple design.

All that you need is to track an epoch's total region count
and the count of transfered regions during merge procedure. Thus, if some older
epoch database has fewer than 70% (or any other changeable factor) live regions,
they just get copied to the current epoch database file while the
old one is being deleted.

On database recovery, Sophia tracks and builds an index of pages from the earliest
epochs (biggest numbers) down to the oldest.
Log files then are being replayed and epochs become marked as 'transfer'.

Algorithmic Complexity
----------------------

Sophia has been evaluated as having the following complexity (in terms of disk accesses):

**set** worst case is O(1) append-only key write + in-memory index insert

**delete** worst case is O(1) (delete operation is equal to set)

**get** worst case is O(1) random region read, which itself does amortized
O(log region\_key\_count) key compares + in-memory key index search + in-memory
region search

**range** range queries are very fast due to the fact that each iteration needs to
compare no more than two keys without a search, and access through mmaped
database. Roughly complexity can be equally evaluated as sequential reading
of the mmaped file.


Version 1.2 (latest design)
-----------

There have been major changes in storage architecture since version 1.1.

Version 1.1 defines strict 2-level storage model between in-memory and
disk. It gives worst-case guarantee O(1) for any ordered key read in
terms of disk access.

This approach had its limitations, since it was unable to efficiently
maintain memory limit with required performance.
Additionally there was a need for multi-threaded compaction.

Sophia has evolved in a way that expands original ideas.
Architecture has been designed to efficiently work with memory and large amount of keys.

In fact, it became even simplier.

Design
------

Sophia's main storage unit is a Node.

Each Node represents a single file with associated in-memory region index and two
in-memory key indexes. Node file consists of Branches.

Each Branch consists of a set of sorted Regions and Region Index.

A single Region holds keys with values. It has the same semantical meaning as a
B-Tree page, but organized in a different way.
It does not have a tree structure or any internal page-to-page relationships and thus no
meta-data overhead.

A Region Index is represented by an ordered range of regions with their min
and max keys and on-disk reference. Regions never overlap.

A Key Index is very similar to LSM zero-level (memtable), but has
a different key lifecycle. All modifications first get into the index and hold up
until they are explicitly removed by the merger.

Before getting added to the in-memory Key Index, modifications are first written
to the Write-Ahead Log.

<p align="center">
  <img src="https://github.com/pmwkaa/sophia/blob/master/documentation/arch/v12.png"/>
</p>

Lifecycle
---------

Database lifecycle is organized in terms of two major operations: Branch and Compaction.

When a Node's in-memory Key Index size reaches a special watermark value or global
memory limit, Branch operation is scheduled.

When some Node branch counter reaches a special watermark value, Compaction operation is scheduled.

**Branch operation**

1. rotate in-memory Key Index (use second one empty)
(Node updates during Branch goes to second index)
2. create new Regions and Region Index from Key Index
3. create new Node Branch
4. sequentially write Branch to the end of Node file
5. sync
6. free index and rotate

**Compaction operation**

1. sequentially read Node file into memory
2. create iterator for each Branch
3. merge and split key-stream:
  1. create one or more Nodes
  2. delete Node
4. sequentially write new Node or Nodes
5. sync
6. redistribute online updates between new Nodes
7. remove old Node
8. rename new Node or Nodes when completed

Optimization Game
-----------------

All background operations are planned by special scheduler.

There is a game between available memory, a number of Branches and Search times.

Each additional branch says that there is a possible additional disk access during the search.
During the search, only branch regions that have min >= key <= max are examined.
In the same time, it is unable to maintain memory limits without branching,
because compaction times are greater than possible rate of incoming data.

Sophia is designed to be read optimized. There is a high possibility that latest created Branches
(hot data) are stored in the file system cache. Scheduler is aware about nodes which have
largest in-memory Key Index and biggest number of Branches. These are processed first.

Additionally all operations are planned taking current system state in account, like
memory usage statistics, current load profiler (redzone), operations priorities, checkpoint, backup, etc.

Sophia compaction is multi-threaded. Each worker (thread) requests scheduler for a new task.
Basic unit of a background task is an operation on a single Node.

Sophia is designed to efficiently utilize available memory.
If there is more memory available, then branch/compaction operations become more infrequent and
system becomes more disk-efficient. Best performance can be obtained with no memory limit set. 
Sophia is Hard-Drive (and Flash) friendly, since all operations are delayed and
executed in large sequential reads and writes, without overwrite.

Garbage Collection
------------------

Garbage collection (MVCC related) is executed automatically by Compaction task.

Also, scheduler periodically checks if there are any nodes which have large
percentage of transactional versions (duplicates) stored per node.

Algorithmic Complexity
----------------------

Sophia has following algorithmic complexity (in terms of disk access):

**set** worst case is O(1) write-ahead-log append-only key write  + in-memory node index search + in-memory index insert

**delete** worst case is O(1) (delete operation is equal to set)

**get** worst case is amortized O(max\_branch\_count\_per\_node) random region read from a single node file, which itself does in-memory key index search + in-memory region search

**range** worst case of full database traversal is amortized O(total\_region\_count) + in-memory key-index searches for each node
