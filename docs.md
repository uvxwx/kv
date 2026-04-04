Todo
1. tests/CMakeLists.txt as in webshotd
2. reorder fields in structs to save space
3. handle cancellation 

Ordered by importance
1. Minimize aborts.
2. Fairness is last.

Knowledge
1. eng::Mutex exists to protect metadata: writer, amountReaders, ...

v2
1. TTL
2. waitForChange
3. Faster hash tables: abseil, boost, ...
4. ConcurrentVariable instead of `unique_lock`s

v3
1. Bound Store::locks size by removing unused locks: implement safe erasure.
2. Fairness

v4
1. Graph-based deadlock detector?

