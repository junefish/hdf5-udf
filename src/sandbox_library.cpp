/*
 * HDF5-UDF: User-Defined Functions for HDF5
 *
 * File: sandbox_library.cpp
 *
 * Seccomp does not dereference pointers, so string-based rules are a no-go.
 * This file provides wrappers for system calls whose string-based arguments
 * we want to check (LD_PRELOAD-style).
 *
 * Note that this code is compiled into a shared library. The syscall_intercept
 * library modifies the resulting shared library's constructor so that Glibc
 * system calls can be intercepted and their arguments checked by ourselves.
 */
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <glob.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <seccomp.h>
#include <syscall.h>
#include <libsyscall_intercept_hook_point.h>
#include <algorithm>
#include <vector>
#include <string>

// List of files allowed to be accessed by the UDF. Wildcards are allowed.
static std::vector<std::string> files_allowed {
    "/etc/resolv.conf",    
};

extern "C" {

////////////////////////////////
// syscall-intercept interface
////////////////////////////////

static void __attribute__((unused)) print_syscall(std::string name, long arg)
{
    char *path = (char *) arg;
    syscall_no_intercept(SYS_write, 2, name.c_str(), name.size());
    syscall_no_intercept(SYS_write, 2, " ", 1);
    syscall_no_intercept(SYS_write, 2, path, strlen(path));
    syscall_no_intercept(SYS_write, 2, "\n", 1);   
}

static int syscall_intercept(
    long syscall_nr,
    long arg0, long arg1, long arg2, long arg3, long arg4, long arg5,
    long *ret)
{
    (void) arg2;
    (void) arg3;
    (void) arg4;
    (void) arg5;
 
    auto test_file_ok = [&](long arg)
    {
        char *path = (char *) arg;
        for (auto &p: files_allowed)
            if (p.compare(path) == 0)
                return 1;
        *ret = -EPERM;
        return 0;
    };
 
    // A non-zero return value returned by the callback function is used to signal
    // to the intercepting library that the specific system call was ignored by the
    // user and the original syscall should be executed. A zero return value signals
    // that the user takes over the system call. 
    switch (syscall_nr)
    {
        case SYS_stat:
        case SYS_lstat:
        case SYS_open:
            return test_file_ok(arg0);
        case SYS_openat:
            return test_file_ok(arg1);
        case SYS_fstat:
        default:
            return 1;
    }
}

static __attribute__((constructor)) void syscall_intercept_init()
{
    std::vector<std::string> files(files_allowed.begin(), files_allowed.end());
    files_allowed.clear();

    // Resolve wildcards in files[]
    for (auto &path: files)
    {
        // Update list of files the program is allowed to open
        if (path.find("*") == std::string::npos)
            files_allowed.push_back(path);
        else
        {
            glob_t globbuf;
            if (glob(path.c_str(), GLOB_NOSORT, NULL, &globbuf) != 0)
                continue;
            for (size_t n=0; n < globbuf.gl_pathc; ++n)
                files_allowed.push_back(globbuf.gl_pathv[n]);
            globfree(&globbuf);
        }
    }

    // Set up our system call hook
    intercept_hook_point = &syscall_intercept;
}


////////////////////////////////////
// System call filtering interface
////////////////////////////////////

#define ALLOW(syscall, ...) do { \
    if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(syscall), __VA_ARGS__) < 0) { \
        fprintf(stderr, "Failed to configure seccomp rule for '" #syscall "' syscall\n"); \
        return false; \
    } \
} while (0)

bool syscall_filter_init()
{
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL_PROCESS);

    // One particular use case of HDF5-UDF is to retrieve data
    // from servers exposed on the Internet and to provide that
    // data to the application using the HDF5 dataset interface.
    // The syscalls below should be sufficient to allow a program
    // to connect to an external host and communicate with it.
    // Any other syscall is denied by seccomp and will cause the
    // UDF thread to be killed.

    // Fundamental system calls we want to allow
    ALLOW(brk, 0);
    ALLOW(exit_group, 0);

    // Sockets-related system calls
    ALLOW(socket, 0);
    ALLOW(setsockopt, 0);
    ALLOW(ioctl, 1, SCMP_A1(SCMP_CMP_EQ, FIONREAD));
    ALLOW(connect, 0);
    ALLOW(select, 0);
    ALLOW(poll, 0);
    ALLOW(read, 0);
    ALLOW(recv, 0);
    ALLOW(recvfrom, 0);
    ALLOW(write, 0);
    ALLOW(send, 0);
    ALLOW(sendto, 0);
    ALLOW(sendmsg, 0);
    ALLOW(close, 0);

    // System calls issued by gethostbyname(). Some of these could be potentially
    // misused by malicious user-defined functions; we rely on the syscall-intercept
    // routines above to check their string-based arguments to decide to allow or
    // reject them.
    ALLOW(stat, 0);
    ALLOW(lstat, 0);
    ALLOW(fstat, 0);
    ALLOW(fstat64, 0);
    ALLOW(open, 1, SCMP_A1(SCMP_CMP_MASKED_EQ, O_RDONLY, O_RDONLY));
    ALLOW(openat, 1, SCMP_A2(SCMP_CMP_MASKED_EQ, O_RDONLY, O_RDONLY));
    ALLOW(mmap, 0);
    ALLOW(mmap2, 0);
    ALLOW(munmap, 0);
    ALLOW(lseek, 0);
    ALLOW(_llseek, 0);
    ALLOW(futex, 0);
    ALLOW(uname, 0);
    ALLOW(mprotect, 0);    

    // Load seccomp rules
    int ret = seccomp_load(ctx);
    seccomp_release(ctx);
    if (ret < 0)
        fprintf(stderr, "Failed to load seccomp filter: %s\n", strerror(errno));

    return ret == 0;
}

} // extern "C"