//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//

#include "include/target_conditionals.h"
#include "include/process_shims.h"

#if !TARGET_OS_WINDOWS
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <grp.h>
#include <signal.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>

#include <stdio.h>

#if __has_include(<crt_externs.h>)
#include <crt_externs.h>
#elif defined(_WIN32)
#include <stdlib.h>
#elif __has_include(<unistd.h>)
#include <unistd.h>
extern char **environ;
#endif

int _was_process_exited(int status) {
    return WIFEXITED(status);
}

int _get_exit_code(int status) {
    return WEXITSTATUS(status);
}

int _was_process_signaled(int status) {
    return WIFSIGNALED(status);
}

int _get_signal_code(int status) {
    return WTERMSIG(status);
}

int _was_process_suspended(int status) {
    return WIFSTOPPED(status);
}

#if TARGET_OS_LINUX
#include <stdio.h>

int _shims_snprintf(
    char * _Nonnull str,
    int len,
    const char * _Nonnull format,
    char * _Nonnull str1,
    char * _Nonnull str2
) {
    return snprintf(str, len, format, str1, str2);
}
#endif

// MARK: - Darwin (posix_spawn)
#if TARGET_OS_MAC
static int _subprocess_spawn_prefork(
    pid_t  * _Nonnull  pid,
    const char  * _Nonnull  exec_path,
    const posix_spawn_file_actions_t _Nullable * _Nonnull file_actions,
    const posix_spawnattr_t _Nullable * _Nonnull spawn_attrs,
    char * _Nullable const args[_Nonnull],
    char * _Nullable const env[_Nullable],
    uid_t * _Nullable uid,
    gid_t * _Nullable gid,
    int number_of_sgroups, const gid_t * _Nullable sgroups,
    int create_session
) {
    // Set `POSIX_SPAWN_SETEXEC` flag since we are forking ourselves
    short flags = 0;
    int rc = posix_spawnattr_getflags(spawn_attrs, &flags);
    if (rc != 0) {
        return rc;
    }

    rc = posix_spawnattr_setflags(
        (posix_spawnattr_t *)spawn_attrs, flags | POSIX_SPAWN_SETEXEC
    );
    if (rc != 0) {
        return rc;
    }
    // Setup pipe to catch exec failures from child
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return errno;
    }
    // Set FD_CLOEXEC so the pipe is automatically closed when exec succeeds
    flags = fcntl(pipefd[0], F_GETFD);
    if (flags == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return errno;
    }
    flags |= FD_CLOEXEC;
    if (fcntl(pipefd[0], F_SETFD, flags) == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return errno;
    }

    flags = fcntl(pipefd[1], F_GETFD);
    if (flags == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return errno;
    }
    flags |= FD_CLOEXEC;
    if (fcntl(pipefd[1], F_SETFD, flags) == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return errno;
    }

    // Finally, fork
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated"
    pid_t childPid = fork();
#pragma GCC diagnostic pop
    if (childPid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return errno;
    }

    if (childPid == 0) {
        // Child process
        close(pipefd[0]);  // Close unused read end

        // Perform setups
        if (number_of_sgroups > 0 && sgroups != NULL) {
            if (setgroups(number_of_sgroups, sgroups) != 0) {
                return errno;
            }
        }

        if (uid != NULL) {
            if (setuid(*uid) != 0) {
                return errno;
            }
        }

        if (gid != NULL) {
            if (setgid(*gid) != 0) {
                return errno;
            }
        }

        if (create_session != 0) {
            (void)setsid();
        }

        // Use posix_spawnas exec
        int error = posix_spawn(pid, exec_path, file_actions, spawn_attrs, args, env);
        // If we reached this point, something went wrong
        write(pipefd[1], &error, sizeof(error));
        close(pipefd[1]);
        _exit(EXIT_FAILURE);
    } else {
        // Parent process
        close(pipefd[1]);  // Close unused write end
        // Communicate child pid back
        *pid = childPid;
        // Read from the pipe until pipe is closed
        // Eitehr due to exec succeeds or error is written
        int childError = 0;
        if (read(pipefd[0], &childError, sizeof(childError)) > 0) {
            // We encountered error
            close(pipefd[0]);
            return childError;
        } else {
            // Child process exec was successful
            close(pipefd[0]);
            return 0;
        }
    }
}

