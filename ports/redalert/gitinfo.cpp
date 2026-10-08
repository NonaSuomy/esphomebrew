// Stands in for Vanilla Conquer's generated common/gitinfo.cpp (CMake fills it
// from git). The PAX build pins the upstream commit in apps/redalert/pax.json.
#include "common/gitinfo.h"

const char GitSHA1[] = "pax";
const char GitShortSHA1[] = "pax";
const char GitCommitDate[] = "";
const char GitCommitAuthorName[] = "";
const char GitTag[] = "";
time_t GitCommitTimeStamp = 0;
bool GitUncommittedChanges = false;
bool GitHaveInfo = false;
int GitRevision = 0;
