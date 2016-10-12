#include "includes.h"

/* Initialize default values in fuse_entry_param structure */
static void
dfs_epInit(struct fuse_entry_param *ep) {
    ep->attr.st_ino = ep->ino;
    ep->generation = 1;
    ep->attr_timeout = 1.0;
    ep->entry_timeout = 1.0;
}

/* Create a new directory entry and associated inode */
static int
create(ino_t parent, const char *name, mode_t mode, uid_t uid, gid_t gid,
       dev_t rdev, const char *target, struct fuse_entry_param *ep) {
    struct gfs *gfs = getfs();
    struct inode *dir, *inode;
    struct fs *fs;
    ino_t ino;

    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, parent);
    dir = dfs_getInode(fs, parent, NULL, true, true);
    if (dir == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, parent, ENOENT);
        return ENOENT;
    }
    assert(S_ISDIR(dir->i_stat.st_mode));
    inode = dfs_inodeInit(fs, mode, uid, gid, rdev, target);
    ino = inode->i_stat.st_ino;
    dfs_dirAdd(dir, ino, mode, name);
    if (S_ISDIR(mode)) {
        dir->i_stat.st_nlink++;
    }
    dfs_updateInodeTimes(dir, false, true, true);
    memcpy(&ep->attr, &inode->i_stat, sizeof(struct stat));
    dfs_inodeUnlock(inode);
    dfs_inodeUnlock(dir);
    ep->ino = dfs_setHandle(fs, ino);
    dfs_unlock(gfs);
    dfs_epInit(ep);
    return 0;
}

/* Remove a directory entry */
static int
dremove(struct fs *fs, struct inode *dir, const char *name,
        ino_t ino, bool rmdir) {
    struct inode * inode = dfs_getInode(fs, ino, NULL, true, true);

    if (inode == NULL) {
        dfs_reportError(__func__, ino, ESTALE);
        return ESTALE;
    }
    assert(inode->i_stat.st_nlink);
    if (rmdir) {
        if (inode->i_stat.st_nlink > 2) {
        /*
           // Docker deletes some directories while not empty XXX
        if ((inode->i_stat.st_nlink > 2) ||
            (inode->i_dirent != NULL)) {
        */
            dfs_inodeUnlock(inode);
            dfs_reportError(__func__, ino, EEXIST);
            return EEXIST;
        }
        dir->i_stat.st_nlink--;
        inode->i_removed = true;
    } else {
        inode->i_stat.st_nlink--;

        /* Flag a file as removed on last unlink */
        if (inode->i_stat.st_nlink == 0) {
            inode->i_removed = true;
        }
    }
    dfs_dirRemove(dir, name);
    dfs_updateInodeTimes(dir, false, false, true);
    dfs_inodeUnlock(inode);
    return 0;
}

/* Remove a directory entry */
static int
dfs_remove(ino_t parent, const char *name, bool rmdir) {
    struct gfs *gfs = getfs();
    struct inode *dir;
    struct fs *fs;
    ino_t ino;
    int err;

    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, parent);
    dir = dfs_getInode(fs, parent, NULL, true, true);
    if (dir == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, parent, ENOENT);
        return ENOENT;
    }
    assert(S_ISDIR(dir->i_stat.st_mode));
    /* XXX Combine lookup and removal */
    ino = dfs_dirLookup(fs, dir, name);
    if (ino == DFS_INVALID_INODE) {
        dfs_reportError(__func__, ino, ESTALE);
        err = ESTALE;
    } else {
        err = dremove(fs, dir, name, ino, rmdir);
    }
    dfs_inodeUnlock(dir);
    dfs_unlock(gfs);
    return err;
}

/* Lookup the specified name in the specified directory */
static void
dfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct gfs *gfs = getfs();
    struct fuse_entry_param ep;
    struct inode *inode, *dir;
    struct fs *fs;
    ino_t ino;

    dfs_displayEntry(__func__, parent, 0, name);
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, parent);
    dir = dfs_getInode(fs, parent, NULL, false, false);
    if (dir == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    ino = dfs_dirLookup(fs, dir, name);
    if (ino == DFS_INVALID_INODE) {
        dfs_unlock(gfs);
        dfs_inodeUnlock(dir);

        /* Let kernel remember lookup failure as a negative entry */
        memset(&ep, 0, sizeof(struct fuse_entry_param));
        ep.entry_timeout = 1.0;
        fuse_reply_entry(req, &ep);
        return;
    }
    inode = dfs_getInode(fs, ino, NULL, false, false);
    dfs_inodeUnlock(dir);
    if (inode == NULL) {
        fuse_reply_err(req, ENOENT);
        dfs_unlock(gfs);
    } else {
        memcpy(&ep.attr, &inode->i_stat, sizeof(struct stat));
        dfs_inodeUnlock(inode);
        ep.ino = dfs_setHandle(dfs_checkfs(fs, ino), ino);
        dfs_unlock(gfs);
        dfs_epInit(&ep);
        fuse_reply_entry(req, &ep);
    }
}

