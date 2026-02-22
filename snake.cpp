// ============================================================
//  VSNAKE — Retro Terminal Snake Game (C++, No ncurses)
//  Iteration 18: PC speaker ioctl sound + BEL fallback
//  Compile: g++ -std=c++11 snake.cpp -o snake
// ============================================================

#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>

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

// ─── ANSI Constants ─────────────────────────────────────────
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
#define ERASE_LINE   "\033[K"
#define ERASE_BELOW  "\033[J"

// ─── Board ──────────────────────────────────────────────────
static const int BOARD_WIDTH  = 40;
static const int BOARD_HEIGHT = 20;
static const int MIN_TERM_W   = BOARD_WIDTH * 2 + 10;
static const int MIN_TERM_H   = BOARD_HEIGHT + 6;

// ─── Game Constants ─────────────────────────────────────────
static const char* APP_DIR_NAME   = "vsnake";
static const char* SCORE_FILENAME = "snake_scores.txt";
static const int APPLE_MAX_TRIES  = 1000;

// ─── Timing ─────────────────────────────────────────────────
static const int   RENDER_TICK_US    = 30000;
static const int   BASE_MOVE_US      = 120000;
static const int   MIN_MOVE_US       = 60000;
static const int   SPEED_SCORE_STEP  = 50;
static const int   SPEED_REDUCE_US   = 5000;
static const float VERT_SPEED_FACTOR = 1.2f;

// ─── Animation ──────────────────────────────────────────────
static const int APPLE_BLINK_HALF   = 16;
static const int HEAD_GLOW_PERIOD   = 10;
static const int APPLE_SPARKLE_RATE = 12;
static const int FLASH_DURATION     = 24;

// ─── Signal ─────────────────────────────────────────────────
static volatile sig_atomic_t g_interrupted = 0;
void signalHandler(int) { g_interrupted = 1; }

// ─── Direction ──────────────────────────────────────────────
enum Direction { UP, DOWN, LEFT, RIGHT };

static bool isOpposite(Direction a, Direction b) {
    return (a == UP && b == DOWN) || (a == DOWN && b == UP) ||
           (a == LEFT && b == RIGHT) || (a == RIGHT && b == LEFT);
}
static bool isVertical(Direction d) { return d == UP || d == DOWN; }

struct Point {
    int x, y;
    bool operator==(const Point& o) const { return x == o.x && y == o.y; }
};

struct ScoreEntry {
    std::string timestamp;
    int score;
};

// ─── App State Machine ─────────────────────────────────────
enum AppState {
    STATE_MENU, STATE_PLAYING, STATE_GAMEOVER,
    STATE_RESIZED, STATE_TOO_SMALL, STATE_LEADERBOARD, STATE_EXIT
};

// ─── Game State ─────────────────────────────────────────────
struct GameState {
    std::deque<Point> snake;
    Point             apple;
    Direction         dir, nextDir;
    int               score;
    int               boardWidth, boardHeight;
    int               termWidth, termHeight;
    int               offsetX, offsetY;
    bool              running, gameOver, gameWon;
    bool              termResized, termTooSmall;
    bool              paused, restartRequested;
    bool              dirChangedThisTick, hasQueuedDir;
    Direction         queuedDir;
    long long         moveAccumulator;
    unsigned long     frameCount;
    int               appleFlashTimer, scoreFlashTimer, prevScore;
    std::vector<char> grid;
    std::string       renderBuf;

    void allocateBuffers() {
        grid.resize(boardWidth * boardHeight);
        renderBuf.reserve((boardWidth * 2 + 80) * (boardHeight + 8));
    }
};

// ─── Terminal ───────────────────────────────────────────────
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
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    rawModeEnabled = true;
}

void clearScreen()  { write(STDOUT_FILENO, "\033[2J\033[1;1H", 11); }
void hideCursor()   { write(STDOUT_FILENO, "\033[?25l", 6); }
void showCursor()   { write(STDOUT_FILENO, "\033[?25h", 6); }

void getTerminalSize(int &w, int &h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||
        ws.ws_col == 0 || ws.ws_row == 0) { w = 80; h = 24; }
    else { w = ws.ws_col; h = ws.ws_row; }
}

long long nowMicros() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

void performCleanup() {
    write(STDOUT_FILENO, "\033[?1049l", 8);
    write(STDOUT_FILENO, "\033[0m", 4);
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
    showCursor();
    disableRawMode();
}
void atexitCleanup() { performCleanup(); }

