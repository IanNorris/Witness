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