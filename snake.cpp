#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <iostream>
#include <deque>
#include <vector>
#include <string>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/select.h>

// ─── ANSI Color Constants ───────────────────────────────────
#define RESET        "\033[0m"
#define BOLD         "\033[1m"
#define DIM          "\033[2m"
#define REVERSE      "\033[7m"
#define RED          "\033[31m"
#define GREEN        "\033[32m"
#define YELLOW       "\033[33m"
#define CYAN         "\033[36m"
#define BRIGHT_GREEN "\033[92m"
#define BRIGHT_CYAN  "\033[96m"
#define BRIGHT_WHITE "\033[97m"

// ─── Game Constants ─────────────────────────────────────────
static const char* APP_DIR_NAME    = "vsnake";
static const char* SCORE_FILENAME  = "snake_scores.txt";
static const int   TICK_US         = 120000;
static const int   MIN_BOARD_W     = 10;
static const int   MIN_BOARD_H     = 10;
static const int   MIN_TERM_W      = 30;
static const int   MIN_TERM_H      = 16;
static const int   APPLE_MAX_TRIES = 1000;

// ─── Animation Constants (in frames at ~120ms/tick) ─────────
static const int APPLE_BLINK_HALF  = 4;
static const int HEAD_GLOW_PERIOD  = 3;
static const int FLASH_DURATION    = 6;

// ─── Async-signal-safe interrupt flag ───────────────────────
static volatile sig_atomic_t g_interrupted = 0;

void signalHandler(int) {
    g_interrupted = 1;
}

// ─── Direction Enum ─────────────────────────────────────────
enum Direction { UP, DOWN, LEFT, RIGHT };

// ─── Point Struct ───────────────────────────────────────────
struct Point {
    int x, y;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

// ─── Score Entry ────────────────────────────────────────────
struct ScoreEntry {
    std::string timestamp;
    int score;
};

// ─── Game State ─────────────────────────────────────────────
struct GameState {
    std::deque<Point> snake;
    Point             apple;
    Direction         dir;
    Direction         nextDir;
    int               score;
    int               boardWidth;
    int               boardHeight;
    int               termWidth;
    int               termHeight;
    bool              running;
    bool              gameOver;
    bool              gameWon;
    bool              termResized;
    bool              termTooSmall;
    bool              paused;
    bool              restartRequested;

    bool              dirChangedThisTick;

    // Animation state (render-only)
    unsigned long     frameCount;
    int               appleFlashTimer;
    int               scoreFlashTimer;
    int               prevScore;

    // Persistent render buffers
    std::vector<char> grid;
    std::string       renderBuf;

    void allocateBuffers() {
        grid.resize(boardWidth * boardHeight);
        renderBuf.reserve((boardWidth * 2 + 80) * (boardHeight + 6));
    }
};

// ─── Terminal helpers ───────────────────────────────────────
static struct termios origTermios;
static bool rawModeEnabled = false;

void disableRawMode() {
    if (rawModeEnabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &origTermios);
        rawModeEnabled = false;
    }
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &origTermios);
    struct termios raw = origTermios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    rawModeEnabled = true;
}

void clearScreen() {
    write(STDOUT_FILENO, "\033[2J\033[1;1H", 11);
}

void hideCursor() {
    write(STDOUT_FILENO, "\033[?25l", 6);
}

void showCursor() {
    write(STDOUT_FILENO, "\033[?25h", 6);
}

void getTerminalSize(int &w, int &h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||
        ws.ws_col == 0 || ws.ws_row == 0) {
        w = 80;
        h = 24;
    } else {
        w = ws.ws_col;
        h = ws.ws_row;
    }
}

// ─── Monotonic clock ────────────────────────────────────────
long long nowMicros() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

// ─── Cleanup ────────────────────────────────────────────────
void performCleanup() {
    write(STDOUT_FILENO, RESET, strlen(RESET));
    showCursor();
    disableRawMode();
    clearScreen();
}

