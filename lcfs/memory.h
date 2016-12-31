#ifndef _MEMORY_H
#define _MEMORY_H

/* Type of malloc requests */
enum lc_memTypes {
    LC_MEMTYPE_GFS = 0,
    LC_MEMTYPE_DIRENT = 1,
    LC_MEMTYPE_DCACHE = 2,
    LC_MEMTYPE_ICACHE = 3,
    LC_MEMTYPE_INODE = 4,
    LC_MEMTYPE_PCACHE = 5,
    LC_MEMTYPE_PCLOCK = 6,
    LC_MEMTYPE_EXTENT = 7,
    LC_MEMTYPE_BLOCK = 8,
    LC_MEMTYPE_PAGE = 9,
    LC_MEMTYPE_DATA = 10,
    LC_MEMTYPE_DPAGEHASH = 11,
    LC_MEMTYPE_HPAGE = 12,
    LC_MEMTYPE_XATTR = 13,
    LC_MEMTYPE_XATTRNAME = 14,
    LC_MEMTYPE_XATTRVALUE = 15,
    LC_MEMTYPE_XATTRBUF = 16,
    LC_MEMTYPE_XATTRINODE = 17,
    LC_MEMTYPE_STATS = 18,
    LC_MEMTYPE_MAX = 19,
};

#endif