#if 0
/* Forget an inode - not relevant for this file system */
static void
dfs_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup) {
    dfs_displayEntry(__func__, 0, ino, NULL);
    fuse_reply_none(req);
}
#endif

/* Get attributes of a file */
static void
dfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct inode *inode;
    struct stat stbuf;
    struct fs *fs;

    dfs_displayEntry(__func__, 0, ino, NULL);
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, ino);
    inode = dfs_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        dfs_unlock(gfs);
        //dfs_reportError(__func__, 0, ENOENT);
        fuse_reply_err(req, ENOENT);
    }
    memcpy(&stbuf, &inode->i_stat, sizeof(struct stat));
    dfs_inodeUnlock(inode);
    stbuf.st_ino = dfs_setHandle(dfs_checkfs(fs, stbuf.st_ino), stbuf.st_ino);
    dfs_unlock(gfs);
    fuse_reply_attr(req, &stbuf, 1.0);
}

/* Truncate a file */
static void
dfs_truncate(struct inode *inode, off_t size) {
    assert(S_ISREG(inode->i_stat.st_mode));
    if (size < inode->i_stat.st_size) {
        dfs_truncPages(inode, size);
    }
    inode->i_stat.st_size = size;
}

/* Change the attributes of the specified inode as requested */
static void
dfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
            int to_set, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    bool ctime = false, mtime = false, atime = false;
    struct inode *inode;
    struct stat stbuf;
    struct fs *fs;

    dfs_displayEntry(__func__, ino, 0, NULL);
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, ino);
    inode = dfs_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    if (to_set & FUSE_SET_ATTR_MODE) {
        assert((inode->i_stat.st_mode & S_IFMT) == (attr->st_mode & S_IFMT));
        inode->i_stat.st_mode = attr->st_mode;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_UID) {
        inode->i_stat.st_uid = attr->st_uid;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_GID) {
        inode->i_stat.st_gid = attr->st_gid;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_SIZE) {
        dfs_truncate(inode, attr->st_size);
        mtime = true;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_ATIME) {
        inode->i_stat.st_atime = attr->st_atime;
        atime = false;
    } else if (to_set & FUSE_SET_ATTR_ATIME_NOW) {
        atime = true;
    }
    if (to_set & FUSE_SET_ATTR_MTIME) {
        inode->i_stat.st_mtime = attr->st_mtime;
        mtime = false;
    } else if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
        mtime = true;
        ctime = true;
    }
    if (ctime || mtime || atime) {
        dfs_updateInodeTimes(inode, atime, mtime, ctime);
    }
    memcpy(&stbuf, &inode->i_stat, sizeof(struct stat));
    dfs_inodeUnlock(inode);
    stbuf.st_ino = dfs_setHandle(dfs_checkfs(fs, stbuf.st_ino), stbuf.st_ino);
    dfs_unlock(gfs);
    fuse_reply_attr(req, &stbuf, 1.0);
}

/* Read target information for a symbolic link */
static void
dfs_readlink(fuse_req_t req, fuse_ino_t ino) {
    char buf[DFS_FILENAME_MAX + 1];
    struct gfs *gfs = getfs();
    struct inode *inode;
    struct fs *fs;
    int size;

    dfs_displayEntry(__func__, 0, ino, NULL);
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, ino);
    inode = dfs_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    assert(S_ISLNK(inode->i_stat.st_mode));
    size = strlen(inode->i_target);
    assert(size <= DFS_FILENAME_MAX);
    strncpy(buf, inode->i_target, size);
    dfs_inodeUnlock(inode);
    buf[size] = 0;
    dfs_unlock(gfs);
    fuse_reply_readlink(req, buf);
}

/* Create a special file */
static void
dfs_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
          mode_t mode, dev_t rdev) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    int err;

    dfs_displayEntry(__func__, parent, 0, name);
    err = create(parent, name, mode & ~ctx->umask,
                 ctx->uid, ctx->gid, rdev, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
    }
}

