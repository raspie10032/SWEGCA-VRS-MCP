#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int allow(int rs, const char *path) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) { perror(path); return -1; }
    struct landlock_path_beneath_attr a = {
        .allowed_access = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE,
        .parent_fd = fd
    };
    int ret = syscall(__NR_landlock_add_rule, rs, LANDLOCK_RULE_PATH_BENEATH, &a, 0);
    if (ret < 0) perror("landlock_add_rule");
    close(fd);
    return ret;
}
int main(int argc, char **argv) {
    const char *workspace = getenv("VRS22_WORKSPACE");
    const char *test_root = getenv("VRS22_TEST_ROOT");
    const char *python_root = getenv("VRS22_PYTHON_RUNTIME_ROOT");
    const char *snapshot_root = getenv("VRS22_SHELL_SNAPSHOT_ROOT");
    if (!workspace || !*workspace) { fprintf(stderr, "VRS22_WORKSPACE unset\n"); return 125; }
    if (!test_root || !*test_root) { fprintf(stderr, "VRS22_TEST_ROOT unset\n"); return 125; }
    if (!python_root || !*python_root) {
        fprintf(stderr, "VRS22_PYTHON_RUNTIME_ROOT unset\n"); return 125;
    }
    if (!snapshot_root || !*snapshot_root) {
        fprintf(stderr, "VRS22_SHELL_SNAPSHOT_ROOT unset\n"); return 125;
    }
    struct landlock_ruleset_attr rules = {
        .handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE
    };
    int rs = syscall(__NR_landlock_create_ruleset, &rules, sizeof(rules), 0);
    if (rs < 0) { perror("landlock_create_ruleset"); return 125; }
    const char *paths[] = {"/usr", "/usr/local/bin", "/etc", "/lib", "/lib64", "/bin", "/sbin", "/dev", "/proc", "/sys", "/run", "/tmp", workspace, test_root, python_root, snapshot_root, NULL};
    for (int i = 0; paths[i]; i++) if (allow(rs, paths[i]) < 0) return 125;
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) { perror("prctl"); return 125; }
    if (syscall(__NR_landlock_restrict_self, rs, 0) < 0) { perror("landlock_restrict_self"); return 125; }
    close(rs);
    char **cmd = calloc((size_t)argc + 1, sizeof(char*));
    if (!cmd) return 125;
    cmd[0] = "/usr/local/bin/vrs22-real-bash";
    for (int i = 1; i < argc; i++) cmd[i] = argv[i];
    execv(cmd[0], cmd);
    perror("execv real bash");
    return 125;
}
