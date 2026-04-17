

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}


void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}


int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    char header[64];
    const char *type_str;

    // Step 1: Convert type to string
    if (type == OBJ_BLOB) type_str = "blob";
    else if (type == OBJ_TREE) type_str = "tree";
    else if (type == OBJ_COMMIT) type_str = "commit";
    else return -1;

    // Step 2: Build header
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    // Step 3: Allocate full object (header + data)
    size_t total_len = header_len + len;
    char *full_obj = malloc(total_len);
    if (!full_obj) return -1;

    memcpy(full_obj, header, header_len);
    memcpy(full_obj + header_len, data, len);

    // Step 4: Compute hash
    compute_hash(full_obj, total_len, id_out);

    // Step 5: Check if already exists
    if (object_exists(id_out)) {
        free(full_obj);
        return 0;
    }

    // Step 6: Get path
    char path[512];
    object_path(id_out, path, sizeof(path));

    // Extract directory path
    char dir[512];
    strncpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = '\0';

    mkdir(OBJECTS_DIR, 0755);  // ensure base dir
    mkdir(dir, 0755);          // shard dir

    // Step 7: Temp file
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/tmpXXXXXX", dir);

    int fd = mkstemp(temp_path);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }

    // Step 8: Write data
    if (write(fd, full_obj, total_len) != (ssize_t)total_len) {
        close(fd);
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    fsync(fd);
    close(fd);

    // Step 9: Rename (atomic)
    if (rename(temp_path, path) < 0) {
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    free(full_obj);
    return 0;
}


int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    rewind(fp);

    char *buffer = malloc(file_size);
    if (!buffer) {
        fclose(fp);
        return -1;
    }

    if (fread(buffer, 1, file_size, fp) != (size_t)file_size) {
        fclose(fp);
        free(buffer);
        return -1;
    }
    fclose(fp);

    // Step 1: Verify hash
    ObjectID check_id;
    compute_hash(buffer, file_size, &check_id);

    if (memcmp(id->hash, check_id.hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }

    // Step 2: Find header/data split
    char *null_pos = memchr(buffer, '\0', file_size);
    if (!null_pos) {
        free(buffer);
        return -1;
    }

    size_t header_len = null_pos - buffer + 1;
    char type_str[10];
    size_t size;

    sscanf(buffer, "%s %zu", type_str, &size);

    // Step 3: Set type
    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1;
    }

    // Step 4: Extract data
    *data_out = malloc(size);
    if (!*data_out) {
        free(buffer);
        return -1;
    }

    memcpy(*data_out, buffer + header_len, size);
    *len_out = size;

    free(buffer);
    return 0;
}
