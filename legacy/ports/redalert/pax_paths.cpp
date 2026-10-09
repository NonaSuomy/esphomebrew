// Paths and file lookup for the Red Alert PAX (replaces common/paths_posix.cpp
// and common/file_posix.cpp). Program, data and user files all live in one
// folder on the SD card; REDALERT.INI there can still override them.
#include "pax_port.h"

#include "common/file.h"
#include "common/paths.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

const char* PathsClass::Program_Path()
{
    if (ProgramPath.empty()) {
        ProgramPath = PAX_RA_DATA_DIR;
    }
    return ProgramPath.c_str();
}

const char* PathsClass::Data_Path()
{
    if (DataPath.empty()) {
        DataPath = PAX_RA_DATA_DIR;
    }
    return DataPath.c_str();
}

const char* PathsClass::User_Path()
{
    if (UserPath.empty()) {
        UserPath = PAX_RA_DATA_DIR;
    }
    return UserPath.c_str();
}

bool PathsClass::Create_Directory(const char* dirname)
{
    if (dirname == nullptr || pax_svc == nullptr || pax_svc->file_mkdir == nullptr) {
        return false;
    }

    char path[512];
    if (dirname[0] == '/') {
        if (snprintf(path, sizeof(path), "%s", dirname) >= (int)sizeof(path)) {
            return false;
        }
    } else if (snprintf(path, sizeof(path), "%s/%s", PAX_RA_DATA_DIR, dirname) >= (int)sizeof(path)) {
        return false;
    }
    return pax_svc->file_mkdir(path) == 0;
}

bool PathsClass::Is_Absolute(const char* path)
{
    return path != nullptr && path[0] == '/';
}

std::string PathsClass::Concatenate_Paths(const char* path1, const char* path2)
{
    return std::string(path1) + SEP + path2;
}

std::string PathsClass::Get_Filename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return std::string(slash != nullptr ? slash + 1 : path);
}

std::string PathsClass::Argv_Path(const char*)
{
    return PAX_RA_DATA_DIR;
}

static bool wildcard_match_ci(const char* pattern, const char* name)
{
    while (*pattern != '\0') {
        if (*pattern == '*') {
            do {
                ++pattern;
            } while (*pattern == '*');
            if (*pattern == '\0') {
                return true;
            }
            do {
                if (wildcard_match_ci(pattern, name)) {
                    return true;
                }
            } while (*name++ != '\0');
            return false;
        }
        if (*name == '\0' || (*pattern != '?' && tolower((unsigned char)*pattern) != tolower((unsigned char)*name))) {
            return false;
        }
        ++pattern;
        ++name;
    }
    return *name == '\0';
}

class Find_File_Data_PAX : public Find_File_Data
{
public:
    Find_File_Data_PAX() : entry_count_(0), entry_index_(0), entry_offset_(0), current_time_(0)
    {
        entries_[0] = '\0';
        name_[0] = '\0';
        full_name_[0] = '\0';
        directory_[0] = '\0';
        pattern_[0] = '\0';
    }

    const char* GetFullName() const override
    {
        return name_[0] != '\0' ? full_name_ : nullptr;
    }

    const char* GetName() const override
    {
        return name_[0] != '\0' ? name_ : nullptr;
    }

    unsigned int GetTime() const override
    {
        return current_time_;
    }

    bool FindFirst(const char* filespec) override
    {
        Close();
        if (filespec == nullptr || pax_svc == nullptr || pax_svc->file_list_dir == nullptr) {
            return false;
        }

        const char* slash = strrchr(filespec, '/');
        const char* pattern = filespec;
        if (slash == nullptr) {
            if (snprintf(directory_, sizeof(directory_), "%s", PAX_RA_DATA_DIR) >= (int)sizeof(directory_)) {
                return false;
            }
        } else {
            size_t directory_length = (size_t)(slash - filespec);
            if (directory_length == 0) {
                directory_length = 1; // the filesystem root
            }
            if (directory_length >= sizeof(directory_)) {
                return false;
            }
            memcpy(directory_, filespec, directory_length);
            directory_[directory_length] = '\0';
            pattern = slash + 1;
        }
        if (*pattern == '\0' || strlen(pattern) >= sizeof(pattern_)) {
            return false;
        }
        strcpy(pattern_, pattern);

        entry_count_ = pax_svc->file_list_dir(directory_, entries_, sizeof(entries_));
        if (entry_count_ <= 0) {
            Close();
            return false;
        }
        return FindNextMatching();
    }

    bool FindNext() override
    {
        return FindNextMatching();
    }

    void Close() override
    {
        entry_count_ = entry_index_ = entry_offset_ = 0;
        current_time_ = 0;
        entries_[0] = '\0';
        name_[0] = '\0';
        full_name_[0] = '\0';
    }

private:
    bool FindNextMatching()
    {
        while (entry_index_ < entry_count_ && entry_offset_ < sizeof(entries_)) {
            const char* entry = entries_ + entry_offset_;
            const size_t remaining = sizeof(entries_) - entry_offset_;
            const size_t length = strnlen(entry, remaining);
            if (length == remaining || length == 0) {
                break;
            }
            entry_offset_ += length + 1;
            ++entry_index_;

            size_t name_length = length;
            if (name_length > 0 && entry[name_length - 1] == '/') {
                --name_length;
            }
            if (name_length == 0 || name_length >= sizeof(name_)) {
                continue;
            }
            memcpy(name_, entry, name_length);
            name_[name_length] = '\0';
            if (!wildcard_match_ci(pattern_, name_)) {
                continue;
            }
            const int written = snprintf(full_name_, sizeof(full_name_), "%s%s%s", directory_,
                                         directory_[strlen(directory_) - 1] == '/' ? "" : "/", name_);
            if (written < 0 || written >= (int)sizeof(full_name_)) {
                name_[0] = '\0';
                continue;
            }

            current_time_ = 0;
            if (pax_svc->file_stat != nullptr) {
                pax_file_stat_t stat = {};
                if (pax_svc->file_stat(full_name_, &stat) == 0 && stat.mtime > 0) {
                    current_time_ = (unsigned int)stat.mtime;
                }
            }
            return true;
        }
        name_[0] = '\0';
        full_name_[0] = '\0';
        current_time_ = 0;
        return false;
    }

    char entries_[8192];
    int entry_count_;
    int entry_index_;
    size_t entry_offset_;
    char directory_[512];
    char pattern_[256];
    char name_[256];
    char full_name_[768];
    unsigned int current_time_;
};

Find_File_Data* Find_File_Data::CreateFindData()
{
    return new Find_File_Data_PAX();
}
