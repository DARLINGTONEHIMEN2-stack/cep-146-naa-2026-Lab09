# cep-146-naa-2026-Lab09

## Project Overview
**CEP-146 Lab 09 — Option A: TTC Transit Terminal Departures Board**

A real-time, live-refreshing terminal departure board written in C, styled
after the TTC (Toronto Transit Commission) colour scheme. Displays upcoming
departures with countdown timers, service alerts, and a direction filter.

## Features
- **Live terminal UI** — Refreshes every 500ms using ncurses; no Enter key needed
- **TTC colour theme** — Red header bar, white/yellow route info, colour-coded countdowns
- **Countdown colours** — Green (>5 min), Yellow (2–5 min), Red (<2 min / Arriving)
- **Service Alerts** — Randomly assigned Delay or Stalled statuses each session
- **Direction toggle** — Press `D` to cycle All / Eastbound+Northbound / Westbound+Southbound

## Files
| File           | Description                                         |
|----------------|-----------------------------------------------------|
| `departures.c` | Main C source — ncurses UI, parser, countdown logic |
| `routes.dat`   | Route data file (pipe-separated schedule records)   |
| `Makefile`     | Build rules (`make all`, `make clean`, `make run`)  |
| `run.sh`       | Shell script — builds then launches the program     |

## Building & Running
```bash
# Build only
make all

# Build and run
make run

# Or via the workflow script
bash run.sh
```

## Routes Data Format (`routes.dat`)
Lines starting with `#` are comments. Data lines are pipe-separated:
```
ROUTE_NUM|ROUTE_NAME|DIRECTION|STOP_NAME|START_HH:MM|END_HH:MM|HEADWAY_MINS|OFFSET_MINS
```

## Key Bindings (while running)
| Key | Action                          |
|-----|---------------------------------|
| `D` | Toggle direction filter         |
| `Q` | Quit the departure board        |

## Dependencies
- `gcc` — C11 compiler
- `ncurses` — Terminal UI library (`-lncurses`)

## Environment
- Replit NixOS (stable-25_05)
- Language: C (C11 standard)
- Stop simulated: King Street & Bay Street (Union Station Area), TTC