// ===== SOUND DISABLED ======================================

inline void soundEat() {}
inline void soundGameOver() {}     //Zack here, fucked up the sound.
inline void soundMenuMove() {}     //So now removing it with laziness     
inline void soundMenuSelect() {}   // added dummy functions so code wont break
inline void soundPauseToggle() {}  //sorry :(

// ─── Timestamp ──────────────────────────────────────────────
std::string getCurrentTimestamp() {
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
    return std::string(buf);
}

// ─── XDG Score Path ─────────────────────────────────────────
static bool ensureDirectoryExists(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path.c_str(), 0755) == 0;
}

static bool mkdirRecursive(const std::string &path) {
    if (path.empty()) return false;
    std::string built;
    for (size_t i = 0; i < path.size(); i++) {
        built += path[i];
        if (path[i] == '/' && built.size() > 1) {
            struct stat st;
            if (stat(built.c_str(), &st) != 0)
                if (mkdir(built.c_str(), 0755) != 0 && errno != EEXIST)
                    return false;
        }
    }
    return ensureDirectoryExists(path);
}

static std::string getScoreFilePath() {
    std::string dataDir;
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] != '\0')
        dataDir = std::string(xdg) + "/" + APP_DIR_NAME;
    if (dataDir.empty()) {
        const char* home = getenv("HOME");
        if (home && home[0] != '\0')
            dataDir = std::string(home) + "/.local/share/" + APP_DIR_NAME;
    }
    if (!dataDir.empty() && mkdirRecursive(dataDir))
        return dataDir + "/" + SCORE_FILENAME;
    return SCORE_FILENAME;
}

