/* Enable POSIX extensions (required for usleep, etc.) */
#define _POSIX_C_SOURCE 200112L

/*
 * departures.c - TTC Transit Terminal Departure Board
 *
 * Simulates a real-time TTC (Toronto Transit Commission) departure board.
 * Reads route and schedule data from routes.dat and displays a live-
 * refreshing countdown board styled in TTC colours (red and white).
 *
 * Features:
 *   - Live terminal UI that refreshes every second (no Enter key needed)
 *   - Countdown timers for the next 3 departures per route
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
#define SHOW_NEXT          3          /* Departures to display per route     */
#define ALERT_CHANCE      15          /* % chance a route gets a service alert*/
#define DELAY_MIN_MINS     3          /* Minimum delay (minutes)             */
#define DELAY_MAX_MINS    12          /* Maximum delay (minutes)             */

/* ── Colour pair IDs (ncurses) ──────────────────────────────────────────── */

#define CLR_HEADER     1   /* TTC red background, white text (header bar)   */
#define CLR_SUBHEADER  2   /* Dark red bg, white text (column labels)       */
#define CLR_ROUTE_NUM  3   /* Bold yellow on black (route number)           */
#define CLR_ROUTE_NAME 4   /* White on black (route name & direction)       */
#define CLR_TIME_GREEN 5   /* Green on black (> 5 min away)                 */
#define CLR_TIME_YELLOW 6  /* Yellow on black (2-5 min away)                */
#define CLR_TIME_RED   7   /* Red on black (< 2 min away)                   */
#define CLR_ALERT_DELAY 8  /* Yellow on dark red (Delay alert)              */
#define CLR_ALERT_STALL 9  /* White on red (Stalled alert)                  */
#define CLR_BORDER    10   /* Dark grey border elements                     */
#define CLR_FOOTER    11   /* Black on white (footer / key hints)           */
#define CLR_DIM       12   /* Dim white (no upcoming departures message)    */

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
        strncpy(r->route_num,   route_num,   sizeof(r->route_num)   - 1);
        strncpy(r->route_name,  route_name,  sizeof(r->route_name)  - 1);
        strncpy(r->direction,   direction,   sizeof(r->direction)   - 1);
        strncpy(r->stop_name,   stop_name,   sizeof(r->stop_name)   - 1);
        r->route_num[sizeof(r->route_num)-1]   = '\0';
        r->route_name[sizeof(r->route_name)-1] = '\0';
        r->direction[sizeof(r->direction)-1]   = '\0';
        r->stop_name[sizeof(r->stop_name)-1]   = '\0';

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

        r->status       = STATUS_ON_TIME;
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
                routes[i].status = STATUS_STALLED;
                routes[i].delay_minutes = 0;
            } else {
                routes[i].status = STATUS_DELAYED;
                routes[i].delay_minutes =
                    DELAY_MIN_MINS + rand() % (DELAY_MAX_MINS - DELAY_MIN_MINS + 1);
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
 * get_next_departures() fills 'out' with the next 'count' departure times
 * (in minutes-from-midnight) for route r, starting from 'now_minutes'.
 * Delays are added to each scheduled time.  Returns the number filled.
 */
static int get_next_departures(const Route *r, int now_minutes,
                               int *out, int count)
{
    int found = 0;

    /* We search across "today" and "tomorrow" in case now is near midnight */
    for (int wrap = 0; wrap <= 1 && found < count; wrap++) {
        int base = wrap * 1440;
        for (int i = 0; i < r->num_departures && found < count; i++) {
            int t = r->departures[i] + base + r->delay_minutes;
            if (t > now_minutes) {
                out[found++] = t % 1440;
            }
        }
    }
    return found;
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
     *   Primary red  : COLOR_RED  (terminal approximation of TTC #DA291C)
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
}

/* ── Draw the TTC header bar ────────────────────────────────────────────── */

static void draw_header(int cols, const char *stop_name, time_t now_t)
{
    /* Full-width red bar with TTC branding */
    attron(COLOR_PAIR(CLR_HEADER) | A_BOLD);

    /* Clear the top two rows in red */
    for (int c = 0; c < cols; c++) mvaddch(0, c, ' ');
    for (int c = 0; c < cols; c++) mvaddch(1, c, ' ');

    /* Row 0: TTC logo text + clock */
    char clock_buf[16];
    struct tm *lt = localtime(&now_t);
    strftime(clock_buf, sizeof(clock_buf), "%I:%M:%S %p", lt);

    mvprintw(0, 1, " TTC  TORONTO TRANSIT COMMISSION");

    /* Right-align the clock on row 0 */
    int clock_col = cols - (int)strlen(clock_buf) - 2;
    if (clock_col > 33) mvprintw(0, clock_col, "%s", clock_buf);

    /* Row 1: Stop name + direction filter */
    char stop_line[128];
    snprintf(stop_line, sizeof(stop_line), " STOP: %-30s", stop_name);
    mvprintw(1, 0, "%s", stop_line);

    /* Right-align direction filter label */
    const char *filt = filter_labels[direction_filter];
    int filt_col = cols - (int)strlen(filt) - 2;
    if (filt_col > (int)strlen(stop_line))
        mvprintw(1, filt_col, "%s", filt);

    attroff(COLOR_PAIR(CLR_HEADER) | A_BOLD);
}

/* ── Draw column header row ─────────────────────────────────────────────── */

static void draw_column_headers(int row, int cols)
{
    attron(COLOR_PAIR(CLR_SUBHEADER) | A_BOLD);
    for (int c = 0; c < cols; c++) mvaddch(row, c, ' ');
    mvprintw(row, 1,  "%-4s", "RTE");
    mvprintw(row, 6,  "%-18s", "ROUTE NAME");
    mvprintw(row, 25, "%-12s", "DIRECTION");
    mvprintw(row, 38, "%-12s", "DEPARTURES");
    mvprintw(row, 61, "%-14s", "STATUS");
    attroff(COLOR_PAIR(CLR_SUBHEADER) | A_BOLD);
}

/* ── Format a single countdown cell ────────────────────────────────────── */

/*
 * Returns the colour pair to use for a countdown value, and writes
 * a human-readable countdown string into buf.
 */
static int format_countdown(int mins_away, char *buf, int buf_len)
{
    int colour;
    if (mins_away < 1) {
        snprintf(buf, buf_len, "ARRIVING");
        colour = CLR_TIME_RED;
    } else if (mins_away < 2) {
        snprintf(buf, buf_len, "1 min");
        colour = CLR_TIME_RED;
    } else if (mins_away <= 5) {
        snprintf(buf, buf_len, "%d mins", mins_away);
        colour = CLR_TIME_YELLOW;
    } else {
        snprintf(buf, buf_len, "%d mins", mins_away);
        colour = CLR_TIME_GREEN;
    }
    return colour;
}

/* ── Draw one route row ─────────────────────────────────────────────────── */

static void draw_route_row(int row, const Route *r, int now_minutes, int cols)
{
    /* Horizontal separator line */
    attron(COLOR_PAIR(CLR_BORDER));
    for (int c = 0; c < cols; c++) mvaddch(row, c, '-');
    attroff(COLOR_PAIR(CLR_BORDER));
    row++;

    /* Route number (bold yellow) */
    attron(COLOR_PAIR(CLR_ROUTE_NUM) | A_BOLD);
    mvprintw(row, 1, "%-4s", r->route_num);
    attroff(COLOR_PAIR(CLR_ROUTE_NUM) | A_BOLD);

    /* Route name */
    attron(COLOR_PAIR(CLR_ROUTE_NAME));
    mvprintw(row, 6,  "%-18s", r->route_name);
    mvprintw(row, 25, "%-12s", r->direction);
    attroff(COLOR_PAIR(CLR_ROUTE_NAME));

    /* Next departures */
    int next[SHOW_NEXT];
    int found = get_next_departures(r, now_minutes, next, SHOW_NEXT);

    int col = 38;
    for (int i = 0; i < found && col < cols - 10; i++) {
        int diff = next[i] - now_minutes;
        if (diff < 0) diff += 1440;  /* Wrap past midnight */

        char cbuf[16];
        int clr = format_countdown(diff, cbuf, sizeof(cbuf));

        attron(COLOR_PAIR(clr) | (diff < 2 ? A_BOLD : 0));
        mvprintw(row, col, "%-10s", cbuf);
        attroff(COLOR_PAIR(clr) | A_BOLD);
        col += 11;
    }

    if (found == 0) {
        attron(COLOR_PAIR(CLR_DIM) | A_DIM);
        mvprintw(row, 38, "No service");
        attroff(COLOR_PAIR(CLR_DIM) | A_DIM);
    }

    /* Service alert badge */
    if (col > 60) col = 61;
    if (r->status == STATUS_STALLED) {
        attron(COLOR_PAIR(CLR_ALERT_STALL) | A_BOLD | A_BLINK);
        mvprintw(row, 61, "** STALLED **");
        attroff(COLOR_PAIR(CLR_ALERT_STALL) | A_BOLD | A_BLINK);
    } else if (r->status == STATUS_DELAYED) {
        attron(COLOR_PAIR(CLR_ALERT_DELAY) | A_BOLD);
        mvprintw(row, 61, "DELAY +%d min ", r->delay_minutes);
        attroff(COLOR_PAIR(CLR_ALERT_DELAY) | A_BOLD);
    } else {
        attron(COLOR_PAIR(CLR_TIME_GREEN));
        mvprintw(row, 61, "On Time      ");
        attroff(COLOR_PAIR(CLR_TIME_GREEN));
    }
}

/* ── Draw the footer / key-binding hint bar ─────────────────────────────── */

static void draw_footer(int rows, int cols)
{
    int row = rows - 1;
    attron(COLOR_PAIR(CLR_FOOTER) | A_BOLD);
    for (int c = 0; c < cols; c++) mvaddch(row, c, ' ');
    mvprintw(row, 1, " [D] Toggle Direction   [Q] Quit   "
                     "Refreshes every second");
    attroff(COLOR_PAIR(CLR_FOOTER) | A_BOLD);
}

/* ── Main render pass ───────────────────────────────────────────────────── */

static void render(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    /* Suppress output if terminal is too narrow to look right */
    if (cols < 50 || rows < 8) {
        clear();
        mvprintw(0, 0, "Terminal too small. Please resize to at least 80x20.");
        refresh();
        return;
    }

    clear();

    /* Current time */
    time_t now_t    = time(NULL);
    struct tm *lt   = localtime(&now_t);
    int now_minutes = lt->tm_hour * 60 + lt->tm_min;

    /* Determine stop name from first loaded route */
    const char *stop_name = (num_routes > 0) ? routes[0].stop_name
                                              : "Unknown Stop";

    draw_header(cols, stop_name, now_t);
    draw_column_headers(2, cols);

    /* Render each visible route row (2 display rows each: separator + data) */
    int draw_row = 3;
    int visible  = 0;

    for (int i = 0; i < num_routes; i++) {
        if (!route_matches_filter(&routes[i])) continue;

        /* Stop drawing if we've run out of vertical space */
        if (draw_row + 2 > rows - 2) {
            attron(COLOR_PAIR(CLR_DIM) | A_DIM);
            mvprintw(draw_row, 2, "... more routes not shown (resize terminal)");
            attroff(COLOR_PAIR(CLR_DIM) | A_DIM);
            break;
        }

        draw_route_row(draw_row, &routes[i], now_minutes, cols);
        draw_row += 2;
        visible++;
    }

    /* Bottom separator */
    if (draw_row < rows - 1) {
        attron(COLOR_PAIR(CLR_BORDER));
        for (int c = 0; c < cols; c++) mvaddch(draw_row, c, '=');
        attroff(COLOR_PAIR(CLR_BORDER));
    }

    if (visible == 0) {
        mvprintw(5, 2, "No routes match the current filter.");
    }

    draw_footer(rows, cols);
    refresh();
}

/* ── Entry Point ────────────────────────────────────────────────────────── */

int main(void)
{
    /* Load route data */
    if (parse_routes(DATA_FILE) == 0) {
        fprintf(stderr,
            "Error: Could not read route data from '%s'.\n"
            "Make sure the file exists in the current directory.\n",
            DATA_FILE);
        return EXIT_FAILURE;
    }

    /* Assign random service alerts for this session */
    assign_alerts();

    /* Initialise ncurses */
    init_ncurses();

    /* Main event loop — refresh every REFRESH_MS milliseconds */
    int running = 1;
    while (running) {
        render();

        /* Poll for keypress without blocking */
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

        /* Sleep for REFRESH_MS milliseconds between renders using select() */
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = REFRESH_MS * 1000;
        select(0, NULL, NULL, NULL, &tv);
    }

    endwin();
    printf("Thanks for riding with TTC!\n");
    return EXIT_SUCCESS;
}
