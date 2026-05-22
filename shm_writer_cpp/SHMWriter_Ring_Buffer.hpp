#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <chrono>
#include <atomic>
#include <string>
#include <cstdint>
#include <stdexcept>
#include <thread>

// ============================================================================
// Ring Buffer Shared Memory Writer for Multi-Consumer Scenarios
// ============================================================================
// This implementation solves the problem of slow consumers (e.g., FFmpeg)
// causing bus errors when frames are overwritten during read operations.
//
// Key Features:
// - Multiple frame slots (configurable, default 30)
// - Reference counting per slot (tracks active readers)
// - Lock-free atomic operations
// - Automatic overflow handling (drop frames if all slots busy)
// - Compatible with multiple consumers at different speeds
//
// Memory Layout:
// [RingBufferHeader][Slot0 Metadata][Slot0 Data][Slot1 Metadata][Slot1 Data]...
// ============================================================================

#pragma pack(push, 1)
struct RingBufferHeader {
    uint32_t num_slots;           // Total number of frame slots
    uint32_t frame_width;          // Maximum frame width
    uint32_t frame_height;         // Maximum frame height
    uint64_t total_frames_written; // Total frames written (for monitoring)
    uint64_t total_frames_dropped; // Total frames dropped due to busy slots
    std::atomic<uint32_t> write_index; // Current write position (atomic)
};

struct SlotMetadata {
    std::atomic<uint64_t> frame_seq;    // Sequence: odd=writing, even=stable
    std::atomic<uint32_t> reader_count; // Number of active readers
    uint32_t width;                      // Actual frame width
    uint32_t height;                     // Actual frame height
    uint64_t frame_number;               // Logical frame number
    uint64_t timestamp_us;               // Timestamp in microseconds
    uint32_t data_size;                  // Actual data size in bytes
    uint32_t _padding;                   // Alignment padding
};
#pragma pack(pop)

class SHMWriter_Ring_Buffer {
public:
    SHMWriter_Ring_Buffer(uint32_t num_slots = 30) 
        : shm_fd(-1), shm_ptr(nullptr), header(nullptr),
          num_slots_(num_slots), slot_data_size(0), total_size(0),
          frame_counter(0), initialized(false) {}

    ~SHMWriter_Ring_Buffer() { cleanup(); }

    // Initialize ring buffer SHM for a specific camera
    // width/height: maximum expected frame dimensions
    // Returns true on success
    bool init(int cam_id, int width, int height) {
        if (initialized) return true;

        shm_name = "/camera_shm_ring_" + std::to_string(cam_id);
        
        // Calculate sizes
        slot_data_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3; // BGR
        size_t slot_total_size = sizeof(SlotMetadata) + slot_data_size;
        total_size = sizeof(RingBufferHeader) + (slot_total_size * num_slots_);

        std::cout << "[SHM Ring] Initializing: " << shm_name 
                  << " with " << num_slots_ << " slots"
                  << " (" << width << "x" << height << ")"
                  << " total size: " << (total_size / 1024 / 1024) << " MB\n";

        // Create/open shared memory
        shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
        if (shm_fd < 0) {
            perror("[SHM Ring] shm_open failed");
            return false;
        }

        // Set size
        if (ftruncate(shm_fd, total_size) != 0) {
            perror("[SHM Ring] ftruncate failed");
            close(shm_fd);
            shm_fd = -1;
            return false;
        }

        // Map to memory
        shm_ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_ptr == MAP_FAILED) {
            perror("[SHM Ring] mmap failed");
            shm_ptr = nullptr;
            close(shm_fd);
            shm_fd = -1;
            return false;
        }

        // Initialize header
        header = reinterpret_cast<RingBufferHeader*>(shm_ptr);
        header->num_slots = num_slots_;
        header->frame_width = width;
        header->frame_height = height;
        header->total_frames_written = 0;
        header->total_frames_dropped = 0;
        header->write_index.store(0, std::memory_order_release);