// ─── Leaderboard I/O ───────────────────────────────────────
void saveScore(int score) {
    std::string path = getScoreFilePath();
    std::ofstream file(path.c_str(), std::ios::app);
    if (file.is_open())
        file << getCurrentTimestamp() << " | " << score << "\n";
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

// ─── Movement ───────────────────────────────────────────────
long long calcBaseInterval(int score) {
    int steps = score / SPEED_SCORE_STEP;
    long long iv = BASE_MOVE_US - (long long)steps * SPEED_REDUCE_US;
    return iv < MIN_MOVE_US ? MIN_MOVE_US : iv;
}

long long calcMoveInterval(int score, Direction d) {
    long long iv = calcBaseInterval(score);
    if (isVertical(d)) iv = (long long)(iv * VERT_SPEED_FACTOR);
    return iv;
}

// ─── Apple Spawning ─────────────────────────────────────────
bool spawnApple(GameState &g) {
    int total = g.boardWidth * g.boardHeight;
    if ((int)g.snake.size() >= total) return false;

    if ((int)g.snake.size() > total * 3 / 4) {
        std::vector<char> occ(total, 0);
        for (auto &s : g.snake) occ[s.y * g.boardWidth + s.x] = 1;
        std::vector<Point> free;
        for (int y = 0; y < g.boardHeight; y++)
            for (int x = 0; x < g.boardWidth; x++)
                if (!occ[y * g.boardWidth + x]) free.push_back({x, y});
        if (free.empty()) return false;
        g.apple = free[rand() % (int)free.size()];
        g.appleFlashTimer = FLASH_DURATION;
        return true;
    }

    for (int a = 0; a < APPLE_MAX_TRIES; a++) {
        Point p = {rand() % g.boardWidth, rand() % g.boardHeight};
        bool on = false;
        for (auto &s : g.snake) if (s == p) { on = true; break; }
        if (!on) { g.apple = p; g.appleFlashTimer = FLASH_DURATION; return true; }
    }

    for (int y = 0; y < g.boardHeight; y++)
        for (int x = 0; x < g.boardWidth; x++) {
            Point p = {x, y}; bool on = false;
            for (auto &s : g.snake) if (s == p) { on = true; break; }
            if (!on) { g.apple = p; g.appleFlashTimer = FLASH_DURATION; return true; }
        }
    return false;
}

// ─── Centering ──────────────────────────────────────────────
static void calcCenteringOffsets(GameState &g) {
    int vw = BOARD_WIDTH * 2 + 4;
    int vh = BOARD_HEIGHT + 5;
    g.offsetX = std::max(0, (g.termWidth - vw) / 2);
    g.offsetY = std::max(0, (g.termHeight - vh) / 2);
}

// ─── Init ───────────────────────────────────────────────────
void initGame(GameState &g) {
    getTerminalSize(g.termWidth, g.termHeight);
    g.termTooSmall = (g.termWidth < MIN_TERM_W || g.termHeight < MIN_TERM_H);
    g.boardWidth = BOARD_WIDTH;
    g.boardHeight = BOARD_HEIGHT;
    calcCenteringOffsets(g);

    g.snake.clear();
    int cx = g.boardWidth / 2, cy = g.boardHeight / 2;
    g.snake.push_back({cx, cy});
    g.snake.push_back({cx - 1, cy});
    g.snake.push_back({cx - 2, cy});

    g.dir = RIGHT; g.nextDir = RIGHT;
    g.score = 0; g.running = true;
    g.gameOver = false; g.gameWon = false;
    g.termResized = false; g.paused = false;
    g.restartRequested = false;
    g.dirChangedThisTick = false;
    g.hasQueuedDir = false; g.queuedDir = RIGHT;
    g.moveAccumulator = 0; g.frameCount = 0;
    g.appleFlashTimer = 0; g.scoreFlashTimer = 0; g.prevScore = 0;

    g.allocateBuffers();
    spawnApple(g);
}

// ─── Resize Check ───────────────────────────────────────────
bool checkTerminalResize(GameState &g) {
    int nw, nh; getTerminalSize(nw, nh);
    if (nw != g.termWidth || nh != g.termHeight) {
        g.termResized = true; g.running = false; return true;
    }
    return false;
}

// ─── Direction Change ───────────────────────────────────────
static void tryChangeDirection(GameState &g, Direction d) {
    if (!g.dirChangedThisTick) {
        if (!isOpposite(d, g.dir)) {
            g.nextDir = d; g.dirChangedThisTick = true; g.hasQueuedDir = false;
        }
    } else {
        if (!isOpposite(d, g.nextDir) && d != g.nextDir) {
            g.queuedDir = d; g.hasQueuedDir = true;
        }
    }
}

// ─── Input ──────────────────────────────────────────────────
void readInput(GameState &g) {
    char c = 0;
    while (true) {
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 0};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) break;
        if (read(STDIN_FILENO, &c, 1) != 1) break;

        if (c == 'q' || c == 'Q') { g.running = false; return; }
        if (c == 'r' || c == 'R') { g.restartRequested = true; g.running = false; return; }
        if (c == 'p' || c == 'P') { g.paused = !g.paused; soundPauseToggle(); continue; }
        if (g.paused) continue;

        if (c == '\033') {
            char seq[2] = {0, 0};
            fd_set f2; struct timeval t2;
            FD_ZERO(&f2); FD_SET(STDIN_FILENO, &f2); t2 = {0, 5000};
            if (select(STDIN_FILENO + 1, &f2, nullptr, nullptr, &t2) > 0)
                read(STDIN_FILENO, &seq[0], 1);
            FD_ZERO(&f2); FD_SET(STDIN_FILENO, &f2); t2 = {0, 5000};
            if (select(STDIN_FILENO + 1, &f2, nullptr, nullptr, &t2) > 0)
                read(STDIN_FILENO, &seq[1], 1);
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A': tryChangeDirection(g, UP);    break;
                    case 'B': tryChangeDirection(g, DOWN);  break;
                    case 'D': tryChangeDirection(g, LEFT);  break;
                    case 'C': tryChangeDirection(g, RIGHT); break;
                }
            }
            continue;
        }
        switch (c) {
            case 'w': case 'W': case 'k': case 'K': tryChangeDirection(g, UP);    break;
            case 's': case 'S': case 'j': case 'J': tryChangeDirection(g, DOWN);  break;
            case 'a': case 'A': case 'h': case 'H': tryChangeDirection(g, LEFT);  break;
            case 'd': case 'D': case 'l': case 'L': tryChangeDirection(g, RIGHT); break;
        }
    }
}

