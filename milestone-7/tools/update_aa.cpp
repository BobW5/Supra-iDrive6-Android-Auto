// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
//
// update_aa: the whole manual update (updating.md steps 2-6) as one command on the head unit, so no SSH console
// is needed to run it. Put update_aa next to the release files on the flash drive and run:
//
//   update_aa /mnt/umassXXXXX
//
// It expects the release layout on the stick (the same one install.md / updating.md use):
//   <stick>/nbt-aad                       new daemon
//   <stick>/pf_aa                         new firewall installer (run as <stick>/pf_aa --install --check)
//   <stick>/Releases/                     plugin files (copied into /opt/hmi/ID5/data/ro/Plugins/NbtAad)
//
// What it does, in order (every step verifies its own result before the next):
//   1. stop a running nbt-aad (pidin -> kill): the "Force Stop" of updating.md step 2, no iDrive needed.
//   2. move /opt/aa/nbt-aad to /opt/aa/nbt-aad.old (the rollback copy, same as by hand), copy the new one in,
//      chmod +x.
//   3. move the whole plugin folder out of Plugins/ to /opt/aa/NbtAad.old and copy the new files in - the folder
//      MUST not stay under Plugins/, iDrive would try to load two apps.
//   4. run the stick's pf_aa: --install then --check.
//   5. reboot the head unit so the new plugin is loaded and /fs/sda0 is read-only again (step 6 of updating.md,
//      normally "hold the power button 30 seconds").
//
// Rollback, if something goes wrong: exactly the by-hand one (updating.md "Roll back"), then reboot. Nothing in
// this tool is new: every file move is the same move the manual guide makes. The .old copies are your undo button.
//
// Build: added to the Makefile's `all` target like the other tools (qcc, no QNX-specific includes).
//
// No stick path given: it looks for the release layout on any mounted flash drive (/mnt/umass*, the same
// search the daemon's USB logging uses, src/usb_log.hpp), which is how the iDrive menu row "Update from USB"
// starts it - /opt/aa/update_aa /opt/aa/update-usb.log, no argument.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace {

// All the fixed paths (the car layout from updating.md).
const char* const kAA       = "/opt/aa";
const char* const kDaemon   = "/opt/aa/nbt-aad";
const char* const kPlugin   = "/opt/hmi/ID5/data/ro/Plugins/NbtAad";
const char* const kPluginOld = "/opt/aa/NbtAad.old";
const char* const kDaemonOld = "/opt/aa/nbt-aad.old";

int gFails = 0;
bool gNoReboot = false;

// The five plugin files updating.md says to verify (step 4): all present in the new folder.
const char* const kPluginFiles[5] = {
    "libNbtAad.so", "NbtAad.manifest", "NbtAad.xml",
    "NbtAad_ALL_Texts.zip", "NbtAad_ALL_Images.zip",
};

void note(const char* what) { printf("  [update_aa] %s\n", what); }
void ok(const char* what)   { printf("  [update_aa] OK: %s\n", what); }
void fail(const char* what) { printf("  [update_aa] FAIL: %s\n", what); ++gFails; }

// Progress marker the iDrive app watches while the update runs (the "Update from USB" row, below): the last line
// in /dev/shmem/update-aa-status before the reboot. Written after every step; not needed when the tool is run by
// hand over ssh.
void status(const char* s) {
    FILE* f = fopen("/dev/shmem/update-aa-status", "w");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
    note(s);
}

// Run a command, show its output, return the exit code (-1: it did not run).
int sh(const char* cmd) {
    note(cmd);
    int rc = system(cmd);
    if (rc == -1) { fail("could not run: " cmd); return -1; }
    return WEXITSTATUS(rc);
}

// Copy <src> (a file) onto <dst> and keep it executable - the cp/chmod +x of updating.md step 3, with the
// result reported so a half-copied binary cannot be mistaken for a good one.
int copyFile(const char* src, const char* dst) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "cp -f %s %s && chmod +x %s", src, dst, dst);
    int rc = sh(cmd);
    if (rc != 0) { fail(cmd); return rc; }
    ok("copied " src);
    return 0;
}

