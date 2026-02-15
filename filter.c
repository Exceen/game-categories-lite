/*
 *  this file is part of Game Categories Lite
 *
 *  Copyright (C) 2011  Codestation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pspiofilemgr.h>
#include <string.h>
#include <stdlib.h>
#include "psppaf.h"
#include "utils.h"
#include "config.h"
#include "logger.h"

extern int global_pos;
extern int model;

static char *game_filter_data[2] = { NULL, NULL };
static SceSize game_filter_size[2];
static int game_counter[2];

static char *category_filter_data[2] = { NULL, NULL };
static SceSize category_filter_size[2];
static int category_counter[2];

static char ascii_tolower(char c) {
    if(c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

static int ascii_equals_ignore_case(const char *a, const char *b) {
    if(!a || !b) return 0;
    while(*a && *b) {
        if(ascii_tolower(*a++) != ascii_tolower(*b++)) return 0;
    }
    return (*a == '\0' && *b == '\0');
}

static int is_legacy_name_only_entry(const char *entry) {
    if(!entry || !*entry) return 0;
    if(strchr(entry, '/') || strchr(entry, '\\')) return 0;
    if(sce_paf_private_strlen(entry) >= 4 && entry[3] == ':') return 0;
    return 1;
}

static int is_comment_line(const char *line) {
    return (line && (line[0] == '#' || line[0] == ';'));
}

static int is_iso_ext(const char *name) {
    const char *ext = strrchr(name, '.');
    if(!ext) return 0;
    char e[5] = {0};
    int i = 0;
    while(ext[i] && i < 4) { e[i] = ascii_tolower(ext[i]); i++; }
    if(!strcmp(e, ".iso") || !strcmp(e, ".cso") || !strcmp(e, ".zso") || !strcmp(e, ".jso") || !strcmp(e, ".dax")) {
        return 1;
    }
    return 0;
}

static void normalize_game_path(const char *in, char *out, int out_size);

#define GCLITE_HIDE_ISO 0

static int is_isogame_token_path(const char *norm_path) {
    if(!norm_path) return 0;
    return (strstr(norm_path, "/@isogame@") != NULL);
}

static int is_iso_path(const char *norm_path) {
    if(!norm_path) return 0;
    if(sce_paf_private_strncmp(norm_path, "/iso/", 5) == 0) return 1;
    const char *fname = strrchr(norm_path, '/');
    if(fname) fname++;
    else fname = norm_path;
    return is_iso_ext(fname);
}

#if GCLITE_HIDE_ISO
static int parse_hex_u32(const char *s, u32 *out) {
    if(!s || !*s || !out) return 0;
    u32 v = 0;
    while(*s) {
        char c = *s++;
        int d;
        if(c >= '0' && c <= '9') d = c - '0';
        else if(c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if(c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | (u32)d;
    }
    *out = v;
    return 1;
}

// Entry with path and mtime for sorting
typedef struct IsoEntry {
    char *path;
    ScePspDateTime mtime;
} IsoEntry;

typedef struct IsoCache {
    int built;
    char **paths;      // Array of path strings (final sorted result)
    IsoEntry *entries; // Temp storage during build (with mtime)
    int count;
} IsoCache;

static IsoCache iso_cache[2];

// Combined global cache for both devices (firmware uses global indices)
static IsoCache global_iso_cache = {0, NULL, NULL, 0};

static int strcasecmp_ascii(const char *a, const char *b) {
    if(!a) a = "";
    if(!b) b = "";
    while(*a && *b) {
        char ca = ascii_tolower(*a++);
        char cb = ascii_tolower(*b++);
        if(ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
    return (int)(unsigned char)ascii_tolower(*a) - (int)(unsigned char)ascii_tolower(*b);
}

// Extract filename from path for sorting (firmware sorts by filename, not full path)
static const char *get_filename_from_path(const char *path) {
    if(!path) return "";
    const char *last_slash = strrchr(path, '/');
    return last_slash ? (last_slash + 1) : path;
}

// Compare by filename only (case-insensitive) - matches firmware behavior
static int compare_iso_paths_by_filename(const char *a, const char *b) {
    return strcasecmp_ascii(get_filename_from_path(a), get_filename_from_path(b));
}

static void sort_strings(char **arr, int count) {
    int i;
    for(i = 1; i < count; ++i) {
        char *key = arr[i];
        int j = i - 1;
        // Sort by filename only to match firmware's sorting order
        while(j >= 0 && compare_iso_paths_by_filename(arr[j], key) > 0) {
            arr[j + 1] = arr[j];
            --j;
        }
        arr[j + 1] = key;
    }
}

static void free_iso_cache(int location) {
    IsoCache *cache = &iso_cache[location];
    if(cache->paths) {
        int i;
        for(i = 0; i < cache->count; ++i) {
            if(cache->paths[i]) sce_paf_private_free(cache->paths[i]);
        }
        sce_paf_private_free(cache->paths);
    }
    if(cache->entries) {
        sce_paf_private_free(cache->entries);
    }
    cache->paths = NULL;
    cache->entries = NULL;
    cache->count = 0;
    cache->built = 0;
}

// Compare two ScePspDateTime values for descending sort (most recent first)
// Returns: negative if a is MORE recent, positive if b is MORE recent, 0 if equal
static int compare_mtime_desc(const ScePspDateTime *a, const ScePspDateTime *b) {
    // Compare year first
    if(a->year != b->year) return (b->year - a->year);
    if(a->month != b->month) return (b->month - a->month);
    if(a->day != b->day) return (b->day - a->day);
    if(a->hour != b->hour) return (b->hour - a->hour);
    if(a->minute != b->minute) return (b->minute - a->minute);
    if(a->second != b->second) return (b->second - a->second);
    return (int)(b->microsecond - a->microsecond);
}

// Sort IsoEntry array by mtime descending (most recent first)
static void sort_entries_by_mtime_desc(IsoEntry *entries, int count) {
    int i;
    for(i = 1; i < count; ++i) {
        IsoEntry key = entries[i];
        int j = i - 1;
        while(j >= 0 && compare_mtime_desc(&entries[j].mtime, &key.mtime) > 0) {
            entries[j + 1] = entries[j];
            --j;
        }
        entries[j + 1] = key;
    }
}

// Count ISO files recursively in a directory
static int count_isos_recursive(const char *dirpath, const char *subdir, int depth) {
    if(depth > 2) return 0; // Limit recursion depth

    char fullpath[256];
    if(subdir && subdir[0]) {
        sce_paf_private_snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, subdir);
    } else {
        sce_paf_private_strncpy(fullpath, dirpath, sizeof(fullpath) - 1);
        fullpath[sizeof(fullpath) - 1] = '\0';
    }

    SceUID d = sceIoDopen(fullpath);
    if(d < 0) return 0;

    int count = 0;
    SceIoDirent ent;
    sce_paf_private_memset(&ent, 0, sizeof(ent));
    while(sceIoDread(d, &ent) > 0) {
        // Skip . and ..
        if(ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
           (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) {
            sce_paf_private_memset(&ent, 0, sizeof(ent));
            continue;
        }

        if(FIO_S_ISDIR(ent.d_stat.st_mode)) {
            // Skip directories starting with ._ (macOS metadata)
            if(ent.d_name[0] == '.' && ent.d_name[1] == '_') {
                sce_paf_private_memset(&ent, 0, sizeof(ent));
                continue;
            }
            // Recurse into subdirectory
            char newsub[128];
            if(subdir && subdir[0]) {
                sce_paf_private_snprintf(newsub, sizeof(newsub), "%s/%s", subdir, ent.d_name);
            } else {
                sce_paf_private_strncpy(newsub, ent.d_name, sizeof(newsub) - 1);
                newsub[sizeof(newsub) - 1] = '\0';
            }
            count += count_isos_recursive(dirpath, newsub, depth + 1);
        } else if(is_iso_ext(ent.d_name)) {
            // Skip files starting with ._ (macOS metadata files)
            if(ent.d_name[0] == '.' && ent.d_name[1] == '_') {
                sce_paf_private_memset(&ent, 0, sizeof(ent));
                continue;
            }
            count++;
        }
        sce_paf_private_memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
    return count;
}

// Collect ISO files recursively into cache
static int collect_isos_recursive(const char *dirpath, const char *subdir, int depth,
                                  char **paths, int max_count, int *idx) {
    if(depth > 2) return 0; // Limit recursion depth

    char fullpath[256];
    if(subdir && subdir[0]) {
        sce_paf_private_snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, subdir);
    } else {
        sce_paf_private_strncpy(fullpath, dirpath, sizeof(fullpath) - 1);
        fullpath[sizeof(fullpath) - 1] = '\0';
    }

    SceUID d = sceIoDopen(fullpath);
    if(d < 0) return 0;

    SceIoDirent ent;
    sce_paf_private_memset(&ent, 0, sizeof(ent));
    while(sceIoDread(d, &ent) > 0 && *idx < max_count) {
        // Skip . and ..
        if(ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
           (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) {
            sce_paf_private_memset(&ent, 0, sizeof(ent));
            continue;
        }

        if(FIO_S_ISDIR(ent.d_stat.st_mode)) {
            // Skip directories starting with ._ (macOS metadata)
            if(ent.d_name[0] == '.' && ent.d_name[1] == '_') {
                sce_paf_private_memset(&ent, 0, sizeof(ent));
                continue;
            }
            // Recurse into subdirectory
            char newsub[128];
            if(subdir && subdir[0]) {
                sce_paf_private_snprintf(newsub, sizeof(newsub), "%s/%s", subdir, ent.d_name);
            } else {
                sce_paf_private_strncpy(newsub, ent.d_name, sizeof(newsub) - 1);
                newsub[sizeof(newsub) - 1] = '\0';
            }
            collect_isos_recursive(dirpath, newsub, depth + 1, paths, max_count, idx);
        } else if(is_iso_ext(ent.d_name)) {
            // Skip files starting with ._ (macOS metadata files)
            if(ent.d_name[0] == '.' && ent.d_name[1] == '_') {
                kprintf("collect_isos_recursive: SKIPPING ._ file [%s]\n", ent.d_name);
                sce_paf_private_memset(&ent, 0, sizeof(ent));
                continue;
            }
            char raw[256];
            char norm[256];
            if(subdir && subdir[0]) {
                sce_paf_private_snprintf(raw, sizeof(raw), "/iso/%s/%s", subdir, ent.d_name);
            } else {
                sce_paf_private_snprintf(raw, sizeof(raw), "/iso/%s", ent.d_name);
            }
            normalize_game_path(raw, norm, sizeof(norm));
            paths[*idx] = sce_paf_private_malloc(sce_paf_private_strlen(norm) + 1);
            if(paths[*idx]) {
                sce_paf_private_strcpy(paths[*idx], norm);
                (*idx)++;
            }
        }
        sce_paf_private_memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
    return *idx;
}

// Collect ISO files recursively with mtime into IsoEntry array
static int collect_isos_with_mtime(const char *dirpath, const char *subdir, int depth,
                                   IsoEntry *entries, int max_count, int *idx) {
    if(depth > 2) return 0;

    char fullpath[256];
    if(subdir && subdir[0]) {
        sce_paf_private_snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, subdir);
    } else {
        sce_paf_private_strncpy(fullpath, dirpath, sizeof(fullpath) - 1);
        fullpath[sizeof(fullpath) - 1] = '\0';
    }

    SceUID d = sceIoDopen(fullpath);
    if(d < 0) return 0;

    SceIoDirent ent;
    sce_paf_private_memset(&ent, 0, sizeof(ent));
    while(sceIoDread(d, &ent) > 0 && *idx < max_count) {
        if(ent.d_name[0] == '.' && (ent.d_name[1] == '\0' ||
           (ent.d_name[1] == '.' && ent.d_name[2] == '\0'))) {
            sce_paf_private_memset(&ent, 0, sizeof(ent));
            continue;
        }

        if(FIO_S_ISDIR(ent.d_stat.st_mode)) {
            if(ent.d_name[0] == '.' && ent.d_name[1] == '_') {
                sce_paf_private_memset(&ent, 0, sizeof(ent));
                continue;
            }
            char newsub[128];
            if(subdir && subdir[0]) {
                sce_paf_private_snprintf(newsub, sizeof(newsub), "%s/%s", subdir, ent.d_name);
            } else {
                sce_paf_private_strncpy(newsub, ent.d_name, sizeof(newsub) - 1);
                newsub[sizeof(newsub) - 1] = '\0';
            }
            collect_isos_with_mtime(dirpath, newsub, depth + 1, entries, max_count, idx);
        } else if(is_iso_ext(ent.d_name)) {
            if(ent.d_name[0] == '.' && ent.d_name[1] == '_') {
                sce_paf_private_memset(&ent, 0, sizeof(ent));
                continue;
            }
            char raw[256];
            char norm[256];
            if(subdir && subdir[0]) {
                sce_paf_private_snprintf(raw, sizeof(raw), "/iso/%s/%s", subdir, ent.d_name);
            } else {
                sce_paf_private_snprintf(raw, sizeof(raw), "/iso/%s", ent.d_name);
            }
            normalize_game_path(raw, norm, sizeof(norm));
            entries[*idx].path = sce_paf_private_malloc(sce_paf_private_strlen(norm) + 1);
            if(entries[*idx].path) {
                sce_paf_private_strcpy(entries[*idx].path, norm);
                entries[*idx].mtime = ent.d_stat.sce_st_mtime;
                (*idx)++;
            }
        }
        sce_paf_private_memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(d);
    return *idx;
}

static int build_iso_cache(int location) {
    IsoCache *cache = &iso_cache[location];
    free_iso_cache(location);

    char dirpath[128];
    sce_paf_private_strcpy(dirpath, "xx0:/ISO");
    SET_DEVICENAME(dirpath, location == INTERNAL_STORAGE ? INTERNAL_STORAGE : MEMORY_STICK);

    // Count ISOs recursively (includes subdirectories)
    int count = count_isos_recursive(dirpath, NULL, 0);

    if(count <= 0) {
        return 0;
    }

    cache->paths = sce_paf_private_malloc(count * sizeof(char *));
    if(!cache->paths) {
        cache->count = 0;
        return 0;
    }
    sce_paf_private_memset(cache->paths, 0, count * sizeof(char *));

    // Collect ISOs recursively
    int idx = 0;
    collect_isos_recursive(dirpath, NULL, 0, cache->paths, count, &idx);

    cache->count = idx;
    cache->built = 1;
    if(cache->count > 1) {
        sort_strings(cache->paths, cache->count);
    }
    kprintf("build_iso_cache: found %d ISOs for location %d\n", cache->count, location);
    // Dump first few entries for debugging
    for(int i = 0; i < cache->count && i < 5; i++) {
        kprintf("build_iso_cache: [%d] = %s\n", i, cache->paths[i]);
    }
    if(cache->count > 5) {
        kprintf("build_iso_cache: ... and %d more\n", cache->count - 5);
    }
    return cache->count;
}

// Free the global combined cache
static void free_global_iso_cache(void) {
    if(global_iso_cache.paths) {
        int i;
        for(i = 0; i < global_iso_cache.count; ++i) {
            if(global_iso_cache.paths[i]) sce_paf_private_free(global_iso_cache.paths[i]);
        }
        sce_paf_private_free(global_iso_cache.paths);
    }
    global_iso_cache.paths = NULL;
    global_iso_cache.count = 0;
    global_iso_cache.built = 0;
}

// Build a combined global cache from both ef0 and ms0
// The firmware uses global indices across both devices
static int build_global_iso_cache(void) {
    free_global_iso_cache();

    // Count ISOs from both devices
    char dirpath_ef[128], dirpath_ms[128];
    sce_paf_private_strcpy(dirpath_ef, "ef0:/ISO");
    sce_paf_private_strcpy(dirpath_ms, "ms0:/ISO");

    int count_ef = count_isos_recursive(dirpath_ef, NULL, 0);
    int count_ms = count_isos_recursive(dirpath_ms, NULL, 0);
    int total = count_ef + count_ms;

    kprintf("build_global_iso_cache: ef0=%d, ms0=%d, total=%d\n", count_ef, count_ms, total);

    if(total <= 0) {
        return 0;
    }

    global_iso_cache.paths = sce_paf_private_malloc(total * sizeof(char *));
    if(!global_iso_cache.paths) {
        global_iso_cache.count = 0;
        return 0;
    }
    sce_paf_private_memset(global_iso_cache.paths, 0, total * sizeof(char *));

    // Collect ISOs from both devices
    int idx = 0;
    collect_isos_recursive(dirpath_ef, NULL, 0, global_iso_cache.paths, total, &idx);
    collect_isos_recursive(dirpath_ms, NULL, 0, global_iso_cache.paths, total, &idx);

    global_iso_cache.count = idx;
    global_iso_cache.built = 1;

    // Sort the combined list by filename (to match firmware behavior)
    if(global_iso_cache.count > 1) {
        sort_strings(global_iso_cache.paths, global_iso_cache.count);
    }

    kprintf("build_global_iso_cache: found %d total ISOs\n", global_iso_cache.count);
    for(int i = 0; i < global_iso_cache.count && i < 10; i++) {
        kprintf("build_global_iso_cache: [%d] = %s\n", i, global_iso_cache.paths[i]);
    }
    if(global_iso_cache.count > 10) {
        kprintf("build_global_iso_cache: ... and %d more\n", global_iso_cache.count - 10);
    }
    // Log any cache entries that contain filter keywords to help debug
    // Cache paths are already lowercase, so these searches should match
    for(int i = 0; i < global_iso_cache.count; i++) {
        const char *p = global_iso_cache.paths[i];
        if(strstr(p, "half-minute") || strstr(p, "midnight") ||
           strstr(p, "spider") || strstr(p, "vcs ") || strstr(p, "vcs.")) {
            kprintf("build_global_iso_cache: FILTER_MATCH [%d] = %s\n", i, p);
        }
    }

    // Also log all filter entries so we can compare
    kprintf("build_global_iso_cache: FILTER ENTRIES for ms0 (loc=0):\n");
    if(game_filter_data[0]) {
        int c = game_counter[0];
        char *buf = game_filter_data[0];
        char norm[256];
        while(c > 0) {
            while(!*buf) buf++;
            normalize_game_path(buf, norm, sizeof(norm));
            if(is_iso_ext(norm)) {
                kprintf("  FILTER: %s\n", norm);
            }
            buf += sce_paf_private_strlen(buf);
            c--;
        }
    }
    return global_iso_cache.count;
}

// Build per-device ISO cache
static int build_device_iso_cache(int location) {
    IsoCache *cache = &iso_cache[location];
    free_iso_cache(location);

    char dirpath[128];
    sce_paf_private_strcpy(dirpath, "xx0:/ISO");
    SET_DEVICENAME(dirpath, location == INTERNAL_STORAGE ? INTERNAL_STORAGE : MEMORY_STICK);

    int count = count_isos_recursive(dirpath, NULL, 0);
    if(count <= 0) {
        kprintf("build_device_iso_cache: no ISOs found for location %d\n", location);
        return 0;
    }

    // Allocate temp array for entries with mtime
    IsoEntry *entries = sce_paf_private_malloc(count * sizeof(IsoEntry));
    if(!entries) {
        cache->count = 0;
        return 0;
    }
    sce_paf_private_memset(entries, 0, count * sizeof(IsoEntry));

    // Collect ISOs with their mtimes
    int idx = 0;
    collect_isos_with_mtime(dirpath, NULL, 0, entries, count, &idx);

    if(idx <= 0) {
        sce_paf_private_free(entries);
        cache->count = 0;
        return 0;
    }

    // Sort by mtime descending (most recent first) to match XMB ordering
    if(idx > 1) {
        sort_entries_by_mtime_desc(entries, idx);
    }

    // Now build the final paths array from sorted entries
    cache->paths = sce_paf_private_malloc(idx * sizeof(char *));
    if(!cache->paths) {
        // Free entry paths
        for(int i = 0; i < idx; i++) {
            if(entries[i].path) sce_paf_private_free(entries[i].path);
        }
        sce_paf_private_free(entries);
        cache->count = 0;
        return 0;
    }

    // Transfer paths from entries to cache->paths (sorted order)
    for(int i = 0; i < idx; i++) {
        cache->paths[i] = entries[i].path;  // Transfer ownership
    }
    sce_paf_private_free(entries);  // Free entries array but not the strings

    cache->count = idx;
    cache->built = 1;
    kprintf("build_device_iso_cache: location %d has %d ISOs (sorted by mtime desc)\n", location, cache->count);
    for(int i = 0; i < cache->count && i < 10; i++) {
        kprintf("  [%d] = %s\n", i, cache->paths[i]);
    }
    return cache->count;
}

static int resolve_isogame_token(int location, const char *category_name, const char *token_hex,
                                 char *out_path, int out_size) {
    if(!out_path || out_size <= 0) return 0;
    out_path[0] = '\0';
    if(!token_hex || !*token_hex) return 0;

    u32 idx = 0;
    if(!parse_hex_u32(token_hex, &idx)) {
        kprintf("resolve_isogame_token: failed to parse hex [%s]\n", token_hex);
        return 0;
    }

    // Track expected index range to detect when firmware resets its indexing
    // If we see an index way outside our expected range, rebuild caches
    static u32 last_idx_seen[2] = {0, 0};
    static u32 expected_idx_base[2] = {0, 0};

    if(iso_cache[location].built && last_idx_seen[location] > 0) {
        // Check if this index is drastically different from what we expect
        // If firmware resets (e.g., from 0x60 to 0x14), we need to rebuild
        int expected_min = (int)expected_idx_base[location];
        int expected_max = expected_min + iso_cache[location].count + 20; // some margin
        if((int)idx < expected_min - 5 || (int)idx > expected_max) {
            kprintf("resolve_isogame_token: idx %u outside expected range [%d-%d], rebuilding caches\n",
                    idx, expected_min, expected_max);
            free_iso_cache(INTERNAL_STORAGE);
            free_iso_cache(MEMORY_STICK);
            free_global_iso_cache();
            last_idx_seen[location] = 0;
            expected_idx_base[location] = 0;
        }
    }

    // Build per-device caches if not already built
    // We need BOTH caches to calculate the offset for ms0
    if(!iso_cache[INTERNAL_STORAGE].built) {
        kprintf("resolve_isogame_token: building cache for ef0\n");
        build_device_iso_cache(INTERNAL_STORAGE);
    }
    if(!iso_cache[MEMORY_STICK].built) {
        kprintf("resolve_isogame_token: building cache for ms0\n");
        build_device_iso_cache(MEMORY_STICK);
        // Set expected base for ms0 based on ef0 count
        expected_idx_base[MEMORY_STICK] = iso_cache[INTERNAL_STORAGE].count + 1;
    }
    if(!iso_cache[INTERNAL_STORAGE].built) {
        expected_idx_base[INTERNAL_STORAGE] = 1;
    }

    // Track last seen index
    last_idx_seen[location] = idx;

    // Also ensure global cache is built for filename matching fallback
    if(!global_iso_cache.built || !global_iso_cache.paths) {
        build_global_iso_cache();
    }

    IsoCache *cache = &iso_cache[location];
    if(!cache->paths || cache->count <= 0) {
        kprintf("resolve_isogame_token: cache empty for location %d\n", location);
        return 0;
    }

    // Firmware uses GLOBAL 1-based indexing across both devices
    // ef0 ISOs get indices 1 to ef0_count
    // ms0 ISOs get indices (ef0_count + 1) to (ef0_count + ms0_count)
    int ef0_count = iso_cache[INTERNAL_STORAGE].count;
    int pos;

    if(location == MEMORY_STICK && ef0_count > 0) {
        // For ms0, subtract ef0 count to get the position in ms0-only cache
        pos = (int)idx - 1 - ef0_count;
        kprintf("resolve_isogame_token: ms0 adjustment: idx=%u - 1 - ef0_count(%d) = pos %d\n",
                idx, ef0_count, pos);
    } else {
        // For ef0 or when ef0 is empty, use simple 1-based indexing
        pos = (idx == 0) ? 0 : (int)idx - 1;
    }

    kprintf("resolve_isogame_token: location=%d, idx=%u, pos=%d, cache_count=%d\n",
            location, idx, pos, cache->count);

    if(pos < 0 || pos >= cache->count) {
        kprintf("resolve_isogame_token: idx %u (pos %d) out of range for location %d (has %d entries)\n",
                idx, pos, location, cache->count);
        return 0;
    }
    sce_paf_private_strncpy(out_path, cache->paths[pos], out_size - 1);
    out_path[out_size - 1] = '\0';
    kprintf("resolve_isogame_token: idx %u -> [%s]\n", idx, out_path);
    return 1;
}

// Count how many ISOs in the global cache are in the given category
static int count_isos_in_category(const char *category, int location) {
    (void)location; // Now using global cache
    if(!category || !*category) return 0;

    // Build global cache if not already built
    if(!global_iso_cache.built || !global_iso_cache.paths) {
        build_global_iso_cache();
    }

    IsoCache *cache = &global_iso_cache;
    if(!cache->paths) return 0;

    char prefix[128];
    sce_paf_private_snprintf(prefix, sizeof(prefix), "/iso/%s/", category);
    int prefix_len = sce_paf_private_strlen(prefix);
    for(int i = 0; i < prefix_len; i++) {
        prefix[i] = ascii_tolower(prefix[i]);
    }

    int count = 0;
    for(int i = 0; i < cache->count; i++) {
        if(sce_paf_private_strncmp(cache->paths[i], prefix, prefix_len) == 0) {
            count++;
        }
    }
    return count;
}

// Check if a filter entry is a filename-only entry (no path, just a .iso/.cso/.etc filename)
static int is_filename_only_filter(const char *filter) {
    if(!filter || !*filter) return 0;
    // If it starts with / and has path structure, it's a path
    if(filter[0] == '/') return 0;
    // Check if it's just a filename with ISO extension
    return is_iso_ext(filter);
}

// Check if an ISO path's filename matches a filter filename (case-insensitive)
static int filename_matches_filter(const char *iso_path, const char *filter_filename) {
    if(!iso_path || !filter_filename) return 0;
    const char *iso_fname = get_filename_from_path(iso_path);
    return strcasecmp_ascii(iso_fname, filter_filename) == 0;
}

// Check if the resolved ISO matches any filter entry by filename
// This is more robust than index-based matching since it doesn't depend on stable indices
static int check_iso_by_filename(const char *resolved_iso, int location) {
    if(!resolved_iso || !*resolved_iso) return 0;
    if(location < 0 || location > 1) return 0;

    int c = game_counter[location];
    char *buf = game_filter_data[location];
    if(!buf) return 0;

    const char *iso_filename = get_filename_from_path(resolved_iso);
    if(!iso_filename || !*iso_filename) return 0;

    kprintf("check_iso_by_filename: checking filename [%s]\n", iso_filename);

    char norm_filter[256];
    while(c) {
        while(!*buf) buf++;
        normalize_game_path(buf, norm_filter, sizeof(norm_filter));

        // Check if filter entry's filename matches
        const char *filter_filename = get_filename_from_path(norm_filter);
        if(filter_filename && strcasecmp_ascii(iso_filename, filter_filename) == 0) {
            kprintf("check_iso_by_filename: filename match [%s] == [%s]\n",
                    iso_filename, filter_filename);
            return 1;
        }
        buf += sce_paf_private_strlen(buf);
        c--;
    }
    kprintf("check_iso_by_filename: no filename match for [%s]\n", iso_filename);
    return 0;
}

// Check if any ISO in the given category matches a filter entry by filename
// This doesn't rely on @ISOGAME@ index resolution at all
static int check_category_has_filtered_iso(const char *category, int location) {
    if(!category || !*category) return 0;
    if(location < 0 || location > 1) return 0;

    // Build global cache if needed
    if(!global_iso_cache.built || !global_iso_cache.paths) {
        build_global_iso_cache();
    }

    if(!global_iso_cache.paths || global_iso_cache.count <= 0) return 0;

    // Build the category prefix to match
    char prefix[128];
    sce_paf_private_snprintf(prefix, sizeof(prefix), "/iso/%s/", category);
    int prefix_len = sce_paf_private_strlen(prefix);
    for(int i = 0; i < prefix_len; i++) {
        prefix[i] = ascii_tolower(prefix[i]);
    }

    // For each ISO in the category, check if its filename is filtered
    for(int i = 0; i < global_iso_cache.count; i++) {
        if(sce_paf_private_strncmp(global_iso_cache.paths[i], prefix, prefix_len) == 0) {
            // This ISO is in the category - check if it's filtered
            if(check_iso_by_filename(global_iso_cache.paths[i], location)) {
                kprintf("check_category_has_filtered_iso: category [%s] has filtered ISO\n", category);
                return 1;
            }
        }
    }

    // Also check ISOs in root /iso/ (no category subfolder)
    char root_prefix[] = "/iso/";
    int root_prefix_len = 5;
    for(int i = 0; i < global_iso_cache.count; i++) {
        // Check if it's in root /iso/ (not in a subfolder)
        if(sce_paf_private_strncmp(global_iso_cache.paths[i], root_prefix, root_prefix_len) == 0) {
            const char *after_iso = global_iso_cache.paths[i] + root_prefix_len;
            // If there's no more '/' after /iso/, it's in the root
            if(strchr(after_iso, '/') == NULL) {
                if(check_iso_by_filename(global_iso_cache.paths[i], location)) {
                    kprintf("check_category_has_filtered_iso: root ISO is filtered, cat=[%s]\n", category);
                    return 1;
                }
            }
        }
    }

    return 0;
}

// Check if any ISO filter entry matches the given category
// Returns 1 if the category has only ONE hidden ISO (safe to hide the @ISOGAME@ entry)
// Returns 0 if there are multiple ISOs (unsafe - we'd hide the wrong one)
static int check_iso_filter_for_category(const char *category, int location) {
    if(!category || !*category) return 0;
    if(location < 0 || location > 1) return 0;

    int c = game_counter[location];
    char *buf = game_filter_data[location];
    if(!buf) return 0;

    // Build the expected prefix: /iso/category/
    char prefix[128];
    sce_paf_private_snprintf(prefix, sizeof(prefix), "/iso/%s/", category);
    int prefix_len = sce_paf_private_strlen(prefix);

    // Lowercase the prefix for comparison
    for(int i = 0; i < prefix_len; i++) {
        prefix[i] = ascii_tolower(prefix[i]);
    }

    // Count how many filter entries match this category
    int filter_count = 0;
    char norm_filter[256];
    char *scan_buf = buf;
    int scan_c = c;
    while(scan_c) {
        while(!*scan_buf) scan_buf++;
        normalize_game_path(scan_buf, norm_filter, sizeof(norm_filter));
        if(sce_paf_private_strncmp(norm_filter, prefix, prefix_len) == 0) {
            filter_count++;
            kprintf("check_iso_filter_for_category: found filter [%s] in category [%s]\n",
                    norm_filter, category);
        }
        scan_buf += sce_paf_private_strlen(scan_buf);
        scan_c--;
    }

    if(filter_count == 0) {
        return 0;  // No hidden ISOs in this category
    }

    // Check how many ISOs are in this category in the cache
    int cache_count = count_isos_in_category(category, location);
    kprintf("check_iso_filter_for_category: category [%s] has %d filters, %d cached ISOs\n",
            category, filter_count, cache_count);

    // If there's only 1 ISO in the category and 1 filter, safe to hide
    if(cache_count == 1 && filter_count >= 1) {
        kprintf("check_iso_filter_for_category: single ISO in category, safe to hide\n");
        return 1;
    }

    // If all ISOs in category are filtered, safe to hide
    if(cache_count > 0 && filter_count >= cache_count) {
        kprintf("check_iso_filter_for_category: all %d ISOs filtered, safe to hide\n", cache_count);
        return 1;
    }

    kprintf("check_iso_filter_for_category: %d/%d ISOs filtered, NOT safe to hide blindly\n",
            filter_count, cache_count);
    return 0;
}

#endif // GCLITE_HIDE_ISO

static const char *skip_device_prefix(const char *path) {
    if(path && path[0] && path[1] && path[2] && path[3] == ':') {
        return path + 4;
    }
    return path;
}

static void normalize_game_path(const char *in, char *out, int out_size) {
    if(out_size <= 0) return;
    if(!in) {
        out[0] = '\0';
        return;
    }
    const char *p = skip_device_prefix(in);
    int pos = 0;
    if(p && *p != '/' && out_size > 1) {
        out[pos++] = '/';
    }
    while(p && *p && pos < out_size - 1) {
        char c = *p++;
        if(c == '\\') c = '/';
        out[pos++] = ascii_tolower(c);
    }
    out[pos] = '\0';
}

// Compare strings ignoring trailing spaces
static int strcmp_trim(const char *a, const char *b) {
    int len_a = sce_paf_private_strlen(a);
    int len_b = sce_paf_private_strlen(b);
    // Trim trailing spaces from a
    while(len_a > 0 && a[len_a - 1] == ' ') len_a--;
    // Trim trailing spaces from b
    while(len_b > 0 && b[len_b - 1] == ' ') len_b--;
    // Compare lengths first
    if(len_a != len_b) return 1;
    // Compare content
    return sce_paf_private_strncmp(a, b, len_a);
}

static int category_names_match(const char *filter, const char *name) {
    if(strcmp_trim(filter, name) == 0) return 1;
    // normalize CAT_ and/or NN prefixes for both strings
    const char *a = filter;
    const char *b = name;
    if(config.prefix) {
        if(sce_paf_private_strncmp(a, "CAT_", 4) == 0) a += 4;
        if(sce_paf_private_strncmp(b, "CAT_", 4) == 0) b += 4;
    }
    if(config.catsort) {
        if(a[0] >= '0' && a[0] <= '9' && a[1] >= '0' && a[1] <= '9') a += 2;
        if(b[0] >= '0' && b[0] <= '9' && b[1] >= '0' && b[1] <= '9') b += 2;
    }
    return strcmp_trim(a, b) == 0;
}

int check_game_filter_for(const char *path, int location) {
    if(location < 0 || location > 1) return 0;
    int c = game_counter[location];
    char *buf = game_filter_data[location];
    char norm_path[256];
    char norm_filter[256];

    if(buf == NULL) return 0;
    normalize_game_path(path, norm_path, sizeof(norm_path));
    if(!*norm_path) return 0;
    // ISO-like entries are not hidden by gclite_filter (reduces XMB category overhead)
    if(is_isogame_token_path(norm_path) || is_iso_path(norm_path)) {
        kprintf("check_game_filter_for: skipping ISO entry [%s]\n", path);
        return 0;
    }
    const char *norm_leaf = strrchr(norm_path, '/');
    norm_leaf = norm_leaf ? (norm_leaf + 1) : norm_path;
    kprintf("checking <<%s>> in game filter\n", path);
    while(c) {
        while(!*buf) buf++;
        if(is_legacy_name_only_entry(buf)) {
            // Legacy one-name-per-line behavior (v1.7 style), case-insensitive.
            // Applies across all categories/devices by matching only the folder leaf.
            kprintf("veryfing legacy-name %s vs leaf %s\n", buf, norm_leaf);
            if(ascii_equals_ignore_case(buf, norm_leaf)) {
                kprintf("legacy name match for [%s]\n", path);
                return 1;
            }
        } else {
            normalize_game_path(buf, norm_filter, sizeof(norm_filter));
            kprintf("veryfing %s\n", norm_filter);
            if(sce_paf_private_strcmp(norm_filter, norm_path) == 0) {
                kprintf("match for [%s]\n", path);
                return 1;
            }
        }
        buf += sce_paf_private_strlen(buf);
        c--;
    }
    kprintf("no match: norm=%s\n", norm_path);
    return 0;
}

int check_category_filter_for(const char *name, int location) {
    if(location < 0 || location > 1) return 0;
    int c = category_counter[location];
    char *buf = category_filter_data[location];

    if(buf == NULL) return 0;
    kprintf("checking <<%s>> in category filter\n", name);
    while(c) {
        while(!*buf) buf++;
        kprintf("veryfing %s\n", buf);
        if(category_names_match(buf, name)) {
            kprintf("match for [%s]\n", name);
            return 1;
        }
        buf += sce_paf_private_strlen(buf);
        c--;
    }
    return 0;
}

void unload_filter() {
    for(int i = 0; i < 2; i++) {
        if(game_filter_data[i] != NULL) {
            sce_paf_private_free(game_filter_data[i]);
            game_filter_data[i] = NULL;
            game_counter[i] = 0;
            game_filter_size[i] = 0;
        }
        if(category_filter_data[i] != NULL) {
            sce_paf_private_free(category_filter_data[i]);
            category_filter_data[i] = NULL;
            category_counter[i] = 0;
            category_filter_size[i] = 0;
        }
#if GCLITE_HIDE_ISO
        free_iso_cache(i);
#endif
    }
#if GCLITE_HIDE_ISO
    free_global_iso_cache();
#endif
}

static int stricmp_ascii(const char *a, const char *b) {
    while(*a && *b) {
        char ca = ascii_tolower(*a++);
        char cb = ascii_tolower(*b++);
        if(ca != cb) return ca - cb;
    }
    return ascii_tolower(*a) - ascii_tolower(*b);
}

static int is_space_char(char c) {
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static void trim_inplace(char *s) {
    if(!s) return;
    char *start = s;
    while(*start && is_space_char(*start)) start++;
    if(start != s) {
        char *d = s;
        while(*start) *d++ = *start++;
        *d = '\0';
    }
    int len = sce_paf_private_strlen(s);
    while(len > 0 && is_space_char(s[len - 1])) s[--len] = '\0';
}

static int parse_device_token(const char *tok) {
    if(!tok || !*tok) return -1;
    char a = ascii_tolower(tok[0]);
    char b = ascii_tolower(tok[1]);
    char c = ascii_tolower(tok[2]);
    if(a == 'm' && b == 's' && c == '0') return MEMORY_STICK;
    if(a == 'e' && b == 'f' && c == '0') return INTERNAL_STORAGE;
    return -1;
}

static int parse_device_value(char *line, int *location, char *value, int value_size) {
    if(!line || !location || !value || value_size <= 0) return 0;
    char *sep = strchr(line, ',');
    if(!sep) {
        if(line[3] == ':' || line[3] == ' ' || line[3] == '\t') sep = line + 3;
    }
    if(!sep) return 0;
    *sep = '\0';
    char *dev = line;
    char *val = sep + 1;
    trim_inplace(dev);
    trim_inplace(val);
    if(!*dev || !*val) return 0;
    if(dev[3] == ':') dev[3] = '\0';
    int loc = parse_device_token(dev);
    if(loc < 0) return 0;
    sce_paf_private_strncpy(value, val, value_size - 1);
    value[value_size - 1] = '\0';
    *location = loc;
    return 1;
}

static int parse_device_path(char *line, int *location, char *value, int value_size) {
    if(!line || !location || !value || value_size <= 0) return 0;
    char *sep = strchr(line, ',');
    if(sep) {
        *sep = '\0';
        char *dev = line;
        char *val = sep + 1;
        trim_inplace(dev);
        trim_inplace(val);
        if(!*dev || !*val) return 0;
        if(dev[3] == ':') dev[3] = '\0';
        int loc = parse_device_token(dev);
        if(loc < 0) return 0;
        sce_paf_private_strncpy(value, val, value_size - 1);
        value[value_size - 1] = '\0';
        *location = loc;
        return 1;
    }
    trim_inplace(line);
    if(sce_paf_private_strlen(line) >= 4 && line[3] == ':') {
        char dev[4];
        dev[0] = line[0];
        dev[1] = line[1];
        dev[2] = line[2];
        dev[3] = '\0';
        int loc = parse_device_token(dev);
        if(loc < 0) return 0;
        sce_paf_private_strncpy(value, line, value_size - 1);
        value[value_size - 1] = '\0';
        *location = loc;
        return 1;
    }
    if(sce_paf_private_strlen(line) >= 4 && (line[3] == ' ' || line[3] == '\t')) {
        char dev[4];
        dev[0] = line[0];
        dev[1] = line[1];
        dev[2] = line[2];
        dev[3] = '\0';
        int loc = parse_device_token(dev);
        if(loc < 0) return 0;
        char *val = line + 4;
        trim_inplace(val);
        if(!*val) return 0;
        sce_paf_private_strncpy(value, val, value_size - 1);
        value[value_size - 1] = '\0';
        *location = loc;
        return 1;
    }
    return 0;
}

static int has_device_prefix(const char *line) {
    if(!line) return 0;
    int len = sce_paf_private_strlen(line);
    if(len < 4) return 0;
    char dev[4];
    dev[0] = line[0];
    dev[1] = line[1];
    dev[2] = line[2];
    dev[3] = '\0';
    if(parse_device_token(dev) < 0) return 0;
    char sep = line[3];
    return (sep == ':' || sep == ',' || sep == ' ' || sep == '\t');
}

static int load_filters() {
    SceUID fd;
    int device_root = (model == 4) ? INTERNAL_STORAGE : MEMORY_STICK;

    unload_filter();
    kprintf("loading filters\n");
    sce_paf_private_strcpy(filebuf, "xx0:/seplugins/gclite_filter.txt");
    SET_DEVICENAME(filebuf, device_root);
    if((fd = sceIoOpen(filebuf, PSP_O_RDONLY, 0777)) < 0) {
        kprintf("filters not found\n");
        return -1;
    }

    SceSize sz = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    char *raw = sce_paf_private_malloc(sz);
    if(!raw) {
        sceIoClose(fd);
        return -1;
    }
    sceIoRead(fd, raw, sz);
    sceIoClose(fd);

    enum { SEC_NONE, SEC_BLACKLIST, SEC_HIDDEN_CATS, SEC_HIDDEN_APPS };
    int section = SEC_NONE;
    int has_sections = 0;
    int game_count[2] = {0, 0};
    int cat_count[2] = {0, 0};
    SceSize game_bytes[2] = {0, 0};
    SceSize cat_bytes[2] = {0, 0};

    char linebuf[512];
    // Detect whether this file uses v1.8 sectioned format.
    // If no known section headers exist, fall back to legacy v1.7-style
    // one-name-per-line hidden app entries.
    u32 pos = 0, start = 0;
    while(pos <= sz) {
        if(pos == sz || raw[pos] == '\n' || raw[pos] == '\r') {
            u32 len = (pos > start) ? (pos - start) : 0;
            if(len > 0) {
                u32 n = (len >= sizeof(linebuf)) ? (sizeof(linebuf) - 1) : len;
                sce_paf_private_memcpy(linebuf, raw + start, n);
                linebuf[n] = '\0';
                trim_inplace(linebuf);
                if(*linebuf) {
                    if(stricmp_ascii(linebuf, "===CATEGORIES RENAME BLACKLIST===") == 0 ||
                       stricmp_ascii(linebuf, "===HIDDEN CATEGORIES===") == 0 ||
                       stricmp_ascii(linebuf, "===HIDDEN APPS===") == 0) {
                        has_sections = 1;
                        break;
                    }
                }
            }
            if(pos + 1 < sz && raw[pos] == '\r' && raw[pos + 1] == '\n') pos++;
            start = pos + 1;
        }
        pos++;
    }

    if(!has_sections) {
        kprintf("gclite_filter legacy fallback mode: headerless list detected\n");
    }

    section = SEC_NONE;
    pos = 0;
    start = 0;
    while(pos <= sz) {
        if(pos == sz || raw[pos] == '\n' || raw[pos] == '\r') {
            u32 len = (pos > start) ? (pos - start) : 0;
            if(len > 0) {
                u32 n = (len >= sizeof(linebuf)) ? (sizeof(linebuf) - 1) : len;
                sce_paf_private_memcpy(linebuf, raw + start, n);
                linebuf[n] = '\0';
                trim_inplace(linebuf);
                if(*linebuf) {
                    if(is_comment_line(linebuf)) {
                        // ignore
                    } else if(stricmp_ascii(linebuf, "===CATEGORIES RENAME BLACKLIST===") == 0) section = SEC_BLACKLIST;
                    else if(stricmp_ascii(linebuf, "===HIDDEN CATEGORIES===") == 0) section = SEC_HIDDEN_CATS;
                    else if(stricmp_ascii(linebuf, "===HIDDEN APPS===") == 0) section = SEC_HIDDEN_APPS;
                    else if(section == SEC_HIDDEN_CATS) {
                        int loc; char val[256];
                        if(parse_device_value(linebuf, &loc, val, sizeof(val))) {
                            cat_count[loc]++;
                            cat_bytes[loc] += sce_paf_private_strlen(val) + 1;
                        } else if(!has_device_prefix(linebuf)) {
                            int l = sce_paf_private_strlen(linebuf);
                            if(l > 0) {
                                cat_count[MEMORY_STICK]++;
                                cat_count[INTERNAL_STORAGE]++;
                                cat_bytes[MEMORY_STICK] += l + 1;
                                cat_bytes[INTERNAL_STORAGE] += l + 1;
                            }
                        }
                    } else if(section == SEC_HIDDEN_APPS) {
                        int loc; char val[256];
                        if(parse_device_path(linebuf, &loc, val, sizeof(val))) {
                            game_count[loc]++;
                            game_bytes[loc] += sce_paf_private_strlen(val) + 1;
                        } else if(!has_device_prefix(linebuf)) {
                            int l = sce_paf_private_strlen(linebuf);
                            if(l > 0) {
                                game_count[MEMORY_STICK]++;
                                game_count[INTERNAL_STORAGE]++;
                                game_bytes[MEMORY_STICK] += l + 1;
                                game_bytes[INTERNAL_STORAGE] += l + 1;
                            }
                        }
                    } else if(!has_sections) {
                        int loc; char val[256];
                        if(parse_device_path(linebuf, &loc, val, sizeof(val))) {
                            game_count[loc]++;
                            game_bytes[loc] += sce_paf_private_strlen(val) + 1;
                        } else if(!has_device_prefix(linebuf)) {
                            int l = sce_paf_private_strlen(linebuf);
                            if(l > 0) {
                                game_count[MEMORY_STICK]++;
                                game_count[INTERNAL_STORAGE]++;
                                game_bytes[MEMORY_STICK] += l + 1;
                                game_bytes[INTERNAL_STORAGE] += l + 1;
                            }
                        }
                    }
                }
            }
            if(pos + 1 < sz && raw[pos] == '\r' && raw[pos + 1] == '\n') pos++;
            start = pos + 1;
        }
        pos++;
    }

    for(int i = 0; i < 2; i++) {
        if(game_bytes[i] > 0) game_filter_data[i] = sce_paf_private_malloc(game_bytes[i]);
        if(cat_bytes[i] > 0) category_filter_data[i] = sce_paf_private_malloc(cat_bytes[i]);
        game_filter_size[i] = game_bytes[i];
        category_filter_size[i] = cat_bytes[i];
        game_counter[i] = game_count[i];
        category_counter[i] = cat_count[i];
    }

    char *game_ptr[2] = { game_filter_data[0], game_filter_data[1] };
    char *cat_ptr[2] = { category_filter_data[0], category_filter_data[1] };

    section = SEC_NONE;
    pos = 0; start = 0;
    while(pos <= sz) {
        if(pos == sz || raw[pos] == '\n' || raw[pos] == '\r') {
            u32 len = (pos > start) ? (pos - start) : 0;
            if(len > 0) {
                u32 n = (len >= sizeof(linebuf)) ? (sizeof(linebuf) - 1) : len;
                sce_paf_private_memcpy(linebuf, raw + start, n);
                linebuf[n] = '\0';
                trim_inplace(linebuf);
                if(*linebuf) {
                    if(is_comment_line(linebuf)) {
                        // ignore
                    } else if(stricmp_ascii(linebuf, "===CATEGORIES RENAME BLACKLIST===") == 0) section = SEC_BLACKLIST;
                    else if(stricmp_ascii(linebuf, "===HIDDEN CATEGORIES===") == 0) section = SEC_HIDDEN_CATS;
                    else if(stricmp_ascii(linebuf, "===HIDDEN APPS===") == 0) section = SEC_HIDDEN_APPS;
                    else if(section == SEC_HIDDEN_CATS) {
                        int loc; char val[256];
                        if(parse_device_value(linebuf, &loc, val, sizeof(val)) && cat_ptr[loc]) {
                            int l = sce_paf_private_strlen(val);
                            sce_paf_private_memcpy(cat_ptr[loc], val, l);
                            cat_ptr[loc][l] = '\0';
                            cat_ptr[loc] += l + 1;
                        } else if(!has_device_prefix(linebuf)) {
                            int l = sce_paf_private_strlen(linebuf);
                            if(l > 0) {
                                if(cat_ptr[MEMORY_STICK]) {
                                    sce_paf_private_memcpy(cat_ptr[MEMORY_STICK], linebuf, l);
                                    cat_ptr[MEMORY_STICK][l] = '\0';
                                    cat_ptr[MEMORY_STICK] += l + 1;
                                }
                                if(cat_ptr[INTERNAL_STORAGE]) {
                                    sce_paf_private_memcpy(cat_ptr[INTERNAL_STORAGE], linebuf, l);
                                    cat_ptr[INTERNAL_STORAGE][l] = '\0';
                                    cat_ptr[INTERNAL_STORAGE] += l + 1;
                                }
                            }
                        }
                    } else if(section == SEC_HIDDEN_APPS) {
                        int loc; char val[256];
                        if(parse_device_path(linebuf, &loc, val, sizeof(val)) && game_ptr[loc]) {
                            int l = sce_paf_private_strlen(val);
                            sce_paf_private_memcpy(game_ptr[loc], val, l);
                            game_ptr[loc][l] = '\0';
                            game_ptr[loc] += l + 1;
                        } else if(!has_device_prefix(linebuf)) {
                            int l = sce_paf_private_strlen(linebuf);
                            if(l > 0) {
                                if(game_ptr[MEMORY_STICK]) {
                                    sce_paf_private_memcpy(game_ptr[MEMORY_STICK], linebuf, l);
                                    game_ptr[MEMORY_STICK][l] = '\0';
                                    game_ptr[MEMORY_STICK] += l + 1;
                                }
                                if(game_ptr[INTERNAL_STORAGE]) {
                                    sce_paf_private_memcpy(game_ptr[INTERNAL_STORAGE], linebuf, l);
                                    game_ptr[INTERNAL_STORAGE][l] = '\0';
                                    game_ptr[INTERNAL_STORAGE] += l + 1;
                                }
                            }
                        }
                    } else if(!has_sections) {
                        int loc; char val[256];
                        if(parse_device_path(linebuf, &loc, val, sizeof(val)) && game_ptr[loc]) {
                            int l = sce_paf_private_strlen(val);
                            sce_paf_private_memcpy(game_ptr[loc], val, l);
                            game_ptr[loc][l] = '\0';
                            game_ptr[loc] += l + 1;
                        } else if(!has_device_prefix(linebuf)) {
                            int l = sce_paf_private_strlen(linebuf);
                            if(l > 0) {
                                if(game_ptr[MEMORY_STICK]) {
                                    sce_paf_private_memcpy(game_ptr[MEMORY_STICK], linebuf, l);
                                    game_ptr[MEMORY_STICK][l] = '\0';
                                    game_ptr[MEMORY_STICK] += l + 1;
                                }
                                if(game_ptr[INTERNAL_STORAGE]) {
                                    sce_paf_private_memcpy(game_ptr[INTERNAL_STORAGE], linebuf, l);
                                    game_ptr[INTERNAL_STORAGE][l] = '\0';
                                    game_ptr[INTERNAL_STORAGE] += l + 1;
                                }
                            }
                        }
                    }
                }
            }
            if(pos + 1 < sz && raw[pos] == '\r' && raw[pos + 1] == '\n') pos++;
            start = pos + 1;
        }
        pos++;
    }

    sce_paf_private_free(raw);
    kprintf("filters loaded: games ms=%i, games ef=%i, cats ms=%i, cats ef=%i\n",
            game_counter[MEMORY_STICK], game_counter[INTERNAL_STORAGE],
            category_counter[MEMORY_STICK], category_counter[INTERNAL_STORAGE]);
    return (game_counter[0] || game_counter[1] || category_counter[0] || category_counter[1]) ? 0 : -1;
}

int load_filter() {
    return load_filters();
}

int check_game_filter(const char *path) {
    return check_game_filter_for(path, global_pos);
}

int check_category_filter(const char *name) {
    return check_category_filter_for(name, global_pos);
}
