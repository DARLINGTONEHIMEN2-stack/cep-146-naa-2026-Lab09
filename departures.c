/* Enable POSIX extensions (required for select, etc.) */
#define _POSIX_C_SOURCE 200112L

/*
 * departures.c - TTC Transit Terminal Departure Board
 *
 * Simulates a real-time TTC (Toronto Transit Commission) departure board.
 * Reads route and schedule data from routes.dat and displays a live-
 * refreshing countdown board styled in TTC colours (red and white).
 *
 * Features:
 *   - Live terminal UI that refreshes every 500 ms (no Enter key needed)
 *   - Countdown timers showing the next 2 departures per route
 *   - HH:MM scheduled time displayed alongside each countdown
 *   - Seconds-level countdown for arrivals under 2 minutes
 *   - Service Alerts: random Delay / Stalled statuses per session
 *   - Direction toggle: press 'd' to cycle All / Eastbound+Northbound /
 *     Westbound+Southbound views
 *   - Colour-coded urgency: green > 5 min, yellow 2-5 min, red < 2 min
 *
 * Build:  gcc -o departures departures.c -lncurses
 * Run:    ./departures
 *
 * Data file: routes.dat (must be in the same directory)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ncurses.h>
#include <unistd.h>
#include <sys/select.h>
#include <ctype.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define MAX_ROUTES        64          /* Maximum routes loaded from file     */
#define MAX_DEPARTURES   200          /* Maximum timetable entries per route */
#define MAX_NAME_LEN      64          /* Maximum length for names/strings    */
#define DATA_FILE    "routes.dat"     /* Route data file path                */
#define REFRESH_MS       500          /* UI refresh interval (milliseconds)  */
#define SHOW_NEXT          2          /* Departures to display per route     */
#define ALERT_CHANCE      15          /* % chance a route gets a service alert*/
#define DELAY_MIN_MINS     3          /* Minimum delay (minutes)             */
#define DELAY_MAX_MINS    12          /* Maximum delay (minutes)             */

/* Column layout constants (positions within the terminal row) */
#define COL_ROUTE_NUM     1           /* Route number column start           */
#define COL_ROUTE_NAME    6           /* Route name column start             */
#define COL_DIRECTION    24           /* Direction column start              */
#define COL_DEP1         38           /* First departure countdown column    */
#define COL_DEP2         51           /* Second departure countdown column   */
#define COL_STATUS       65           /* Service status column               */
#define COL_WIDTH_DEP    12           /* Width of each departure cell        */
#define MIN_COLS         80           /* Minimum terminal width to display   */
#define MIN_ROWS         10           /* Minimum terminal height to display  */

/* ── Colour pair IDs (ncurses) ──────────────────────────────────────────── */

#define CLR_HEADER      1   /* TTC red background, white text (header bar)  */
#define CLR_SUBHEADER   2   /* Red bg, white text (column labels)           */
#define CLR_ROUTE_NUM   3   /* Bold yellow on black (route number)          */
#define CLR_ROUTE_NAME  4   /* White on black (route name & direction)      */
#define CLR_TIME_GREEN  5   /* Green on black (> 5 min away)                */
#define CLR_TIME_YELLOW 6   /* Yellow on black (2-5 min away)               */
#define CLR_TIME_RED    7   /* Bold red on black (< 2 min / arriving)       */
#define CLR_ALERT_DELAY 8   /* Yellow on red (Delay alert)                  */
#define CLR_ALERT_STALL 9   /* White on red blinking (Stalled alert)        */
#define CLR_BORDER     10   /* Red separators                               */
#define CLR_FOOTER     11   /* Black on white (footer / key hints)          */
#define CLR_DIM        12   /* Dim white on black (secondary info)          */
#define CLR_TIME_NOW   13   /* Bright red on black (imminent arrival)       */

/* ── Service Status ─────────────────────────────────────────────────────── */

typedef enum {
    STATUS_ON_TIME = 0,
    STATUS_DELAYED,
    STATUS_STALLED
} ServiceStatus;

/* ── Data Structures ────────────────────────────────────────────────────── */

/*
 * Route: holds all data for one direction of one TTC route at this stop.
 * Departure times are stored as minutes-from-midnight integers so arithmetic
 * is simple (no HH:MM string parsing at runtime).
 */
