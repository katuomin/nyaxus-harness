// Read-only argv policy: the fail-closed terminal auto-approve classifier
// (design: docs/readonly-argv.md).
//
// Pure logic - no model artifacts, no daemon. The safe tables are the
// product, so this asserts the full matrix for BOTH shell flavors on every
// platform: the daemon picks the shell it will run (unix /bin/sh preferred,
// cmd.exe fallback) and classifies against exactly that flavor. The git
// policy and the union dynamic-syntax scanner are flavor-independent; the
// per-shell tables (cmd.exe: dir/type/..., /bin/sh: ls/cat/...), the env
// prefix, the null device, and name case-sensitivity are flavor-specific.
#include <iostream>
#include <string>
#include <vector>

#include "nyaxus/argvpolicy.hpp"

using namespace nyaxus;
namespace ap = nyaxus::argvpolicy;
using ap::Shell;

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
    std::cout << (cond ? "  PASS  " : "  FAIL  ") << what << "\n";
    std::cout.flush();
    if (!cond) g_fail++;
}

static void section(const std::string& name) {
    std::cout << "\n== " << name << " ==\n";
    std::cout.flush();
}

static std::string shell_name(Shell shell) {
    return shell == Shell::Cmd ? "cmd" : "sh";
}

static const char* kNul(Shell shell) {
    return shell == Shell::Cmd ? "NUL" : "/dev/null";
}

static const char* kListCmd(Shell shell) {
    return shell == Shell::Cmd ? "dir" : "ls";
}

static void ro_cases(Shell shell, std::vector<std::string> read_only,
                     std::vector<std::string> gates) {
    const std::string tag = "[" + shell_name(shell) + "] ";
    for (const std::string& command : read_only) {
        check(ap::evaluate(command, shell).read_only, tag + "read-only: " + command);
    }
    for (const std::string& command : gates) {
        check(!ap::evaluate(command, shell).read_only, tag + "gates: " + command);
    }
}