/* Create a directory */
static void
dfs_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
    struct gfs *gfs = getfs();
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    int err;

    dfs_displayEntry(__func__, parent, 0, name);
    err = create(parent, name, S_IFDIR | (mode & ~ctx->umask),
                 ctx->uid, ctx->gid, 0, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
        if ((dfs_getInodeHandle(parent) == DFS_ROOT_INODE) &&
            (strcmp(name, "dfs") == 0)) {
            printf("snapshot root inode %ld\n", e.ino);
            gfs->gfs_snap_root = e.ino;
        }
    }
}

/* Remove a file */
static void
dfs_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
    int err;

    dfs_displayEntry(__func__, parent, 0, name);
    err = dfs_remove(parent, name, false);
    fuse_reply_err(req, err);
}

/* Remove a special directory */
static void
dfs_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
    int err;

    dfs_displayEntry(__func__, parent, 0, name);
    err = dfs_remove(parent, name, true);
    fuse_reply_err(req, err);
}

/* Create a symbolic link */
static void
dfs_symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
            const char *name) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    int err;

    dfs_displayEntry(__func__, parent, 0, name);
    err = create(parent, name, S_IFLNK | (0777 & ~ctx->umask),
                 ctx->uid, ctx->gid, 0, link, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
    }
}

/* Rename a file to another (mv) */
static void
dfs_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
           fuse_ino_t newparent, const char *newname) {
    struct gfs *gfs = getfs();
    struct inode *inode, *sdir, *tdir = NULL;
    ino_t ino, target;
    struct fs *fs;

    dfs_displayEntry(__func__, parent, newparent, name);
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, parent);

    /* Follow some locking order while locking the directories */
    if (parent > newparent) {
        tdir = dfs_getInode(fs, newparent, NULL, true, true);
        if (tdir == NULL) {
            dfs_unlock(gfs);
            dfs_reportError(__func__, newparent, ENOENT);
            fuse_reply_err(req, ENOENT);
            return;
        }
        assert(S_ISDIR(tdir->i_stat.st_mode));
    }
    sdir = dfs_getInode(fs, parent, NULL, true, true);
    if (sdir == NULL) {
        if (tdir) {
            dfs_inodeUnlock(tdir);
        }
        dfs_unlock(gfs);
        dfs_reportError(__func__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    assert(S_ISDIR(sdir->i_stat.st_mode));
    if (parent < newparent) {
        tdir = dfs_getInode(fs, newparent, NULL, true, true);
        if (tdir == NULL) {
            dfs_inodeUnlock(sdir);
            dfs_unlock(gfs);
            dfs_reportError(__func__, newparent, ENOENT);
            fuse_reply_err(req, ENOENT);
            return;
        }
        assert(S_ISDIR(tdir->i_stat.st_mode));
    }
    ino = dfs_dirLookup(fs, sdir, name);
    if (ino == DFS_INVALID_INODE) {
        dfs_inodeUnlock(sdir);
        if (tdir) {
            dfs_inodeUnlock(tdir);
        }
        dfs_reportError(__func__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    target = dfs_dirLookup(fs, tdir ? tdir : sdir, newname);

    /* Renaming to another directory */
    if (parent != newparent) {
        if (target != DFS_INVALID_INODE) {
            dremove(fs, tdir, newname, target, false);
        }
        inode = dfs_getInode(fs, ino, NULL, true, true);
        if (inode == NULL) {
            dfs_inodeUnlock(sdir);
            dfs_inodeUnlock(tdir);
            dfs_unlock(gfs);
            dfs_reportError(__func__, ino, ENOENT);
            fuse_reply_err(req, ENOENT);
            return;
        }
        dfs_dirAdd(tdir, ino, inode->i_stat.st_mode, name);
        dfs_dirRemove(sdir, name);
        if (S_ISDIR(inode->i_stat.st_mode)) {
            assert(sdir->i_stat.st_nlink);
            sdir->i_stat.st_nlink--;
            tdir->i_stat.st_nlink++;
        }
        dfs_inodeUnlock(inode);
    } else {

        /* Rename within the directory */
        if (target != DFS_INVALID_INODE) {
            dremove(fs, sdir, newname, target, false);
        }
        dfs_dirRename(sdir, ino, newname);
    }
    dfs_updateInodeTimes(sdir, false, true, true);
    if (tdir) {
        dfs_updateInodeTimes(tdir, false, true, true);
        dfs_inodeUnlock(tdir);
    }
    dfs_inodeUnlock(sdir);
    dfs_unlock(gfs);
    fuse_reply_err(req, 0);
}

/* Create a new link to an inode */
static void
dfs_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
         const char *newname) {
    struct fuse_entry_param ep;
    struct gfs *gfs = getfs();
    struct inode *inode, *dir;
    struct fs *fs;

    dfs_displayEntry(__func__, newparent, ino, newname);
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, ino);
    dir = dfs_getInode(fs, newparent, NULL, true, true);
    if (dir == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, newparent, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    assert(S_ISDIR(dir->i_stat.st_mode));
    inode = dfs_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        dfs_inodeUnlock(dir);
        dfs_unlock(gfs);
        dfs_reportError(__func__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    assert(S_ISREG(inode->i_stat.st_mode));
    dfs_dirAdd(dir, inode->i_stat.st_ino, inode->i_stat.st_mode, newname);
    dfs_updateInodeTimes(dir, false, true, true);
    inode->i_stat.st_nlink++;
    dfs_updateInodeTimes(inode, false, false, true);
    dfs_inodeUnlock(dir);
    memcpy(&ep.attr, &inode->i_stat, sizeof(struct stat));
    dfs_inodeUnlock(inode);
    ep.ino = dfs_setHandle(dfs_checkfs(fs, ino), ino);
    dfs_unlock(gfs);
    dfs_epInit(&ep);
    fuse_reply_entry(req, &ep);
}

/* Set up file handle in case file is shared from another file system */
static void
dfs_setFileHandle(fuse_ino_t ino, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct inode *inode;
    struct fs *fs;
    ino_t inum;

    if ((dfs_getFsHandle(ino) != DFS_ROOT_INODE) &&
        ((fi->flags & 3) == O_RDONLY)) {
        inum = dfs_getInodeHandle(ino);
        dfs_lock(gfs, false);
        fs = dfs_getfs(gfs, ino);
        if (fs->fs_inode[inum] == NULL) {
            inode = dfs_getInode(fs, ino, NULL, false, false);
            dfs_inodeUnlock(inode);
            fi->fh = (uint64_t)inode;
        }
        dfs_unlock(gfs);
    }
}

/* Open a file and return a handle corresponding to the inode number */
static void
dfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, 0, ino, NULL);
    /* XXX Do not allow opening removed files */
    dfs_setFileHandle(ino, fi);
    fuse_reply_open(req, fi);
}

/* Read from a file */
static void
dfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
         struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct inode *inode, *handle;
    off_t endoffset;
    char buf[size];
    struct fs *fs;
    size_t fsize;

    dfs_displayEntry(__func__, ino, 0, NULL);
    if (size == 0) {
        fuse_reply_buf(req, NULL, 0);
    }
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, ino);
    handle = fi ? (struct inode *)fi->fh : NULL;
    inode = dfs_getInode(fs, ino, handle, false, false);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    assert(S_ISREG(inode->i_stat.st_mode));

    /* Reading beyond file size is not allowed */
    fsize = inode->i_stat.st_size;
    if (off >= fsize) {
        dfs_inodeUnlock(inode);
        dfs_unlock(gfs);
        fuse_reply_buf(req, NULL, 0);
        return;
    }
    endoffset = off + size;
    if (endoffset > fsize) {
        endoffset = fsize;
    }
    dfs_readPages(inode, off, endoffset, buf);
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    fuse_reply_buf(req, buf, endoffset - off);
}

