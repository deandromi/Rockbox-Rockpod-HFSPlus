/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2007 Nicolas Pennequin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <stdio.h>
#include "string-extra.h"
#include "system.h"
#include "albumart.h"
#include "metadata.h"
#include "buffering.h"
#include "dircache.h"
#include "misc.h"
#include "pathfuncs.h"
#include "settings.h"
#include "wps.h"
#ifndef PLUGIN
#include "usb.h"
#endif

/* Define LOGF_ENABLE to enable logf output in this file */
/*#define LOGF_ENABLE*/
#include "logf.h"

#if defined(HAVE_JPEG) || defined(PLUGIN)
#define USE_JPEG_COVER
#endif

#ifdef PLUGIN
    #define strmemccpy strlcpy
    /* Note we don't use the return value so this works */
    /* FIXME if strmemccpy gets added to the rb->plugin struct */
#endif

/* Strip filename from a full path
 *
 * buf      - buffer to extract directory to.
 * buf_size - size of buffer.
 * fullpath - fullpath to extract from.
 *
 * Split the directory part of the given fullpath and store it in buf
 *   (including last '/').
 * The function return parameter is a pointer to the filename
 *   inside the given fullpath.
 */
static char* strip_filename(char* buf, int buf_size, const char* fullpath)
{
    char* sep;
    int   len;

    if (!buf || buf_size <= 0 || !fullpath)
        return NULL;

    /* if 'fullpath' is only a filename return immediately */
    sep = strrchr(fullpath, '/');
    if (sep == NULL)
    {
        buf[0] = 0;
        return (char*)fullpath;
    }

    len = MIN(sep - fullpath + 1, buf_size - 1);
    strmemccpy(buf, fullpath, len + 1);
    return (sep + 1);
}

#ifdef USE_JPEG_COVER
static const char * const extensions[] = { "jpeg", "jpg", "bmp" };
static const unsigned char extension_lens[] = { 4, 3, 3 };
/* Try checking for several file extensions, return true if a file is found and
 * leaving the path modified to include the matching extension.
 */
static bool try_exts(char *path, int len)
{
    int i;
    for (i = 0; i < 3; i++)
    {
        if (extension_lens[i] + len > MAX_PATH)
            continue;
        strcpy(path + len, extensions[i]);
        if (file_exists(path))
            return true;
    }
    return false;
}
#define EXT
#else
#define EXT "bmp"
#define try_exts(path, len) file_exists(path)
#endif

/* Per-directory external album art cache.
 *
 * find_albumart() probes the same directory twice for every track (sized art
 * first, then the unsized fallback), and folder-based libraries repeat the
 * same positive or negative search for every track in the folder.  Cache the
 * directory-wide part of the search, keyed by directory, size slot, album and
 * effective artist.  The track-specific "<track><size>.ext" candidate is
 * deliberately not cached: it is still probed first and still wins where it
 * exists.
 *
 * The directory is part of the key, so a track in another folder can never
 * reuse an unrelated result.  The cache is additionally cleared whenever the
 * playback album art state is reset (folder change, stop/USB handoff); reboot
 * starts from empty BSS.  The cache is bounded and direct-mapped in a small
 * round-robin set; changing folders simply evicts old entries.
 */
#define AA_CACHE_SLOTS 4
#define AA_CACHE_TEXT_LEN 64

struct albumart_cache_entry
{
    bool valid;
    bool found;
    char dir[MAX_PATH + 1];
    char size[16];
    char album[AA_CACHE_TEXT_LEN];
    char artist[AA_CACHE_TEXT_LEN];
    char path[MAX_PATH + 1];
};

static struct albumart_cache_entry albumart_cache[AA_CACHE_SLOTS];
static unsigned int albumart_cache_next;
#ifndef PLUGIN
static bool albumart_cache_usb_inserted;
#endif

void albumart_cache_clear(void)
{
    for (int i = 0; i < AA_CACHE_SLOTS; i++)
        albumart_cache[i].valid = false;
    albumart_cache_next = 0;
}

/* USB can add/remove/replace art behind the cache; drop it on either
 * transition (not for the whole charging session, so caching still works
 * while plugged in). */
static void albumart_cache_poll_usb(void)
{
#ifndef PLUGIN
    bool inserted = (usb_detect() == USB_INSERTED);

    if (inserted != albumart_cache_usb_inserted)
    {
        albumart_cache_usb_inserted = inserted;
        albumart_cache_clear();
    }
#endif
}