// ─── Game Update ────────────────────────────────────────────
void updateGame(GameState &g) {
    if (g.paused) return;
    g.dir = g.nextDir;
    Point head = g.snake.front(), nh = head;
    switch (g.dir) {
        case UP: nh.y--; break; case DOWN: nh.y++; break;
        case LEFT: nh.x--; break; case RIGHT: nh.x++; break;
    }

    if (nh.x < 0 || nh.x >= g.boardWidth || nh.y < 0 || nh.y >= g.boardHeight) {
        g.gameOver = true; g.running = false; soundGameOver(); return;
    }

    bool growing = (nh == g.apple);
    int limit = (int)g.snake.size() - (growing ? 0 : 1);
    for (int i = 0; i < limit; i++) {
        if (g.snake[i] == nh) {
            g.gameOver = true; g.running = false; soundGameOver(); return;
        }
    }

    g.snake.push_front(nh);
    if (growing) {
        g.score += 10;
        soundEat();
        if (!spawnApple(g)) { g.gameWon = true; g.running = false; }
    } else {
        g.snake.pop_back();
    }
}

// ─── Rendering ──────────────────────────────────────────────
void render(GameState &g) {
    if (g.score != g.prevScore) {
        g.scoreFlashTimer = FLASH_DURATION;
        g.prevScore = g.score;
    }

    bool appleFlashing    = g.appleFlashTimer > 0;
    bool appleVisible     = ((g.frameCount / APPLE_BLINK_HALF) % 2) == 0;
    bool appleFlashBright = (g.appleFlashTimer > FLASH_DURATION / 2);
    int headPhase         = (g.frameCount / HEAD_GLOW_PERIOD) % 3;
    int sparklePhase      = (g.frameCount / APPLE_SPARKLE_RATE) % 3;

    if (!g.paused) {
        g.frameCount++;
        if (g.appleFlashTimer > 0) g.appleFlashTimer--;
        if (g.scoreFlashTimer > 0) g.scoreFlashTimer--;
    }

    std::fill(g.grid.begin(), g.grid.end(), ' ');
    int bodyLen = (int)g.snake.size() - 1;
    for (size_t i = 1; i < g.snake.size(); i++) {
        int seg = (int)i - 1;
        int zone = (bodyLen <= 0) ? 0 : (seg * 4 / bodyLen);
        if (zone > 3) zone = 3;
        g.grid[g.snake[i].y * g.boardWidth + g.snake[i].x] = (char)('a' + zone);
    }
    g.grid[g.snake.front().y * g.boardWidth + g.snake.front().x] = 'H';
    g.grid[g.apple.y * g.boardWidth + g.apple.x] = '@';

    std::string &buf = g.renderBuf;
    buf.clear();
    buf += "\033[1;1H";

    int vbw = g.boardWidth * 2 + 4;
    std::string hpad(g.offsetX, ' ');

    char scoreCStr[32];
    snprintf(scoreCStr, sizeof(scoreCStr), "Score: %d", g.score);
    int scoreVisLen = (int)strlen(scoreCStr);

    for (int r = 0; r < g.offsetY; r++) buf += ERASE_LINE "\n";

    {
        int pad = std::max(0, (g.termWidth - scoreVisLen) / 2);
        for (int i = 0; i < pad; i++) buf += ' ';
        if (g.scoreFlashTimer > 0) {
            float ratio = (float)g.scoreFlashTimer / FLASH_DURATION;
            if (ratio > 0.75f)      buf += BOLD BRIGHT_WHITE;
            else if (ratio > 0.5f)  buf += BOLD BRIGHT_GREEN;
            else if (ratio > 0.25f) buf += BOLD GREEN;
            else                    buf += YELLOW;
        } else {
            buf += BOLD YELLOW;
        }
        buf += scoreCStr;
        buf += RESET;
    }
    buf += ERASE_LINE "\n";

    buf += hpad; buf += CYAN;
    for (int i = 0; i < vbw; i++) buf += '#';
    buf += RESET ERASE_LINE "\n";

    for (int y = 0; y < g.boardHeight; y++) {
        buf += hpad;
        buf += CYAN "##" RESET;
        int base = y * g.boardWidth;
        for (int x = 0; x < g.boardWidth; x++) {
            char c = g.grid[base + x];
            switch (c) {
                case 'H':
                    switch (headPhase) {
                        case 0: buf += BOLD BRIGHT_GREEN "OO" RESET; break;
                        case 1: buf += BOLD BRIGHT_CYAN  "OO" RESET; break;
                        case 2: buf += BOLD BRIGHT_WHITE "OO" RESET; break;
                    }
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
                        switch (sparklePhase) {
                            case 0: buf += BOLD RED          "@@" RESET; break;
                            case 1: buf += BOLD YELLOW       "**" RESET; break;
                            case 2: buf += BOLD BRIGHT_WHITE "##" RESET; break;
                        }
                    } else {
                        buf += DIM RED "@@" RESET;
                    }
                    break;
                default: buf += "  "; break;
            }
        }
        buf += CYAN "##" RESET ERASE_LINE "\n";
    }

    buf += hpad; buf += CYAN;
    for (int i = 0; i < vbw; i++) buf += '#';
    buf += RESET ERASE_LINE "\n";

    {
        const char* t = "Move: WASD/HJKL/Arrows | P: Pause | R: Restart | Q: Menu";
        int pad = std::max(0, (g.termWidth - (int)strlen(t)) / 2);
        for (int i = 0; i < pad; i++) buf += ' ';
        buf += CYAN; buf += t; buf += RESET;
    }
    buf += ERASE_LINE "\n";
    buf += ERASE_BELOW;

    if (g.paused) {
        const char* pm = "  PAUSED -- Press P to resume  ";
        int ml = (int)strlen(pm);
        int cr = g.offsetY + 2 + g.boardHeight / 2;
        int cc = g.offsetX + 3 + std::max(0, (g.boardWidth * 2 - ml) / 2);
        if (cc < 1) cc = 1;
        char pos[32];
        snprintf(pos, sizeof(pos), "\033[%d;%dH", cr, cc);
        buf += pos;
        buf += BOLD YELLOW REVERSE;
        buf += pm;
        buf += RESET;
    }

    write(STDOUT_FILENO, buf.c_str(), buf.size());
}

