# Babe32 — Barely Adequate Browser ESP32

A text web browser that runs on the Cheap Black Display (CBD) - a cheap ESP32-S3 capacitive touchscreen module. It fetches pages directly or via proxy fallback, renders a subset of HTML with LVGL. Optionally loads images. No Javascript - be realistic!

<img width="2467" height="1705" alt="IMG_20260515_112920734_HDR~2" src="https://github.com/user-attachments/assets/7ffdf1d2-8a12-4af6-b262-b229d2e6b059" />

https://youtu.be/If5GsIW79E0?si=GUtGvhs5Pg7Q8_43
---
## Features

- **Web browsing** — fetches raw HTML (1MB max) directly over TLS; falls back to a self-hosted PHP proxy, then a Brightdata residential proxy for sites that block direct requests; rendered as plain text by the on-device HTML parser
- **HTML rendering** — headings (h1–h6), paragraphs, links, bold/italic/monospace, font sizes, inline colour
- **Image viewing** — thumbnail images fetched via a resize proxy, tappable for full-screen view
- **HTML forms** — text inputs, dropdowns, submit buttons (GET and POST)
- **Touch navigation** — swipe left/right for back/forward; tap links; pull-to-refresh
- **On-screen keyboard** — LVGL keyboard slides up on text focus
- **WiFi setup UI** — scan APs, enter password, stores up to 10 networks in NVS
- **WiFi signal indicator** — live strength bar in the header
- **Navigation history** — 50-URL back/forward stack
- **Boot menu** — quick Wikipedia search on startup; battery voltage display (Vbat)
- **AI Chat** — built-in chat screen backed by a server-side PHP endpoint
- **Power management** — backlight dim → off → light-sleep after inactivity; deep-sleep Off button (battery build)
- **Battery voltage displayed** — needs circuit mod

---

## Hardware

| Component | Detail |
|-----------|--------|
| Board     | Guition / Sunton JC3248W535C |
| MCU       | ESP32-S3, dual-core 240 MHz with 16MB flash and 8MB RAM|
| Flash     | 16 MB QIO (quad SPI) |
| PSRAM     | 8 MB OPI (octal SPI) high-speed |
| Display   | 320×480 AXS15231B (landscape: 480×320), QSPI interface |
| Touch     | GT911, I²C |