// ---- step 2: stop nbt-aad -------------------------------------------------
// pidin lists the processes; nbt-aad's pid is on its line. The manual guide does the same by eye (updating.md
// step 2) and kills it; here we just don't need iDrive to "Force Stop" first. The parsing is done in C, the same
// way main.cpp finds HmiMain3D's pid: the host shell on the NBT Evo has no awk.
int stopDaemon() {
    status("stopping nbt-aad");
    FILE* p = popen("pidin arg 2>/dev/null", "r");
    if (!p) { fail("could not list processes (pidin)"); return 1; }
    char pids[256] = "";
    char line[512];
    while (fgets(line, sizeof line, p)) {
        int pid = atoi(line);
        if (pid > 0 && (strstr(line, "/nbt-aad") || strstr(line, "nbt-aad "))) {
            char t[20];
            int n = snprintf(t, sizeof t, "%d ", pid);
            if ((int)strlen(pids) + n < 255) strcat(pids, t);
        }
    }
    pclose(p);
    pids[strcspn(pids, " ")] = 0;
    if (!*pids) { note("nbt-aad is not running"); return 0; }
    char cmd[512];
    snprintf(cmd, sizeof cmd, "kill %s", pids);
    int rc = sh(cmd);
    if (rc != 0) { fail("could not stop nbt-aad: " pids); return 1; }
    // The process table is not instant on QNX: give it a moment so step 3's move is not racing a live binary.
    sleep(2);
    ok("stopped nbt-aad");
    return 0;
}

// ---- step 3: swap the daemon ----------------------------------------------
int swapDaemon(const char* stick) {
    status("swapping the daemon");
    char src[512];
    snprintf(src, sizeof src, "%s/nbt-aad", stick);
    if (!access(src, X_OK)) { fail(src); return 1; }

    note("moving the old daemon to its rollback copy");
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -rf %s; mv %s %s", kDaemonOld, kDaemon, kDaemonOld);
        if (sh(cmd)) { fail("could not move " kDaemon); return 1; }
    }
    if (copyFile(src, kDaemon)) return 1;
    ok("daemon updated");
    return 0;
}

// ---- step 4: swap the plugin ----------------------------------------------
int swapPlugin(const char* stick) {
    status("swapping the iDrive plugin");
    char rel[512];
    snprintf(rel, sizeof rel, "%s/Releases", stick);
    // Five files, one check - the ls -l verification of updating.md step 4.
    int missing = 0;
    for (int i = 0; i < 5; ++i) {
        char p[600];
        snprintf(p, sizeof p, "%s/%s", rel, kPluginFiles[i]);
        if (access(p, R_OK)) { fail(p); ++missing; }
    }
    if (missing) { fail("the stick's Releases/ is missing plugin files"); return 1; }

    note("moving the old plugin folder out of Plugins/ (iDrive loads it by the folder)");
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "rm -rf %s; mv %s %s; mkdir -p %s", kPluginOld, kPlugin, kPluginOld, kPlugin);
        sh(cmd);   // first update: the folder is absent, mv fails, mkdir makes it
    }
    {
        char cmd[512];
        snprintf(cmd, sizeof cmd, "cp %s/Releases/* %s/", stick, kPlugin);
        if (sh(cmd)) { fail("plugin files did not copy"); return 1; }
    }
    // Verify the five files landed - the same verification the manual guide does by eye.
    missing = 0;
    for (int i = 0; i < 5; ++i) {
        char p[600];
        snprintf(p, sizeof p, "%s/%s", kPlugin, kPluginFiles[i]);
        if (access(p, R_OK)) { fail(p); ++missing; }
    }
    if (missing) { fail("plugin folder is incomplete after the copy"); return 1; }
    ok("plugin updated");
    return 0;
}

// ---- step 5: firewall rules ------------------------------------------------
int firewall(const char* stick) {
    status("installing the firewall rules (pf_aa)");
    char cmd[512];
    snprintf(cmd, sizeof cmd, "%s/pf_aa --install && %s/pf_aa --check", stick, stick);
    if (sh(cmd) != 0) { fail("pf_aa did not complete: the firewall rules may be missing"); return 1; }
    ok("firewall rules installed");
    return 0;
}

