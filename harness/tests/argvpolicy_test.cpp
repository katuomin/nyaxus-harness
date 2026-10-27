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
              "sed -ni 's/a/b/' file", "sed --in-place file",
              "find . -delete", "find . -exec ls {}",
              "find . -ok ls {}", "find . -fprint f", "find . -printf x",
              "find . -execdir ls"});

    // The sed inline script is command data that can escape stream
    // editing: s's w flag and w/W write files, r/R read files, e runs a
    // shell. Classification therefore scans every script part (-e values
    // plus the bare positional script) command by command; anything the
    // scanner cannot parse exactly gates (fail-closed). -f/--file loads
    // the script from an unvouched file and gates outright.
    section("POSIX sed: provably-safe inline script");
    ro_cases(Shell::Posix,
             {"sed 's/a/b/g'", "sed -e 's/a/b/' -e 'y/ab/cd/'",
              "sed -n '10,20p'", "sed 's|x|y|3'", "sed '/re/d'",
              "sed '$d'", "sed ':a;N;$!ba;s/\\n/ /g'",
              "sed -n '/x/{s/a/b/p}'", "sed '1~2p'", "sed '1,+1p'",
              "sed '/h/I!d'", "sed 's/\\[/X/'", "sed 'q'", "sed 'q5'",
              "sed 'l 40'", "sed '=;='", "sed 'a\\'",
              "sed --expression=s/a/b/ file", "sed -ne s/a/b/g file"},
             {"sed 's/a/b/w out.txt'",   // s w flag: file write
              "sed '/x/w f'",            // w command
              "sed '/x/W f'",            // W command
              "sed 'r f'",               // r command
              "sed '$r f'",
              "sed 'e echo hi'",         // e command: shell exec
              "sed 's/a/b/e'",           // s e flag: shell exec
              "sed -f script.sed",       // unvouched script file
              "sed --file=script.sed",
              "sed -fscript.sed",
              "sed -nf script.sed",      // bundled -f in a bundle
              "sed -l 40 'l'",           // -l takes a value; gates
              "sed --debug 'p'",         // unknown long option
              "sed 's/a/b'",             // unterminated s body
              "sed '/x[/d'",             // unterminated address regex
              "sed 'X'",                 // unknown command letter
              "sed 'T skip'",            // unclassified GNU command
              "sed 'v'",                 // GNU version command
              "sed 's/a/b/x'",           // unknown s flag
              "sed 'a\nw out.txt'"});    // next line after bare a RUNS

    section("POSIX sed: single-quote script data (sh literals)");
    ro_cases(Shell::Posix,
             {"sed 's/$d/X/' file", "sed '$!d' file",
              "echo '!not-history'"},
             {});

    // Risk-verb taxonomy checklist; fail-closed default posture
    // (unknown command -> gate) is what keeps most of these safe, so no
    // entry was added where the shape already gates.

    section("risk-verb taxonomy: state-mutating verbs (both flavors)");
    for (Shell shell : {Shell::Posix, Shell::Cmd}) {
        ro_cases(shell,
                 {},
                 {"date -s \"next week\"", "date", "date +%s -s 1234",
                  "ip addr add 10.0.0.1/24 dev eth0",
                  "ip link set eth0 up",
                  "mount -t tmpfs tmpfs /mnt/x",
                  "umount /mnt/x",
                  "kill -9 1234", "pkill node", "killall chrome",
                  "dotnet tool install --global mytool",
                  "dotnet tool restore"});
    }

    section("risk-verb taxonomy: mutating flags on safe names");
    ro_cases(Shell::Posix,
             {"sort a b | uniq -c", "sort keys.txt", "rg pattern file",
              "rg --type rust todo", "grep -rn pattern src/",
              "grep -r pattern .", "grep -H pattern f",
              "grep -o pattern src/", "egrep -i x y", "fgrep x y",
              "tail -f log.txt", "head -f -n 5 log.txt",
              "find . -maxdepth 1 -type f",
              "find . -name \"*.txt\" -print"},
             {"git commit -m fix",
              "sort -o out.txt in.txt", "sort --output=out.txt in.txt",
              "sort -ofu in.txt",   // bundled -o cluster
              "rg --follow pattern src/", "rg --pre 'x' pattern src/",
              "grep -R pattern .", "grep -nR pattern .",
              "grep --dereference-recursive pattern .",
              "grep --dereference-regex pattern .",
              "egrep -R x y", "fgrep -R x y",
              "find -L . -type f", "find -H . -type f",
              "find . -name x -L"});

    // Command-data vs filesystem-operand separation: for already-allowlisted
    // read-only commands, an argument the command itself interprets as data
    // (pattern text, script, sort spec, replacement text) must not be
    // classified by the write-flag tables, and the end-of-options marker
    // hands the rest to the operands. Deny flags in real option position,
    // and the process-spawn data smuggles found by the audit, still gate.

    section("data options: command data is not a write flag");
    ro_cases(Shell::Posix,
             {"grep -e -R pattern",          // -e data: searches for "-R"
              "grep -e -Rx src/",            // bundled in the option value
              "grep -e -R src/ -e -Rx f",    // plus another data option
              "grep -ne -R pattern file",    // getopt bundle -n + -e <word>
              "grep --regexp=-R file",       // attached long value
              "grep --regexp -R file",       // separate long value
              "grep -f -R names.txt",        // -f consumes "-R" as file name
              "grep -- -R src/",             // "-R" after -- is the pattern
              "rg -e --prefix todo",         // --pre deny must not hit data
              "rg -r --follow pattern src/", // -r consumes "--follow"
              "rg -r -R x y",
              "rg --replace=-R todo",
              "rg --regexp -R todo",
              "sort -k -o data.txt",         // key spec "-o", not an option
              "sort -t- data.txt", "sort -t: -k2,3 data.txt",
              "sort -nk 2",                  // bundle -n + -k <word>
              "sort -k2,2 data.txt",         // attached: -k2,2
              "sort -S -o data.txt",
              "sort -- -o data.txt",         // operands after --
              "sed -e -i file",              // script "-i" is data
              "sed --expression=-i file",
              "sed -e s/-R/x/ file",
              "sed -- -i file",
              "find . -name -L",             // -name consumes "-L" (GNU find
              "find . -iname -H"},           // takes the next arg verbatim)
             {"grep -R -e -R src/",          // an option is an option
              "grep -e -R src/ -R",          // trailing real option still gates
              "grep --dereference-recursive --regexp -R .",
              "rg --follow -r -R x",         // --follow before the data option
              "rg --pre x -r -R y",
              "sort -o -k2 data.txt",
              "sort -k2,2 -o out.txt in.txt",
              "sort --compress-program=x data.txt",
              "sort --compress-program gzip data.txt",
              "sed -i -e s/x/y/ file",       // -i in option position
              "sed --in-place --expression s/x/y/ file",
              "find -L . -name x",
              "find . -name x -L"});

    section("process-spawn data smuggles gate");
    ro_cases(Shell::Posix, {},
             {"man -P less ls", "man --pager=less ls", "man -H x",
              "man --html=lynx ls",
              "fd -x echo x", "fd -X rm .", "fd --exec echo .",
              "fd --exec-batch=echo .", "fdfind -x echo",
              "git grep -O pattern",          // pager command runs via shell
              "git grep -Oless pattern",
              "git grep --open-files-in-pager pattern"});
    ro_cases(Shell::Posix,
             {"man ls", "man -a gcc", "man -w ls",  // reads stay allowed
              "fd -e txt --hidden .", "fdfind --type file x"},
             {});

    section("cmd.exe: no end-of-options semantics in the tables");
    ro_cases(Shell::Cmd, {"icacls dir /T"},
             {"icacls dir -- /grant u:R"});

    if (g_fail == 0) {
        std::cout << "\nARGVPOLICY_TEST_PASSED\n";
        return 0;
    }
    std::cout << "\n" << g_fail << " FAILURES\n";
    return 1;
}