// ─── Centering Helpers ──────────────────────────────────────
static std::string centerText(const std::string &s, int tw) {
    int p = std::max(0, (tw - (int)s.size()) / 2);
    return std::string(p, ' ') + s;
}
static std::string centerColorText(const std::string &s, int vl, int tw) {
    int p = std::max(0, (tw - vl) / 2);
    return std::string(p, ' ') + s;
}

static void flushInput() {
    char d; fd_set fds; struct timeval tv;
    while (true) {
        FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds); tv = {0, 0};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) break;
        read(STDIN_FILENO, &d, 1);
    }
}

// ─── Start Menu ─────────────────────────────────────────────
AppState showStartMenu() {
    flushInput();
    clearScreen();

    int sel = 0;
    const int NOPTS = 3;
    std::string buf;
    buf.reserve(4096);
    unsigned long frame = 0;

    while (true) {
        if (g_interrupted) return STATE_EXIT;
        long long fs = nowMicros();

        int tw, th; getTerminalSize(tw, th);
        if (tw < MIN_TERM_W || th < MIN_TERM_H) return STATE_TOO_SMALL;

        {
            char c = 0;
            while (true) {
                fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
                struct timeval tv = {0, 0};
                if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) break;
                if (read(STDIN_FILENO, &c, 1) != 1) break;

                if (c == 'q' || c == 'Q') return STATE_EXIT;
                if (c == '1') { soundMenuSelect(); return STATE_PLAYING; }
                if (c == '2') { soundMenuSelect(); return STATE_LEADERBOARD; }

                if (c == '\r' || c == '\n' || c == ' ') {
                    soundMenuSelect();
                    switch (sel) {
                        case 0: return STATE_PLAYING;
                        case 1: return STATE_LEADERBOARD;
                        case 2: return STATE_EXIT;
                    }
                }

                if (c == '\033') {
                    char seq[2] = {0, 0};
                    fd_set f2; struct timeval t2;
                    FD_ZERO(&f2); FD_SET(STDIN_FILENO, &f2); t2 = {0, 5000};
                    if (select(STDIN_FILENO + 1, &f2, nullptr, nullptr, &t2) > 0)
                        read(STDIN_FILENO, &seq[0], 1);
                    FD_ZERO(&f2); FD_SET(STDIN_FILENO, &f2); t2 = {0, 5000};
                    if (select(STDIN_FILENO + 1, &f2, nullptr, nullptr, &t2) > 0)
                        read(STDIN_FILENO, &seq[1], 1);
                    if (seq[0] == '[') {
                        int prev = sel;
                        if (seq[1] == 'A') sel = (sel - 1 + NOPTS) % NOPTS;
                        else if (seq[1] == 'B') sel = (sel + 1) % NOPTS;
                        if (sel != prev) soundMenuMove();
                    }
                    continue;
                }

                {
                    int prev = sel;
                    switch (c) {
                        case 'w': case 'W': case 'k': case 'K':
                            sel = (sel - 1 + NOPTS) % NOPTS; break;
                        case 's': case 'S': case 'j': case 'J':
                            sel = (sel + 1) % NOPTS; break;
                    }
                    if (sel != prev) soundMenuMove();
                }
            }
        }

        frame++;
        int breathPhase = (frame / 20) % 3;
        const char* breathAttr;
        switch (breathPhase) {
            case 0: breathAttr = DIM;  break;
            case 1: breathAttr = "";   break;
            default: breathAttr = BOLD; break;
        }

        buf.clear();
        buf += "\033[1;1H";

        int menuH = 13;
        int topPad = std::max(1, (th - menuH) / 2);
        for (int i = 0; i < topPad; i++) buf += ERASE_LINE "\n";

        std::string bline = "========================================";
        int blVis = (int)bline.size();
        std::string blCol = std::string(CYAN) + bline + RESET;
        buf += centerColorText(blCol, blVis, tw) + ERASE_LINE "\n";

        std::string titleText = "V   S   N   A   K   E";
        int titleVis = (int)titleText.size();
        std::string titleCol = std::string(breathAttr) + BRIGHT_GREEN + titleText + RESET;
        buf += centerColorText(titleCol, titleVis, tw) + ERASE_LINE "\n";
        buf += centerColorText(blCol, blVis, tw) + ERASE_LINE "\n";
        buf += ERASE_LINE "\n";

        int decoPhase = (frame / 8) % 3;
        std::string snakeHead;
        switch (decoPhase) {
            case 0: snakeHead = std::string(BOLD) + BRIGHT_GREEN + "O>" + RESET; break;
            case 1: snakeHead = std::string(BOLD) + BRIGHT_CYAN  + "O>" + RESET; break;
            case 2: snakeHead = std::string(BOLD) + BRIGHT_WHITE + "O>" + RESET; break;
        }
        std::string deco = std::string(DIM) + GREEN + "~" + RESET
                         + BRIGHT_GREEN + "o" + RESET
                         + GREEN + "o" + RESET
                         + BRIGHT_GREEN + "o" + RESET
                         + GREEN + "o" + RESET
                         + snakeHead;
        buf += centerColorText(deco, 9, tw) + ERASE_LINE "\n";
        buf += ERASE_LINE "\n";

        const char* labels[] = {"Start Game", "Leaderboard", "Quit"};
        const char* keys[]   = {"1", "2", "Q"};

        for (int i = 0; i < NOPTS; i++) {
            char plain[48];
            snprintf(plain, sizeof(plain), " %c  [%s]  %-14s",
                     (i == sel) ? '>' : ' ', keys[i], labels[i]);
            int plen = (int)strlen(plain);

            if (i == sel) {
                std::string col = std::string(BOLD) + YELLOW + REVERSE + plain + RESET;
                buf += centerColorText(col, plen, tw);
            } else {
                std::string col = std::string(CYAN) + "[" + keys[i] + "]" + RESET
                                + "  " + labels[i];
                int vlen = 1 + (int)strlen(keys[i]) + 1 + 2 + (int)strlen(labels[i]);
                buf += centerColorText(col, vlen, tw);
            }
            buf += ERASE_LINE "\n";
        }

        buf += ERASE_LINE "\n";
        std::string footer = "Navigate: Arrows/WS  Select: Enter/Space";
        buf += centerColorText(std::string(DIM) + footer + RESET,
                               (int)footer.size(), tw);
        buf += ERASE_LINE "\n";
        buf += ERASE_BELOW;

        write(STDOUT_FILENO, buf.c_str(), buf.size());

        long long el = nowMicros() - fs;
        long long sl = RENDER_TICK_US - el;
        if (sl > 0) usleep(static_cast<useconds_t>(sl));
    }
}

