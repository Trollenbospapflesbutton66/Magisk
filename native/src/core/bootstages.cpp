#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <linux/input.h>
#include <libgen.h>
#include <set>
#include <string>

#include <magisk.hpp>
#include <db.hpp>
#include <base.hpp>
#include <daemon.hpp>
#include <resetprop.hpp>
#include <selinux.hpp>

#include "core.hpp"

using namespace std;

// Boot stage state
enum : int {
    FLAG_NONE = 0,
    FLAG_POST_FS_DATA_DONE = (1 << 0),
    FLAG_LATE_START_DONE = (1 << 1),
    FLAG_BOOT_COMPLETE = (1 << 2),
    FLAG_SAFE_MODE = (1 << 3),
};

static int boot_state = FLAG_NONE;

bool zygisk_enabled = false;

/*********
 * Setup *
 *********/

static bool mount_mirror(const std::string_view from, const std::string_view to) {
    return !xmkdirs(to.data(), 0755) &&
           // recursively bind mount to mirror dir, rootfs will fail before 3.12 kernel
           // because of MS_NOUSER
           !mount(from.data(), to.data(), nullptr, MS_BIND | MS_REC, nullptr) &&
           // make mirror dir as a private mount so that it won't be affected by magic mount
           !xmount(nullptr, to.data(), nullptr, MS_PRIVATE | MS_REC, nullptr);
}

static void mount_mirrors() {
    LOGI("* Mounting mirrors\n");
    auto self_mount_info = parse_mount_info("self");

    // Bind remount module root to clear nosuid
    if (access(SECURE_DIR, F_OK) == 0 || SDK_INT < 24) {
        auto dest = MAGISKTMP + "/" MODULEMNT;
        xmkdir(SECURE_DIR, 0700);
        xmkdir(MODULEROOT, 0755);
        xmkdir(dest.data(), 0755);
        xmount(MODULEROOT, dest.data(), nullptr, MS_BIND, nullptr);
        xmount(nullptr, dest.data(), nullptr, MS_REMOUNT | MS_BIND | MS_NOATIME, nullptr);
        xmount(nullptr, dest.data(), nullptr, MS_PRIVATE, nullptr);
        chmod(SECURE_DIR, 0700);
        restorecon();
    }

    // check and mount sepolicy.rules
    {
        dev_t rules_dev;
        auto rules = MAGISKTMP + "/" BLOCKDIR "/rules";
        if (struct stat st{}; stat(rules.data(), &st) == 0 && (st.st_mode & S_IFBLK)) {
            rules_dev = st.st_rdev;
        } else {
            // install from recovery, find now
            // this helps Magisk app to copy sepolicy.rules when fixing environment
            rules_dev = find_rules_device(self_mount_info);
        }

        for (const auto &info: self_mount_info) {
            if (info.root == "/" && info.device == rules_dev) {
                auto flags = split_ro(info.fs_option, ",");
                auto rw = std::any_of(flags.begin(), flags.end(), [](const auto &flag) {
                    return flag == "rw"sv;
                });
                if (!rw) continue;
                string custom_rules_dir = find_rules_dir(info.target.data());
                xmkdir(custom_rules_dir.data(), 0700);
                auto rules_dir = MAGISKTMP + "/" RULESDIR;
                mount_mirror(custom_rules_dir, rules_dir);
                break;
            }
        }
    }

    // Prepare worker
    auto worker_dir = MAGISKTMP + "/" WORKERDIR;
    xmount("worker", worker_dir.data(), "tmpfs", 0, "mode=755");
    xmount(nullptr, worker_dir.data(), nullptr, MS_PRIVATE, nullptr);

    // Recursively bind mount / to mirror dir
    if (auto mirror_dir = MAGISKTMP + "/" MIRRDIR; !mount_mirror("/", mirror_dir)) {
        LOGI("fallback to mount subtree\n");
        // rootfs may fail, fallback to bind mount each mount point
        set<string, greater<>> mounted_dirs {{ MAGISKTMP }};
        for (const auto &info: self_mount_info) {
            if (info.type == "rootfs"sv) continue;
            // the greatest mount point that less than info.target, which is possibly a parent
            if (auto last_mount = mounted_dirs.upper_bound(info.target);
                last_mount != mounted_dirs.end() && info.target.starts_with(*last_mount + '/')) {
                continue;
            }
            if (mount_mirror(info.target, mirror_dir + info.target)) {
                LOGD("%-8s: %s <- %s\n", "rbind", (mirror_dir + info.target).data(), info.target.data());
                mounted_dirs.insert(info.target);
            }
        }
    }
}

