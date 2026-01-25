# Mars Memory Allocator

A **fault‑tolerant custom memory allocator written in C**, designed for a Mars rover–style environment where **radiation, bit flips, and unreliable writes** are real constraints. This project goes far beyond a standard `malloc/free` implementation by aggressively validating memory integrity and quarantining corrupted blocks instead of failing silently.

This allocator was built as a systems‑level exercise in **low‑level memory management, defensive programming, and reliability engineering**.

---

## Key Features

### 🛰 Radiation & Corruption Detection
- **Checksummed headers and footers** to detect bit flips
- **Mirrored footers** for redundancy
- Automatic **quarantine and removal of corrupted blocks**

### 🧠 Secure Allocation Strategy
- **Best‑fit allocation** to reduce fragmentation
- Explicit block **splitting and coalescing**
- Alignment‑aware allocations

### 🛡 Memory Safety Mechanisms
- **Payload canaries** to detect buffer overflows
- **Double‑free detection**
- Bounds‑checked reads and writes

### 🔁 Reliable Writes (Brownout Protection)
- Write → read → verify loop
- Retries writes up to 3 times
- Treats persistent write failure as memory corruption

### 🔄 Optimised `realloc`
- In‑place shrinking and growing when possible
- Neighbor block merging
- Fallback to malloc–copy–free only when required

---

## Public API

```c
void* mm_malloc(size_t size);
void  mm_free(void* ptr);
void* mm_realloc(void* ptr, size_t new_size);
int   mm_read(void* ptr, size_t offset, void* buf, size_t len);
int   mm_write(void* ptr, size_t offset, const void* src, size_t len);
```

Unlike standard allocators:
- Reads and writes **validate block integrity before accessing memory**
- Writes verify persistence before returning success

---

## How It Works (High‑Level)

1. Memory is managed as a **doubly‑linked list of blocks**
2. Each block contains:
   - Header (metadata + checksum)
   - User payload
   - Canary
   - Mirrored footer
3. On every traversal or access:
   - Checksums and footers are validated
   - Corruption triggers immediate block removal
4. Free blocks are coalesced to limit fragmentation

This design prioritises **correctness and survivability over raw speed**.

---

## Build Instructions

```bash
make
```

This produces:
- `liballocator.so` – shared allocator library
- `runme` – test harness

---

## Running the Demo

```bash
./runme
```

The test program exercises:
- Allocation and deallocation
- Safe reads and writes
- Reallocation behavior
- Corruption detection paths

---

## Project Structure

```
.
├── allocator.c     # Core allocator implementation
├── allocator.h     # Public API and data structures
├── runme.c         # Test harness
├── Makefile        # Build configuration
├── liballocator.so # Compiled shared library
```

---

## Design Philosophy

This allocator assumes:
- Memory **can and will** be corrupted
- Silent failure is worse than aggressive detection
- Recoverability matters more than performance

It is intentionally over‑engineered to demonstrate **systems thinking**, not to replace `glibc malloc`.

---

## Skills Demonstrated

- Low‑level C and pointer arithmetic
- Custom memory management
- Defensive systems programming
- Data integrity verification
- Debugging and reasoning about undefined behavior

---

## Disclaimer

This project is **not intended for production use**. It is a learning and demonstration project focused on correctness, safety, and robustness under extreme assumptions.

---

## Author

Hamzah Ibrahim

Computer Science student | Systems & Low‑level Programming Enthusiast