typedef struct {
    char route_num[16];                  /* e.g. "504"                      */
    char route_name[MAX_NAME_LEN];       /* e.g. "King"                     */
    char direction[32];                  /* e.g. "Eastbound"                */
    char stop_name[MAX_NAME_LEN];        /* e.g. "King & Bay"               */
    int  departures[MAX_DEPARTURES];     /* Minutes-from-midnight array     */
    int  num_departures;                 /* Number of entries in array      */
    ServiceStatus status;                /* Current service status          */
    int  delay_minutes;                  /* Extra delay if STATUS_DELAYED   */
} Route;

/* ── Global State ───────────────────────────────────────────────────────── */

static Route routes[MAX_ROUTES];
static int   num_routes = 0;

/*
 * direction_filter: controls which routes are shown.
 *   0 = All directions
 *   1 = Eastbound and Northbound only
 *   2 = Westbound and Southbound only
 */
static int direction_filter = 0;
static const char *filter_labels[] = {
    "ALL ROUTES", "EASTBOUND / NORTHBOUND", "WESTBOUND / SOUTHBOUND"
};

/* ── Utility: parse HH:MM string into minutes-from-midnight ─────────────── */

static int hhmm_to_minutes(const char *hhmm)
{
    int h = 0, m = 0;
    if (sscanf(hhmm, "%d:%d", &h, &m) != 2) return -1;
    return h * 60 + m;
}

/* ── Utility: convert minutes-from-midnight to "H:MMp" string ───────────── */

/*
 * Format a timetable time as a compact 12-hour clock string, e.g. "4:47p".
 * buf must be at least 8 bytes.
 */
static void minutes_to_hhmm12(int minutes_from_midnight, char *buf, int buf_len)
{
    int total = minutes_from_midnight % 1440;
    if (total < 0) total += 1440;
    int h  = total / 60;
    int m  = total % 60;
    char ampm = (h < 12) ? 'a' : 'p';
    if (h == 0)       h = 12;
    else if (h > 12)  h -= 12;
    snprintf(buf, buf_len, "%d:%02d%c", h, m, ampm);
}

/* ── Load routes from data file ─────────────────────────────────────────── */

/*
 * parse_routes() reads routes.dat and populates the global routes[] array.
 * Lines beginning with '#' or blank are skipped (comments).
 *
 * Expected format per data line (pipe-separated):
 *   ROUTE_NUM|ROUTE_NAME|DIRECTION|STOP_NAME|START|END|HEADWAY|OFFSET
 *
 * Departure times are generated between START and END every HEADWAY minutes,
 * beginning at (START + OFFSET) minutes.  Times past midnight (>= 1440) are
 * wrapped so the board correctly handles late-night service.
 */
static int parse_routes(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return 0;   /* caller will handle the error */
    }

    char line[512];
    while (fgets(line, sizeof(line), fp) && num_routes < MAX_ROUTES) {

        /* Skip comment lines and blank lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        /* Tokenise the pipe-separated fields */
        char route_num[16], route_name[MAX_NAME_LEN];
        char direction[32], stop_name[MAX_NAME_LEN];
        char start_str[16], end_str[16];
        int  headway = 0, offset = 0;

        if (sscanf(p, "%15[^|]|%63[^|]|%31[^|]|%63[^|]|%15[^|]|%15[^|]|%d|%d",
                   route_num, route_name, direction, stop_name,
                   start_str, end_str, &headway, &offset) != 8) {
            continue;   /* Malformed line — skip it */
        }

        if (headway <= 0) continue;

        Route *r = &routes[num_routes];

        /* Use snprintf to safely copy strings (avoids truncation warnings) */
        snprintf(r->route_num,  sizeof(r->route_num),  "%s", route_num);
        snprintf(r->route_name, sizeof(r->route_name), "%s", route_name);
        snprintf(r->direction,  sizeof(r->direction),  "%s", direction);
        snprintf(r->stop_name,  sizeof(r->stop_name),  "%s", stop_name);

        int start = hhmm_to_minutes(start_str);
        int end   = hhmm_to_minutes(end_str);
        if (start < 0 || end < 0) continue;

        /* Handle overnight service: end before start means next-day minutes */
        if (end < start) end += 1440;

        r->num_departures = 0;
        int t = start + offset;
        while (t <= end && r->num_departures < MAX_DEPARTURES) {
            r->departures[r->num_departures++] = t % 1440;
            t += headway;
        }

        r->status        = STATUS_ON_TIME;
        r->delay_minutes = 0;
        num_routes++;
    }

    fclose(fp);
    return num_routes;
}

