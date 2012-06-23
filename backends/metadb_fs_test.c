#include "operations.h"
#include <assert.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>


#define MAX_FILENAME_LEN 1024
#define MAX_NUM_ENTRIES 100000
#define FILE_FORMAT "%016lx"

#define ASSERT(x) \
  if (!((x))) { fprintf(stderr, "%s %d failed\n", __FILE__, __LINE__); }

int num_print_entries;
char* entry_list[MAX_NUM_ENTRIES];

static
void print_meta_obj_key(metadb_key_t *mkey) {
    printf("%ld, %ld, ", mkey->parent_id, mkey->partition_id);
    int i;
    for (i = 0; i < HASH_LEN; ++i)
        printf("%c", mkey->name_hash[i]);
    printf("\n");
}
static
void print_entries(void* buf, metadb_key_t* iter_key, metadb_val_t* iter_obj) {
    if (entry_list[num_print_entries] != NULL) {
        memcpy(entry_list[num_print_entries], iter_key, sizeof(metadb_key_t));
    }
    ++num_print_entries;
    if (iter_obj != NULL && buf == NULL)
        print_meta_obj_key(iter_key);
}
static
void init_meta_obj_key(metadb_key_t *mkey,
                       metadb_inode_t dir_id,
                       int partition_id, const char* path)
{
    mkey->parent_id = dir_id;
    mkey->partition_id = partition_id;
    memset(mkey->name_hash, 0, sizeof(mkey->name_hash));
    giga_hash_name(path, mkey->name_hash);
}