        // Initialize all slot metadata
        for (uint32_t i = 0; i < num_slots_; i++) {
            SlotMetadata* meta = getSlotMetadata(i);
            meta->frame_seq.store(0, std::memory_order_release);
            meta->reader_count.store(0, std::memory_order_release);
            meta->width = 0;
            meta->height = 0;
            meta->frame_number = 0;
            meta->timestamp_us = 0;
            meta->data_size = 0;
        }

        initialized = true;
        std::cout << "[SHM Ring] Initialized successfully\n";
        return true;
    }

    // Write a frame to the ring buffer
    // Returns true if frame was written, false if dropped
    bool writeFrame(const uint8_t* data, int frame_w, int frame_h) {
        if (!initialized || !data) {
            std::cerr << "[SHM Ring] Not initialized or null data\n";
            return false;
        }

        size_t copy_size = static_cast<size_t>(frame_w) * static_cast<size_t>(frame_h) * 3;
        if (copy_size > slot_data_size) {
            std::cerr << "[SHM Ring] Frame size (" << copy_size 
                      << ") exceeds slot capacity (" << slot_data_size << ")\n";
            return false;
        }

        // Get next write slot
        uint32_t write_idx = header->write_index.load(std::memory_order_acquire);
        uint32_t next_idx = (write_idx + 1) % num_slots_;
        
        SlotMetadata* meta = getSlotMetadata(next_idx);
        uint8_t* slot_data = getSlotData(next_idx);

        // Check if slot is busy (has active readers)
        // Wait briefly, but don't block indefinitely
        int wait_attempts = 0;
        const int max_wait_attempts = 10; // ~10ms max wait
        while (meta->reader_count.load(std::memory_order_acquire) > 0) {
            if (++wait_attempts >= max_wait_attempts) {
                // Slot still busy, drop this frame
                header->total_frames_dropped++;
                std::cerr << "[SHM Ring] Slot " << next_idx 
                          << " busy (readers=" << meta->reader_count.load() 
                          << "), dropping frame. Total dropped: " 
                          << header->total_frames_dropped << "\n";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // Mark slot as write-in-progress (odd sequence number)
        uint64_t current_seq = meta->frame_seq.load(std::memory_order_acquire);
        meta->frame_seq.store(current_seq + 1, std::memory_order_release);

        // Copy frame data
        std::memcpy(slot_data, data, copy_size);

        // Update metadata
        meta->width = frame_w;
        meta->height = frame_h;
        meta->frame_number = ++frame_counter;
        meta->timestamp_us = currentTimeUs();
        meta->data_size = copy_size;

        // Mark slot as stable (even sequence number)
        meta->frame_seq.store(current_seq + 2, std::memory_order_release);

        // Advance write index
        header->write_index.store(next_idx, std::memory_order_release);
        header->total_frames_written++;

        return true;
    }

    // Get statistics
    void getStats(uint64_t& frames_written, uint64_t& frames_dropped) const {
        if (!initialized) {
            frames_written = 0;
            frames_dropped = 0;
            return;
        }
        frames_written = header->total_frames_written;
        frames_dropped = header->total_frames_dropped;
    }

    // Print current buffer status (for debugging)
    void printStatus() const {
        if (!initialized) {
            std::cout << "[SHM Ring] Not initialized\n";
            return;
        }

        std::cout << "[SHM Ring] Status for " << shm_name << ":\n";
        std::cout << "  Write Index: " << header->write_index.load() << "\n";
        std::cout << "  Frames Written: " << header->total_frames_written << "\n";
        std::cout << "  Frames Dropped: " << header->total_frames_dropped << "\n";
        std::cout << "  Slots:\n";
        
        for (uint32_t i = 0; i < num_slots_; i++) {
            SlotMetadata* meta = getSlotMetadata(i);
            std::cout << "    [" << i << "] seq=" << meta->frame_seq.load()
                      << " readers=" << meta->reader_count.load()
                      << " frame#=" << meta->frame_number << "\n";
        }
    }

    // Unlink shared memory (call when removing camera)
    bool unlinkSharedMemory() {
        if (shm_name.empty()) return true;
        
        if (shm_unlink(shm_name.c_str()) == 0) {
            std::cout << "[SHM Ring] Unlinked " << shm_name << "\n";
            return true;
        } else {
            if (errno == ENOENT) return true;
            perror("[SHM Ring] shm_unlink failed");
            return false;
        }
    }

    // Cleanup (munmap + close, but don't unlink)
    void cleanup() {
        if (shm_ptr) {
            munmap(shm_ptr, total_size);
            shm_ptr = nullptr;
            header = nullptr;
        }
        if (shm_fd >= 0) {
            close(shm_fd);
            shm_fd = -1;
        }
        initialized = false;
    }

private:
    std::string shm_name;
    int shm_fd;
    void* shm_ptr;
    RingBufferHeader* header;
    uint32_t num_slots_;
    size_t slot_data_size;    // Size of data portion per slot
    size_t total_size;        // Total SHM size
    uint64_t frame_counter;
    bool initialized;

    // Get pointer to slot metadata
    SlotMetadata* getSlotMetadata(uint32_t slot_idx) const {
        if (slot_idx >= num_slots_) return nullptr;
        
        size_t slot_total_size = sizeof(SlotMetadata) + slot_data_size;
        uint8_t* base = reinterpret_cast<uint8_t*>(shm_ptr) + sizeof(RingBufferHeader);
        return reinterpret_cast<SlotMetadata*>(base + (slot_idx * slot_total_size));
    }

    // Get pointer to slot data
    uint8_t* getSlotData(uint32_t slot_idx) const {
        SlotMetadata* meta = getSlotMetadata(slot_idx);
        if (!meta) return nullptr;
        return reinterpret_cast<uint8_t*>(meta) + sizeof(SlotMetadata);
    }

    uint64_t currentTimeUs() const {
        auto t = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(
            t.time_since_epoch()).count();
    }
};

// ============================================================================
// Reader Class for Consumers
// ============================================================================
// Use this class in your consumer processes (model_runner, clip_saver, etc.)
// to safely read frames from the ring buffer.
// ============================================================================

class SHMReader_Ring_Buffer {
public:
    SHMReader_Ring_Buffer() 
        : shm_fd(-1), shm_ptr(nullptr), header(nullptr),
          total_size(0), last_read_frame_number(0), initialized(false) {}

    ~SHMReader_Ring_Buffer() { cleanup(); }

    // Connect to existing ring buffer SHM
    bool connect(int cam_id) {
        if (initialized) return true;

        shm_name = "/camera_shm_ring_" + std::to_string(cam_id);

        // Open existing shared memory (read-only is safer for readers)
        shm_fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
        if (shm_fd < 0) {
            perror("[SHM Ring Reader] shm_open failed");
            return false;
        }

        // Get size from header (we need to map at least the header first)
        RingBufferHeader temp_header;
        if (pread(shm_fd, &temp_header, sizeof(RingBufferHeader), 0) != sizeof(RingBufferHeader)) {
            perror("[SHM Ring Reader] Failed to read header");
            close(shm_fd);
            shm_fd = -1;
            return false;
        }

        // Calculate total size
        size_t slot_data_size = static_cast<size_t>(temp_header.frame_width) * 
                                static_cast<size_t>(temp_header.frame_height) * 3;
        size_t slot_total_size = sizeof(SlotMetadata) + slot_data_size;
        total_size = sizeof(RingBufferHeader) + (slot_total_size * temp_header.num_slots);

        // Map entire SHM
        shm_ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if (shm_ptr == MAP_FAILED) {
            perror("[SHM Ring Reader] mmap failed");
            shm_ptr = nullptr;
            close(shm_fd);
            shm_fd = -1;
            return false;
        }

        header = reinterpret_cast<RingBufferHeader*>(shm_ptr);
        initialized = true;

        std::cout << "[SHM Ring Reader] Connected to " << shm_name 
                  << " (" << header->num_slots << " slots)\n";
        return true;
    }

    // Read the latest available frame
    // Returns true if frame was read successfully
    // frame_out: output buffer (must be pre-allocated, at least width*height*3 bytes)
    // width_out, height_out: actual frame dimensions
    // frame_number_out: frame sequence number
    bool readLatestFrame(uint8_t* frame_out, int& width_out, int& height_out, 
                         uint64_t& frame_number_out) {
        if (!initialized || !frame_out) return false;

        // Get current write index (latest frame)
        uint32_t write_idx = header->write_index.load(std::memory_order_acquire);
        
        return readSlot(write_idx, frame_out, width_out, height_out, frame_number_out);
    }

    // Read a specific slot (for advanced use cases)
    bool readSlot(uint32_t slot_idx, uint8_t* frame_out, int& width_out, 
                  int& height_out, uint64_t& frame_number_out) {
        if (!initialized || !frame_out || slot_idx >= header->num_slots) return false;

        SlotMetadata* meta = getSlotMetadata(slot_idx);
        uint8_t* slot_data = getSlotData(slot_idx);

        // Check if slot is stable (even sequence number)
        uint64_t seq_before = meta->frame_seq.load(std::memory_order_acquire);
        if (seq_before % 2 != 0) {
            // Write in progress, skip
            return false;
        }

        // Increment reader count
        meta->reader_count.fetch_add(1, std::memory_order_acq_rel);

        // Double-check sequence hasn't changed (writer started)
        uint64_t seq_check = meta->frame_seq.load(std::memory_order_acquire);
        if (seq_check != seq_before) {
            // Writer started, abort
            meta->reader_count.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }

        // Safe to read
        width_out = meta->width;
        height_out = meta->height;
        frame_number_out = meta->frame_number;
        
        std::memcpy(frame_out, slot_data, meta->data_size);

        // Verify sequence is still the same (no write occurred during read)
        uint64_t seq_after = meta->frame_seq.load(std::memory_order_acquire);
        
        // Decrement reader count
        meta->reader_count.fetch_sub(1, std::memory_order_acq_rel);

        if (seq_after != seq_before) {
            // Frame was overwritten during read, data may be corrupted
            std::cerr << "[SHM Ring Reader] Frame corrupted during read (slot " 
                      << slot_idx << ")\n";
            return false;
        }

        last_read_frame_number = frame_number_out;
        return true;
    }

    // Get lag (how many frames behind the writer)
    int getLag() const {
        if (!initialized) return 0;
        
        uint32_t write_idx = header->write_index.load(std::memory_order_acquire);
        SlotMetadata* meta = getSlotMetadata(write_idx);
        uint64_t latest_frame = meta->frame_number;
        
        return static_cast<int>(latest_frame - last_read_frame_number);
    }

    void cleanup() {
        if (shm_ptr) {
            munmap(shm_ptr, total_size);
            shm_ptr = nullptr;
            header = nullptr;
        }
        if (shm_fd >= 0) {
            close(shm_fd);
            shm_fd = -1;
        }
        initialized = false;
    }

private:
    std::string shm_name;
    int shm_fd;
    void* shm_ptr;
    RingBufferHeader* header;
    size_t total_size;
    uint64_t last_read_frame_number;
    bool initialized;

    SlotMetadata* getSlotMetadata(uint32_t slot_idx) const {
        if (slot_idx >= header->num_slots) return nullptr;
        
        size_t slot_data_size = static_cast<size_t>(header->frame_width) * 
                                static_cast<size_t>(header->frame_height) * 3;
        size_t slot_total_size = sizeof(SlotMetadata) + slot_data_size;
        uint8_t* base = reinterpret_cast<uint8_t*>(shm_ptr) + sizeof(RingBufferHeader);
        return reinterpret_cast<SlotMetadata*>(base + (slot_idx * slot_total_size));
    }

    uint8_t* getSlotData(uint32_t slot_idx) const {
        SlotMetadata* meta = getSlotMetadata(slot_idx);
        if (!meta) return nullptr;
        return reinterpret_cast<uint8_t*>(meta) + sizeof(SlotMetadata);
    }
};