/* ── Assign random service alerts for this session ──────────────────────── */

/*
 * assign_alerts() randomly sets some routes to DELAYED or STALLED status.
 * Uses a seeded RNG so alerts are stable for the duration of the session
 * but different each launch.  ALERT_CHANCE % of routes get an alert.
 */
static void assign_alerts(void)
{
    srand((unsigned int)time(NULL));
    for (int i = 0; i < num_routes; i++) {
        int roll = rand() % 100;
        if (roll < ALERT_CHANCE) {
            /* Decide: stalled (1-in-3 of alerts) or delayed (2-in-3) */
            if (rand() % 3 == 0) {
                routes[i].status        = STATUS_STALLED;
                routes[i].delay_minutes = 0;
            } else {
                routes[i].status        = STATUS_DELAYED;
                routes[i].delay_minutes =
                    DELAY_MIN_MINS +
                    rand() % (DELAY_MAX_MINS - DELAY_MIN_MINS + 1);
            }
        }
    }
}

/* ── Check if a route matches the current direction filter ──────────────── */

static int route_matches_filter(const Route *r)
{
    if (direction_filter == 0) return 1;

    /* Filter 1: Eastbound and Northbound */
    if (direction_filter == 1) {
        return (strncmp(r->direction, "East", 4) == 0 ||
                strncmp(r->direction, "North", 5) == 0);
    }
    /* Filter 2: Westbound and Southbound */
    return (strncmp(r->direction, "West", 4) == 0 ||
            strncmp(r->direction, "South", 5) == 0);
}

/* ── Calculate the next N departure times (accounting for delays) ────────── */

/*
 * get_next_departures() fills 'out_minutes' with the next 'count' departure
 * times (minutes-from-midnight) for route r, starting after 'now_seconds'
 * seconds from midnight (so we can honour sub-minute delays).
 * Returns the number of entries filled.
 */
static int get_next_departures(const Route *r, long now_seconds,
                               int *out_minutes, int count)
{
    int found = 0;
    long now_m = now_seconds / 60;   /* current whole-minute mark */

    /* Search today and "tomorrow" in case we're near midnight */
    for (int wrap = 0; wrap <= 1 && found < count; wrap++) {
        long base = (long)wrap * 1440;
        for (int i = 0; i < r->num_departures && found < count; i++) {
            long t = (long)r->departures[i] + base + r->delay_minutes;
            /*
             * Include a departure if it hasn't fully left yet:
             * we treat anything still >= now_minutes as upcoming.
             * Using whole minutes keeps the display stable (no jitter).
             */
            if (t > now_m) {
                out_minutes[found++] = (int)(t % 1440);
            }
        }
    }
    return found;
}

/* ── Format a departure countdown cell ─────────────────────────────────── */

/*
 * format_departure_cell() builds a short string like:
 *   "4m (4:47p)"   — for 4 minutes away
 *   "45s (4:43p)"  — for 45 seconds away (imminent)
 *   "** NOW **  "  — for currently departing
 *
 * dep_min : departure time in minutes-from-midnight
 * now_secs: current time in seconds-from-midnight
 * buf     : output buffer (at least COL_WIDTH_DEP + 1 bytes)
 * Returns the ncurses colour pair ID to use for this cell.
 */
