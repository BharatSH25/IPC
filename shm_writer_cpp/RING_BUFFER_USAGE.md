# Ring Buffer Shared Memory - Usage Guide

## Overview

The ring buffer implementation provides a production-grade solution for sharing video frames between one writer and multiple consumers with different processing speeds.

## Key Components

### 1. **SHMWriter_Ring_Buffer** (Writer Class)
- Manages a circular buffer of N frame slots (default: 30)
- Handles frame writing with automatic overflow protection
- Tracks statistics (frames written, frames dropped)

### 2. **SHMReader_Ring_Buffer** (Reader Class)
- Connects to existing ring buffer
- Safely reads frames with reference counting
- Prevents bus errors even when writer overwrites slots

## Quick Start

### Writer Side (Camera/Frame Producer)

```cpp
#include "SHMWriter_Ring_Buffer.hpp"

// Create writer with 30 slots (adjust based on your needs)
SHMWriter_Ring_Buffer writer(30);

// Initialize for camera ID 1, max resolution 1920x1080
if (!writer.init(1, 1920, 1080)) {
    std::cerr << "Failed to initialize SHM writer\n";
    return -1;
}

// Write frames in your capture loop
while (capturing) {
    cv::Mat frame = captureFrame(); // Your frame capture logic
    
    if (!writer.writeFrame(frame.data, frame.cols, frame.rows)) {
        // Frame was dropped (all slots busy)
        // This is normal under heavy load
    }
}

// Cleanup when done
writer.cleanup();
writer.unlinkSharedMemory(); // Only call this when removing camera
```

### Reader Side (Consumer: Model Runner, Clip Saver, etc.)

```cpp
#include "SHMWriter_Ring_Buffer.hpp"

// Create reader
SHMReader_Ring_Buffer reader;

// Connect to camera 1's ring buffer
if (!reader.connect(1)) {
    std::cerr << "Failed to connect to SHM\n";
    return -1;
}

// Allocate buffer for reading (max size)
std::vector<uint8_t> frame_buffer(1920 * 1080 * 3);

// Read frames in your processing loop
while (processing) {
    int width, height;
    uint64_t frame_number;
    
    if (reader.readLatestFrame(frame_buffer.data(), width, height, frame_number)) {
        // Successfully read frame
        cv::Mat frame(height, width, CV_8UC3, frame_buffer.data());
        
        // Process frame (can take time, no bus errors!)
        processFrame(frame);
        
        // Check lag
        int lag = reader.getLag();
        if (lag > 10) {
            std::cout << "Warning: Consumer lagging " << lag << " frames\n";
        }
    } else {
        // No frame available or write in progress, retry
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Cleanup
reader.cleanup();
```

## Integration Examples

### Example 1: Update Your Camera Supervisor

Modify your existing `SHMWriter.cpp` to use the ring buffer:

```cpp
// In CameraThread class, replace:
// #include "SHMWriter.hpp"
// SHMWriter shm;

// With:
#include "SHMWriter_Ring_Buffer.hpp"
SHMWriter_Ring_Buffer shm;  // Uses default 30 slots

// The rest of your code remains the same!
// init() and writeFrame() have the same signature
```

### Example 2: Update Clip Saver (FFmpeg Writer)

In your `clip_saver` code:

```cpp
#include "SHMWriter_Ring_Buffer.hpp"

class ClipSaver {
private:
    SHMReader_Ring_Buffer shm_reader;
    std::vector<uint8_t> frame_buffer;
    
public:
    bool init(int camera_id) {
        if (!shm_reader.connect(camera_id)) {
            return false;
        }
        
        // Allocate max buffer
        frame_buffer.resize(1920 * 1080 * 3);
        return true;
    }
    
    void saveClip(const std::string& output_path, int duration_sec) {
        // Initialize FFmpeg encoder
        // ... your FFmpeg setup code ...
        
        auto start_time = std::chrono::steady_clock::now();
        
        while (std::chrono::steady_clock::now() - start_time < 
               std::chrono::seconds(duration_sec)) {
            
            int width, height;
            uint64_t frame_number;
            
            // Read frame - this is now safe even if FFmpeg is slow!
            if (shm_reader.readLatestFrame(frame_buffer.data(), 
                                           width, height, frame_number)) {
                
                // Encode frame with FFmpeg (can take time)
                encodeFrame(frame_buffer.data(), width, height);
            }
            
            // Small sleep to match target FPS
            std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS
        }
        
        // Finalize FFmpeg encoding
        // ... your FFmpeg cleanup code ...
    }
};
```

### Example 3: Update Model Runner

In your Python model runner:

```python
# You'll need to create a Python wrapper using ctypes or pybind11
# For now, you can use your existing shm_reader.py with modifications

from shm_reader_python.shm_reader import CameraSHM

class ModelRunner:
    def __init__(self, camera_id):
        self.shm = CameraSHM(camera_id)
        # Update shm_reader.py to use ring buffer naming convention
        # Change: f"/camera_shm_{camera_id}"
        # To: f"/camera_shm_ring_{camera_id}"
        
    def run(self):
        while True:
            frame = self.shm.read_frame()
            if frame is not None:
                # Process with your model (YOLO, etc.)
                results = self.model.predict(frame)
                # ... handle results ...
```

## Configuration Guidelines

### Choosing Buffer Size

| Use Case | Recommended Slots | Memory (1080p) | Rationale |
|----------|------------------|----------------|-----------|
| Fast consumers only | 10-15 | ~60-90 MB | Minimal buffering needed |
| Mixed (fast + slow) | 30-40 | ~180-240 MB | Balance memory & tolerance |
| Very slow consumers | 60-120 | ~360-720 MB | Maximum tolerance for lag |

### Memory Calculation

```
Memory per camera = (width × height × 3 × num_slots) + overhead
For 1920×1080 with 30 slots: ~180 MB
For 1280×720 with 30 slots: ~80 MB
```

### Monitoring

```cpp
// Periodically check stats
uint64_t written, dropped;
writer.getStats(written, dropped);

float drop_rate = (float)dropped / written * 100.0f;
if (drop_rate > 5.0f) {
    std::cout << "Warning: High drop rate " << drop_rate << "%\n";
    std::cout << "Consider increasing buffer size or optimizing consumers\n";
}

// Print detailed status
writer.printStatus();
```

## Troubleshooting

### Problem: High Frame Drop Rate

**Symptoms**: `total_frames_dropped` increasing rapidly

**Solutions**:
1. Increase buffer size: `SHMWriter_Ring_Buffer writer(60);`
2. Optimize slow consumers (profile FFmpeg encoding)
3. Reduce frame resolution or FPS
4. Add more CPU cores for parallel processing

### Problem: Consumer Lag Increasing

**Symptoms**: `reader.getLag()` returns large values

**Solutions**:
1. Consumer is too slow - optimize processing
2. Skip frames: read every Nth frame instead of all
3. Use separate thread for I/O vs processing

### Problem: Bus Errors Still Occurring

**Symptoms**: Segmentation faults in readers

**Possible Causes**:
1. Reader not using reference counting properly
2. Buffer size mismatch between writer and reader
3. SHM was unlinked while readers active

**Solutions**:
1. Ensure all readers use `SHMReader_Ring_Buffer`
2. Only call `unlinkSharedMemory()` after all readers disconnected
3. Check `reader.connect()` returns true before reading

## Migration Checklist

- [ ] Update camera supervisor to use `SHMWriter_Ring_Buffer`
- [ ] Update clip_saver to use `SHMReader_Ring_Buffer`
- [ ] Update model_runner to use ring buffer naming
- [ ] Update face_detection to use ring buffer naming
- [ ] Test with single camera, single consumer
- [ ] Test with single camera, multiple consumers
- [ ] Test with multiple cameras
- [ ] Monitor memory usage and adjust buffer sizes
- [ ] Set up monitoring for drop rates and lag
- [ ] Document any custom configurations

## Performance Tips

1. **Pre-allocate buffers**: Readers should allocate frame buffers once, not per-read
2. **Avoid unnecessary copies**: Use `cv::Mat` wrappers around raw buffers
3. **Tune buffer size**: Start with 30, increase if drops occur
4. **Monitor system**: Use `top`/`htop` to watch memory and CPU
5. **Profile consumers**: Identify which consumer is slowest

## Advanced: Custom Buffer Sizes Per Camera

```cpp
// High-resolution camera needs more buffering
if (camera_id == 1) {
    writer = new SHMWriter_Ring_Buffer(60);  // 60 slots
} else {
    writer = new SHMWriter_Ring_Buffer(30);  // 30 slots
}
```

## Comparison: Old vs New

| Aspect | Old (Single Buffer) | New (Ring Buffer) |
|--------|-------------------|------------------|
| Memory | ~6 MB per camera | ~180 MB per camera (30 slots) |
| Slow consumers | Bus errors | No errors, frames dropped if too slow |
| Max consumers | Limited by read speed | Unlimited (practical: 10-20) |
| Latency | Minimal | Minimal for fast consumers |
| Complexity | Simple | Moderate |
| Production-ready | No (for mixed speeds) | Yes |

## Next Steps

1. Test the implementation with your existing setup
2. Gradually migrate consumers one by one
3. Monitor performance and adjust buffer sizes
4. Consider implementing metrics export (Prometheus, etc.)
5. Add alerting for high drop rates or consumer lag