/* Write to a file */
static void
dfs_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
          size_t size, off_t off, struct fuse_file_info *fi) {
    off_t endoffset, poffset, woff = 0;
    struct gfs *gfs = getfs();
    uint64_t page, spage;
    struct inode *inode;
    size_t wsize, psize;
    struct fs *fs;

    dfs_displayEntry(__func__, ino, 0, NULL);
    spage = off / DFS_BLOCK_SIZE;
    endoffset = off + size;
    page = spage;
    wsize = size;
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, ino);
    inode = dfs_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    assert(S_ISREG(inode->i_stat.st_mode));

    /* Break the down the write into pages and link those to the file */
    while (wsize) {
        if (page == spage) {
            poffset = off % DFS_BLOCK_SIZE;
            psize = DFS_BLOCK_SIZE - poffset;
        } else {
            poffset = 0;
            psize = DFS_BLOCK_SIZE;
        }
        if (psize > wsize) {
            psize = wsize;
        }
        dfs_addPage(inode, page, poffset, psize, &buf[woff]);
        page++;
        woff += psize;
        wsize -= psize;
    }

    /* Update inode size if needed */
    if (endoffset > inode->i_stat.st_size) {
        inode->i_stat.st_size = endoffset;
    }
    dfs_updateInodeTimes(inode, false, true, true);

    /* Update block count */
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    fuse_reply_write(req, size);
}

