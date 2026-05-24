# Cellular Network Coverage Monitoring System

Server-side desktop application for collecting, storing, and visualizing mobile network telemetry. Implements a full data pipeline: from receiving raw ZeroMQ measurements from an Android client to rendering an interactive signal heatmap on an OpenStreetMap canvas.

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Key Algorithms](#key-algorithms)
- [Dependencies](#dependencies)
- [Build](#build)
- [Run](#run)
- [Project Structure](#project-structure)
- [Database Schema](#database-schema)
- [Telemetry Protocol](#telemetry-protocol)

---

## Overview

The system is written in **C++17** and built with **CMake**. It consists of four concurrent modules running in a single process:

| Module | File | Role |
|---|---|---|
| Network | `network.cpp` | ZeroMQ receive loop, protocol parsing, PostgreSQL writes |
| Altitude Service | `AltitudeService.cpp` | Backfills missing elevation data via Open-Meteo HTTP API |
| Map Manager | `MapManager.cpp` | Tile download, disk cache, async OpenGL texture upload |
| GUI | `GUI.cpp` | SDL2 + OpenGL 3.3 window, Dear ImGui / ImPlot rendering |

The GUI module runs on the **main OS thread** (required by OpenGL context binding). The network module runs in a dedicated thread. Tile loading runs in detached threads (`std::thread::detach`); decoded pixels are handed to the main thread through `pending_textures` queue.

Inter-module communication goes through `SharedBuffer` - a mutex-protected object exposing `addData`, `consumeNewData`, `getFlags`, `setFlags`.

---

## Architecture

```
Android Client
     |
     | ZeroMQ REQ (port 20077)
     v
network.cpp  <----> SharedBuffer <----> GUI.cpp
     |                                     |
     v                                     v
PostgreSQL                          MapManager.cpp
                                          |
                                    AltitudeService.cpp
```

Sensor state is encoded as a 4-character flag string, e.g. `"1100"` (GPS on, LTE on, GSM off, NR off), returned to the Android client as a ZeroMQ REP payload on every message.

---

## Key Algorithms

### Web Mercator Projection

Geographic coordinates (WGS 84) are converted to normalized tile coordinates using the spherical Mercator projection (EPSG:3857):

$$x = \frac{\lambda + 180}{360}$$

$$y = \frac{1}{2}\left(1 - \frac{\ln\!\left(\tan\varphi + \sec\varphi\right)}{\pi}\right)$$

where $\lambda$ is longitude in degrees and $\varphi$ is latitude in radians. Valid latitude range: $[-85.0511°,\; +85.0511°]$.

Tile indices at zoom level $z$:

$$t_x = \lfloor x \cdot 2^z \rfloor, \quad t_y = \lfloor y \cdot 2^z \rfloor$$

Tiles are cached locally at `tiles_cache/{z}/{x}/{y}.png` (zoom levels 7-19, 300+ directories).

### IDW Signal Interpolation

The signal heatmap is computed with Inverse Distance Weighting (Shepard, 1968). For a query point $\mathbf{x}_0$:

$$\hat{P}(\mathbf{x}_0) = \frac{\displaystyle\sum_{i=1}^{n} w_i \cdot P_i^{\mathrm{lin}}}{\displaystyle\sum_{i=1}^{n} w_i}, \qquad w_i = \frac{1}{d(\mathbf{x}_0,\, \mathbf{x}_i)^{\,p}}$$

where $p = 2$ (default), $P_i^{\mathrm{lin}}$ is signal power in linear units, $d$ is Euclidean distance.

Because signal measurements are stored in dBm, conversion to linear scale is applied before averaging and inverted after:

$$P_{\mathrm{lin}} = 10^{\,P_{\mathrm{dBm}}/10}, \qquad P_{\mathrm{dBm}} = 10 \cdot \log_{10}(P_{\mathrm{lin}})$$

Pixel color is linearly interpolated from blue (weak signal) to red (strong signal) based on normalized $\hat{P}$.

### RSRP Quality Thresholds

| RSRP (dBm) | Quality |
|---|---|
| > -80 | Excellent |
| -90 to -80 | Good |
| -100 to -90 | Fair |
| -110 to -100 | Poor |
| < -110 | Unacceptable |

### RSRQ Formula

$$\mathrm{RSRQ} = N \cdot \frac{\mathrm{RSRP}}{\mathrm{RSSI}}$$

where $N$ is the number of resource blocks in the channel bandwidth.

---

## Dependencies

### System packages (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libzmq3-dev \
    libpq-dev \
    libsdl2-dev \
    libglew-dev \
    libcurl4-openssl-dev \
    libssl-dev \
    postgresql \
    postgresql-client
```

### Vendored (included in `third_party/`)

- **Dear ImGui** - Immediate Mode GUI
- **ImPlot** - plotting extension for Dear ImGui

---

## Build

```bash
# Clone and enter the project
git clone <repo-url>
cd <project-dir>

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build (use -j$(nproc) to parallelize)
cmake --build build -j$(nproc)
```

The binary is placed at `build/telemetry_app`.

### Debug build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

---

## Run

### 1. Set up PostgreSQL

```sql
-- Connect as postgres superuser
CREATE USER telemetry_user WITH PASSWORD 'your_password';
CREATE DATABASE telemetry_db OWNER telemetry_user;

-- Connect to telemetry_db and create the table
CREATE TABLE telemetry (
    id          SERIAL PRIMARY KEY,
    lat         DOUBLE PRECISION,
    lon         DOUBLE PRECISION,
    timestamp   TIMESTAMP,
    net_type    TEXT,
    band        TEXT,
    cell_id     TEXT,
    pci         TEXT,
    tac         TEXT,
    mcc         TEXT,
    mnc         TEXT,
    rsrp        DOUBLE PRECISION,
    rsrq        DOUBLE PRECISION,
    rssi        DOUBLE PRECISION,
    snr         DOUBLE PRECISION,
    cqi         INTEGER,
    alt         DOUBLE PRECISION
);
```

### 2. Configure the connection string

Edit the database connection string in `src/network.cpp` (or the relevant config header) to match your PostgreSQL credentials:

```
host=localhost dbname=telemetry_db user=telemetry_user password=your_password
```

### 3. Run the application

```bash
# From the project root
./build/telemetry_app
```

The application binds ZeroMQ REP socket on **port 20077**. Make sure this port is accessible from the Android client device (same network or forwarded).

### 4. Connect the Android client

Point the Android client to the server IP on port `20077`. The server will start accepting telemetry messages immediately on launch.

### Tile cache

On first use, map tiles are downloaded from `tile.openstreetmap.org` and cached to `tiles_cache/`. Subsequent launches load tiles from disk without network requests. The cache directory is created automatically.

The application respects the [OSM Tile Usage Policy](https://operations.osmfoundation.org/policies/tiles/). The HTTP User-Agent is set to `ImGui-Maps-App/1.0`.

### Elevation backfill

Missing altitude values are filled automatically via [Open-Meteo Elevation API](https://open-meteo.com/en/docs/elevation-api) in batches of 80 coordinate pairs with a 500 ms delay between batches to respect API rate limits.

---

## Project Structure

```
.
+-- src/
|   +-- main.cpp              - Entry point, thread launch
|   +-- network.cpp           - ZeroMQ receive loop, PostgreSQL writes
|   +-- GUI.cpp               - SDL2 window, ImGui/ImPlot render loop
|   +-- MapManager.cpp        - Tile cache, Mercator projection, OpenGL textures
|   +-- MapWindow.cpp         - Map widget, heatmap rendering
|   +-- AltitudeService.cpp   - Open-Meteo elevation backfill
|   +-- headers/              - Header files
+-- third_party/
|   +-- imgui/                - Dear ImGui source
|   +-- implot/               - ImPlot source
+-- tiles_cache/              - OSM tile disk cache (z 7-19)
+-- build/                    - CMake build output
+-- CMakeLists.txt
```

---

## Database Schema

The `telemetry` table stores one row per received measurement:

| Column | Type | Description |
|---|---|---|
| `lat`, `lon` | DOUBLE PRECISION | WGS 84 coordinates |
| `timestamp` | TIMESTAMP | Unix epoch via `to_timestamp()` |
| `net_type` | TEXT | `"LTE"`, `"GSM"`, or `"NR"` |
| `band` | TEXT | Frequency band |
| `cell_id` | TEXT | Cell identifier |
| `pci` | TEXT | Physical Cell ID (or BSIC for GSM) |
| `tac` | TEXT | Tracking Area Code (or LAC for GSM) |
| `mcc`, `mnc` | TEXT | Mobile Country/Network Code |
| `rsrp` | DOUBLE PRECISION | dBm |
| `rsrq` | DOUBLE PRECISION | dB |
| `rssi` | DOUBLE PRECISION | dBm |
| `snr` | DOUBLE PRECISION | dB |
| `cqi` | INTEGER | 0-15 (LTE only) |
| `alt` | DOUBLE PRECISION | Elevation in meters |

---

## Telemetry Protocol

Messages arrive as semicolon-delimited strings. Fields 1-4 are common across all network types; field 5 is the network type discriminator.

**Common prefix:** `lat;lon;alt;unix_timestamp;net_type;...`

**LTE** (19+ fields):
```
lat;lon;alt;ts;LTE;band;cell_id;?;mcc;mnc;pci;tac;?;cqi;rsrp;rsrq;rssi;snr
```

**GSM** (15+ fields):
```
lat;lon;alt;ts;GSM;cell_id;bsic;band;lac;mcc;mnc;?;rsrp;rssi
```

**NR / 5G** (16+ fields):
```
lat;lon;alt;ts;NR;band;cell_id;pci;?;tac;mcc;mnc;rsrp;rsrq;snr
```

The Android platform uses `INT_MAX` (`2147483647`) as a sentinel for missing measurements. The server's `sanitize()` function maps these to SQL `NULL` before inserting.

Messages beginning with `"SKIP"` are reference packets with no measurement data - the server returns the sensor flag string without writing to the database.