int _subprocess_spawn(
    pid_t  * _Nonnull  pid,
    const char  * _Nonnull  exec_path,
    const posix_spawn_file_actions_t _Nullable * _Nonnull file_actions,
    const posix_spawnattr_t _Nullable * _Nonnull spawn_attrs,
    char * _Nullable const args[_Nonnull],
    char * _Nullable const env[_Nullable],
    uid_t * _Nullable uid,
    gid_t * _Nullable gid,
    int number_of_sgroups, const gid_t * _Nullable sgroups,
    int create_session
) {
    int require_pre_fork = uid != NULL ||
        gid != NULL ||
        number_of_sgroups > 0 ||
        create_session > 0;

    if (require_pre_fork != 0) {
        int rc = _subprocess_spawn_prefork(
            pid,
            exec_path,
            file_actions, spawn_attrs,
            args, env,
            uid, gid, number_of_sgroups, sgroups, create_session
        );
        return rc;
    }

    // Spawn
    return posix_spawn(pid, exec_path, file_actions, spawn_attrs, args, env);
}

#endif // TARGET_OS_MAC

// MARK: - Linux (fork/exec + posix_spawn fallback)

#if _POSIX_SPAWN
static int _subprocess_posix_spawn_fallback(
    pid_t * _Nonnull pid,
    const char * _Nonnull exec_path,
    const char * _Nullable working_directory,
    const int file_descriptors[_Nonnull],
    char * _Nullable const args[_Nonnull],
    char * _Nullable const env[_Nullable],
    gid_t * _Nullable process_group_id
) {
    // Setup stdin, stdout, and stderr
    posix_spawn_file_actions_t file_actions;

    int rc = posix_spawn_file_actions_init(&file_actions);
    if (rc != 0) { return rc; }
    if (file_descriptors[0] >= 0) {
        rc = posix_spawn_file_actions_adddup2(
            &file_actions, file_descriptors[0], STDIN_FILENO
        );
        if (rc != 0) { return rc; }
    }
    if (file_descriptors[2] >= 0) {
        rc = posix_spawn_file_actions_adddup2(
            &file_actions, file_descriptors[2], STDOUT_FILENO
        );
        if (rc != 0) { return rc; }
    }
    if (file_descriptors[4] >= 0) {
        rc = posix_spawn_file_actions_adddup2(
            &file_actions, file_descriptors[4], STDERR_FILENO
        );
        if (rc != 0) { return rc; }
    }

    // Close parent side
    if (file_descriptors[1] >= 0) {
        rc = posix_spawn_file_actions_addclose(&file_actions, file_descriptors[1]);
        if (rc != 0) { return rc; }
    }
    if (file_descriptors[3] >= 0) {
        rc = posix_spawn_file_actions_addclose(&file_actions, file_descriptors[3]);
        if (rc != 0) { return rc; }
    }
    if (file_descriptors[5] >= 0) {
        rc = posix_spawn_file_actions_addclose(&file_actions, file_descriptors[5]);
        if (rc != 0) { return rc; }
    }

    // Setup spawnattr
    posix_spawnattr_t spawn_attr;
    rc = posix_spawnattr_init(&spawn_attr);
    if (rc != 0) { return rc; }
    // Masks
    sigset_t no_signals;
    sigset_t all_signals;
    sigemptyset(&no_signals);
    sigfillset(&all_signals);
    rc = posix_spawnattr_setsigmask(&spawn_attr, &no_signals);
    if (rc != 0) { return rc; }
    rc = posix_spawnattr_setsigdefault(&spawn_attr, &all_signals);
    if (rc != 0) { return rc; }
    // Flags
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (process_group_id != NULL) {
        flags |= POSIX_SPAWN_SETPGROUP;
        rc = posix_spawnattr_setpgroup(&spawn_attr, *process_group_id);
        if (rc != 0) { return rc; }
    }
    rc = posix_spawnattr_setflags(&spawn_attr, flags);

    // Spawn!
    rc = posix_spawn(
        pid, exec_path,
        &file_actions, &spawn_attr,
        args, env
    );
    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&spawn_attr);
    return rc;
}
#endif // _POSIX_SPAWN

