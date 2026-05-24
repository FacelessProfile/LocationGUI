# Cellular Network Coverage Monitoring System
# Система мониторинга качества покрытия сотовых сетей

---

**Read in:** [English](#english) | [Русский](#russian)

---

<a name="english"></a>

# English

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

---

---

<a name="russian"></a>

# Русский

## Содержание

- [Обзор](#obzor)
- [Архитектура](#arhitektura)
- [Ключевые алгоритмы](#algoritmy)
- [Зависимости](#zavisimosti)
- [Сборка](#sborka)
- [Запуск](#zapusk)
- [Структура проекта](#struktura)
- [Схема базы данных](#baza)
- [Протокол телеметрии](#protokol)

---

<a name="obzor"></a>
## Обзор

Система написана на **C++17** и собирается системой **CMake**. Состоит из четырёх конкурентных модулей, работающих в рамках одного процесса:

| Модуль | Файл | Роль |
|---|---|---|
| Сетевой | `network.cpp` | Цикл приёма ZeroMQ, разбор протокола, запись в PostgreSQL |
| Сервис высот | `AltitudeService.cpp` | Восполнение отсутствующих высотных отметок через Open-Meteo HTTP API |
| Менеджер карты | `MapManager.cpp` | Загрузка тайлов, дисковый кеш, асинхронная передача текстур в OpenGL |
| GUI | `GUI.cpp` | Окно SDL2 + OpenGL 3.3, рендеринг Dear ImGui / ImPlot |

Модуль GUI выполняется в **главном потоке ОС** (требование OpenGL к привязке контекста). Сетевой модуль запускается в выделенном потоке. Загрузка тайлов производится в отсоединённых потоках (`std::thread::detach`); декодированные пиксели передаются в главный поток через очередь `pending_textures`.

Взаимодействие модулей строится через объект `SharedBuffer` - потокобезопасный буфер с мьютексной защитой, предоставляющий методы `addData`, `consumeNewData`, `getFlags`, `setFlags`.

---

<a name="arhitektura"></a>
## Архитектура

```
Android-клиент
     |
     | ZeroMQ REQ (порт 20077)
     v
network.cpp  <----> SharedBuffer <----> GUI.cpp
     |                                     |
     v                                     v
PostgreSQL                          MapManager.cpp
                                          |
                                    AltitudeService.cpp
```

Состояние датчиков кодируется 4-символьной строкой флагов, например `"1100"` (GPS включён, LTE включён, GSM выключен, NR выключен), и возвращается Android-клиенту в качестве ZeroMQ REP ответа на каждое сообщение.

---

<a name="algoritmy"></a>
## Ключевые алгоритмы

### Картографическая проекция Меркатора

Географические координаты (WGS 84) преобразуются в нормированные координаты тайлов с применением сферической проекции Меркатора (EPSG:3857):

$$x = \frac{\lambda + 180}{360}$$

$$y = \frac{1}{2}\left(1 - \frac{\ln\!\left(\tan\varphi + \sec\varphi\right)}{\pi}\right)$$

где $\lambda$ - долгота в градусах, $\varphi$ - широта в радианах. Допустимый диапазон широт: $[-85{,}0511°;\; +85{,}0511°]$.

Индексы тайла на уровне масштабирования $z$:

$$t_x = \lfloor x \cdot 2^z \rfloor, \quad t_y = \lfloor y \cdot 2^z \rfloor$$

Тайлы кешируются локально по пути `tiles_cache/{z}/{x}/{y}.png` (уровни масштабирования 7-19, более 300 директорий).

### Пространственная интерполяция IDW

Тепловая карта сигнала вычисляется методом обратных взвешенных расстояний (IDW, Shepard, 1968). Для расчётной точки $\mathbf{x}_0$:

$$\hat{P}(\mathbf{x}_0) = \frac{\displaystyle\sum_{i=1}^{n} w_i \cdot P_i^{\mathrm{lin}}}{\displaystyle\sum_{i=1}^{n} w_i}, \qquad w_i = \frac{1}{d(\mathbf{x}_0,\, \mathbf{x}_i)^{\,p}}$$

где $p = 2$ (по умолчанию), $P_i^{\mathrm{lin}}$ - мощность $i$-го измерения в линейных единицах, $d$ - евклидово расстояние.

Поскольку измерения хранятся в дБм, перед интерполяцией выполняется перевод в линейную шкалу и обратно:

$$P_{\mathrm{lin}} = 10^{\,P_{\mathrm{dBm}}/10}, \qquad P_{\mathrm{dBm}} = 10 \cdot \log_{10}(P_{\mathrm{lin}})$$

Цвет пикселя линейно интерполируется в палитре от синего (слабый сигнал) к красному (сильный сигнал) на основе нормированного $\hat{P}$.

### Пороговые значения RSRP

| RSRP (дБм) | Качество |
|---|---|
| > -80 | Отличное |
| от -90 до -80 | Хорошее |
| от -100 до -90 | Удовлетворительное |
| от -110 до -100 | Слабое |
| < -110 | Неудовлетворительное |

### Формула RSRQ

$$\mathrm{RSRQ} = N \cdot \frac{\mathrm{RSRP}}{\mathrm{RSSI}}$$

где $N$ - число ресурсных блоков в полосе пропускания канала.

---

<a name="zavisimosti"></a>
## Зависимости

### Системные пакеты (Ubuntu/Debian)

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

### Вендорные (включены в `third_party/`)

- **Dear ImGui** - Immediate Mode GUI
- **ImPlot** - расширение для построения графиков поверх Dear ImGui

---

<a name="sborka"></a>
## Сборка

```bash
# Клонировать и перейти в директорию проекта
git clone <repo-url>
cd <project-dir>

# Конфигурация
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Сборка (флаг -j$(nproc) для параллельной компиляции)
cmake --build build -j$(nproc)
```

Бинарный файл размещается по пути `build/telemetry_app`.

### Debug-сборка

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
```

---

<a name="zapusk"></a>
## Запуск

### 1. Настройка PostgreSQL

```sql
-- Подключиться от имени суперпользователя postgres
CREATE USER telemetry_user WITH PASSWORD 'your_password';
CREATE DATABASE telemetry_db OWNER telemetry_user;

-- Подключиться к telemetry_db и создать таблицу
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

### 2. Настройка строки подключения

Отредактировать строку подключения к БД в `src/network.cpp` (или соответствующем заголовочном файле):

```
host=localhost dbname=telemetry_db user=telemetry_user password=your_password
```

### 3. Запуск приложения

```bash
# Из корневой директории проекта
./build/telemetry_app
```

Приложение привязывает ZeroMQ REP-сокет на **порту 20077**. Порт должен быть доступен с Android-устройства (одна сеть или проброс порта).

### 4. Подключение Android-клиента

Указать в Android-клиенте IP-адрес сервера и порт `20077`. Сервер начинает принимать телеметрию сразу после запуска.

### Тайловый кеш

При первом использовании тайлы карты загружаются с `tile.openstreetmap.org` и сохраняются в `tiles_cache/`. При последующих запусках тайлы считываются с диска без сетевых запросов. Директория кеша создаётся автоматически.

Приложение соблюдает [политику использования тайлов OSM](https://operations.osmfoundation.org/policies/tiles/). HTTP User-Agent установлен в `ImGui-Maps-App/1.0`.

### Восполнение высотных отметок

Отсутствующие значения высоты заполняются автоматически через [Open-Meteo Elevation API](https://open-meteo.com/en/docs/elevation-api) пакетами по 80 пар координат с паузой 500 мс между пакетами для соблюдения лимита запросов API.

---

<a name="struktura"></a>
## Структура проекта

```
.
+-- src/
|   +-- main.cpp              - Точка входа, запуск потоков
|   +-- network.cpp           - Цикл приёма ZeroMQ, запись в PostgreSQL
|   +-- GUI.cpp               - Окно SDL2, цикл рендеринга ImGui/ImPlot
|   +-- MapManager.cpp        - Тайловый кеш, проекция Меркатора, текстуры OpenGL
|   +-- MapWindow.cpp         - Виджет карты, рендеринг тепловой карты
|   +-- AltitudeService.cpp   - Восполнение высот через Open-Meteo
|   +-- headers/              - Заголовочные файлы
+-- third_party/
|   +-- imgui/                - Исходники Dear ImGui
|   +-- implot/               - Исходники ImPlot
+-- tiles_cache/              - Дисковый кеш тайлов OSM (z 7-19)
+-- build/                    - Выходные файлы CMake
+-- CMakeLists.txt
```

---

<a name="baza"></a>
## Схема базы данных

Таблица `telemetry` хранит одну строку на каждое принятое измерение:

| Столбец | Тип | Описание |
|---|---|---|
| `lat`, `lon` | DOUBLE PRECISION | Координаты WGS 84 |
| `timestamp` | TIMESTAMP | Unix epoch через `to_timestamp()` |
| `net_type` | TEXT | `"LTE"`, `"GSM"` или `"NR"` |
| `band` | TEXT | Частотный диапазон |
| `cell_id` | TEXT | Идентификатор соты |
| `pci` | TEXT | Physical Cell ID (BSIC для GSM) |
| `tac` | TEXT | Tracking Area Code (LAC для GSM) |
| `mcc`, `mnc` | TEXT | Код страны / оператора |
| `rsrp` | DOUBLE PRECISION | дБм |
| `rsrq` | DOUBLE PRECISION | дБ |
| `rssi` | DOUBLE PRECISION | дБм |
| `snr` | DOUBLE PRECISION | дБ |
| `cqi` | INTEGER | 0-15 (только LTE) |
| `alt` | DOUBLE PRECISION | Высота над уровнем моря, м |

---

<a name="protokol"></a>
## Протокол телеметрии

Сообщения поступают в виде строк с разделителем `";"`. Поля 1-4 общие для всех типов сети; поле 5 - дискриминатор типа сети.

**Общий префикс:** `lat;lon;alt;unix_timestamp;net_type;...`

**LTE** (19+ полей):
```
lat;lon;alt;ts;LTE;band;cell_id;?;mcc;mnc;pci;tac;?;cqi;rsrp;rsrq;rssi;snr
```

**GSM** (15+ полей):
```
lat;lon;alt;ts;GSM;cell_id;bsic;band;lac;mcc;mnc;?;rsrp;rssi
```

**NR / 5G** (16+ полей):
```
lat;lon;alt;ts;NR;band;cell_id;pci;?;tac;mcc;mnc;rsrp;rsrq;snr
```

Платформа Android использует `INT_MAX` (`2147483647`) как маркер отсутствующего измерения. Функция `sanitize()` на сервере преобразует такие значения в SQL `NULL` перед записью.

Сообщения, начинающиеся с `"SKIP"`, являются опорными пакетами без данных измерений - сервер возвращает строку флагов датчиков без записи в базу данных.