// ---- step 6: reboot --------------------------------------------------------
// The manual guide reboots by holding the power button 30 s. This is the software equivalent; if the car
// ignores it, the hardware switch still works as before.
int rebootNow() {
    if (gNoReboot) { status("done - files in place, reboot by hand"); return 0; }
    status("rebooting the head unit");
    sh("reboot 2>/dev/null || /sbin/reboot 2>/dev/null || echo 'no reboot command: hold the power button 30 s'");
    // The unit is going down; give the last lines a moment to flush.
    sleep(2);
    return 0;
}

// ---- flash drive detection ------------------------------------------------
// The release files can live anywhere on a mounted stick. iDrive mounts sticks as /mnt/umass<...> - the same
// tree the daemon's USB logging scans (src/usb_log.hpp) - so we search there. The search is shallow: every
// directory up to two levels below the stick root.
bool stickHasRelease(const char* root) {
    // <root>/nbt-aad must exist and be executable; Releases/ next to it.
    char p[512];
    snprintf(p, sizeof p, "%s/nbt-aad", root);
    if (access(p, X_OK)) return false;
    snprintf(p, sizeof p, "%s/Releases", root);
    if (access(p, R_OK)) return false;
    return true;
}

// First /mnt/umass* that carries the release layout; its full path in out. root defaults to "/mnt".
bool findReleaseStick(char* out, size_t n, const char* root = "/mnt") {
    DIR* d = opendir(root);
    if (!d) return false;
    struct dirent* e;
    bool found = false;
    while ((e = readdir(d)) != NULL && !found) {
        if (strncmp(e->d_name, "umass", 5) != 0) continue;
        char sub[512];
        snprintf(sub, sizeof sub, "%s/%s", root, e->d_name);
        if (stickHasRelease(sub)) {
            snprintf(out, n, "%s", sub);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

int usage() {
    printf("usage: update_aa [flash-drive-mount | --no-reboot | --help]\n"
           "  e.g. update_aa /mnt/umass01200\n"
           "  no mount: the release is looked up on any mounted stick (/mnt/umass*)\n"
           "options:\n"
           "  --no-reboot    stop after the files are in place (reboot by hand)\n"
           "  --help         this text\n");
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    const char* stick = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--no-reboot")) gNoReboot = true;
        else if (!strcmp(argv[i], "--help")) return usage();
        else if (stick) { fail("extra argument"); return usage(); }
        else if (argv[i][0] != '/') { fail("stick path must start with /"); return usage(); }
        else stick = argv[i];
    }
    if (!stick) {
        // No mount given: look for the release layout on any mounted stick - the iDrive menu row starts it this
        // way (/opt/aa/update_aa, no argument).
        char found[512];
        if (!findReleaseStick(found, sizeof found)) {
            printf("[update_aa] no flash drive with the release files (nbt-aad + Releases/ + pf_aa) on /mnt/umass*:\n"
                   "plug in the stick with the new release, or pass the mount: update_aa /mnt/umassXXXXX\n");
            return 1;
        }
        stick = found;
        printf("[update_aa] release found on %s\n", stick);
    }
    if (access(stick, R_OK) != 0) {
        printf("[update_aa] no such flash drive: %s (ls /mnt to find the umass* one)\n", stick);
        return 1;
    }

    printf("[update_aa] updating Android Auto from %s\n", stick);
    stopDaemon();
    swapDaemon(stick);
    swapPlugin(stick);
    firewall(stick);
    rebootNow();

    if (gFails) {
        printf("[update_aa] %d step(s) FAILED - the .old copies are the rollback (updating.md 'Roll back'):\n"
               "  mv %s %s\n"
               "  mv %s %s\n"
               "then reboot.\n",
               gFails, kDaemon, kDaemonOld, kPlugin, kPluginOld);
        return 1;
    }
    printf("[update_aa] done%s: Android Auto is updated.\n", gNoReboot ? ", reboot when ready" : "");
    return 0;
}