void atexitCleanup() {
    performCleanup();
}

// ─── Timestamp ──────────────────────────────────────────────
std::string getCurrentTimestamp() {
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
    return std::string(buf);
}

// ─── XDG-Compliant Score File Path ──────────────────────────
//
// Follows the XDG Base Directory Specification:
//
//   Priority 1: $XDG_DATA_HOME/vsnake/snake_scores.txt
//   Priority 2: $HOME/.local/share/vsnake/snake_scores.txt
//   Priority 3: ./snake_scores.txt  (last resort fallback)
//
// Creates intermediate directories if they don't exist.
// Uses mkdir() with 0755 permissions (user rwx, group/other rx).
// Silently ignores EEXIST (directory already exists).
//
static bool ensureDirectoryExists(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

// Recursively create path components (like mkdir -p)
static bool mkdirRecursive(const std::string &path) {
    if (path.empty()) return false;

    // Walk the path string and create each component
    std::string built;
    for (size_t i = 0; i < path.size(); i++) {
        built += path[i];
        if (path[i] == '/' && built.size() > 1) {
            struct stat st;
            if (stat(built.c_str(), &st) != 0) {
                if (mkdir(built.c_str(), 0755) != 0 && errno != EEXIST) {
                    return false;
                }
            }
        }
    }
    // Create final component
    return ensureDirectoryExists(path);
}

static std::string getScoreFilePath() {
    std::string dataDir;

    // Priority 1: $XDG_DATA_HOME
    const char* xdgDataHome = getenv("XDG_DATA_HOME");
    if (xdgDataHome && xdgDataHome[0] != '\0') {
        dataDir = std::string(xdgDataHome) + "/" + APP_DIR_NAME;
    }

    // Priority 2: $HOME/.local/share
    if (dataDir.empty()) {
        const char* home = getenv("HOME");
        if (home && home[0] != '\0') {
            dataDir = std::string(home) + "/.local/share/" + APP_DIR_NAME;
        }
    }

    // Attempt to create the directory and return the full path
    if (!dataDir.empty()) {
        if (mkdirRecursive(dataDir)) {
            return dataDir + "/" + SCORE_FILENAME;
        }
        // If directory creation failed (permissions, etc.),
        // fall through to fallback
    }

    // Priority 3: current directory (last resort)
    return SCORE_FILENAME;
}

// ─── Leaderboard I/O ───────────────────────────────────────
void saveScore(int score) {
    std::string path = getScoreFilePath();
    std::ofstream file(path.c_str(), std::ios::app);
    if (file.is_open()) {
        file << getCurrentTimestamp() << " | " << score << "\n";
    }
}

std::vector<ScoreEntry> loadScores() {
    std::string path = getScoreFilePath();
    std::vector<ScoreEntry> scores;
    std::ifstream file(path.c_str());
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            size_t sep = line.find(" | ");
            if (sep != std::string::npos) {
                ScoreEntry e;
                e.timestamp = line.substr(0, sep);
                e.score = std::atoi(line.substr(sep + 3).c_str());
                scores.push_back(e);
            }
        }
    }
    std::sort(scores.begin(), scores.end(),
              [](const ScoreEntry &a, const ScoreEntry &b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.timestamp > b.timestamp;
              });
    return scores;
}