static int format_departure_cell(int dep_min, long now_secs,
                                 char *buf, int buf_len)
{
    long dep_secs  = (long)dep_min * 60;
    long diff_secs = dep_secs - now_secs;
    /* Handle midnight wrap */
    if (diff_secs < -43200) diff_secs += 86400;
    if (diff_secs >  43200) diff_secs -= 86400;

    char time_str[10];
    minutes_to_hhmm12(dep_min, time_str, sizeof(time_str));

    int colour;

    if (diff_secs <= 0) {
        snprintf(buf, buf_len, "** NOW **   ");
        colour = CLR_TIME_NOW;
    } else if (diff_secs < 60) {
        /* Under 1 minute: show seconds */
        snprintf(buf, buf_len, "%lds (%s) ", diff_secs, time_str);
        colour = CLR_TIME_RED;
    } else if (diff_secs < 120) {
        /* 1-2 minutes */
        snprintf(buf, buf_len, "1m (%s) ", time_str);
        colour = CLR_TIME_RED;
    } else if (diff_secs <= 300) {
        /* 2-5 minutes */
        snprintf(buf, buf_len, "%ldm (%s) ", diff_secs / 60, time_str);
        colour = CLR_TIME_YELLOW;
    } else {
        /* Over 5 minutes */
        snprintf(buf, buf_len, "%ldm (%s) ", diff_secs / 60, time_str);
        colour = CLR_TIME_GREEN;
    }
    return colour;
}

/* ── ncurses initialisation ─────────────────────────────────────────────── */

static void init_ncurses(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);   /* Non-blocking getch() for live refresh       */
    curs_set(0);             /* Hide the text cursor                        */

    start_color();
    use_default_colors();

    /*
     * TTC colour palette:
     *   Primary red  : COLOR_RED   (terminal approximation of TTC #DA291C)
     *   Accent black : COLOR_BLACK
     *   Text white   : COLOR_WHITE
     */
    init_pair(CLR_HEADER,      COLOR_WHITE,  COLOR_RED);
    init_pair(CLR_SUBHEADER,   COLOR_WHITE,  COLOR_RED);
    init_pair(CLR_ROUTE_NUM,   COLOR_YELLOW, COLOR_BLACK);
    init_pair(CLR_ROUTE_NAME,  COLOR_WHITE,  COLOR_BLACK);
    init_pair(CLR_TIME_GREEN,  COLOR_GREEN,  COLOR_BLACK);
    init_pair(CLR_TIME_YELLOW, COLOR_YELLOW, COLOR_BLACK);
    init_pair(CLR_TIME_RED,    COLOR_RED,    COLOR_BLACK);
    init_pair(CLR_ALERT_DELAY, COLOR_YELLOW, COLOR_RED);
    init_pair(CLR_ALERT_STALL, COLOR_WHITE,  COLOR_RED);
    init_pair(CLR_BORDER,      COLOR_RED,    COLOR_BLACK);
    init_pair(CLR_FOOTER,      COLOR_BLACK,  COLOR_WHITE);
    init_pair(CLR_DIM,         COLOR_WHITE,  COLOR_BLACK);
    init_pair(CLR_TIME_NOW,    COLOR_RED,    COLOR_BLACK);
}

/* ── Draw the TTC header bar ────────────────────────────────────────────── */

/*
 * Draws two rows of TTC-red header:
 *   Row 0: "TTC  TORONTO TRANSIT COMMISSION"  |  current time (HH:MM:SS AM/PM)
 *   Row 1: "STOP: <stop name>"                |  direction filter label
 */
static void draw_header(int cols, const char *stop_name, struct tm *lt,
                        int tick)
{
    attron(COLOR_PAIR(CLR_HEADER) | A_BOLD);

    /* Fill both rows with red background */
    for (int c = 0; c < cols; c++) mvaddch(0, c, ' ');
    for (int c = 0; c < cols; c++) mvaddch(1, c, ' ');

    /* Row 0: TTC branding */
    mvprintw(0, 1, " TTC  TORONTO TRANSIT COMMISSION");

    /* Blinking colon for the live clock feel */
    char clock_buf[20];
    if (tick % 2 == 0) {
        strftime(clock_buf, sizeof(clock_buf), "%I:%M:%S %p", lt);
    } else {
        strftime(clock_buf, sizeof(clock_buf), "%I %M %S %p", lt);
    }
    int clock_col = cols - (int)strlen(clock_buf) - 2;
    if (clock_col > 33) mvprintw(0, clock_col, "%s", clock_buf);

    /* Row 1: Stop name and direction filter */
    mvprintw(1, 1, " STOP: %-28s", stop_name);

    const char *filt    = filter_labels[direction_filter];
    int         filt_col = cols - (int)strlen(filt) - 2;
    if (filt_col > 38) mvprintw(1, filt_col, "%s", filt);

    attroff(COLOR_PAIR(CLR_HEADER) | A_BOLD);
}