/* Flush a file */
static void
dfs_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
}

/* Release open count on a file */
static void
dfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, ino, 0, NULL);
    fuse_lowlevel_notify_inval_inode(getfs()->gfs_ch, ino, 0, -1);
    fuse_reply_err(req, 0);
}

/* Sync a file */
static void
dfs_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
          struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
}

/* Open a directory */
static void
dfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, 0, ino, NULL);
    /* XXX Do not allow opening removed directories */
    dfs_setFileHandle(ino, fi);
    fuse_reply_open(req, fi);
}

/* Read entries from a directory */
static void
dfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
            struct fuse_file_info *fi) {
    struct inode *dir, *handle;
    struct gfs *gfs = getfs();
    struct dirent *dirent;
    size_t esize, csize;
    char buf[size];
    struct stat st;
    int count = 0;
    struct fs *fs;

    dfs_displayEntry(__func__, ino, 0, NULL);
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, ino);
    handle = fi ? (struct inode *)fi->fh : NULL;
    dir = dfs_getInode(fs, ino, handle, false, false);
    if (dir == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    assert(S_ISDIR(dir->i_stat.st_mode));
    dirent = dir->i_dirent;
    while ((count < off) && dirent) {
        dirent = dirent->di_next;
        count++;
    }
    memset(&st, 0, sizeof(struct stat));
    csize = 0;
    while (dirent != NULL) {
        count++;
        st.st_ino = dfs_setHandle(dfs_checkfs(fs, dirent->di_ino),
                                  dirent->di_ino);
        st.st_mode = dirent->di_mode;
        esize = fuse_add_direntry(req, &buf[csize], size - csize,
                                  dirent->di_name, &st, count);
        csize += esize;
        if (csize >= size) {
            csize -= esize;
            break;
        }
        dirent = dirent->di_next;
    }
    dfs_inodeUnlock(dir);
    dfs_unlock(gfs);
    if (csize) {
        fuse_reply_buf(req, buf, csize);
    } else {
        fuse_reply_buf(req, NULL, 0);
    }
}

/* Release a directory */
static void
dfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
}

static void
dfs_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
             struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
}

/* File system statfs */
static void
dfs_statfs(fuse_req_t req, fuse_ino_t ino) {
    struct gfs *gfs = getfs();
    struct statvfs buf;

    dfs_displayEntry(__func__, ino, 0, NULL);
    buf.f_bsize = DFS_BLOCK_SIZE;
    buf.f_frsize = DFS_BLOCK_SIZE;
    buf.f_blocks = gfs->gfs_super->sb_tblocks;
    buf.f_bfree = buf.f_blocks - gfs->gfs_super->sb_nblock;
    buf.f_bavail = buf.f_bfree;
    buf.f_files = UINT32_MAX;
    buf.f_ffree = buf.f_files - gfs->gfs_super->sb_ninode;
    buf.f_favail = buf.f_ffree;
    buf.f_flag = 0;
    buf.f_namemax = DFS_FILENAME_MAX;
    buf.f_fsid = 0;
    fuse_reply_statfs(req, &buf);
}

/* Set extended attributes on a file, currently used for creating a new file
 * system
 */
static void
dfs_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
             const char *value, size_t size, int flags) {
    struct gfs *gfs = getfs();
    int err;

    dfs_displayEntry(__func__, ino, 0, name);
    err = dfs_newClone(gfs, ino, name);
    fuse_reply_err(req, err);
}

#if 0
/* Get extended attributes of the specified inode */
static void
dfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size) {
    dfs_displayEntry(__func__, ino, 0, name);
    fuse_reply_buf(req, NULL, 0);
}

/* List extended attributes on a file */
static void
dfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
    dfs_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_buf(req, NULL, 0);
}
#endif

/* Remove extended attributes */
static void
dfs_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name) {
    struct gfs *gfs = getfs();
    int err;

    dfs_displayEntry(__func__, ino, 0, name);
    err = dfs_removeClone(gfs, ino);
    fuse_reply_err(req, err);
}