// ─── Safe Apple Spawning ────────────────────────────────────
bool spawnApple(GameState &g) {
    int totalCells = g.boardWidth * g.boardHeight;

    if ((int)g.snake.size() >= totalCells) {
        return false;
    }

    if ((int)g.snake.size() > totalCells * 3 / 4) {
        std::vector<char> occupied(totalCells, 0);
        for (const auto &s : g.snake) {
            occupied[s.y * g.boardWidth + s.x] = 1;
        }
        std::vector<Point> freeCells;
        freeCells.reserve(totalCells - (int)g.snake.size());
        for (int y = 0; y < g.boardHeight; y++) {
            for (int x = 0; x < g.boardWidth; x++) {
                if (!occupied[y * g.boardWidth + x]) {
                    freeCells.push_back({x, y});
                }
            }
        }
        if (freeCells.empty()) return false;
        g.apple = freeCells[rand() % (int)freeCells.size()];
        g.appleFlashTimer = FLASH_DURATION;
        return true;
    }

    for (int attempt = 0; attempt < APPLE_MAX_TRIES; attempt++) {
        Point p = { rand() % g.boardWidth, rand() % g.boardHeight };
        bool onSnake = false;
        for (const auto &s : g.snake) {
            if (s == p) { onSnake = true; break; }
        }
        if (!onSnake) {
            g.apple = p;
            g.appleFlashTimer = FLASH_DURATION;
            return true;
        }
    }

    for (int y = 0; y < g.boardHeight; y++) {
        for (int x = 0; x < g.boardWidth; x++) {
            Point p = {x, y};
            bool onSnake = false;
            for (const auto &s : g.snake) {
                if (s == p) { onSnake = true; break; }
            }
            if (!onSnake) {
                g.apple = p;
                g.appleFlashTimer = FLASH_DURATION;
                return true;
            }
        }
    }

    return false;
}

// ─── Initialization ────────────────────────────────────────
void initGame(GameState &g) {
    getTerminalSize(g.termWidth, g.termHeight);

    g.termTooSmall = (g.termWidth < MIN_TERM_W || g.termHeight < MIN_TERM_H);

    g.boardWidth  = (g.termWidth - 6) / 2;
    g.boardHeight = g.termHeight - 6;
    if (g.boardWidth  < MIN_BOARD_W) g.boardWidth  = MIN_BOARD_W;
    if (g.boardHeight < MIN_BOARD_H) g.boardHeight = MIN_BOARD_H;

    g.snake.clear();
    int cx = g.boardWidth  / 2;
    int cy = g.boardHeight / 2;
    g.snake.push_back({cx,     cy});
    g.snake.push_back({cx - 1, cy});
    g.snake.push_back({cx - 2, cy});

    g.dir                = RIGHT;
    g.nextDir            = RIGHT;
    g.score              = 0;
    g.running            = true;
    g.gameOver           = false;
    g.gameWon            = false;
    g.termResized        = false;
    g.paused             = false;
    g.restartRequested   = false;
    g.dirChangedThisTick = false;

    g.frameCount         = 0;
    g.appleFlashTimer    = 0;
    g.scoreFlashTimer    = 0;
    g.prevScore          = 0;

    g.allocateBuffers();
    spawnApple(g);
}

// ─── Resize Check ───────────────────────────────────────────
bool checkTerminalResize(GameState &g) {
    int newW, newH;
    getTerminalSize(newW, newH);
    if (newW != g.termWidth || newH != g.termHeight) {
        g.termResized = true;
        g.running     = false;
        return true;
    }
    return false;
}