dev_t find_rules_device(const std::vector<mount_info> &infos) {
    const int UNKNOWN = 0;
    const int PERSIST = 1;
    const int METADATA = 2;
    const int CACHE = 3;
    const int DATA = 4;
    int matched = UNKNOWN;
    dev_t rules_dev = 0;
    bool encrypted = getprop("ro.crypto.state") == "encrypted";

    for (const auto &info: infos) {
        if (info.target.ends_with(RULESDIR))
            return info.device;
        if (info.root != "/" || info.source.find("/dm-") != string::npos)
            continue;
        if (info.type != "ext4" && info.type != "f2fs")
            continue;
        auto flags = split_ro(info.fs_option, ",");
        auto rw = std::any_of(flags.begin(), flags.end(), [](const auto &flag) {
            return flag == "rw"sv;
        });
        if (!rw) continue;
        int new_matched;
        if (info.target == "/cache" && matched < CACHE) {
            new_matched = CACHE;
        } else if (info.target == "/data" && matched < DATA) {
            if (encrypted && access("/data/unencrypted", F_OK)) {
                continue;
            } else {
                new_matched = DATA;
            }
        } else if (info.target == "/metadata" && matched < METADATA) {
            new_matched = METADATA;
        } else if ((info.target == "/persist" || info.target == "/mnt/vendor/persist") &&
                   matched < PERSIST) {
            new_matched = PERSIST;
        } else continue;

        rules_dev = info.device;
        matched = new_matched;
    }
    return rules_dev;
}

static bool magisk_env() {
    char buf[4096];

    LOGI("* Initializing Magisk environment\n");

    preserve_stub_apk();
    string pkg;
    get_manager(0, &pkg);

    ssprintf(buf, sizeof(buf), "%s/0/%s/install", APP_DATA_DIR,
            pkg.empty() ? "xxx" /* Ensure non-exist path */ : pkg.data());

    // Alternative binaries paths
    const char *alt_bin[] = { "/cache/data_adb/magisk", "/data/magisk", buf };
    for (auto alt : alt_bin) {
        struct stat st{};
        if (lstat(alt, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                unlink(alt);
                continue;
            }
            rm_rf(DATABIN);
            cp_afc(alt, DATABIN);
            rm_rf(alt);
            break;
        }
    }
    rm_rf("/cache/data_adb");

    // Directories in /data/adb
    xmkdir(DATABIN, 0755);
    xmkdir(SECURE_DIR "/post-fs-data.d", 0755);
    xmkdir(SECURE_DIR "/service.d", 0755);

    restore_databincon();

    if (access(DATABIN "/busybox", X_OK))
        return false;

    sprintf(buf, "%s/" BBPATH "/busybox", MAGISKTMP.data());
    mkdir(dirname(buf), 0755);
    cp_afc(DATABIN "/busybox", buf);
    exec_command_async(buf, "--install", "-s", dirname(buf));

    if (access(DATABIN "/magiskpolicy", X_OK) == 0) {
        sprintf(buf, "%s/magiskpolicy", MAGISKTMP.data());
        cp_afc(DATABIN "/magiskpolicy", buf);
    }

    return true;
}

void reboot() {
    if (RECOVERY_MODE)
        exec_command_sync("/system/bin/reboot", "recovery");
    else
        exec_command_sync("/system/bin/reboot");
}

static bool check_data() {
    bool mnt = false;
    file_readline("/proc/mounts", [&](string_view s) {
        if (str_contains(s, " /data ") && !str_contains(s, "tmpfs")) {
            mnt = true;
            return false;
        }
        return true;
    });
    if (!mnt)
        return false;
    auto crypto = getprop("ro.crypto.state");
    if (!crypto.empty()) {
        if (crypto != "encrypted") {
            // Unencrypted, we can directly access data
            return true;
        } else {
            // Encrypted, check whether vold is started
            return !getprop("init.svc.vold").empty();
        }
    }
    // ro.crypto.state is not set, assume it's unencrypted
    return true;
}

void unlock_blocks() {
    int fd, dev, OFF = 0;

    auto dir = xopen_dir("/dev/block");
    if (!dir)
        return;
    dev = dirfd(dir.get());

    for (dirent *entry; (entry = readdir(dir.get()));) {
        if (entry->d_type == DT_BLK) {
            if ((fd = openat(dev, entry->d_name, O_RDONLY | O_CLOEXEC)) < 0)
                continue;
            if (ioctl(fd, BLKROSET, &OFF) < 0)
                PLOGE("unlock %s", entry->d_name);
            close(fd);
        }
    }
}

