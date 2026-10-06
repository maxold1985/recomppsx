#pragma once

#include <string>
#include <cerrno>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#endif

namespace psxrecomp {
namespace compat {

template <typename T>
inline T clamp_value(T value, T lo, T hi)
{
    return value < lo ? lo : (value > hi ? hi : value);
}

inline bool is_directory(const std::string& path)
{
#ifdef _WIN32
    struct _stat st;
    return _stat(path.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

inline bool directory_exists(const std::string& path)
{
    return is_directory(path);
}

inline bool make_directory(const std::string& path)
{
    if (path.empty() || directory_exists(path))
        return true;
#ifdef _WIN32
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

inline bool create_directories(const std::string& path)
{
    if (path.empty())
        return false;
    if (directory_exists(path))
        return true;

    std::string current;
    current.reserve(path.size());

    for (std::size_t i = 0; i < path.size(); ++i) {
        const char ch = path[i];
        current.push_back(ch);

        const bool separator = (ch == '/' || ch == '\\');
        if (!separator)
            continue;

        // Skip drive root such as C:\\.
        if (current.size() == 3 && current[1] == ':')
            continue;

        while (current.size() > 1 &&
               (current[current.size() - 1] == '/' || current[current.size() - 1] == '\\'))
            current.erase(current.size() - 1);

        if (!current.empty() && !make_directory(current))
            return false;

        current.push_back('/');
    }

    while (!current.empty() &&
           (current[current.size() - 1] == '/' || current[current.size() - 1] == '\\'))
        current.erase(current.size() - 1);

    return current.empty() ? true : make_directory(current);
}

inline std::string join_path(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a[a.size() - 1];
    if (last == '/' || last == '\\')
        return a + b;
    return a + "/" + b;
}

} // namespace compat
} // namespace psxrecomp