// ─── Leaderboard Screen ────────────────────────────────────
AppState showLeaderboardScreen() {
    clearScreen();
    auto scores = loadScores();
    int tw, th; getTerminalSize(tw, th);

    std::string border = std::string(CYAN) + "=====================================" + RESET;
    std::string title  = std::string(BOLD) + YELLOW + "L E A D E R B O A R D" + RESET;
    std::string div    = std::string(CYAN) + "-------------------------------------" + RESET;

    std::string buf;
    buf += "\n\n";
    buf += centerColorText(border, 37, tw) + "\n";
    buf += centerColorText(title, 21, tw) + "\n";
    buf += centerColorText(border, 37, tw) + "\n\n";

    int n = std::min((int)scores.size(), 10);
    if (n == 0) {
        buf += centerText("(no saved scores)", tw) + "\n";
    } else {
        for (int i = 0; i < n; i++) {
            std::string rank = std::to_string(i + 1);
            if (i < 9) rank = " " + rank;
            std::string plain = rank + ". " + scores[i].timestamp
                              + "  |  " + std::to_string(scores[i].score);
            std::string col = std::string(CYAN) + rank + "." + RESET + " "
                            + scores[i].timestamp + "  "
                            + CYAN + "|" + RESET + "  "
                            + YELLOW + std::to_string(scores[i].score) + RESET;
            buf += centerColorText(col, (int)plain.size(), tw) + "\n";
        }
    }

    buf += "\n";
    buf += centerColorText(div, 37, tw) + "\n\n";
    buf += centerColorText(std::string(BOLD) + GREEN + "Press [R] to Return to Menu" + RESET, 27, tw) + "\n";
    buf += centerColorText(std::string(BOLD) + RED + "Press [Q] to Quit" + RESET, 17, tw) + "\n";
    write(STDOUT_FILENO, buf.c_str(), buf.size());

    flushInput();
    while (true) {
        if (g_interrupted) return STATE_EXIT;
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 50000};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'r' || c == 'R') return STATE_MENU;
                if (c == 'q' || c == 'Q') return STATE_EXIT;
            }
        }
    }
}

