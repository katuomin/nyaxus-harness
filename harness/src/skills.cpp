#include "nyaxus/skills.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace nyaxus {
namespace skills {
namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Strip one layer of matching surrounding quotes.
std::string unquote(const std::string& v) {
    const std::string t = trim(v);
    if (t.size() >= 2 &&
        ((t.front() == '"' && t.back() == '"') ||
         (t.front() == '\'' && t.back() == '\''))) {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// List one directory's immediate entries (non-recursive).
bool list_dir(const std::string& dir, std::vector<std::string>& files,
              std::vector<std::string>& subdirs) {
#if defined(_WIN32)
    WIN32_FIND_DATAA fdata;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fdata);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        const std::string name = fdata.cFileName;
        if (name == "." || name == "..") continue;
        if ((fdata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            subdirs.push_back(name);
        } else {
            files.push_back(name);
        }
    } while (FindNextFileA(h, &fdata));
    FindClose(h);
    return true;
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        struct stat st;
        if (stat((dir + "/" + name).c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) subdirs.push_back(name);
            else files.push_back(name);
        }
    }
    closedir(d);
    return true;
#endif
}

// Bounded-recursive SKILL.md discovery under one root.
void collect(const std::string& dir, int depth,
             std::vector<std::string>& skill_paths) {
    if (depth > 4) return;
    std::vector<std::string> files, subdirs;
    if (!list_dir(dir, files, subdirs)) return;
    for (const std::string& f : files) {
        if (f == "SKILL.md") skill_paths.push_back(dir + "/" + f);
    }
    for (const std::string& sd : subdirs) {
        collect(dir + "/" + sd, depth + 1, skill_paths);
    }
}

// Bounded recursive file listing under one directory, as slash-relative paths
// (files before subdirectories; dotfiles and dotdirs skipped; 200-entry cap).
void list_files_rec(const std::string& dir, const std::string& prefix, int depth,
                    std::vector<std::string>& out) {
    if (depth > 4 || out.size() >= 200) return;
    std::vector<std::string> files, subdirs;
    if (!list_dir(dir, files, subdirs)) return;
    for (const std::string& f : files) {
        if (f.empty() || f.front() == '.') continue;
        out.push_back(prefix.empty() ? f : prefix + "/" + f);
        if (out.size() >= 200) return;
    }
    for (const std::string& sd : subdirs) {
        if (sd.empty() || sd.front() == '.') continue;
        list_files_rec(dir + "/" + sd,
                       prefix.empty() ? sd : prefix + "/" + sd, depth + 1, out);
    }
}

}  // namespace

std::string validate_name(const std::string& name) {
    if (name.size() < 1 || name.size() > 64) {
        return "name must be 1-64 characters";
    }
    for (char c : name) {
        if (!std::islower(static_cast<unsigned char>(c)) &&
            !std::isdigit(static_cast<unsigned char>(c)) && c != '-') {
            return "name must be [a-z0-9-] (lowercase letters, digits, '-')";
        }
    }
    return "";
}

std::string validate_description(const std::string& description) {
    if (description.size() < 1) {
        return "description must not be empty";
    }
    if (description.size() > 1024) {
        return "description must be at most 1024 characters";
    }
    return "";
}

std::string xml_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string skill_envelope(const Skill& skill, const std::string& source,
                           const std::vector<std::string>& aux_files) {
    std::ostringstream out;
    out << "<skill_content name=\"" << xml_escape(skill.name) << "\">\n";
    out << "<source>" << xml_escape(source) << "</source>\n";
    out << "<directory>" << xml_escape(skill.dir) << "</directory>\n";
    out << "Relative paths in this skill resolve against <directory>.\n";
    if (!aux_files.empty()) {
        out << "Files (read the ones you need with the terminal tool):\n";
        for (const std::string& f : aux_files) {
            out << "- " << xml_escape(f) << "\n";
        }
    }
    // The escaped body is the tag neutralization: no raw < can survive, so
    // the body cannot open or close the wrapper from the inside.
    out << xml_escape(skill.content) << "\n";
    out << "</skill_content>";
    return out.str();
}