// ─── Non-blocking Input ─────────────────────────────────────
void readInput(GameState &g) {
    char c = 0;
    while (true) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 0};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
            break;
        if (read(STDIN_FILENO, &c, 1) != 1) break;

        if (c == 'q' || c == 'Q') {
            g.running = false;
            return;
        }

        if (c == 'r' || c == 'R') {
            g.restartRequested = true;
            g.running = false;
            return;
        }

        if (c == 'p' || c == 'P') {
            g.paused = !g.paused;
            continue;
        }

        if (g.paused) continue;

        if (c == '\033') {
            char seq[2] = {0, 0};
            fd_set f2; struct timeval t2;
            FD_ZERO(&f2); FD_SET(STDIN_FILENO, &f2);
            t2 = {0, 5000};
            if (select(STDIN_FILENO + 1, &f2, nullptr, nullptr, &t2) > 0)
                read(STDIN_FILENO, &seq[0], 1);
            FD_ZERO(&f2); FD_SET(STDIN_FILENO, &f2);
            t2 = {0, 5000};
            if (select(STDIN_FILENO + 1, &f2, nullptr, nullptr, &t2) > 0)
                read(STDIN_FILENO, &seq[1], 1);

            if (seq[0] == '[' && !g.dirChangedThisTick) {
                switch (seq[1]) {
                    case 'A': if (g.dir != DOWN)  { g.nextDir = UP;    g.dirChangedThisTick = true; } break;
                    case 'B': if (g.dir != UP)    { g.nextDir = DOWN;  g.dirChangedThisTick = true; } break;
                    case 'D': if (g.dir != RIGHT) { g.nextDir = LEFT;  g.dirChangedThisTick = true; } break;
                    case 'C': if (g.dir != LEFT)  { g.nextDir = RIGHT; g.dirChangedThisTick = true; } break;
                }
            }
            continue;
        }

        if (g.dirChangedThisTick) continue;

        switch (c) {
            case 'w': case 'W': case 'k': case 'K':
                if (g.dir != DOWN)  { g.nextDir = UP;    g.dirChangedThisTick = true; } break;
            case 's': case 'S': case 'j': case 'J':
                if (g.dir != UP)    { g.nextDir = DOWN;  g.dirChangedThisTick = true; } break;
            case 'a': case 'A': case 'h': case 'H':
                if (g.dir != RIGHT) { g.nextDir = LEFT;  g.dirChangedThisTick = true; } break;
            case 'd': case 'D': case 'l': case 'L':
                if (g.dir != LEFT)  { g.nextDir = RIGHT; g.dirChangedThisTick = true; } break;
        }
    }
}

// ─── Game Update (UNCHANGED) ────────────────────────────────
void updateGame(GameState &g) {
    if (g.paused) return;

    g.dir = g.nextDir;

    Point head = g.snake.front();
    Point nh   = head;
    switch (g.dir) {
        case UP:    nh.y--; break;
        case DOWN:  nh.y++; break;
        case LEFT:  nh.x--; break;
        case RIGHT: nh.x++; break;
    }

    if (nh.x < 0 || nh.x >= g.boardWidth ||
        nh.y < 0 || nh.y >= g.boardHeight) {
        g.gameOver = true;
        g.running  = false;
        return;
    }

    bool growing = (nh == g.apple);

    int checkLimit = (int)g.snake.size() - (growing ? 0 : 1);
    for (int i = 0; i < checkLimit; i++) {
        if (g.snake[i] == nh) {
            g.gameOver = true;
            g.running  = false;
            return;
        }
    }

    g.snake.push_front(nh);

    if (growing) {
        g.score += 10;
        if (!spawnApple(g)) {
            g.gameWon = true;
            g.running = false;
        }
    } else {
        g.snake.pop_back();
    }
}

