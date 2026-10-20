// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash_ai_commands.h — Complete Command Map for the AI Agent
// Every command the AI can use, categorized and described.
// This is the AI's "brain map" of what it can do.

#ifndef JASH_AI_COMMANDS_H
#define JASH_AI_COMMANDS_H

// ── Command Categories ──────────────────────────────────────────────────────

typedef struct {
    const char *name;        // command name
    const char *description; // what it does
    const char *usage;       // example usage
    int danger_level;        // 0=safe, 1=confirm, 2=blocked
} jash_cmd_info_t;

// ── SAFE COMMANDS (danger_level = 0) ────────────────────────────────────────
// The AI can execute these without asking

static const jash_cmd_info_t JASH_CMD_MAP[] = {

    // ═══ NAVIGATION ═══
    {"cd",      "Change working directory",           "cd src/",           1},
    {"pwd",     "Print current working directory",    "pwd",               0},
    {"pushd",   "Push dir and change to it",          "pushd /tmp",        0},
    {"popd",    "Pop dir from stack and change",      "popd",              0},
    {"dirs",    "Display directory stack",            "dirs",              0},

    // ═══ FILE LISTING ═══
    {"ls",      "List directory contents",            "ls -la",            0},
    {"tree",    "Display directory tree",             "tree -L 2",         0},
    {"find",    "Find files by criteria",             "find . -name '*.c'",0},
    {"locate",  "Find files by database",             "locate filename",   0},
    {"which",   "Locate a command",                   "which gcc",         0},
    {"whereis", "Locate binary, source, man page",    "whereis python3",   0},

    // ═══ FILE OPERATIONS ═══
    {"touch",   "Create file or update timestamp",    "touch newfile.c",   0},
    {"cat",     "Concatenate and display files",      "cat README.md",     0},
    {"head",    "Display first lines of file",        "head -20 file.c",   0},
    {"tail",    "Display last lines of file",         "tail -f log.txt",   0},
    {"less",    "View file contents paginated",       "less large_file",   0},
    {"more",    "View file contents paginated",       "more file",         0},
    {"cp",      "Copy files or directories",          "cp -r src/ dst/",   1},
    {"mv",      "Move or rename files",               "mv old.c new.c",    1},
    {"rm",      "Remove files or directories",        "rm -i old.c",       1},
    {"mkdir",   "Create directories",                 "mkdir -p src/core", 1},
    {"rmdir",   "Remove empty directories",           "rmdir empty_dir",   1},
    {"ln",      "Create links",                       "ln -s target link", 1},
    {"stat",    "Display file status",                "stat file.c",       0},
    {"file",    "Determine file type",                "file binary",       0},
    {"wc",      "Count lines, words, bytes",          "wc -l *.c",         0},
    {"du",      "Disk usage summary",                 "du -sh .",          0},
    {"df",      "Filesystem disk space",              "df -h",             0},

    // ═══ TEXT SEARCH & PROCESSING ═══
    {"grep",    "Search text patterns in files",      "grep -rn 'TODO' .", 0},
    {"rg",      "Ripgrep - fast text search",         "rg 'function' *.c", 0},
    {"sed",     "Stream editor (text transform)",     "sed 's/old/new/g'", 0},
    {"awk",     "Text processing language",           "awk '{print $1}'",  0},
    {"sort",    "Sort lines of text",                 "sort file.txt",     0},
    {"uniq",    "Filter duplicate lines",             "sort file | uniq",  0},
    {"cut",     "Extract columns from lines",         "cut -d: -f1 /etc/passwd", 0},
    {"tr",      "Translate/delete characters",        "tr 'a-z' 'A-Z'",   0},
    {"diff",    "Compare files line by line",         "diff a.c b.c",      0},
    {"cmp",     "Compare files byte by byte",         "cmp file1 file2",   0},
    {"tee",     "Read stdin and write to file+stdout", "cmd | tee out.txt", 0},

    // ═══ SYSTEM INFO ═══
    {"uname",   "System information",                 "uname -a",          0},
    {"uptime",  "System uptime and load",             "uptime",            0},
    {"whoami",  "Current username",                   "whoami",            0},
    {"id",      "User and group IDs",                 "id",                0},
    {"date",    "Display/set date and time",          "date",              0},
    {"cal",     "Display calendar",                   "cal",               0},
    {"free",    "Memory usage",                       "free -h",           0},
    {"lscpu",   "CPU information",                    "lscpu",             0},
    {"lsblk",   "Block device information",           "lsblk",             0},
    {"lsusb",   "USB device information",             "lsusb",             0},
    {"lspci",   "PCI device information",             "lspci",             0},
    {"env",     "Print environment variables",        "env",               0},
    {"printenv","Print specific env variable",        "printenv HOME",     0},

    // ═══ PROCESS MANAGEMENT ═══
    {"ps",      "List processes",                     "ps aux",            0},
    {"top",     "Interactive process viewer",         "top -bn1",          0},
    {"htop",    "Enhanced process viewer",            "htop",              0},
    {"kill",    "Send signal to process",             "kill -9 PID",       1},
    {"killall", "Kill processes by name",             "killall firefox",   1},
    {"pkill",   "Kill by pattern match",              "pkill node",        1},
    {"pgrep",   "Find process by name",               "pgrep python",      0},
    {"jobs",    "List background jobs",               "jobs",              0},
    {"fg",      "Bring job to foreground",            "fg %1",             0},
    {"bg",      "Resume job in background",           "bg %1",             0},
    {"nohup",   "Run command immune to hangups",      "nohup cmd &",       0},
    {"nice",    "Run with modified priority",         "nice cmd",          0},
    {"time",    "Measure command execution time",     "time make",         0},

    // ═══ NETWORK ═══
    {"curl",    "Transfer data from URLs",            "curl -s url",       0},
    {"wget",    "Download files from web",            "wget url",          0},
    {"ping",    "Test network connectivity",          "ping -c 4 google.com", 0},
    {"ip",      "Network configuration",              "ip addr",           0},
    {"ifconfig","Network interface config (legacy)",  "ifconfig",          0},
    {"netstat", "Network statistics",                 "netstat -tlnp",     0},
    {"ss",      "Socket statistics",                  "ss -tlnp",          0},
    {"nslookup","DNS lookup",                         "nslookup google.com", 0},
    {"dig",     "DNS lookup (detailed)",              "dig google.com",    0},
    {"host",    "DNS lookup",                         "host google.com",   0},
    {"scp",     "Secure copy over SSH",               "scp file host:/tmp",1},
    {"ssh",     "Secure shell remote login",          "ssh user@host",     1},

    // ═══ ARCHIVE & COMPRESS ═══
    {"tar",     "Archive files",                      "tar czf arc.tar.gz dir/", 1},
    {"gzip",    "Compress with gzip",                 "gzip file",         1},
    {"gunzip",  "Decompress gzip",                    "gunzip file.gz",    0},
    {"zip",     "Create zip archive",                 "zip -r arc.zip dir/", 1},
    {"unzip",   "Extract zip archive",                "unzip arc.zip",     0},

    // ═══ DEVELOPMENT ═══
    {"gcc",     "C compiler",                         "gcc -o out main.c", 0},
    {"g++",     "C++ compiler",                       "g++ -o out main.cpp", 0},
    {"make",    "Build automation",                   "make -j4",          0},
    {"cmake",   "Cross-platform build system",        "cmake -B build",    0},
    {"ninja",   "Fast build system",                  "ninja -C build",    0},
    {"git",     "Version control",                    "git status",        0},
    {"python3", "Python interpreter",                 "python3 script.py", 0},
    {"python",  "Python interpreter (alias)",         "python script.py",  0},
    {"node",    "Node.js runtime",                    "node script.js",    0},
    {"npm",     "Node package manager",               "npm install",       1},
    {"gdb",     "GNU debugger",                       "gdb ./binary",      0},
    {"valgrind","Memory debugging",                   "valgrind ./binary", 0},
    {"strace",  "System call tracer",                 "strace ./binary",   0},

    // ═══ DISK & SYSTEM ═══
    {"mount",   "Mount filesystems",                  "mount /dev/sda1 /mnt", 2},
    {"umount",  "Unmount filesystems",                "umount /mnt",       2},
    {"fdisk",   "Partition table manipulator",        "fdisk -l",          2},
    {"mkfs",    "Create filesystem",                  "mkfs.ext4 /dev/sda1", 2},
    {"dd",      "Low-level data copy",                "dd if=/dev/zero of=test bs=1M count=1", 2},
    {"chmod",   "Change file permissions",            "chmod +x script.sh",1},
    {"chown",   "Change file ownership",              "chown user:group f",1},
    {"chgrp",   "Change group ownership",             "chgrp group f",     1},
    {"umask",   "Set file creation mask",             "umask 022",         0},
    {"ulimit",  "Set resource limits",                "ulimit -a",         0},

    // ═══ JASH BUILTINS ═══
    {"echo",    "Print arguments to stdout",          "echo $HOME",        0},
    {"printf",  "Formatted print",                    "printf '%s\\n' txt",0},
    {"read",    "Read line from stdin",               "read -p 'Name: ' var", 0},
    {"export",  "Set environment variable",           "export PATH=$PATH:/new", 0},
    {"unset",   "Remove environment variable",        "unset OLD_VAR",     0},
    {"alias",   "Create command alias",               "alias g='git'",     0},
    {"unalias", "Remove alias",                       "unalias g",         0},
    {"history", "Show command history",               "history",           0},
    {"type",    "Show command type",                  "type cd",           0},
    {"help",    "Show builtin help",                  "help",              0},
    {"version", "Show jash version",                  "version",           0},
    {"clear",   "Clear terminal screen",              "clear",             0},
    {"source",  "Execute commands from file",         "source script.sh",  0},
    {"eval",    "Evaluate arguments as command",      "eval echo hello",   0},
    {"exec",    "Execute command (replaces shell)",   "exec cmd",          0},
    {"trap",    "Handle signals",                     "trap 'echo hi' INT",0},
    {"let",     "Evaluate arithmetic",                "let x=5+3",         0},
    {"set",     "Set shell options",                  "set -e",            0},
    {"shopt",   "Shell options",                      "shopt -s extglob",  0},
    {"declare", "Declare variable attributes",        "declare -i x=5",    0},
    {"readonly","Mark variables readonly",            "readonly VAR=val",  0},
    {"shift",   "Shift positional parameters",        "shift",             0},
    {"test",    "Evaluate conditional expression",    "test -f file",      0},
    {"command", "Execute command (skip functions)",    "command ls",        0},
    {"enable",  "Enable/disable builtins",            "enable -n kill",    0},
    {"hash",    "Hash table for command paths",       "hash -r",           0},
    {"builtin", "Execute builtin directly",           "builtin cd /tmp",   0},
    {"compgen", "Generate completion matches",        "compgen -c",        0},
    {"getopts", "Parse positional parameters",        "getopts 'ab:' opt", 0},
    {"mapfile", "Read lines into array",              "mapfile arr < file",0},
    {"array",   "Shell arrays",                       "array set arr 0 val", 0},
    {"calc",    "Inline calculator",                  "calc 2+2",          0},
    {"plugin",  "Load/unload plugins",                "plugin list",       0},
    {"theme",   "Switch color themes",                "theme monokai",     0},
    {"statusbar","Toggle status bar",                 "statusbar toggle",  0},
    {"jash_state","Save/restore shell state",         "jash_state save",   0},
    {"exit",    "Exit the shell",                     "exit",              0},

    // ═══ JASH AI COMMANDS ═══
    {"? help",     "Show AI agent help",              "? help",            0},
    {"? on",       "Activate AI agent mode",          "? on",              0},
    {"? auto",     "Activate deep integration mode",  "? auto",            0},
    {"? learn",    "Activate learning mode",          "? learn",           0},
    {"? off",      "Deactivate AI agent",             "? off",             0},
    {"? status",   "Show AI agent status",            "? status",          0},

    {NULL, NULL, NULL, 0}
};

// ── Helper: get danger level of a command ───────────────────────────────────
static inline int jash_cmd_danger(const char *cmd) {
    if (!cmd) return 0;
    for (int i = 0; JASH_CMD_MAP[i].name; i++) {
        const char *n = JASH_CMD_MAP[i].name;
        const char *c = cmd;
        while (*n && *c && *n == *c) { n++; c++; }
        if (*n == '\0' && (*c == '\0' || *c == ' ')) {
            return JASH_CMD_MAP[i].danger_level;
        }
    }
    return 0; // unknown = safe (let the system handle it)
}

// ── Helper: get command description ─────────────────────────────────────────
static inline const char* jash_cmd_desc(const char *cmd) {
    if (!cmd) return NULL;
    for (int i = 0; JASH_CMD_MAP[i].name; i++) {
        const char *n = JASH_CMD_MAP[i].name;
        const char *c = cmd;
        while (*n && *c && *n == *c) { n++; c++; }
        if (*n == '\0' && (*c == '\0' || *c == ' ')) {
            return JASH_CMD_MAP[i].description;
        }
    }
    return NULL;
}

#endif // JASH_AI_COMMANDS_H