std::optional<Skill> parse(const std::string& text, const std::string& path) {
    std::string body = text;
    if (body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        body = body.substr(3);  // UTF-8 BOM
    }
    std::string name, description, content = body;
    bool disable = false;

    // Frontmatter: a leading '---' line, a block of 'key: value' lines, and a
    // closing '---' line. Only the flat standard fields are read.
    std::size_t pos = 0;
    while (pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (body.compare(pos, 3, "---") == 0) {
        const std::size_t eol = body.find('\n', pos);
        if (eol != std::string::npos &&
            trim(body.substr(pos + 3, eol - (pos + 3))).empty()) {
            std::size_t close = eol;
            while (close < body.size()) {
                const std::size_t next = body.find('\n', close + 1);
                const std::size_t line_start = close + 1;
                const std::size_t line_end =
                    (next == std::string::npos) ? body.size() : next;
                std::string line = body.substr(line_start, line_end - line_start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (trim(line) == "---") {
                    const std::string fm =
                        body.substr(eol + 1, line_start - (eol + 1));
                    std::istringstream fmss(fm);
                    std::string fl;
                    while (std::getline(fmss, fl)) {
                        if (!fl.empty() && fl.back() == '\r') fl.pop_back();
                        const std::string tline = trim(fl);
                        if (tline.empty() || tline[0] == '#') continue;
                        const std::size_t colon = tline.find(':');
                        if (colon == std::string::npos) continue;
                        const std::string key =
                            lower(trim(tline.substr(0, colon)));
                        const std::string val =
                            unquote(tline.substr(colon + 1));
                        if (key == "name") {
                            name = val;
                        } else if (key == "description") {
                            description = val;
                        } else if (key == "disable-model-invocation") {
                            disable = (lower(val) == "true");
                        }
                    }
                    content = body.substr(line_end + 1);
                    break;
                }
                close = next;
                if (next == std::string::npos) break;
            }
        }
    }

    name = trim(name);
    description = trim(description);
    if (name.empty() || description.empty()) return std::nullopt;
    // Strict validation is the permanent stance: a malformed frontmatter is
    // a rejection, not a warning (discover reports the reason).
    if (!validate_name(name).empty() || !validate_description(description).empty()) {
        return std::nullopt;
    }
    Skill s;
    s.name = name;
    s.description = description;
    s.content = trim(content);
    s.path = path;
    const std::size_t slash = path.find_last_of("/\\");
    s.dir = (slash == std::string::npos) ? "" : path.substr(0, slash);
    s.disable_model_invocation = disable;
    return s;
}

std::vector<Skill> discover(const std::vector<std::string>& roots,
                            std::vector<std::string>* rejected) {
    std::vector<Skill> out;
    std::vector<std::string> seen;  // dedupe by name (first root wins)
    for (const std::string& root : roots) {
        if (root.empty()) continue;
        std::vector<std::string> skill_paths;
        collect(root, 0, skill_paths);
        for (const std::string& sp : skill_paths) {
            const std::string text = read_file(sp);
            if (text.empty()) continue;
            std::optional<Skill> skill = parse(text, sp);
            if (!skill) {
                if (rejected) rejected->push_back(sp + " (invalid frontmatter)");
                continue;
            }
            bool dup = false;
            for (const std::string& n : seen) {
                if (n == skill->name) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            seen.push_back(skill->name);
            out.push_back(std::move(*skill));
        }
    }
    return out;
}

std::vector<std::string> list_files(const std::string& dir) {
    std::vector<std::string> out;
    list_files_rec(dir, "", 0, out);
    return out;
}

std::string catalog_block(const std::vector<Skill>& skills) {
    std::vector<const Skill*> invocable;
    for (const Skill& s : skills) {
        if (!s.disable_model_invocation) invocable.push_back(&s);
    }
    if (invocable.empty()) return "";
    std::ostringstream out;
    out << "You can invoke specialized skills on demand. Each skill is a "
        << "self-contained instruction pack for a specific kind of task; call "
        << "the `skill` tool with a skill's name to load its full "
        << "instructions when a task matches one. Available skills:\n";
    std::size_t omitted = 0;
    for (const Skill* s : invocable) {
        // Values are XML-escaped (prompt-injection defense), and the whole
        // block is capped at the fixed budget (predictable for authors).
        const std::string line =
            "- " + xml_escape(s->name) + ": " + xml_escape(s->description) + "\n";
        if (out.str().size() + line.size() > kCatalogBudgetBytes) {
            ++omitted;
            continue;
        }
        out << line;
    }
    if (omitted > 0) {
        out << "(" << omitted << " more skill" << (omitted == 1 ? "" : "s")
            << " omitted: catalog over the "
            << (kCatalogBudgetBytes / 1024) << "KB budget)\n";
    }
    return out.str();
}

}  // namespace skills
}  // namespace nyaxus