// ─── Post-Game Screens ──────────────────────────────────────
static AppState waitForMenuOrExit() {
    flushInput();
    while (true) {
        if (g_interrupted) return STATE_EXIT;
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 50000};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'r' || c == 'R') return STATE_MENU;
                if (c == 'q' || c == 'Q') return STATE_EXIT;
            }
        }
    }
}

void showEndScreen(int score, bool won) {
    clearScreen();
    saveScore(score);
    auto scores = loadScores();
    int tw, th; getTerminalSize(tw, th);

    std::string titleText = won ? "Y O U   W I N !" : "G A M E   O V E R";
    std::string titleCol = won
        ? (std::string(BOLD) + BRIGHT_GREEN + titleText + RESET)
        : (std::string(BOLD) + RED + titleText + RESET);
    std::string border = std::string(CYAN) + "=============================" + RESET;
    std::string scoreLine = std::string(BOLD) + YELLOW + "Final Score: " + RESET
                          + BRIGHT_WHITE + std::to_string(score) + RESET;
    std::string scoreVis = "Final Score: " + std::to_string(score);
    std::string div = std::string(CYAN) + "-----------------------------" + RESET;

    std::string buf;
    buf += "\n\n";
    buf += centerColorText(border, 29, tw) + "\n";
    buf += centerColorText(titleCol, (int)titleText.size(), tw) + "\n";
    buf += centerColorText(border, 29, tw) + "\n\n";
    buf += centerColorText(scoreLine, (int)scoreVis.size(), tw) + "\n\n";
    buf += centerColorText(std::string(BOLD) + CYAN + "Top Scores:" + RESET, 11, tw) + "\n";
    buf += centerColorText(div, 29, tw) + "\n";

    int n = std::min((int)scores.size(), 10);
    for (int i = 0; i < n; i++) {
        std::string rank = std::to_string(i + 1);
        if (i < 9) rank = " " + rank;
        std::string plain = rank + ". " + scores[i].timestamp
                          + "  |  " + std::to_string(scores[i].score);
        std::string col = std::string(CYAN) + rank + "." + RESET + " "
                        + scores[i].timestamp + "  "
                        + CYAN + "|" + RESET + "  "
                        + YELLOW + std::to_string(scores[i].score) + RESET;
        buf += centerColorText(col, (int)plain.size(), tw) + "\n";
    }
    if (scores.empty()) buf += centerText("(no scores yet)", tw) + "\n";

    buf += centerColorText(div, 29, tw) + "\n\n";
    buf += centerColorText(std::string(BOLD) + GREEN + "Press [R] to Return to Menu" + RESET, 27, tw) + "\n";
    buf += centerColorText(std::string(BOLD) + RED + "Press [Q] to Quit" + RESET, 17, tw) + "\n";
    write(STDOUT_FILENO, buf.c_str(), buf.size());
}