#define test_bit(bit, array) (array[bit / 8] & (1 << (bit % 8)))

static bool check_key_combo() {
    uint8_t bitmask[(KEY_MAX + 1) / 8];
    vector<int> events;
    constexpr char name[] = "/dev/.ev";

    // First collect candidate events that accepts volume down
    for (int minor = 64; minor < 96; ++minor) {
        if (xmknod(name, S_IFCHR | 0444, makedev(13, minor)))
            continue;
        int fd = open(name, O_RDONLY | O_CLOEXEC);
        unlink(name);
        if (fd < 0)
            continue;
        memset(bitmask, 0, sizeof(bitmask));
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bitmask)), bitmask);
        if (test_bit(KEY_VOLUMEDOWN, bitmask))
            events.push_back(fd);
        else
            close(fd);
    }
    if (events.empty())
        return false;

    run_finally fin([&]{ std::for_each(events.begin(), events.end(), close); });

    // Check if volume down key is held continuously for more than 3 seconds
    for (int i = 0; i < 300; ++i) {
        bool pressed = false;
        for (const int &fd : events) {
            memset(bitmask, 0, sizeof(bitmask));
            ioctl(fd, EVIOCGKEY(sizeof(bitmask)), bitmask);
            if (test_bit(KEY_VOLUMEDOWN, bitmask)) {
                pressed = true;
                break;
            }
        }
        if (!pressed)
            return false;
        // Check every 10ms
        usleep(10000);
    }
    LOGD("KEY_VOLUMEDOWN detected: enter safe mode\n");
    return true;
}

/***********************
 * Boot Stage Handlers *
 ***********************/

extern int disable_deny();

static void post_fs_data() {
    if (!check_data())
        return;

    setup_logfile(true);

    LOGI("** post-fs-data mode running\n");

    unlock_blocks();
    mount_mirrors();
    prune_su_access();

    if (access(SECURE_DIR, F_OK) != 0) {
        LOGE(SECURE_DIR " is not present, abort\n");
        goto early_abort;
    }

    if (!magisk_env()) {
        LOGE("* Magisk environment incomplete, abort\n");
        goto early_abort;
    }

    if (getprop("persist.sys.safemode", true) == "1" ||
        getprop("ro.sys.safemode") == "1" || check_key_combo()) {
        boot_state |= FLAG_SAFE_MODE;
        // Disable all modules and denylist so next boot will be clean
        disable_modules();
        disable_deny();
    } else {
        exec_common_scripts("post-fs-data");
        db_settings dbs;
        get_db_settings(dbs, ZYGISK_CONFIG);
        zygisk_enabled = dbs[ZYGISK_CONFIG];
        initialize_denylist();
        handle_modules();
    }

early_abort:
    // We still do magic mount because root itself might need it
    load_modules();
    boot_state |= FLAG_POST_FS_DATA_DONE;
}

static void late_start() {
    setup_logfile(false);

    LOGI("** late_start service mode running\n");

    exec_common_scripts("service");
    exec_module_scripts("service");

    boot_state |= FLAG_LATE_START_DONE;
}

static void boot_complete() {
    boot_state |= FLAG_BOOT_COMPLETE;
    setup_logfile(false);

    LOGI("** boot-complete triggered\n");

    // At this point it's safe to create the folder
    if (access(SECURE_DIR, F_OK) != 0)
        xmkdir(SECURE_DIR, 0700);

    // Ensure manager exists
    check_pkg_refresh();
    get_manager(0, nullptr, true);
}

void boot_stage_handler(int code) {
    // Make sure boot stage execution is always serialized
    static pthread_mutex_t stage_lock = PTHREAD_MUTEX_INITIALIZER;
    mutex_guard lock(stage_lock);

    switch (code) {
    case MainRequest::POST_FS_DATA:
        if ((boot_state & FLAG_POST_FS_DATA_DONE) == 0)
            post_fs_data();
        close(xopen(UNBLOCKFILE, O_RDONLY | O_CREAT, 0));
        break;
    case MainRequest::LATE_START:
        if ((boot_state & FLAG_POST_FS_DATA_DONE) && (boot_state & FLAG_SAFE_MODE) == 0)
            late_start();
        break;
    case MainRequest::BOOT_COMPLETE:
        if ((boot_state & FLAG_SAFE_MODE) == 0)
            boot_complete();
        break;
    default:
        __builtin_unreachable();
    }
}