// ─── ANIMATED RENDERING ─────────────────────────────────────
void render(GameState &g) {

    if (g.score != g.prevScore) {
        g.scoreFlashTimer = FLASH_DURATION;
        g.prevScore = g.score;
    }

    bool appleFlashing    = g.appleFlashTimer > 0;
    bool scoreFlashing    = g.scoreFlashTimer > 0;
    bool appleVisible     = ((g.frameCount / APPLE_BLINK_HALF) % 2) == 0;
    bool headGlowPhase    = ((g.frameCount / HEAD_GLOW_PERIOD) % 2) == 0;
    bool appleFlashBright = (g.appleFlashTimer > FLASH_DURATION / 2);

    if (!g.paused) {
        g.frameCount++;
        if (g.appleFlashTimer > 0) g.appleFlashTimer--;
        if (g.scoreFlashTimer > 0) g.scoreFlashTimer--;
    }

    std::fill(g.grid.begin(), g.grid.end(), ' ');

    int bodyLen = (int)g.snake.size() - 1;
    for (size_t i = 1; i < g.snake.size(); i++) {
        int segIdx = (int)i - 1;
        int zone = (bodyLen <= 0) ? 0 : (segIdx * 4 / bodyLen);
        if (zone > 3) zone = 3;
        g.grid[g.snake[i].y * g.boardWidth + g.snake[i].x] = (char)('a' + zone);
    }
    g.grid[g.snake.front().y * g.boardWidth + g.snake.front().x] = 'H';
    g.grid[g.apple.y * g.boardWidth + g.apple.x] = '@';

    std::string &buf = g.renderBuf;
    buf.clear();

    buf += "\033[1;1H";

    char scoreCStr[32];
    snprintf(scoreCStr, sizeof(scoreCStr), "Score: %d", g.score);
    int scoreVisualLen = (int)strlen(scoreCStr);

    int visualBoardWidth = g.boardWidth * 2 + 4;

    // ═══ TOP BORDER ═════════════════════════════════════════
    buf += "  ";
    buf += CYAN;
    for (int i = 0; i < visualBoardWidth; i++) buf += '#';
    buf += RESET;
    buf += "  ";

    if (scoreFlashing) {
        if (g.scoreFlashTimer > FLASH_DURATION / 2) {
            buf += BOLD BRIGHT_WHITE;
        } else {
            buf += BOLD BRIGHT_GREEN;
        }
    } else {
        buf += BOLD YELLOW;
    }
    buf += scoreCStr;
    buf += RESET;

    int usedCols = 2 + visualBoardWidth + 2 + scoreVisualLen;
    for (int i = usedCols; i < g.termWidth; i++) buf += ' ';
    buf += '\n';

    // ═══ BOARD ROWS ═════════════════════════════════════════
    for (int y = 0; y < g.boardHeight; y++) {
        buf += "  ";
        buf += CYAN "##" RESET;

        int base = y * g.boardWidth;
        for (int x = 0; x < g.boardWidth; x++) {
            char c = g.grid[base + x];
            switch (c) {
                case 'H':
                    if (headGlowPhase) buf += BOLD BRIGHT_GREEN "OO" RESET;
                    else               buf += BOLD BRIGHT_CYAN  "OO" RESET;
                    break;
                case 'a': buf += BOLD BRIGHT_GREEN "oo" RESET; break;
                case 'b': buf += BRIGHT_GREEN      "oo" RESET; break;
                case 'c': buf += GREEN             "oo" RESET; break;
                case 'd': buf += DIM GREEN         "oo" RESET; break;
                case '@':
                    if (appleFlashing) {
                        if (appleFlashBright) buf += BOLD BRIGHT_WHITE "@@" RESET;
                        else                  buf += BOLD YELLOW       "@@" RESET;
                    } else if (appleVisible) {
                        buf += BOLD RED "@@" RESET;
                    } else {
                        buf += DIM RED  "@@" RESET;
                    }
                    break;
                default:
                    buf += "  ";
                    break;
            }
        }

        buf += CYAN "##" RESET;

        int rowVisualLen = 2 + visualBoardWidth;
        for (int i = rowVisualLen; i < g.termWidth; i++) buf += ' ';
        buf += '\n';
    }

    // ═══ BOTTOM BORDER ══════════════════════════════════════
    buf += "  ";
    buf += CYAN;
    for (int i = 0; i < visualBoardWidth; i++) buf += '#';
    buf += RESET;
    for (int i = 2 + visualBoardWidth; i < g.termWidth; i++) buf += ' ';
    buf += '\n';

    // ═══ INSTRUCTIONS ═══════════════════════════════════════
    const char* instrText =
        "  Move: WASD/HJKL/Arrows | P: Pause | R: Restart | Q: Quit";
    int instrVisualLen = (int)strlen(instrText);
    buf += CYAN;
    buf += instrText;
    buf += RESET;
    for (int i = instrVisualLen; i < g.termWidth; i++) buf += ' ';
    buf += '\n';

    // ═══ PAUSE OVERLAY ══════════════════════════════════════
    if (g.paused) {
        const char* pauseMsg = "  PAUSED -- Press P to resume  ";
        int msgLen = (int)strlen(pauseMsg);
        int centerRow = 2 + g.boardHeight / 2;
        int contentVisualWidth = g.boardWidth * 2;
        int centerCol = 2 + 2 + (contentVisualWidth - msgLen) / 2;
        if (centerCol < 1) centerCol = 1;
        char posCmd[32];
        snprintf(posCmd, sizeof(posCmd), "\033[%d;%dH", centerRow, centerCol);
        buf += posCmd;
        buf += BOLD YELLOW REVERSE;
        buf += pauseMsg;
        buf += RESET;
    }

    write(STDOUT_FILENO, buf.c_str(), buf.size());
}