static struct albumart_cache_entry *
albumart_cache_find(const char *dir, const char *size, const char *album,
                    const char *artist)
{
    for (int i = 0; i < AA_CACHE_SLOTS; i++)
    {
        struct albumart_cache_entry *e = &albumart_cache[i];

        if (e->valid && !strcmp(e->dir, dir) && !strcmp(e->size, size) &&
            !strcmp(e->album, album) && !strcmp(e->artist, artist))
            return e;
    }

    return NULL;
}

static void albumart_cache_store(const char *dir, const char *size,
                                 const char *album, const char *artist,
                                 bool found, const char *path)
{
    /* Do not truncate a key: an incomplete key could alias another album. */
    if (strlen(dir) > MAX_PATH || strlen(size) >= sizeof(albumart_cache[0].size)
        || strlen(album) >= AA_CACHE_TEXT_LEN
        || strlen(artist) >= AA_CACHE_TEXT_LEN
        || strlen(path) > MAX_PATH)
        return;

    struct albumart_cache_entry *e =
        &albumart_cache[albumart_cache_next++ % AA_CACHE_SLOTS];

    /* Invalidate before rewriting: a concurrent lookup must see either a
     * complete entry or a miss, never a half-updated key. */
    e->valid = false;
    strmemccpy(e->dir, dir, sizeof(e->dir));
    strmemccpy(e->size, size, sizeof(e->size));
    strmemccpy(e->album, album, sizeof(e->album));
    strmemccpy(e->artist, artist, sizeof(e->artist));
    strmemccpy(e->path, path, sizeof(e->path));
    e->found = found;
    e->valid = true;
}

/* Probe only the track-specific candidate.  The directory-wide candidates
 * live in search_albumart_generic() so their result can be cached. */
static bool search_track_albumart(const struct mp3entry *id3,
                                  const char *size_string,
                                  char *path, int path_size)
{
#ifdef USE_JPEG_COVER
    int pathlen;
#endif

    strip_extension(path, path_size - strlen(size_string) - 4, id3->path);
    strcat(path, size_string);
    strcat(path, "." EXT);
#ifdef USE_JPEG_COVER
    pathlen = strlen(path);
#endif
    return try_exts(path, pathlen);
}

/* Look for the directory-wide album art candidates, in the original order:
 *  ./<albumname><size>.{jpeg,jpg,bmp}
 *  ./cover<size>.bmp
 *  ../<albumname><size>.{jpeg,jpg,bmp}
 *  ../cover<size>.{jpeg,jpg,bmp}
 *  ROCKBOX_DIR/albumart/<artist>-<albumname><size>.{jpeg,jpg,bmp}
 */
static bool search_albumart_generic(const struct mp3entry *id3,
                                    const char *size_string,
                                    char *buf, int buflen)
{
    char path[MAX_PATH + 11]; /* need room for filename and null termination */
    char dir[MAX_PATH + 1];
    char key_dir[MAX_PATH + 1];
    bool found = false;
    const char *artist;
    const char *album;
    struct albumart_cache_entry *cached;
    int dirlen;
    int albumlen;
    int pathlen;

    strip_filename(dir, sizeof(dir), id3->path);
    dirlen = strlen(dir);
    albumlen = id3->album ? strlen(id3->album) : 0;
    artist = id3->albumartist != NULL ? id3->albumartist : id3->artist;
    album = id3->album ? id3->album : "";

    /* Keep the key before the parent-directory walk mutates dir. */
    strmemccpy(key_dir, dir, sizeof(key_dir));

    cached = albumart_cache_find(key_dir, size_string, album,
                                 artist ? artist : "");
    if (cached)
    {
        if (!cached->found)
            return false;

        strmemccpy(buf, cached->path, buflen);
        logf("Album art cached: %s", cached->path);
        return true;
    }

    if (albumlen > 0)
    {
        pathlen = snprintf(path, sizeof(path),
                           "%s%s%s." EXT, dir, id3->album, size_string);
        fix_path_part(path, dirlen, albumlen);
        found = try_exts(path, pathlen);
    }

    if (!found)
    {
        pathlen = snprintf(path, sizeof(path),
                           "%scover%s." EXT, dir, size_string);
        found = try_exts(path, pathlen);
    }

#ifdef USE_JPEG_COVER
    if (!found && !*size_string)
    {
        snprintf(path, sizeof(path), "%sfolder.jpg", dir);
        found = file_exists(path);
    }
#endif

    if (!found && artist && id3->album)
    {
        /* look in the albumart subdir of .rockbox */
        pathlen = snprintf(path, sizeof(path),
                           ROCKBOX_DIR "/albumart/%s-%s%s." EXT,
                           artist, id3->album, size_string);
        fix_path_part(path, strlen(ROCKBOX_DIR "/albumart/"), MAX_PATH);
        found = try_exts(path, pathlen);
    }

    /* only try parent if there is one */
    if (!found && dirlen > 0)
    {
        strcpy(path, dir);
        path[dirlen - 1] = 0;
        strip_filename(dir, sizeof(dir), path);
        dirlen = strlen(dir);

        if (dirlen > 0)
        {
            if (albumlen > 0)
            {
                /* we look in the parent directory
                 * for a file specific to the track's album name */
                pathlen = snprintf(path, sizeof(path),
                                   "%s%s%s." EXT, dir, id3->album,
                                   size_string);
                fix_path_part(path, dirlen, albumlen);
                found = try_exts(path, pathlen);
            }

            if (!found)
            {
                /* if it still doesn't exist, we look in the parent directory
                 * for a generic file */
                pathlen = snprintf(path, sizeof(path),
                                   "%scover%s." EXT, dir, size_string);
                found = try_exts(path, pathlen);
            }
        }
    }

    albumart_cache_store(key_dir, size_string, album,
                         artist ? artist : "", found, found ? path : "");

    if (!found)
        return false;

    strmemccpy(buf, path, buflen);
    logf("Album art found: %s", path);
    return true;
}