int main() {
    for (Shell shell : {Shell::Posix, Shell::Cmd}) {
        section(std::string("empty / degenerate (") + shell_name(shell) + ")");
        ro_cases(shell, {}, {"", "   ", ";;"});

        section(std::string("dynamic words gate (union of both shells, ") +
                shell_name(shell) + ")");
        ro_cases(shell, {}, {"ls $HOME", "ls `pwd`", "dir %TEMP%",
                             "echo !var!", "ls ~", "ls $(pwd)",
                             "echo \"100%\"", "echo \"a $b\"", "ls *\"",
                             "echo 'unterminated"});

        section(std::string("shell syntax gates (") + shell_name(shell) + ")");
        ro_cases(shell, {}, {"ls &", "ls &&", "(ls)", "ls |& wc", "ls >&",
                             "ls >> file.txt", "echo x >> log",
                             "cat << EOF", "echo a&b", "ls > /dev/tcp/h/p",
                             "ls > /dev/udp/x/1"});

        section(std::string("redirects: null device ok, files gate (") +
                shell_name(shell) + ")");
        ro_cases(shell,
                 {std::string(kListCmd(shell)) + " > " + kNul(shell),
                  std::string(kListCmd(shell)) + " 2> " + kNul(shell),
                  "git status 2>&1",
                  "git log > " + std::string(kNul(shell)) + " 2>&1",
                  "echo hi > " + std::string(kNul(shell)),
                  std::string(kListCmd(shell)) + " < file.txt"},
                 {std::string(kListCmd(shell)) + " > out.txt",
                  std::string(kListCmd(shell)) + " >> out.txt",
                  "echo hi > out.txt"});

        section(std::string("sequences/pipes: every stage must be read-only (") +
                shell_name(shell) + ")");
        ro_cases(shell,
                 {std::string(kListCmd(shell)) + " && git status",
                  std::string(kListCmd(shell)) + "; git status",
                  std::string(kListCmd(shell)) + " || git status",
                  std::string(kListCmd(shell)) + " | git grep x",
                  "echo a && git status", "git status; echo b"},
                 {std::string(kListCmd(shell)) + " && rm x",
                  std::string(kListCmd(shell)) + "; rm x",
                  "rm x && echo a", std::string(kListCmd(shell)) + " | rm x"});
    }

    section("env-assignment prefix: sh only");
    ro_cases(Shell::Posix,
             {"NO_COLOR=1 ls", "LANG=C ls", "CI=true echo hi"},
             {"PATH=/x ls", "FOO=bar ls"});
    ro_cases(Shell::Cmd, {}, {"NO_COLOR=1 dir", "CI=true echo hi"});

    section("name case-sensitivity");
    ro_cases(Shell::Cmd, {"GIT STATUS", "Dir /s", "Type C:\\x.txt"},
             {"LS", "CAT file.txt"});
    ro_cases(Shell::Posix, {"git status", "ls"},
             {"GIT STATUS", "Git status", "LS"});

    section("git: global flags (both flavors)");
    for (Shell shell : {Shell::Posix, Shell::Cmd}) {
        ro_cases(shell,
                 {"git --no-pager log", "git --paginate status",
                  "git --no-pager --no-pager log"},
                 {"git -c user.name=x log", "git -C /repo status",
                  "git --git-dir=/x status", "git --work-tree=/x status",
                  "git --exec-path=/x status", "git --bare status",
                  "git --git-pager cat status", "git", "git --no-pager"});
    }

    section("git: read-only subcommands (both flavors)");
    for (Shell shell : {Shell::Posix, Shell::Cmd}) {
        ro_cases(shell,
                 {"git status", "git log --oneline -20", "git show HEAD",
                  "git diff --staged", "git blame file",
                  "git rev-parse HEAD", "git describe --tags",
                  "git ls-files", "git cat-file -p HEAD", "git grep pattern",
                  "git shortlog", "git version", "git help log",
                  "git rev-list --count HEAD", "git count-objects -v",
                  "git verify-pack pack.idx", "git for-each-ref", "git fsck"},
                 {"git diff --output=file.txt", "git diff -o file.txt",
                  "git diff HEAD~1"});
    }

    section("git: list-mode subcommands (both flavors)");
    for (Shell shell : {Shell::Posix, Shell::Cmd}) {
        ro_cases(shell,
                 {"git tag", "git tag --list", "git tag -n1", "git branch",
                  "git branch -a", "git branch --merged", "git remote",
                  "git remote -v", "git config", "git config --list",
                  "git config user.name", "git config --global user.name",
                  "git index-version", "git symbolic-ref HEAD"},
                 {"git tag v1", "git tag -d v1", "git branch new",
                  "git branch -d main", "git branch --contains HEAD",
                  "git remote add origin url", "git config user.name bob",
                  "git index-version 4", "git symbolic-ref",
                  "git symbolic-ref HEAD refs/heads/main"});
    }

    section("git: fixed verbs (both flavors)");
    for (Shell shell : {Shell::Posix, Shell::Cmd}) {
        ro_cases(shell, {"git reflog show", "git stash list",
                         "git worktree list"},
                 {"git reflog", "git reflog expire --all", "git stash",
                  "git stash pop", "git worktree", "git worktree add x"});
    }

    section("git: known writes gate (both flavors)");
    for (Shell shell : {Shell::Posix, Shell::Cmd}) {
        ro_cases(shell, {}, {"git add .", "git commit -m x", "git push",
                             "git pull", "git fetch origin", "git merge main",
                             "git rebase main", "git checkout main",
                             "git switch main", "git restore f",
                             "git reset --hard", "git clean -fd", "git gc",
                             "git cherry-pick abc", "git revert abc",
                             "git apply p", "git archive HEAD",
                             "git clone url", "git init", "git mv a b",
                             "git rm f", "git svn fetch",
                             "git update-ref refs/x y", "git write-tree"});
    }

    section("git: unknown subcommand gates (both flavors)");
    for (Shell shell : {Shell::Posix, Shell::Cmd}) {
        ro_cases(shell, {}, {"git frobnicate"});
    }

    section("cmd.exe table: any-args safe");
    ro_cases(Shell::Cmd,
             {"dir /s /b", "dir /a-d", "type C:\\x.txt",
              "findstr /s pattern .", "where node", "echo hello world",
              "cd subdir && dir", "pushd x && popd", "tree /f",
              "netstat -ano", "tasklist /v", "systeminfo", "vol",
              "hostname", "whoami", "ver"},
             {"attrib +r file", "ping host", "python script.py",
              "node script.js", "cargo build", "npm test", "make",
              "set X=Y", "set /p var=", "set /a x=1+1", "title new title",
              "ls", "cat file.txt"});

    section("cmd.exe table: icacls display vs grant");
    ro_cases(Shell::Cmd, {"icacls dir", "icacls C:\\x /T"},
             {"icacls dir /grant u:R", "icacls dir /remove u",
              "icacls dir /setowner u", "icacls dir /reset"});

    section("cmd.exe table: set prints the environment");
    ro_cases(Shell::Cmd, {"set"}, {"set X=Y"});

    section("POSIX table: any-args safe");
    ro_cases(Shell::Posix,
             {"ls", "ls -la", "cat file.txt", "grep -rn pattern src/",
              "rg --type rust todo", "fd -e txt", "head -n 20 log.txt",
              "tail -f log.txt", "wc -l file", "sort a b | uniq -c",
              "du -sh .", "df -h", "stat file", "file binary",
              "which git", "strings blob", "hexdump -C file | head",
              "od -c file", "nl file", "seq 1 3", "sleep 1", "uname -a",
              "ps -ef", "pgrep -f node", "man ls", "basename a/b",
              "dirname a/b", "realpath x", "readlink x", "tr a-z A-Z",
              "cut -d, -f1 file", "jq . config.json", "test -f file",
              "pwd", "true", "false", "whoami", "hostname", "ver",
              "ls FOO=bar"},
             {"date -s \"next week\"", "uptime -w", "python script.py",
              "node script.js", "cargo build", "npm test", "make",
              "dir /s", "echo $x", "rm x"});

    section("POSIX table: sed / find write options");
    ro_cases(Shell::Posix,
             {"sed -n 'p' file", "sed -E 's/a/b/' file",
              "find . -maxdepth 2 -type f", "find . -name \"*.txt\""},
             {"sed -i 's/a/b/' file", "sed -i.bak 's/a/b/' file",
              "sed --in-place file", "find . -delete", "find . -exec ls {}",
              "find . -ok ls {}", "find . -fprint f", "find . -printf x",
              "find . -execdir ls"});

    if (g_fail == 0) {
        std::cout << "\nARGVPOLICY_TEST_PASSED\n";
        return 0;
    }
    std::cout << "\n" << g_fail << " FAILURES\n";
    return 1;
}
