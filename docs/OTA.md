# OTA (Over-The-Air) Firmware Updates

OTA is **opt-in**. It is enabled by adding `-D OTA_ENABLED` to `build_flags` in `platformio.ini`. Without this flag, zero OTA code is compiled — there is no runtime cost and no dependency on an external server.

## How it works

When enabled, the device polls a manifest URL every 5 minutes over HTTPS with HTTP Basic Auth. The manifest contains an MD5 checksum for each firmware mode. If the checksum differs from the currently running firmware, the device downloads the new `.bin`, verifies its MD5, flashes it to the inactive OTA partition, and reboots.

The first check after a fresh USB flash will always trigger an OTA update (the device has no record of its own MD5). After that, it only updates when the manifest changes.

## Enabling OTA

Add `-D OTA_ENABLED` to `build_flags` in `platformio.ini`:

```ini
build_flags = -D MODE_STATIONARY -D OTA_ENABLED
```

Set your server URL in `src/config.h`:

```cpp
constexpr char     OTA_BASE_URL[]        = "https://your-ota-server.example.com";
constexpr char     OTA_MANIFEST_URL[]    = "https://your-ota-server.example.com/firmware/manifest.json";
constexpr uint32_t OTA_CHECK_INTERVAL_MS = 300000UL;  // 5 minutes
```

Set your OTA credentials in `src/secrets.cpp`:

```cpp
const char* OTA_USER     = "otauser";
const char* OTA_PASSWORD = "your_ota_password";
```

## Disabling OTA

Remove `-D OTA_ENABLED` from `build_flags` (or just don't add it). The OTA source files still exist but compile to nothing.

## Versioning

`CHANGELOG.md` in the firmware repo is the single source of truth for the firmware version. The PlatformIO build script `read_version.py` extracts the latest version from the first `## [x.y.z]` heading (skipping `[Unreleased]`) and injects it as `-D FW_VERSION="x.y.z"` at compile time. This version appears in MQTT metrics as the `fw_version` label.

The OTA server's `build.sh` also reads from the same `CHANGELOG.md` to set the version in the manifest.

## Setting up your own OTA server

The OTA server is a simple static file server (nginx) running in Docker. It serves two firmware binaries and a JSON manifest behind HTTP Basic Auth. You can host it anywhere that runs containers — a VPS, a cloud run service, or any Docker-based PaaS.

### Directory structure

```
rtk-ota-server/
├── Dockerfile
├── .htpasswd
├── nginx.conf
├── build.sh
└── firmware/
    ├── manifest.json
    ├── stationary.bin
    └── rover.bin
```

### Dockerfile

```dockerfile
FROM nginx:alpine

COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY .htpasswd /etc/nginx/.htpasswd
COPY firmware/ /usr/share/nginx/html/firmware/

EXPOSE 8080
```

### nginx.conf

```nginx
server {
    listen 8080;
    server_name _;

    root /usr/share/nginx/html;

    location /firmware/ {
        auth_basic "OTA";
        auth_basic_user_file /etc/nginx/.htpasswd;
        autoindex off;
        types {
            application/octet-stream bin;
            application/json        json;
        }
        default_type application/octet-stream;
    }

    location /healthz {
        return 200 'ok';
        add_header Content-Type text/plain;
    }

    location / {
        return 404;
    }
}
```

### .htpasswd

Generate with:
```bash
printf 'otauser:%s\n' "$(openssl passwd -apr1 'your_ota_password')"
```

### firmware/manifest.json

```json
{
  "stationary": {
    "version": "0.5.0",
    "url": "/firmware/stationary.bin",
    "md5": "520e0ea0e4ac5ac9a9bcf2ff31ea7d0e"
  },
  "rover": {
    "version": "0.5.0",
    "url": "/firmware/rover.bin",
    "md5": "4f2973d607766d5763b6f2976ed2ae67"
  }
}
```

The `url` field is a path relative to the server root. The device prepends `OTA_BASE_URL` to form the full download URL. The `version` field is informational (for humans); the device only compares `md5`.

### build.sh

A helper script that builds both firmwares, copies the `.bin` files, computes MD5 checksums, and updates the manifest. It reads the version from `CHANGELOG.md` in the firmware repo. Place the OTA server repo as a sibling directory:

```bash
#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "$0")" && pwd)/firmware"
RTK_REPO="${1:-$(cd "$(dirname "$0")/../rtk-rover-and-station" && pwd)}"

if [ ! -f "$RTK_REPO/platformio.ini" ]; then
    echo "Error: Cannot find rtk-rover-and-station repo at $RTK_REPO"
    echo "Usage: $0 [path-to-rtk-rover-and-station]"
    exit 1
fi

# Read version from CHANGELOG.md (first versioned heading, skip [Unreleased])
VERSION=$(grep -oP '## \[\K[0-9]+\.[0-9]+\.[0-9]+' "$RTK_REPO/CHANGELOG.md" | head -1)
if [ -z "$VERSION" ]; then
    echo "Error: Could not extract version from CHANGELOG.md"
    exit 1
fi

echo "==> Version from CHANGELOG.md: $VERSION"

echo "==> Building stationary firmware..."
(cd "$RTK_REPO" && pio run -e stationary)
cp "$RTK_REPO/.pio/build/stationary/firmware.bin" "$FIRMWARE_DIR/stationary.bin"

echo "==> Building rover firmware..."
(cd "$RTK_REPO" && pio run -e rover)
cp "$RTK_REPO/.pio/build/rover/firmware.bin" "$FIRMWARE_DIR/rover.bin"

STATIONARY_MD5=$(md5sum "$FIRMWARE_DIR/stationary.bin" | cut -d' ' -f1)
ROVER_MD5=$(md5sum "$FIRMWARE_DIR/rover.bin" | cut -d' ' -f1)

echo "==> Updating manifest (version $VERSION)"

python3 -c "
import json
manifest = {
    'stationary': {
        'version': '$VERSION',
        'url': '/firmware/stationary.bin',
        'md5': '$STATIONARY_MD5'
    },
    'rover': {
        'version': '$VERSION',
        'url': '/firmware/rover.bin',
        'md5': '$ROVER_MD5'
    }
}
with open('$FIRMWARE_DIR/manifest.json', 'w') as f:
    json.dump(manifest, f, indent=2)
    f.write('\n')
"

echo "==> Done! version=$VERSION"
echo "    stationary.bin  md5=$STATIONARY_MD5"
echo "    rover.bin       md5=$ROVER_MD5"
echo "Next: commit and push to trigger deployment."
```

### Deployment workflow

1. Make firmware changes in `rtk-rover-and-station`
2. Update `CHANGELOG.md` with the new version and changes
3. Run `./build.sh` from the OTA server repo
4. Commit the updated `.bin` files and `manifest.json`, then push
5. Your container host rebuilds the Docker image and deploys
6. Within 5 minutes, devices fetch the new manifest, see the changed MD5, download, verify, flash, and reboot

### Testing locally

```bash
docker build -t rtk-ota-server .
docker run -p 8080:8080 rtk-ota-server

# Should return 401 (auth required)
curl -s -o /dev/null -w "%{http_code}" http://localhost:8080/firmware/manifest.json

# Should return 200
curl -u otauser:your_ota_password http://localhost:8080/firmware/manifest.json

# Health check (no auth)
curl http://localhost:8080/healthz
```