/* ── Draw column header row ─────────────────────────────────────────────── */

static void draw_column_headers(int row, int cols)
{
    attron(COLOR_PAIR(CLR_SUBHEADER) | A_BOLD);
    for (int c = 0; c < cols; c++) mvaddch(row, c, ' ');
    mvprintw(row, COL_ROUTE_NUM,  "%-4s", "RTE");
    mvprintw(row, COL_ROUTE_NAME, "%-17s", "ROUTE NAME");
    mvprintw(row, COL_DIRECTION,  "%-13s", "DIRECTION");
    mvprintw(row, COL_DEP1,       "%-12s", "NEXT");
    mvprintw(row, COL_DEP2,       "%-12s", "FOLLOWING");
    mvprintw(row, COL_STATUS,     "%-15s", "STATUS");
    attroff(COLOR_PAIR(CLR_SUBHEADER) | A_BOLD);
}

/* ── Draw one route row ─────────────────────────────────────────────────── */

/*
 * Each route occupies two terminal rows:
 *   separator row : a red dashed line
 *   data row      : route number | name | direction | dep1 | dep2 | status
 */
static void draw_route_row(int row, const Route *r, long now_secs, int cols)
{
    /* Separator */
    attron(COLOR_PAIR(CLR_BORDER));
    for (int c = 0; c < cols; c++) mvaddch(row, c, '-');
    attroff(COLOR_PAIR(CLR_BORDER));
    row++;

    /* Route number — bold yellow */
    attron(COLOR_PAIR(CLR_ROUTE_NUM) | A_BOLD);
    mvprintw(row, COL_ROUTE_NUM, "%-4s", r->route_num);
    attroff(COLOR_PAIR(CLR_ROUTE_NUM) | A_BOLD);

    /* Route name and direction — white */
    attron(COLOR_PAIR(CLR_ROUTE_NAME));
    mvprintw(row, COL_ROUTE_NAME, "%-17s", r->route_name);
    mvprintw(row, COL_DIRECTION,  "%-13s", r->direction);
    attroff(COLOR_PAIR(CLR_ROUTE_NAME));

    /* Fetch next departures */
    int next[SHOW_NEXT];
    int found = get_next_departures(r, now_secs, next, SHOW_NEXT);

    /* Departure cell columns */
    int dep_cols[2] = { COL_DEP1, COL_DEP2 };

    for (int i = 0; i < SHOW_NEXT; i++) {
        if (i < found) {
            char cell[32];   /* Generous buffer for any countdown format    */
            int  clr = format_departure_cell(next[i], now_secs,
                                             cell, sizeof(cell));
            int bold = (clr == CLR_TIME_RED || clr == CLR_TIME_NOW) ? A_BOLD : 0;
            int blink = (clr == CLR_TIME_NOW) ? A_BLINK : 0;

            attron(COLOR_PAIR(clr) | bold | blink);
            mvprintw(row, dep_cols[i], "%-12s", cell);
            attroff(COLOR_PAIR(clr) | bold | blink);
        } else {
            /* No more departures for today */
            attron(COLOR_PAIR(CLR_DIM) | A_DIM);
            mvprintw(row, dep_cols[i], "%-12s", "---");
            attroff(COLOR_PAIR(CLR_DIM) | A_DIM);
        }
    }

    /* Service status badge */
    if (r->status == STATUS_STALLED) {
        attron(COLOR_PAIR(CLR_ALERT_STALL) | A_BOLD | A_BLINK);
        mvprintw(row, COL_STATUS, "%-15s", "** STALLED **");
        attroff(COLOR_PAIR(CLR_ALERT_STALL) | A_BOLD | A_BLINK);
    } else if (r->status == STATUS_DELAYED) {
        char delay_str[20];
        snprintf(delay_str, sizeof(delay_str), "DELAY +%d min", r->delay_minutes);
        attron(COLOR_PAIR(CLR_ALERT_DELAY) | A_BOLD);
        mvprintw(row, COL_STATUS, "%-15s", delay_str);
        attroff(COLOR_PAIR(CLR_ALERT_DELAY) | A_BOLD);
    } else {
        attron(COLOR_PAIR(CLR_TIME_GREEN));
        mvprintw(row, COL_STATUS, "%-15s", "On Time");
        attroff(COLOR_PAIR(CLR_TIME_GREEN));
    }
}