// ─── Centering Helpers ──────────────────────────────────────
static std::string centerText(const std::string &s, int termW) {
    int p = (termW - (int)s.size()) / 2;
    if (p < 0) p = 0;
    return std::string(p, ' ') + s;
}

static std::string centerColorText(const std::string &s, int visualLen, int termW) {
    int p = (termW - visualLen) / 2;
    if (p < 0) p = 0;
    return std::string(p, ' ') + s;
}

// ─── End Screen ─────────────────────────────────────────────
void showEndScreen(int score, bool won) {
    clearScreen();
    saveScore(score);
    std::vector<ScoreEntry> scores = loadScores();

    int tw, th;
    getTerminalSize(tw, th);

    std::string titleText = won ? "Y O U   W I N !" : "G A M E   O V E R";
    std::string titleColored = won
        ? (std::string(BOLD) + BRIGHT_GREEN + titleText + RESET)
        : (std::string(BOLD) + RED          + titleText + RESET);
    int titleVisualLen = (int)titleText.size();

    std::string border = std::string(CYAN) + "=============================" + RESET;
    int borderVisualLen = 29;

    std::string scoreLine =
        std::string(BOLD) + YELLOW + "Final Score: " + RESET
        + BRIGHT_WHITE + std::to_string(score) + RESET;
    std::string scoreVisual = "Final Score: " + std::to_string(score);
    int scoreVisualLen = (int)scoreVisual.size();

    std::string buf;
    buf += "\n\n";
    buf += centerColorText(border, borderVisualLen, tw) + "\n";
    buf += centerColorText(titleColored, titleVisualLen, tw) + "\n";
    buf += centerColorText(border, borderVisualLen, tw) + "\n\n";
    buf += centerColorText(scoreLine, scoreVisualLen, tw) + "\n\n";

    std::string topLabel = std::string(BOLD) + CYAN + "Top Scores:" + RESET;
    buf += centerColorText(topLabel, 11, tw) + "\n";

    std::string divider = std::string(CYAN) + "-----------------------------" + RESET;
    buf += centerColorText(divider, 29, tw) + "\n";

    int n = std::min((int)scores.size(), 10);
    for (int i = 0; i < n; i++) {
        std::string rank = std::to_string(i + 1);
        if (i < 9) rank = " " + rank;
        std::string plainLine = rank + ". " + scores[i].timestamp
                              + "  |  " + std::to_string(scores[i].score);
        std::string colorLine =
            std::string(CYAN) + rank + "." + RESET + " "
            + scores[i].timestamp + "  "
            + CYAN + "|" + RESET + "  "
            + YELLOW + std::to_string(scores[i].score) + RESET;
        buf += centerColorText(colorLine, (int)plainLine.size(), tw) + "\n";
    }
    if (scores.empty())
        buf += centerText("(no scores yet)", tw) + "\n";

    buf += centerColorText(divider, 29, tw) + "\n\n";

    std::string restartLine = std::string(BOLD) + GREEN + "Press [R] to Restart" + RESET;
    buf += centerColorText(restartLine, 20, tw) + "\n";
    std::string quitLine = std::string(BOLD) + RED + "Press [Q] to Quit" + RESET;
    buf += centerColorText(quitLine, 17, tw) + "\n";

    write(STDOUT_FILENO, buf.c_str(), buf.size());
}