void showResizedScreen() {
    clearScreen();
    int tw, th; getTerminalSize(tw, th);
    std::string b = std::string(YELLOW) + "==============================" + RESET;
    std::string m = std::string(BOLD) + YELLOW + " Terminal resized during game  " + RESET;
    std::string buf;
    buf += "\n\n";
    buf += centerColorText(b, 30, tw) + "\n";
    buf += centerColorText(m, 30, tw) + "\n";
    buf += centerColorText(b, 30, tw) + "\n\n";
    buf += centerColorText(std::string(GREEN) + "Press [R] to Return to Menu" + RESET, 27, tw) + "\n";
    buf += centerColorText(std::string(RED) + "Press [Q] to Quit" + RESET, 17, tw) + "\n";
    write(STDOUT_FILENO, buf.c_str(), buf.size());
}

void showTooSmallScreen() {
    clearScreen();
    char sm[64]; snprintf(sm, sizeof(sm), "  Minimum size: %d x %d\n\n", MIN_TERM_W, MIN_TERM_H);
    std::string buf;
    buf += "\n";
    buf += std::string(BOLD) + RED + "  Terminal too small!\n" + RESET;
    buf += std::string(YELLOW) + sm + RESET;
    buf += "  Please resize your terminal,\n";
    buf += std::string("  then press ") + GREEN + "[R]" + RESET
         + " for menu or " + RED + "[Q]" + RESET + " to quit.\n";
    write(STDOUT_FILENO, buf.c_str(), buf.size());
}

// ─── Main ───────────────────────────────────────────────────
int main() {
    srand(static_cast<unsigned>(time(nullptr)));

    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    enableRawMode();
    hideCursor();
    write(STDOUT_FILENO, "\033[?1049h", 8);
    atexit(atexitCleanup);

    AppState state = STATE_MENU;
    int lastScore = 0;
    bool lastWon = false;

    while (state != STATE_EXIT) {
        if (g_interrupted) break;

        switch (state) {

        case STATE_MENU:
            state = showStartMenu();
            break;

        case STATE_LEADERBOARD:
            state = showLeaderboardScreen();
            break;

        case STATE_PLAYING: {
            GameState game;
            initGame(game);

            if (game.termTooSmall) { state = STATE_TOO_SMALL; break; }

            clearScreen();
            long long lastFrame = nowMicros();

            while (game.running) {
                long long fs = nowMicros();
                long long dt = fs - lastFrame;
                lastFrame = fs;

                if (g_interrupted) { game.running = false; state = STATE_EXIT; break; }
                if (checkTerminalResize(game)) break;

                readInput(game);
                if (!game.running) break;

                if (!game.paused) {
                    game.moveAccumulator += dt;
                    long long mi = calcMoveInterval(game.score, game.nextDir);
                    if (game.moveAccumulator > mi * 3) game.moveAccumulator = mi;
                    while (game.moveAccumulator >= mi) {
                        updateGame(game);
                        if (!game.running) break;
                        game.moveAccumulator -= mi;
                        game.dirChangedThisTick = false;
                        if (game.hasQueuedDir) {
                            if (!isOpposite(game.queuedDir, game.dir) &&
                                game.queuedDir != game.dir) {
                                game.nextDir = game.queuedDir;
                                game.dirChangedThisTick = true;
                            }
                            game.hasQueuedDir = false;
                        }
                        mi = calcMoveInterval(game.score, game.nextDir);
                    }
                }
                if (!game.running) break;

                render(game);

                long long el = nowMicros() - fs;
                long long sl = RENDER_TICK_US - el;
                if (sl > 0) usleep(static_cast<useconds_t>(sl));
            }

            if (state == STATE_EXIT) break;
            if (game.restartRequested) { state = STATE_PLAYING; }
            else if (game.termResized) { state = STATE_RESIZED; }
            else if (game.gameOver || game.gameWon) {
                lastScore = game.score; lastWon = game.gameWon;
                state = STATE_GAMEOVER;
            } else { state = STATE_MENU; }
            break;
        }

        case STATE_GAMEOVER:
            showEndScreen(lastScore, lastWon);
            state = waitForMenuOrExit();
            break;

        case STATE_RESIZED:
            showResizedScreen();
            state = waitForMenuOrExit();
            break;

        case STATE_TOO_SMALL:
            showTooSmallScreen();
            state = waitForMenuOrExit();
            break;

        case STATE_EXIT: break;
        }
    }

    return 0;
}