The board is widely sold under several brand names (Guition, Sunton, Elecrow 4.0"). Look for the **JC3248W535C** or **ESP32-S3-4848S040** part number.

---

## Architecture

Two FreeRTOS tasks communicate via a message queue:

```
Core 0 — Network Task              Core 1 — UI Task
  WiFi management                    LVGL lv_timer_handler()
  HTTPS fetch (2 MB PSRAM buffer)    GT911 touch polling
  Single-pass HTML tokenizer         Widget builder
  PageElement array builder          Gesture detection
          |                                  |
          +--------[ FreeRTOS Queue ]--------+
                     PageResult msg
```

PSRAM memory budget:

| Region | Size |
|--------|------|
| LVGL frame buffer (×1) | 300 KB |
| Raw HTML buffer | 2 MB |
| Parsed page elements + text pool | ~176 KB |
| Navigation history (50 × 512 B) | 25 KB |
| Image buffer — thumbnail (dynamic) | up to 128 KB |
| Image buffer — full-size (dynamic) | up to 512 KB |
| TLS SSL buffers (redirected to PSRAM) | ~40 KB per connection |

---
<img width="3738" height="2304" alt="IMG_20260515_113138109_HDR" src="https://github.com/user-attachments/assets/ae6c2767-791f-45b1-826b-8fdb0ab1300c" />

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- Linux/macOS/WSL recommended; Windows native should also work
- USB-C cable to the board's programming port

### Build & Flash

```bash
git clone https://github.com/alunmorris/babe32.git
cd babe32

# Default build (base browser)
pio run -e jc3248w535c --target upload

# Battery / power-saving build
pio run -e jc3248w535c_battery --target upload
```

Monitor serial output at 115200 baud:

```bash
pio device monitor
```

### Build Environments

| Environment | Description |
|-------------|-------------|
| `jc3248w535c` | Standard build |
| `jc3248w535c_battery` | Battery power management (dim/sleep) |

---

## Configuration

### WiFi

On first boot, open the boot menu and tap **WiFi Setup**. The browser scans for networks, you enter the password, and credentials are saved to NVS. Up to 10 networks are stored; the device tries each on startup.

### Proxy Setup (optional but recommended)

Babe32 tries to fetch pages directly over TLS first. If a site blocks direct requests (detected per session), it falls back to two proxies in order:

1. **Fallback 1 — self-hosted PHP proxy** (`src/server/babe32proxy.php`)  
   Upload this to any PHP web host. Set `PHP_HOST` and `PHP_PATH` in `src/fetcher.cpp` to point at your server.

2. **Fallback 2 — Brightdata residential proxy**  
   Sign up at [brightdata.com](https://brightdata.com), create a residential proxy, and fill in `PROXY_HOST`, `PROXY_PORT`, and `PROXY_AUTH` in `src/fetcher.cpp`. Brightdata is paid for but very cheap.

Many sites work without any proxy configured. For sites that block direct device requests (e.g. some news sites), the PHP proxy is sufficient.

### AI Chat (optional)

Upload `src/server/aichat.php` to your web server and update the endpoint URL in `src/ui_task.cpp`. The chat screen will then be available from the boot menu.

### Battery Voltage (optional)

Connect two 150k resistors in series between U2 (IP5306) pin 6 (BAT) and ground. Connect the middle (voltage = BAT/2) to GP106. Note that GP105 has an internal pullup and can't be used for this.
<img width="2048" height="1343" alt="WhatsApp Image 2026-06-10 at 08 57 02 (1)" src="https://github.com/user-attachments/assets/e2ccc9f0-85a7-4cdc-a5ae-d553c505bbd5" />
<img width="2048" height="1268" alt="WhatsApp Image 2026-06-10 at 08 57 02" src="https://github.com/user-attachments/assets/40085866-0940-4755-a599-d8b8a4d69241" />

---

## Project Structure

```
babe32/
├── platformio.ini
└── src/
    ├── main.cpp               # Entry point
    ├── display.{h,cpp}        # AXS15231B QSPI display driver + LVGL flush
    ├── touch.{h,cpp}          # GT911 touch driver + LVGL indev
    ├── ui_task.{h,cpp}        # LVGL FreeRTOS task (core 1)
    ├── net_task.{h,cpp}       # Network FreeRTOS task (core 0)
    ├── wifi_mgr.{h,cpp}       # Multi-AP NVS storage + connect
    ├── wifi_setup.{h,cpp}     # WiFi setup UI (scan, password, save)
    ├── fetcher.{h,cpp}        # HTTPS fetch into PSRAM buffer
    ├── html_parser.{h,cpp}    # Single-pass HTML tokenizer
    ├── page_renderer.{h,cpp}  # LVGL widget builder
    ├── image_fetch.{h,cpp}    # Image fetch via resize proxy
    ├── img_task.{h,cpp}       # Non-blocking image queue
    ├── url_utils.{h,cpp}      # URL resolution + proxy wrapping
    ├── history.{h,cpp}        # 50-URL navigation history
    ├── boot_menu.{h,cpp}      # Boot screen + Wikipedia search
    ├── gesture.{h,cpp}        # Swipe detection
    ├── power_mgr.{h,cpp}      # Battery power management
    ├── ui_header.{h,cpp}      # Header bar (back/fwd/URL/signal)
    ├── ui_buttons.{h,cpp}     # Toolbar buttons
    └── server/
        ├── babe32proxy.php    # Self-hosted fetch proxy (fallback)
        ├── image-resize.php   # Image resize proxy
        └── aichat.php         # AI chat backend
```

---

## Limitations

- **Images load slowly** — fetched via resize proxy; can be toggled off with the IMGs button
- **No JavaScript** — pages are rendered server-side text; interactive JS apps won't work
- **No TLS certificate pinning** — the device accepts any certificate; use a trusted network
- **Single tab** — one page at a time
- **480×320 landscape display** — pages are reformatted to fit the width

---

## Licence

MIT — see [LICENSE](LICENSE) for details.
