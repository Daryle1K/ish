#include <limits.h>
#ifndef GDBM_NDBM
#include <ndbm.h>
#else
#include <gdbm-ndbm.h>
#endif
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "debug.h"
#include "kernel/errno.h"
#include "kernel/process.h"
#include "kernel/fs.h"

struct ish_stat {
    dword_t mode;
    dword_t uid;
    dword_t gid;
    dword_t rdev;
};

static DBM *get_db(struct mount *mount) {
    DBM *db = mount->data;
    if (db == NULL) {
        char db_path[PATH_MAX];
        strcpy(db_path, mount->source);
        char *basename = strrchr(db_path, '/') + 1;
        assert(strcmp(basename, "data") == 0);
        strncpy(basename, "meta", 4);
        db = dbm_open(db_path, O_RDWR, 0666);
        assert(db != NULL);
        mount->data = db;
    }
    return db;
}

static datum build_key(char *keydata, const char *path, const char *type) {
    strcpy(keydata, type);
    strcat(keydata, ":");
    strcat(keydata, path);
    datum key = {.dptr = keydata, .dsize = strlen(keydata)};
    return key;
}

static datum read_meta(struct mount *mount, const char *path, const char *type) {
    char keydata[MAX_PATH+strlen(type)+1];
    datum key = build_key(keydata, path, type);
    return dbm_fetch(get_db(mount), key);
}

static void write_meta(struct mount *mount, const char *path, const char *type, datum data) {
    char keydata[MAX_PATH+strlen(type)+1];
    datum key = build_key(keydata, path, type);
    dbm_store(get_db(mount), key, data, DBM_REPLACE);
}

static int delete_meta(struct mount *mount, const char *path, const char *type) {
    char keydata[MAX_PATH+strlen(type)+1];
    datum key = build_key(keydata, path, type);
    return !dbm_delete(get_db(mount), key);
}

static int read_stat(struct mount *mount, const char *path, struct ish_stat *stat) {
    datum d = read_meta(mount, path, "meta");
    if (d.dptr == NULL)
        return 0;
    assert(d.dsize == sizeof(struct ish_stat));
    *stat = *(struct ish_stat *) d.dptr;
    return 1;
}

static void write_stat(struct mount *mount, const char *path, struct ish_stat *stat) {
    datum data;
    data.dptr = (void *) stat;
    data.dsize = sizeof(struct ish_stat);
    write_meta(mount, path, "meta", data);
}

static int delete_stat(struct mount *mount, const char *path) {
    return delete_meta(mount, path, "meta");
}

static struct fd *fakefs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct fd *fd = realfs.open(mount, path, flags, 0644);
    if (IS_ERR(fd))
        return fd;
    if (flags & O_CREAT_) {
        struct ish_stat ishstat;
        ishstat.mode = mode | S_IFREG;
        ishstat.uid = current->uid;
        ishstat.gid = current->gid;
        ishstat.rdev = 0;
        write_stat(mount, path, &ishstat);
    }
    return fd;
}

static int fakefs_unlink(struct mount *mount, const char *path) {
    int err = realfs.unlink(mount, path);
    if (err < 0)
        return err;
    delete_stat(mount, path);
    return 0;
}

static int fakefs_rename(struct mount *mount, const char *src, const char *dst) {
    int err = realfs.rename(mount, src, dst);
    if (err < 0)
        return err;
    struct ish_stat stat;
    if (!read_stat(mount, src, &stat))
        return _ENOENT;
    delete_stat(mount, src);
    write_stat(mount, dst, &stat);
    return 0;
}

