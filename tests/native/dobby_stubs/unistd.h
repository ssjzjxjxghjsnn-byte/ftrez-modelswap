#pragma once
// Host-only seams for the actual Android/Linux DobbyCodePatch implementation.
#define _SC_PAGESIZE 30
#define sysconf dobby_test_sysconf
long dobby_test_sysconf(int name);
