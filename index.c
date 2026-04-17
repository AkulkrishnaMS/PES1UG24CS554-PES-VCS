// index.c — Staging area implementation
// Akulkrishna M S | PES1UG24CS554

#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>


// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (size_t i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (size_t i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            size_t remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (size_t i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (size_t i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (size_t i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { 
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;
    FILE *fp = fopen(".pes/index", "r");
    if (!fp) return 0; // Empty index is fine

    char hash_hex[HASH_HEX_SIZE + 1];
    IndexEntry *e;

    while (index->count < MAX_INDEX_ENTRIES) {
        e = &index->entries[index->count];
        if (fscanf(fp, "%o %64s %ld %zu %255s\n",
                   &e->mode, hash_hex, &e->mtime_sec, &e->size, e->path) != 5) {
            break; // End of file or format mismatch
        }
        hex_to_hash(hash_hex, &e->hash);
        index->count++;
    }

    fclose(fp);
    return 0;
}

static int compare_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path, ((IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    char temp_path[] = ".pes/index.tmp";
    FILE *fp = fopen(temp_path, "w");
    if (!fp) return -1;

    // Dynamically allocate memory for sorting to prevent Stack Overflow!
    IndexEntry *sorted = malloc(index->count * sizeof(IndexEntry));
    if (!sorted) {
        fclose(fp);
        return -1;
    }
    
    memcpy(sorted, index->entries, index->count * sizeof(IndexEntry));
    qsort(sorted, index->count, sizeof(IndexEntry), compare_entries);

    char hash_hex[HASH_HEX_SIZE + 1];

    for (size_t i = 0; i < index->count; i++) {
        hash_to_hex(&sorted[i].hash, hash_hex);
        fprintf(fp, "%o %s %ld %zu %s\n",
                sorted[i].mode, hash_hex, (long)sorted[i].mtime_sec, sorted[i].size, sorted[i].path);
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    free(sorted);

    rename(temp_path, ".pes/index");
    return 0;
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1; // File doesn't exist

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    size_t size = st.st_size;
    void *data = malloc(size > 0 ? size : 1); // Protect against malloc(0)
    
    if (size > 0) {
        if (fread(data, 1, size, fp) != size) {
            free(data);
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);

    ObjectID id;
    if (object_write(OBJ_BLOB, data, size, &id) != 0) {
        free(data);
        return -1;
    }
    free(data);

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
    }

    // Git specific modes: 100755 for executable, 100644 for regular
    if (st.st_mode & S_IXUSR) {
        e->mode = 0100755;
    } else {
        e->mode = 0100644;
    }
    
    e->hash = id;
    e->mtime_sec = st.st_mtime;
    e->size = st.st_size;
    strcpy(e->path, path);

    return index_save(index);
}