/* Look for the first matching album art bitmap in the following list:
 *  ./<trackname><size>.{jpeg,jpg,bmp}
 *  ./<albumname><size>.{jpeg,jpg,bmp}
 *  ./cover<size>.bmp
 *  ../<albumname><size>.{jpeg,jpg,bmp}
 *  ../cover<size>.{jpeg,jpg,bmp}
 *  ROCKBOX_DIR/albumart/<artist>-<albumname><size>.{jpeg,jpg,bmp}
 * <size> is the value of the size_string parameter, <trackname> and
 * <albumname> are read from the ID3 metadata.
 * If a matching bitmap is found, its filename is stored in buf.
 * Return value is true if a bitmap was found, false otherwise.
 *
 * If the first symbol in size_string is a colon (e.g. ":100x100")
 * then the colon is skipped ("100x100" will be used) and the track
 * specific image (./<trackname><size>.bmp) is tried last instead of first.
 */
bool search_albumart_files(const struct mp3entry *id3, const char *size_string,
                           char *buf, int buflen)
{
    char path[MAX_PATH + 11]; /* need room for filename and null termination */
    int track_first = 1;
    const char *trackname;

    if (!id3 || !buf)
        return false;

    albumart_cache_poll_usb();

    trackname = id3->path;

    if (strcmp(trackname, "No file!") == 0)
        return false;

    if (*size_string == ':')
    {
        size_string++;
        track_first = 0;
    }

    if (track_first &&
        search_track_albumart(id3, size_string, path, sizeof(path)))
    {
        strmemccpy(buf, path, buflen);
        logf("Album art found: %s", path);
        return true;
    }

    if (search_albumart_generic(id3, size_string, buf, buflen))
        return true;

    /* With a leading ':' the track-specific image is tried last. */
    if (!track_first &&
        search_track_albumart(id3, size_string, path, sizeof(path)))
    {
        strmemccpy(buf, path, buflen);
        logf("Album art found: %s", path);
        return true;
    }

    return false;
}

#ifndef PLUGIN
/* Look for albumart bitmap in the same dir as the track and in its parent dir.
 * Stores the found filename in the buf parameter.
 * Returns true if a bitmap was found, false otherwise */
bool find_albumart(const struct mp3entry *id3, char *buf, int buflen,
                    const struct dim *dim)
{
    if (!id3 || !buf)
        return false;

    char size_string[15];/* .-32768x-32768\0 */
    logf("Looking for album art for %s", id3->path);

    /* Write the size string, e.g. ".100x100". */
    snprintf(size_string, sizeof(size_string), ".%dx%d",
              dim->width, dim->height);

    /* First we look for a bitmap of the right size */
    if (search_albumart_files(id3, size_string, buf, buflen))
        return true;

    /* Then we look for generic bitmaps */
    *size_string = 0;
    return search_albumart_files(id3, size_string, buf, buflen);
}

#endif /* PLUGIN */