// ─── Resized Screen ─────────────────────────────────────────
void showResizedScreen() {
    clearScreen();
    int tw, th;
    getTerminalSize(tw, th);

    std::string b = std::string(YELLOW) + "==============================" + RESET;
    std::string m = std::string(BOLD) + YELLOW + " Terminal resized during game  " + RESET;
    std::string r = std::string(GREEN) + "Press [R] to Restart" + RESET;
    std::string q = std::string(RED)   + "Press [Q] to Quit"   + RESET;

    std::string buf;
    buf += "\n\n";
    buf += centerColorText(b, 30, tw) + "\n";
    buf += centerColorText(m, 30, tw) + "\n";
    buf += centerColorText(b, 30, tw) + "\n\n";
    buf += centerColorText(r, 20, tw) + "\n";
    buf += centerColorText(q, 17, tw) + "\n";

    write(STDOUT_FILENO, buf.c_str(), buf.size());
}

// ─── Terminal Too Small Screen ──────────────────────────────
void showTooSmallScreen() {
    clearScreen();
    std::string buf;
    buf += "\n";
    buf += std::string(BOLD) + RED + "  Terminal too small!\n" + RESET;
    buf += std::string(YELLOW) + "  Minimum size: 30 x 16\n\n" + RESET;
    buf += "  Please resize your terminal,\n";
    buf += std::string("  then press ") + GREEN + "[R]" + RESET
         + " to retry or " + RED + "[Q]" + RESET + " to quit.\n";
    write(STDOUT_FILENO, buf.c_str(), buf.size());
}

// ─── Post-Game Input ────────────────────────────────────────
bool waitForRestart() {
    {
        char discard;
        fd_set fds; struct timeval tv;
        while (true) {
            FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
            tv = {0, 0};
            if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) break;
            read(STDIN_FILENO, &discard, 1);
        }
    }
    while (true) {
        if (g_interrupted) return false;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 50000};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'r' || c == 'R') return true;
                if (c == 'q' || c == 'Q') return false;
            }
        }
    }
}

// ─── Main ───────────────────────────────────────────────────
int main() {
    srand(static_cast<unsigned>(time(nullptr)));

    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    enableRawMode();
    hideCursor();
    atexit(atexitCleanup);

    bool playing = true;

    while (playing) {
        if (g_interrupted) break;

        GameState game;
        initGame(game);

        if (game.termTooSmall) {
            showTooSmallScreen();
            playing = waitForRestart();
            continue;
        }

        clearScreen();

        while (game.running) {
            long long frameStart = nowMicros();

            if (g_interrupted) {
                game.running = false;
                playing = false;
                break;
            }

            if (checkTerminalResize(game)) break;

            game.dirChangedThisTick = false;

            readInput(game);
            if (!game.running) break;

            updateGame(game);
            if (!game.running) break;

            render(game);

            long long elapsed   = nowMicros() - frameStart;
            long long sleepTime = TICK_US - elapsed;
            if (sleepTime > 0) {
                usleep(static_cast<useconds_t>(sleepTime));
            }
        }

        if (g_interrupted) break;

        if (game.restartRequested) {
            continue;
        }

        if (game.termResized) {
            showResizedScreen();
            playing = waitForRestart();
        } else if (game.gameOver || game.gameWon) {
            showEndScreen(game.score, game.gameWon);
            playing = waitForRestart();
        } else {
            playing = false;
        }
    }

    return 0;
}
