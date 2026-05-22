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

// Packed header to avoid padding surprises
#pragma pack(push,1)
struct SHMHeader {
    uint64_t frame_seq;    // sequence counter: writer increments (odd while writing, even when stable)
    uint32_t width;
    uint32_t height;
    uint64_t frame_number; // logical frame counter
    uint64_t timestamp_us;
};
#pragma pack(pop)

class SHMWriter {
public:
    SHMWriter() : shm_fd(-1), shm_ptr(nullptr), frame_buffer(nullptr),
                  buffer_size(0), total_size(0), frame_counter(0), header(nullptr) {}

    ~SHMWriter() { cleanup(); }

    // Initialize SHM for a specific camera and original frame size.
    // Returns true on success.
    bool init(int cam_id, int width, int height) {
        if(shm_ptr) return true;

        shm_name = "/camera_shm_" + std::to_string(cam_id);

        // BGR8 3 bytes per pixel
        buffer_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
        total_size = sizeof(SHMHeader) + buffer_size;

        // create/open
        shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
        if(shm_fd < 0) { perror("shm_open"); return false; }

        if(ftruncate(shm_fd, total_size) != 0) { perror("ftruncate"); close(shm_fd); shm_fd = -1; return false; }

        shm_ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
        if(shm_ptr == MAP_FAILED) { perror("mmap"); shm_ptr = nullptr; close(shm_fd); shm_fd = -1; return false; }

        header = reinterpret_cast<SHMHeader*>(shm_ptr);
        frame_buffer = reinterpret_cast<uint8_t*>(shm_ptr) + sizeof(SHMHeader);

        // initialize header (frame_seq even = stable)
        header->frame_seq = 0;
        header->width = width;
        header->height = height;
        header->frame_number = 0;
        header->timestamp_us = 0;

        std::cout << "[SHM] initialized: " << shm_name
                  << " buffer for " << width << "x" << height << " (" << buffer_size << " bytes)\n";
        return true;
    }

    // Write frame. frame_w/frame_h may be <= initial sizes. If larger than allocated buffer_size -> skip.
    void writeFrame(const uint8_t* data, int frame_w, int frame_h) {
        if(!shm_ptr || !header || !frame_buffer || !data) return;

        size_t copy_size = static_cast<size_t>(frame_w) * static_cast<size_t>(frame_h) * 3;
        if(copy_size > buffer_size) {
            std::cerr << "[SHM] Frame size (" << copy_size << ") exceeds SHM buffer (" << buffer_size << "). Skipping.\n";
            return;
        }

        // Sequence fencing:
        // - increment frame_seq by 1 (becomes odd) to indicate write in progress
        // - memcpy the frame
        // - update header fields (width/height/frame_number/timestamp)
        // - increment frame_seq by 1 (becomes even) to indicate stable

        // Use atomic operations on the memory location (works across processes because atomic CPU ops act on memory).
        std::atomic<uint64_t>* seq = reinterpret_cast<std::atomic<uint64_t>*>(&header->frame_seq);

        seq->fetch_add(1, std::memory_order_acq_rel); // make odd -> write in progress

        // copy pixel data
        std::memcpy(frame_buffer, data, copy_size);

        // update header fields
        header->width = static_cast<uint32_t>(frame_w);
        header->height = static_cast<uint32_t>(frame_h);

        uint64_t new_frame_num = ++frame_counter;
        header->frame_number = new_frame_num;
        header->timestamp_us = currentTimeUs();

        seq->fetch_add(1, std::memory_order_release); // make even -> stable
    }

    // Unlink the underlying SHM object. Call this when you know no reader needs it anymore.
    // Returns true on success (or if already unlinked).
    bool unlinkSharedMemory() {
        if(shm_name.empty()) return true;
        if(shm_unlink(shm_name.c_str()) == 0) {
            std::cout << "[SHM] Unlinked " << shm_name << "\n";
            return true;
        } else {
            if(errno == ENOENT) return true;
            perror("shm_unlink");
            return false;
        }
    }

    // cleanup: munmap + close. Do NOT unlink here because other processes may be reading.
    void cleanup() {
        if(shm_ptr) {
            munmap(shm_ptr, total_size);
            shm_ptr = nullptr;
            header = nullptr;
            frame_buffer = nullptr;
        }
        if(shm_fd >= 0) {
            close(shm_fd);
            shm_fd = -1;
        }
    }

private:
    std::string shm_name;
    int shm_fd;
    void* shm_ptr;
    uint8_t* frame_buffer;
    size_t buffer_size;
    size_t total_size;
    uint64_t frame_counter;
    SHMHeader* header;

    uint64_t currentTimeUs() {
        auto t = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
    }
};