static int fakefs_symlink(struct mount *mount, const char *target, const char *link) {
    // create a file containing the target
    int fd = openat(mount->root_fd, fix_path(link), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return err_map(errno);
    ssize_t res = write(fd, target, strlen(target));
    close(fd);
    if (res < 0) {
        unlinkat(mount->root_fd, fix_path(link), 0);
        return err_map(errno);
    }

    // customize the stat info so it looks like a link
    struct ish_stat ishstat;
    ishstat.mode = S_IFLNK | 0777; // symlinks always have full permissions
    ishstat.uid = current->uid;
    ishstat.gid = current->gid;
    ishstat.rdev = 0;
    write_stat(mount, link, &ishstat);
    return 0;
}

static int fakefs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat, bool follow_links) {
    struct ish_stat ishstat;
    if (!read_stat(mount, path, &ishstat))
        return _ENOENT;
    int err = realfs.stat(mount, path, fake_stat, follow_links);
    if (err < 0)
        return err;
    fake_stat->mode = ishstat.mode;
    fake_stat->uid = ishstat.uid;
    fake_stat->gid = ishstat.gid;
    fake_stat->rdev = ishstat.rdev;
    return 0;
}

static int fakefs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    // this is truly sad, but there is no alternative
    char path[MAX_PATH];
    int err = fd->ops->getpath(fd, path);
    if (err < 0)
        return err;
    return fakefs_stat(fd->mount, path, fake_stat, false);
}

static int fakefs_setattr(struct mount *mount, const char *path, struct attr attr) {
    struct ish_stat ishstat;
    if (!read_stat(mount, path, &ishstat))
        return _ENOENT;
    int err;
    switch (attr.type) {
        case attr_uid:
            ishstat.uid = attr.uid;
            break;
        case attr_gid:
            ishstat.gid = attr.gid;
            break;
        case attr_mode:
            ishstat.mode = (ishstat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
        case attr_size:
            err = truncate(fix_path(path), attr.size);
            if (err < 0)
                return err;
            break;
    }
    return 0;
}

static int fakefs_fsetattr(struct fd *fd, struct attr attr) {
    char path[MAX_PATH];
    int err = fd->ops->getpath(fd, path);
    if (err < 0)
        return err;
    return fakefs_setattr(fd->mount, path, attr);
}

static int fakefs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    int err = realfs.mkdir(mount, path, 0755);
    if (err < 0)
        return err;
    struct ish_stat ishstat;
    ishstat.mode = mode | S_IFDIR;
    ishstat.uid = current->uid;
    ishstat.gid = current->gid;
    ishstat.rdev = 0;
    write_stat(mount, path, &ishstat);
    return 0;
}

static ssize_t fakefs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    struct ish_stat ishstat;
    if (!read_stat(mount, path, &ishstat))
        return _ENOENT;
    if (!S_ISLNK(ishstat.mode))
        return _EINVAL;

    ssize_t err = realfs.readlink(mount, path, buf, bufsize);
    if (err == _EINVAL) {
        // broken symlinks can't be included in an iOS app or else Xcode craps out
        int fd = openat(mount->root_fd, fix_path(path), O_RDONLY);
        if (fd < 0)
            return err_map(errno);
        int err = read(fd, buf, bufsize);
        if (err < 0)
            return err_map(errno);
        close(fd);
        return err;
    }
    return err;
}

static int fakefs_mount(struct mount *mount) {
    // TODO maybe open the database here
    return realfs.mount(mount);
}

static int fakefs_umount(struct mount *mount) {
    if (mount->data)
        dbm_close(mount->data);
    /* return realfs.umount(mount); */
    return 0;
}

const struct fs_ops fakefs = {
    .mount = fakefs_mount,
    .umount = fakefs_umount,
    .statfs = realfs_statfs,
    .open = fakefs_open,
    .readlink = fakefs_readlink,
    .access = realfs_access,
    .unlink = fakefs_unlink,
    .rename = fakefs_rename,
    .symlink = fakefs_symlink,
    
    .stat = fakefs_stat,
    .fstat = fakefs_fstat,
    .flock = realfs_flock,
    .setattr = fakefs_setattr,
    .fsetattr = fakefs_fsetattr,

    .mkdir = fakefs_mkdir,
};