#if 0
/* Check access permissions on an inode */
static void
dfs_access(fuse_req_t req, fuse_ino_t ino, int mask) {
    dfs_displayEntry(__func__, 0, ino, NULL);
    fuse_reply_err(req, 0);
}
#endif

/* Create a file */
static void
dfs_create(fuse_req_t req, fuse_ino_t parent, const char *name,
           mode_t mode, struct fuse_file_info *fi) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    int err;

    dfs_displayEntry(__func__, parent, 0, name);
    err = create(parent, name, S_IFREG | (mode & ~ctx->umask),
                 ctx->uid, ctx->gid, 0, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        dfs_setFileHandle(e.ino, fi);
        fuse_reply_create(req, &e, fi);
    }
}

#if 0
static void
dfs_getlk(fuse_req_t req, fuse_ino_t ino,
          struct fuse_file_info *fi, struct flock *lock) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
          struct flock *lock, int sleep) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_bmap(fuse_req_t req, fuse_ino_t ino, size_t blocksize, uint64_t idx) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void *arg,
          struct fuse_file_info *fi, unsigned flags,
          const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_poll(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
         struct fuse_pollhandle *ph) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_write_buf(fuse_req_t req, fuse_ino_t ino,
              struct fuse_bufvec *bufv, off_t off, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_retrieve_reply(fuse_req_t req, void *cookie, fuse_ino_t ino, off_t offset,
                   struct fuse_bufvec *bufv) {
    dfs_displayEntry(__func__, ino);
}

/* Forget multiple inodes */
static void
dfs_forget_multi(fuse_req_t req, size_t count,
                 struct fuse_forget_data *forgets) {
    dfs_displayEntry(__func__, 0, count, NULL);
}

static void
dfs_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, int op) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
              off_t offset, off_t length, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, ino);
}
#endif

/* Initialize a new file system */
static void
dfs_init(void *userdata, struct fuse_conn_info *conn) {
    printf("%s: capable 0x%x want 0x%x gfs %p\n", __func__,
           conn->capable, conn->want, userdata);
}

/* Destroy a file system */
static void
dfs_destroy(void *fsp) {
    struct gfs *gfs = (struct gfs *)fsp;

    printf("%s: gfs %p\n", __func__, gfs);
    close(gfs->gfs_fd);
    if (gfs->gfs_super != NULL) {
        free(gfs->gfs_super);
    }
    pthread_mutex_destroy(&gfs->gfs_ilock);
    pthread_rwlock_destroy(&gfs->gfs_rwlock);
}

/* Fuse operations registered with the fuse driver */
struct fuse_lowlevel_ops dfs_ll_oper = {
    .init       = dfs_init,
    .destroy    = dfs_destroy,
    .lookup     = dfs_lookup,
    //.forget     = dfs_forget,
	.getattr	= dfs_getattr,
    .setattr    = dfs_setattr,
	.readlink	= dfs_readlink,
	.mknod  	= dfs_mknod,
	.mkdir  	= dfs_mkdir,
	.unlink  	= dfs_unlink,
	.rmdir		= dfs_rmdir,
	.symlink	= dfs_symlink,
    .rename     = dfs_rename,
    .link       = dfs_link,
    .open       = dfs_open,
    .read       = dfs_read,
    .write      = dfs_write,
    .flush      = dfs_flush,
    .release    = dfs_release,
    .fsync      = dfs_fsync,
    .opendir    = dfs_opendir,
    .readdir    = dfs_readdir,
    .releasedir = dfs_releasedir,
    .fsyncdir   = dfs_fsyncdir,
    .statfs     = dfs_statfs,
    .setxattr   = dfs_setxattr,
#if 0
    .getxattr   = dfs_getxattr,
    .listxattr  = dfs_listxattr,
#endif
    .removexattr  = dfs_removexattr,
    //.access     = dfs_access,
    .create     = dfs_create,
#if 0
    .getlk      = dfs_getlk,
    .setlk      = dfs_setlk,
    .bmap       = dfs_bmap,
    .ioctl      = dfs_ioctl,
    .poll       = dfs_poll,
    .write_buf  = dfs_write_buf,
    .retrieve_reply = dfs_retrieve_reply,
    .forget_multi = dfs_forget_multi,
    .flock      = dfs_flock,
    .fallocate  = dfs_fallocate,
#endif
};
