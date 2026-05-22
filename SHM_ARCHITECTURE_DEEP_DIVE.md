# SHM Ring Buffer: Architecture & Design Deep-Dive

This document provides a "pin-to-pin" explanation of the Shared Memory (SHM) Ring Buffer implementation. It details the low-level synchronization primitives, memory layout, and the specific high-performance problems this architecture solves.

---

## 1. Core Problems Solved

In high-speed video IPC (e.g., 30 FPS at 1080p), simple pipes or sockets fail due to CPU overhead. Basic Shared Memory (single-buffer) fails due to the following two "Production Killers":

### A. The "Torn Read" Problem
**Problem**: The writer starts writing frame #2 while the reader is halfway through reading frame #1. The reader gets a hybrid image (top half of frame 2, bottom half of frame 1), leading to "Data Corruption."
**Solution**: **Atomic Sequence Counters** (Seqlocks).

### B. The "Slow Consumer" Crash (SIGBUS)
**Problem**: A slow reader (like an AI model) is accessing the memory. If the writer decides to delete, resize, or drastically overwrite that memory page, the reader’s memory pointer becomes invalid, causing a **Bus Error (SIGBUS)** and a hard crash.
**Solution**: **Reader Pinning** (Reference Counting).

---

## 2. Memory Layout Explained

The buffer is a single contiguous block of physical RAM mapped into the virtual address space of multiple processes.

### 2.1. Packing & Alignment (`#pragma pack(1)`)
**Why?** Compilers normally add "padding" bytes between `uint32_t` and `uint64_t` for alignment. This is dangerous in IPC because different languages (C++ vs Python) or different compilers might add different amounts of padding. 
**Design Choice**: We force **1-byte alignment**. This ensures that the memory offset for `timestamp_us` is *identical* whether you are reading it from C++ or `struct.unpack` in Python.

### 2.2. The Structure
| Component | Purpose |
| :--- | :--- |
| **RingBufferHeader** | Global metadata (number of slots, write cursor). |
| **SlotMetadata** | Per-slot status (Atomics for sequence and reader count). |
| **Raw Data Portion** | The actual BGR/RGB pixel bytes. |

---

## 3. Critical Parts: The "Magic" in the Code

### 3.1. The Sequence Counter (`frame_seq`)
This is an atomic `uint64_t`. 
- **Odd Number**: Signals that a write is currently in progress.
- **Even Number**: Signals that the data is stable and safe to read.
- **Problem Solved**: It allows the Reader to detect if the Writer "lapped" it during a memory copy without using a slow Mutex.

### 3.2. Reader Pinning (`reader_count`)
An atomic `uint32_t` per slot.
- Before reading, the Consumer increments this.
- After reading, the Consumer decrements this.
- **Problem Solved**: The Writer checks this *before* writing to a slot. If `reader_count > 0`, the writer knows a slow process is still looking at this memory. The writer will wait or drop the frame rather than crashing the reader.

### 3.3. Memory Ordering (`std::memory_order_acquire/release`)
We don't use the default (heavy) `memory_order_seq_cst`.
- **Release**: Ensures all previous memory writes (the pixels) are visible to other CPUs before the sequence counter is updated.
- **Acquire**: Ensures the CPU doesn't try to read the pixels until it has successfully read the sequence counter.
- **Problem Solved**: Prevents "CPU Instruction Reordering" from breaking the logic on multi-core systems (especially ARM/Graviton).

---

## 4. The Step-by-Step Logic

### The Producer (Writer) Protocol
1. Calculate `next_idx = (write_index + 1) % N`.
2. Check `reader_count` for `next_idx`.
3. If busy, wait 100 microseconds (up to 10 times).
4. If still busy, **Drop Frame** (Better to lose a frame than crash the system).
5. Increment `seq` to **Odd** (Signals: "Don't touch this memory!").
6. `memcpy` the pixels.
7. Increment `seq` to **Even** (Signals: "Data is ready!").
8. Update global `write_index`.

### The Consumer (Reader) Protocol
1. Load global `write_index`.
2. Read `seq_before`. If **Odd**, abort (Writer is currently active).
3. Increment `reader_count` (Pin the slot).
4. Read `seq_check`. If it changed, abort (Writer started just as we arrived).
5. `memcpy` pixels to local memory.
6. Decrement `reader_count` (Unpin the slot).
7. Read `seq_after`. If `seq_after != seq_before`, **Discard Data** (Corruption detected).

---

## 5. Interview Deep-Dive (Tough Questions)

If an interviewer sees this code, they will ask these "Senior Level" questions:

**Q1: Why use Atomics instead of a POSIX Semaphore or Mutex?**
*   **Answer**: Performance and Reliability. Mutexes involve kernel syscalls and context switches. If a process holding a Mutex crashes, the whole system deadlocks. Atomics stay in the CPU cache and have no "owner," making the system fault-tolerant.

**Q2: What happens if a Reader process is killed (`kill -9`) while it has a slot pinned?**
*   **Answer**: This is the one "leaky" edge case. The `reader_count` will stay at 1, and the Writer will eventually consider that slot permanently "busy." (In a 30-slot buffer, this is acceptable; in a move to "Ultra-Production," we would add a watchdog or a timestamp to the slot to auto-reset stale pins).

**Q3: How do you handle "False Sharing" in the `SlotMetadata`?**
*   **Answer**: Because `reader_count` and `frame_seq` are in the same struct, they likely sit on the same **CPU Cache Line (64 bytes)**. This could cause "Cache Ping-Pong" between the writer and reader CPU cores. (Follow-up: "I would add padding or `alignas(64)` if I needed to squeeze out an extra 5% performance").

**Q4: Why is `memcpy` done inside the sequence check rather than using a lock?**
*   **Answer**: This is **Optimistic Concurrency Control**. We assume the read will succeed. If it doesn't (detected by `seq_after`), we just throw away the work. For video, a discarded frame is better than a blocked pipeline.

**Q5: How does this architecture handle a Producer that is 10x faster than the Consumer?**
*   **Answer**: The Writer will see the `reader_count` is pinned, try to wait, and then increment `total_frames_dropped`. The system remains stable (no crashes), but the consumer only sees a subset of the frames (e.g., every 10th frame).
