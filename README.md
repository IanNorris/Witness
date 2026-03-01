# Witness

A video surveillance server with motion detection, HLS live streaming, and clip management.

## Quick Start

1. Build the project:
   ```
   cmake -B build -S .
   cmake --build build --config Release
   ```

2. Run the server:
   ```
   build\bin\WitnessServer.exe
   ```

3. On first run, the setup wizard opens in your browser automatically.
   Create an admin account and configure server settings.

## Setup Options

### Web Wizard (default)
When no admin user exists, the server starts a localhost-only setup wizard on a random port and opens your browser. Configure hostname, TLS, cache path, and create an admin account.

### Interactive CLI
```
WitnessServer.exe /setup
```
Prompts for settings interactively in the console. Useful for headless/SSH environments.

### Scripted Setup
```
WitnessServer.exe /setup --json config.json
```
Apply settings from a JSON file. Example config:
```json
{
  "username": "admin",
  "password": "mypassword",
  "hostname": "localhost:8080",
  "tls_mode": "NoSecurity",
  "cache_path": "C:\\WitnessCache"
}
```

### Reconfiguration
Navigate to `/setup` on a running server (requires admin login) to change settings.

## Service Management

```
WitnessServer.exe /installservice    # Install as Windows service
WitnessServer.exe /uninstallservice  # Remove Windows service
```

## TLS Options

| Mode | Description |
|------|-------------|
| `NoSecurity` | HTTP only |
| `SelfSigned` | Auto-generated self-signed certificate |
| `LetsEncrypt` | Let's Encrypt via certbot |
| `Manual` | Provide your own PEM cert and key files |

## Object Detection

Witness can tag recorded clips with detected objects (person, car, dog, etc.) using a local YOLO model via ONNX Runtime. Enable it in the setup wizard, admin panel (`/setup`), or via CLI setup.

| Setting | DB Key | Default | Description |
|---------|--------|---------|-------------|
| Backend | `detection_backend` | *(empty)* | Set to `onnx` to enable detection |
| Hardware | `detection_provider` | `cpu` | `cpu` or `gpu` (NVIDIA CUDA) |
| Confidence | `detection_confidence` | `0.6` | Minimum score (0.1-1.0) to accept a detection |
| Max FPS | `detection_max_fps` | `10` | Max frames/sec to process per camera |
| cuDNN Path | `cudnn_path` | *(auto-scan)* | cuDNN install root (e.g. `C:\Program Files\NVIDIA\CUDNN\v9.3`) |

Existing clips are automatically reprocessed in the background when detection is enabled. You can also manually re-tag individual clips from the clips view.

### GPU Acceleration (CUDA)

GPU mode requires an NVIDIA GPU with the following installed:

1. **NVIDIA GPU Driver** — [nvidia.com/drivers](https://www.nvidia.com/drivers)
2. **CUDA Toolkit 12.x** (not 13+) — [CUDA Toolkit Archive](https://developer.nvidia.com/cuda-toolkit-archive)
3. **cuDNN 9.x** (separate download) — [developer.nvidia.com/cudnn](https://developer.nvidia.com/cudnn) (requires NVIDIA developer account)
   - Extract and copy `bin\cudnn64_9.dll` into your CUDA toolkit's `bin` folder, or set `cudnn_path` in admin settings to the cuDNN version root

Use the **Test GPU** button in the admin panel or setup wizard to verify CUDA is working before enabling GPU mode. The test runs in a separate process so a cuDNN crash won't take down the server.

If CUDA is not available, the server logs a warning once and falls back to CPU automatically.

## Clip Cleanup

Automatic deletion of old clips is disabled by default. Enable it in the admin panel:

| Setting | DB Key | Default | Description |
|---------|--------|---------|-------------|
| Cleanup | `clip_cleanup_enabled` | `false` | Enable/disable automatic deletion |
| Retention | `clip_retention_days` | `10` | Days to keep clips before deletion |

## Logging

Log files are written to `%ProgramData%\Witness\logs\` with 30-day retention and daily rotation. Console shows warnings and errors only; log files include all levels.

## CLI Commands

| Command | Description |
|---------|-------------|
| `/setup` | Interactive console setup wizard |
| `/setup --json <path>` | Apply settings from JSON file |
| `/websetup` | Force web setup wizard (even if admin exists) |
| `/apply-config <path>` | Apply pending config (elevated helper) |
| `/test-cuda [cudnn_path]` | Test CUDA GPU availability (used by probe) |
| `/installservice` | Install as Windows service |
| `/uninstallservice` | Remove Windows service |