void run_test(int nargs, char* args[]) {
    if (nargs < 4) {
        return;
    }

    char* dbname = args[1];
    char* dbname2 = args[2];
    char* extname = args[3];

    struct MetaDB mdb;
    struct MetaDB mdb2;

    metadb_init(&mdb, dbname);
    metadb_init(&mdb2, dbname2);

    int ret;
    int dir_id = 0;
    int partition_id = 0;
    int new_partition_id = 1;

    char filename[MAX_FILENAME_LEN];
    char backup[MAX_FILENAME_LEN];
    struct stat statbuf;
    int num_migrated_entries = 0;
    metadb_inode_t i = 0;

    snprintf(filename, MAX_FILENAME_LEN, "%08x", 10000);
    struct giga_mapping_t mybitmap;

    ASSERT(metadb_create(mdb, 0, 0, OBJ_DIR, 0, "/", "/") == 0);

    ret = metadb_read_bitmap(mdb, 0, 0, "/", &mybitmap);
    ASSERT(ret == 0);

    mybitmap.curr_radix = 0;
    mybitmap.zeroth_server = 10;
    mybitmap.server_count = 21;

    ret = metadb_write_bitmap(mdb, 0, 0, "/", &mybitmap);
    ASSERT(ret == 0);

    memset(&mybitmap, 0, sizeof(mybitmap));
    ret = metadb_read_bitmap(mdb, 0, 0, "/", &mybitmap);
    ASSERT(ret == 0);
    ASSERT(mybitmap.curr_radix == 0);
    ASSERT(mybitmap.zeroth_server == 10);
    ASSERT(mybitmap.server_count == 21);

    metadb_lookup(mdb, dir_id, partition_id, filename, &statbuf);

    size_t num_test_entries = MAX_NUM_ENTRIES;

    for (i = 0; i < num_test_entries; ++i) {
        memset(filename, 0, sizeof(filename));
        snprintf(filename, MAX_FILENAME_LEN, FILE_FORMAT, i);
        memset(backup, 0, sizeof(filename));
        snprintf(backup, MAX_FILENAME_LEN, FILE_FORMAT, i);

        ASSERT(metadb_create(mdb, dir_id, partition_id, OBJ_DIR, i,
                             filename, filename) == 0);
        metadb_lookup(mdb, dir_id, partition_id, filename, &statbuf);
        ASSERT(statbuf.st_ino == i);
        if (giga_file_migration_status(backup, new_partition_id)) {
            ++num_migrated_entries;
        }
    }

    for (i = 0; i < num_test_entries; ++i) {
        memset(filename, 0, sizeof(filename));
        snprintf(filename, MAX_FILENAME_LEN, FILE_FORMAT, i);
        memset(backup, 0, sizeof(filename));
        snprintf(backup, MAX_FILENAME_LEN, FILE_FORMAT, i);

        ASSERT(metadb_lookup(mdb, dir_id, partition_id, filename, &statbuf) == 0);
        ASSERT(statbuf.st_ino == i);
    }

    printf("moved entries: %d \n", num_migrated_entries);

    num_print_entries = 0;
    ASSERT(metadb_readdir(mdb, dir_id, partition_id, NULL, print_entries) == 0);

    uint64_t min_seq, max_seq;
    ret = metadb_extract_do(mdb, dir_id, partition_id,
                                new_partition_id, extname,
                                &min_seq, &max_seq);
    printf("extract entries: %d\n", ret);
    ASSERT(num_migrated_entries == ret);

    printf("extname: %s\n", extname);
    ASSERT(metadb_bulkinsert(mdb2, extname, min_seq, max_seq) == 0);

    ret = metadb_extract_clean(mdb);
    ASSERT(ret == 0);

    num_print_entries = 0;
    int k = 0;
    for (k = 0; k < MAX_NUM_ENTRIES; ++k)
        entry_list[k] = (char *) malloc(sizeof(metadb_key_t));

    ASSERT(metadb_readdir(mdb2, dir_id, new_partition_id, NULL, print_entries) == 0);

    printf("%d, %d, %ld\n", num_migrated_entries, num_print_entries, max_seq);
    ASSERT(num_migrated_entries == num_print_entries);

    printf("\n\n");

    num_print_entries = 0;
    ASSERT(metadb_readdir(mdb2, dir_id, new_partition_id, NULL, print_entries) == 0);
    ASSERT(num_migrated_entries == num_print_entries);

    printf("\n\n");

    int num_found_entries = 0;
    metadb_key_t testkey;

    for (i = 0; i < num_test_entries; ++i) {
        memset(filename, 0, sizeof(filename));
        snprintf(filename, MAX_FILENAME_LEN, FILE_FORMAT, i);

        init_meta_obj_key(&testkey, dir_id, new_partition_id, filename);
        print_meta_obj_key(&testkey);

        ret = metadb_lookup(mdb2, dir_id, new_partition_id, filename, &statbuf);
        if (ret == 0) {
            ASSERT(statbuf.st_ino == i);
            ++num_found_entries;
        }
    }

    for (k = 0; k < MAX_NUM_ENTRIES; ++k)
        free(entry_list[k]);
    printf("%d %d\n", num_migrated_entries, num_found_entries);
    ASSERT(num_migrated_entries == num_found_entries);

    for (i = num_test_entries; i < num_test_entries*2; ++i) {
        memset(filename, 0, sizeof(filename));
        snprintf(filename, MAX_FILENAME_LEN, FILE_FORMAT, i);
        memset(backup, 0, sizeof(filename));
        snprintf(backup, MAX_FILENAME_LEN, FILE_FORMAT, i);

        ASSERT(metadb_create(mdb2, dir_id, partition_id, OBJ_DIR, i,
                             filename, filename) == 0);
        metadb_lookup(mdb2, dir_id, partition_id, filename, &statbuf);
        ASSERT(statbuf.st_ino == i);
    }

    for (i = num_test_entries; i < num_test_entries*4; ++i) {
        memset(filename, 0, sizeof(filename));
        snprintf(filename, MAX_FILENAME_LEN, FILE_FORMAT, i);
        memset(backup, 0, sizeof(filename));
        snprintf(backup, MAX_FILENAME_LEN, FILE_FORMAT, i);

        ASSERT(metadb_create(mdb, dir_id, partition_id, OBJ_DIR, i,
                             filename, filename) == 0);
        metadb_lookup(mdb, dir_id, partition_id, filename, &statbuf);
        ASSERT(statbuf.st_ino == i);
    }

    metadb_close(mdb);
    metadb_close(mdb2);
}

int main(int nargs, char* args[]) {
    run_test(nargs, args);
    return 0;
}