int _subprocess_fork_exec(
    pid_t * _Nonnull pid,
    const char * _Nonnull exec_path,
    const char * _Nullable working_directory,
    const int file_descriptors[_Nonnull],
    char * _Nullable const args[_Nonnull],
    char * _Nullable const env[_Nullable],
    uid_t * _Nullable uid,
    gid_t * _Nullable gid,
    gid_t * _Nullable process_group_id,
    int number_of_sgroups, const gid_t * _Nullable sgroups,
    int create_session,
    void (* _Nullable configurator)(void)
) {
    int require_pre_fork = working_directory != NULL ||
        uid != NULL ||
        gid != NULL ||
        process_group_id != NULL ||
        (number_of_sgroups > 0 && sgroups != NULL) ||
        create_session ||
        configurator != NULL;

#if _POSIX_SPAWN
    // If posix_spawn is available on this platform and
    // we do not require prefork, use posix_spawn if possible.
    //
    // (Glibc's posix_spawn does not support
    // `POSIX_SPAWN_SETEXEC` therefore we have to keep
    // using fork/exec if `require_pre_fork` is true.
    if (require_pre_fork == 0) {
        return _subprocess_posix_spawn_fallback(
            pid, exec_path,
            working_directory,
            file_descriptors,
            args, env,
            process_group_id
        );
    }
#endif

    pid_t child_pid = fork();
    if (child_pid != 0) {
        *pid = child_pid;
        return child_pid < 0 ? errno : 0;
    }

    if (working_directory != NULL) {
        if (chdir(working_directory) != 0) {
            return errno;
        }
    }


    if (uid != NULL) {
        if (setuid(*uid) != 0) {
            return errno;
        }
    }

    if (gid != NULL) {
        if (setgid(*gid) != 0) {
            return errno;
        }
    }

    if (number_of_sgroups > 0 && sgroups != NULL) {
        if (setgroups(number_of_sgroups, sgroups) != 0) {
            return errno;
        }
    }

    if (create_session != 0) {
        (void)setsid();
    }

    if (process_group_id != NULL) {
        (void)setpgid(0, *process_group_id);
    }

    // Bind stdin, stdout, and stderr
    int rc = 0;
    if (file_descriptors[0] >= 0) {
        rc = dup2(file_descriptors[0], STDIN_FILENO);
        if (rc < 0) { return errno; }
    }
    if (file_descriptors[2] >= 0) {
        rc = dup2(file_descriptors[2], STDOUT_FILENO);
        if (rc < 0) { return errno; }
    }
    if (file_descriptors[4] >= 0) {
        rc = dup2(file_descriptors[4], STDERR_FILENO);
        if (rc < 0) { return errno; }
    }
    // Close parent side
    if (file_descriptors[1] >= 0) {
        rc = close(file_descriptors[1]);
    }
    if (file_descriptors[3] >= 0) {
        rc = close(file_descriptors[3]);
    }
    if (file_descriptors[4] >= 0) {
        rc = close(file_descriptors[5]);
    }
    if (rc != 0) {
        return errno;
    }
    // Run custom configuratior
    if (configurator != NULL) {
        configurator();
    }
    // Finally, exec
    execve(exec_path, args, env);
    // If we got here, something went wrong
    return errno;
}

#endif // !TARGET_OS_WINDOWS

#pragma mark - Environment Locking

#if __has_include(<libc_private.h>)
#import <libc_private.h>
void _subprocess_lock_environ(void) {
    environ_lock_np();
}

void _subprocess_unlock_environ(void) {
    environ_unlock_np();
}
#else
void _subprocess_lock_environ(void) { /* noop */ }
void _subprocess_unlock_environ(void) { /* noop */ }
#endif

char ** _subprocess_get_environ(void) {
#if __has_include(<crt_externs.h>)
    return *_NSGetEnviron();
#elif defined(_WIN32)
#include <stdlib.h>
    return _environ;
#elif TARGET_OS_WASI
    return __wasilibc_get_environ();
#elif __has_include(<unistd.h>)
    return environ;
#endif
}


#if TARGET_OS_WINDOWS

#include <windows.h>

typedef struct {
    DWORD pid;
    HWND mainWindow;
} CallbackContext;

static BOOL CALLBACK enumWindowsCallback(
    HWND hwnd,
    LPARAM lParam
) {
    CallbackContext *context = (CallbackContext *)lParam;
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == context->pid) {
        context->mainWindow = hwnd;
        return FALSE; // Stop enumeration
    }
    return TRUE; // Continue enumeration
}

BOOL _subprocess_windows_send_vm_close(
    DWORD pid
) {
    // First attempt to find the Window associate
    // with this process
    CallbackContext context = {0};
    context.pid = pid;
    EnumWindows(enumWindowsCallback, (LPARAM)&context);

    if (context.mainWindow != NULL) {
        if (SendMessage(context.mainWindow, WM_CLOSE, 0, 0)) {
            return TRUE;
        }
    }

    return FALSE;
}

#endif

