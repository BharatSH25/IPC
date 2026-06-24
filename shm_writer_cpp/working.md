# How the SHM Ring Buffer Works: A Dry Run

This document explains the step-by-step logic of the `SHMWriter_Ring_Buffer` using a simplified example with **3 Slots**.

## Initial State
*   **Slots**: `[0]`, `[1]`, `[2]`
*   **Global `write_index`**: `0`
*   **Memory Layout**: 
    - Header (at base)
    - Slot 0 Metadata (seq=0, readers=0) + Frame Data
    - Slot 1 Metadata (seq=0, readers=0) + Frame Data
    - Slot 2 Metadata (seq=0, readers=0) + Frame Data

---

## 🟢 Arrival of Frame 1 (Writing Phase)

1.  **Determine Slot**: The code looks at the current `write_index` (0) and calculates the next slot: `(0 + 1) % 3 = 1`.
2.  **Check for Readers**: It checks `Slot[1].reader_count`. It is `0`, so it's safe to proceed.
3.  **Lock for Writing**: It increments the sequence number of Slot 1 from `0` (even/stable) to `1` (odd/writing).
    > [!NOTE]
    > If a reader tries to grab Slot 1 now, it will see `seq=1` and know the data is "dirty."
4.  **Copy Data**: `std::memcpy` copies the BGR pixels from the camera into Slot 1's memory space.
5.  **Update Metadata**: Sets `width`, `height`, `timestamp`, and `frame_number = 1`.
6.  **Unlock**: Increments sequence number from `1` to `2`. It is now even again (stable).
7.  **Publish**: Updates the global `write_index` to `1`. 
    > [!TIP]
    > Only now do consumers see that Slot 1 is the "Latest Frame."

**Result**: `write_index = 1`. Slots: `[0: empty]`, `[1: Frame 1]`, `[2: empty]`.

---

## 🟡 Arrival of Frame 2

1.  **Determine Slot**: `(1 + 1) % 3 = 2`.
2.  **Safety Check**: `Slot[2].reader_count` is 0.
3.  **Process**:
    - `Slot[2].seq` becomes `1` (writing).
    - Data copied.
    - `Slot[2].seq` becomes `2` (stable).
4.  **Publish**: Global `write_index` becomes `2`.

**Result**: `write_index = 2`. Slots: `[0: empty]`, `[1: Frame 1]`, `[2: Frame 2]`.

---

## 🔵 Arrival of Frame 3 (The Wrap-Around)

1.  **Determine Slot**: `(2 + 1) % 3 = 0`.
2.  **Process**:
    - `Slot[0].seq` becomes `1` (writing).
    - Data copied.
    - `Slot[0].seq` becomes `2` (stable).
3.  **Publish**: Global `write_index` becomes `0`.

**Result**: `write_index = 0`. The buffer is "full," but since it's a ring, it just keeps going.

---

## 🔴 Collision Scenario: Slow Consumer

Imagine a **Model Runner (Reader)** is currently processing **Frame 1** in **Slot 1**.
*   `Slot[1].reader_count = 1`.

### Arrival of Frame 4
1.  **Determine Slot**: `(0 + 1) % 3 = 1`.
2.  **Safety Check**: The writer sees `Slot[1].reader_count` is **not zero**.
3.  **Wait Loop**: The writer sleeps for 100 microseconds and tries again (up to 10 times).
4.  **Drop Action**: If the Model Runner is still reading after 1ms, the writer says: 
    *"I can't wait anymore, or the camera stream will lag."*
5.  **Result**: 
    - `header->total_frames_dropped++`
    - Frame 4 is deleted/ignored.
    - `write_index` stays at `0`.

---

## Why this is "Truly" a Ring Buffer

1.  **Zero Memory Shifts**: We never `memmove` old frames. We just change where the pointer points.
2.  **Multi-Process Safe**: Because we use `SlotMetadata` inside the Shared Memory itself, a completely different process (like a Python Script or an FFmpeg command) can see exactly which slot is being written to and avoid it.
3.  **Continuous Flow**: The `write_index` moves in a never-ending circle: `0 -> 1 -> 2 -> 0 -> 1 -> 2 ...`