/* ── Draw the footer / key-binding hint bar ─────────────────────────────── */

static void draw_footer(int rows, int cols, int num_visible)
{
    int row = rows - 1;
    attron(COLOR_PAIR(CLR_FOOTER) | A_BOLD);
    for (int c = 0; c < cols; c++) mvaddch(row, c, ' ');
    mvprintw(row, 1, " [D] Toggle Direction   [Q] Quit"
                     "   Showing %d route(s)   Live refresh", num_visible);
    attroff(COLOR_PAIR(CLR_FOOTER) | A_BOLD);
}

/* ── Main render pass ───────────────────────────────────────────────────── */

static void render(int tick)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (cols < MIN_COLS || rows < MIN_ROWS) {
        clear();
        mvprintw(0, 0, "Terminal too small — please resize to at least %dx%d.",
                 MIN_COLS, MIN_ROWS);
        refresh();
        return;
    }

    clear();

    /* Current time — capture seconds for sub-minute countdown */
    time_t    now_t = time(NULL);
    struct tm *lt   = localtime(&now_t);
    long now_secs   = (long)lt->tm_hour * 3600
                    + (long)lt->tm_min  * 60
                    + (long)lt->tm_sec;

    /* Stop name from the first loaded route */
    const char *stop_name = (num_routes > 0) ? routes[0].stop_name
                                              : "Unknown Stop";

    draw_header(cols, stop_name, lt, tick);
    draw_column_headers(2, cols);

    /* Render route rows — each takes 2 terminal rows (separator + data) */
    int draw_row    = 3;
    int num_visible = 0;

    for (int i = 0; i < num_routes; i++) {
        if (!route_matches_filter(&routes[i])) continue;

        /* Stop if we've run out of vertical room (reserve footer + separator) */
        if (draw_row + 2 > rows - 2) {
            attron(COLOR_PAIR(CLR_DIM) | A_DIM);
            mvprintw(draw_row, 2,
                     "... %d more route(s) not shown — resize terminal to see all",
                     num_routes - i);
            attroff(COLOR_PAIR(CLR_DIM) | A_DIM);
            break;
        }

        draw_route_row(draw_row, &routes[i], now_secs, cols);
        draw_row += 2;
        num_visible++;
    }

    /* Bottom separator bar */
    if (draw_row <= rows - 2) {
        attron(COLOR_PAIR(CLR_BORDER));
        for (int c = 0; c < cols; c++) mvaddch(draw_row, c, '=');
        attroff(COLOR_PAIR(CLR_BORDER));
    }

    if (num_visible == 0) {
        mvprintw(5, 2, "No routes match the current direction filter.");
    }

    draw_footer(rows, cols, num_visible);
    refresh();
}

/* ── Entry Point ────────────────────────────────────────────────────────── */

int main(void)
{
    /* Load route and schedule data */
    if (parse_routes(DATA_FILE) == 0) {
        fprintf(stderr,
            "Error: Could not read route data from '%s'.\n"
            "Make sure the file is in the current directory.\n",
            DATA_FILE);
        return EXIT_FAILURE;
    }

    /* Randomly assign service alerts for this session */
    assign_alerts();

    /* Initialise the ncurses terminal UI */
    init_ncurses();

    /*
     * Main event loop.
     * We keep a tick counter so the clock colon can blink (cosmetic).
     * getch() is non-blocking (nodelay was set in init_ncurses).
     */
    int running = 1;
    int tick    = 0;

    while (running) {
        render(tick++);

        int ch = getch();
        switch (tolower(ch)) {
            case 'q':
                running = 0;
                break;
            case 'd':
                direction_filter = (direction_filter + 1) % 3;
                break;
            default:
                break;
        }

        /* Sleep for REFRESH_MS milliseconds using select() (POSIX portable) */
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = REFRESH_MS * 1000;
        select(0, NULL, NULL, NULL, &tv);
    }

    endwin();
    printf("Thanks for riding with TTC!\n");
    return EXIT_SUCCESS;
}
