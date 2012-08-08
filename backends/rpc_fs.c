
#include "common/cache.h"
#include "common/connection.h"
#include "common/debugging.h"
#include "common/options.h"
#include "common/giga_index.h"
#include "common/rpc_giga.h"

#include "operations.h"

#include <errno.h>
#include <fcntl.h>
#include <rpc/rpc.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RPCFS_LOG   LOG_DEBUG

#define LOG_MSG(format, ...) \
    logMessage(RPCFS_LOG, __func__, format, __VA_ARGS__); 

static 
void update_client_mapping(struct giga_directory *dir, struct giga_mapping_t *map)
{
    giga_update_cache(&dir->mapping, map);
}

static 
int get_server_for_file(struct giga_directory *dir, const char *name)
{
    index_t index = giga_get_index_for_file(&(dir->mapping), name);
    //int server_id = (index + (&(dir->mapping))->zeroth_server) % 
    //                ((&(dir->mapping))->server_count);
    int server_id = giga_get_server_for_index(&dir->mapping, index);

    LOG_MSG("object[%s] goes to p[%d]-on-s[%d]", name, index, server_id);

    return server_id;

    //TODO: mask this call for better debugging: partition id and server id
    //return giga_get_server_for_file(&dir->mapping, name);
}

int rpc_init()
{
    int ret = 0;
    int server_id = 0;
    
    CLIENT *rpc_clnt = getConnection(server_id);
    giga_result_t rpc_reply;
    
    LOG_MSG(">>> RPC_init: s[%d]", server_id);

    if ((giga_rpc_init_1(giga_options_t.num_servers, &rpc_reply, rpc_clnt)) 
         != RPC_SUCCESS) {
        LOG_ERR("ERR_rpc_init(%s)", clnt_spcreateerror(""));
        exit(1);//TODO: retry again?
    }

    int errnum = rpc_reply.errnum;
    if (errnum == -EAGAIN) {
        int dir_id = 0; // update root server's bitmap
        struct giga_directory *dir = cache_fetch(&dir_id);
        if (dir == NULL) {
            LOG_MSG("ERR_cache: dir(%d) missing!", dir_id);
            ret = -EIO;
        }
        else {
            update_client_mapping(dir, &rpc_reply.giga_result_t_u.bitmap); 
            ret = 0;
        }
    } else if (errnum < 0) {
        ret = errnum;
    }
    
    LOG_MSG("<<< RPC_init: s[%d]", server_id);

    return ret;
}

int rpc_getattr(int dir_id, const char *path, struct stat *stbuf)
{
    int ret = 0;
    
    struct giga_directory *dir = cache_fetch(&dir_id);
    if (dir == NULL) {
        LOG_MSG("ERR_cache: dir(%d) missing!", dir_id);
        ret = -EIO;
    }
    
    int server_id = 0;
    giga_getattr_reply_t rpc_reply;

retry:
    server_id = get_server_for_file(dir, path);
    CLIENT *rpc_clnt = getConnection(server_id);

    LOG_MSG(">>> RPC_getattr(%s): to s[%d]", path, server_id);

    if (giga_rpc_getattr_1(dir_id, (char*)path, &rpc_reply, rpc_clnt) 
        != RPC_SUCCESS) {
        LOG_ERR("ERR_rpc_getattr(%s)", clnt_spcreateerror(path));
        exit(1);//TODO: retry again?
    }
    
    // check return condition 
    //
    ret = rpc_reply.result.errnum;
    if (ret == -EAGAIN) {
        update_client_mapping(dir, &rpc_reply.result.giga_result_t_u.bitmap); 
        LOG_MSG("bitmap update from s%d -- RETRY ...", server_id); 
        goto retry;
    }
    else if (ret < 0) {
        ;
    }
    else {
        *stbuf = rpc_reply.statbuf;
        if (stbuf == NULL)
            LOG_MSG("ERR_getattr(%s): statbuf is NULL!", path);
    }

    LOG_MSG("<<< RPC_getattr(%s): status=[%d]%s", path, ret, strerror(ret));
    
    return ret;
}

int rpc_mkdir(int dir_id, const char *path, mode_t mode)
{
    int ret = 0;
    
    struct giga_directory *dir = cache_fetch(&dir_id);
    if (dir == NULL) {
        LOG_MSG("ERROR_cache: dir(%d) missing!", dir_id);
        ret = -EIO;
    }
    
    int server_id = 0;
    giga_result_t rpc_reply;

retry:
    server_id = get_server_for_file(dir, path);
    CLIENT *rpc_clnt = getConnection(server_id);

    LOG_MSG(">>> RPC_mkdir(%s): to s%d]", path, server_id);

    if (giga_rpc_mkdir_1(dir_id, (char*)path, mode, &rpc_reply, rpc_clnt) 
        != RPC_SUCCESS) {
        LOG_ERR("ERR_rpc_mkdir(%s)", clnt_spcreateerror(path));
        exit(1);//TODO: retry again?
    }

    // check return condition 
    //
    ret = rpc_reply.errnum;
    if (ret == -EAGAIN) {
        update_client_mapping(dir, &rpc_reply.giga_result_t_u.bitmap);
        LOG_MSG("bitmap update from s%d -- RETRY ...", server_id); 
        goto retry;
    } else if (ret < 0) {
        ;
    } else {
        ret = 0;
    }

    LOG_MSG("<<< RPC_mkdir(%s): status=[%d]%s", path, ret, strerror(ret));
    
    return ret;
}

int rpc_mknod(int dir_id, const char *path, mode_t mode, dev_t dev)
{
    int ret = 0;
    
    struct giga_directory *dir = cache_fetch(&dir_id);
    if (dir == NULL) {
        LOG_MSG("ERR_cache: dir(%d) missing!", dir_id);
        ret = -EIO;
    }
    
    int server_id = 0;
    giga_result_t rpc_reply;

retry:
    server_id = get_server_for_file(dir, path);
    CLIENT *rpc_clnt = getConnection(server_id);

    LOG_MSG(">>> RPC_mknod(%s): to s[%d]", path, server_id);

    if (giga_rpc_mknod_1(dir_id, (char*)path, mode, dev, &rpc_reply, rpc_clnt) 
        != RPC_SUCCESS) {
        LOG_ERR("ERR_rpc_mknod(%s)", clnt_spcreateerror(path));
        exit(1);//TODO: retry again?
    }
    
    // check return condition 
    //
    ret = rpc_reply.errnum;
    if (ret == -EAGAIN) {
        update_client_mapping(dir, &rpc_reply.giga_result_t_u.bitmap);
        LOG_MSG("bitmap update from s%d -- RETRY ...", server_id); 
        goto retry;
    } else if (ret < 0) {
        ;
    } else {
        ret = 0;
    }


    LOG_MSG("<<< RPC_mknod(%s): status=[%d]%s", path, ret, strerror(ret));
    
    return ret;
}

struct readdir_args {
    int dir_id;
    int server_id;
};

pthread_mutex_t final_ls_mtx;
static scan_list_t final_ls_start;
static scan_list_t final_ls_end;

void *readdir_thread(void *params)
{
    int ret; 
    struct readdir_args *a = (struct readdir_args*) params;

    int dir_id = (int)a->dir_id;
    int server_id = (int)a->server_id;

    struct giga_directory *dir = cache_fetch(&dir_id);
    if (dir == NULL) {
        LOG_MSG("ERR_cache: dir(%d) missing!", dir_id);
        //ret = -EIO;
        pthread_exit(NULL);
    }
    
    readdir_return_t rpc_reply;
    memset(&rpc_reply, 0, sizeof(rpc_reply));
    scan_list_t ls;
    int more_ents_flag = 0;

    CLIENT *rpc_clnt;
retry:
    rpc_clnt = getConnection(server_id);

    LOG_MSG(">>> RPC_readdir(%d): to s[%d]", dir_id, server_id);

    scan_args_t args;
    args.dir_id = dir_id;
    args.partition_id = -1;
    args.start_key.scan_key_val = NULL;
    args.start_key.scan_key_len = 0;

    int ents=0;
    do {
        memset(&rpc_reply, 0, sizeof(rpc_reply));

        LOG_MSG("readdir_again with (d%d,p%d,key=[%s],len=%d)", 
                args.dir_id, args.partition_id, 
                args.start_key.scan_key_val, args.start_key.scan_key_len);

        if (giga_rpc_readdir_serial_1(args, &rpc_reply, rpc_clnt) != RPC_SUCCESS) {
            LOG_ERR("ERR_rpc_readdir(dirid=%d,s=%d)", dir_id, server_id);
            exit(1);//TODO: retry again?
        }

        // check return condition 
        //
        ret = rpc_reply.errnum;
        if (ret == -EAGAIN) {
            update_client_mapping(dir, &rpc_reply.readdir_return_t_u.bitmap);
            LOG_MSG("bitmap update from s%d -- RETRY ...", server_id); 
            goto retry;
        } else if (ret < 0) {
            ;
        } else {
            ACQUIRE_MUTEX(&final_ls_mtx, "readdir_result_mtx(%d)", server_id);
            if ((args.start_key.scan_key_val == NULL) && (final_ls_start == NULL))
                final_ls_start = rpc_reply.readdir_return_t_u.result.list;
            else 
                final_ls_end->next =rpc_reply.readdir_return_t_u.result.list;

            LOG_MSG("num_ents[s%d] = %d", server_id, rpc_reply.readdir_return_t_u.result.num_entries);
            for (ls=rpc_reply.readdir_return_t_u.result.list; ls!=NULL; ls=ls->next) {
                //LOG_MSG("dentry=[%s]", ls->entry_name);
                if (ls->next == NULL) {
                        final_ls_end = ls;
                }
            }
            RELEASE_MUTEX(&final_ls_mtx, "readdir_result_mtx(%d)", server_id);
            
            more_ents_flag = rpc_reply.readdir_return_t_u.result.more_entries_flag;

            if (more_ents_flag) {
                LOG_MSG("p=%d,end_key=[%s],len=%d", 
                        rpc_reply.readdir_return_t_u.result.end_partition,
                        rpc_reply.readdir_return_t_u.result.end_key.scan_key_val,
                        rpc_reply.readdir_return_t_u.result.end_key.scan_key_len);

                args.partition_id = rpc_reply.readdir_return_t_u.result.end_partition;
                args.start_key.scan_key_len = rpc_reply.readdir_return_t_u.result.end_key.scan_key_len;
                args.start_key.scan_key_val = strdup(rpc_reply.readdir_return_t_u.result.end_key.scan_key_val);
                args.start_key.scan_key_val[args.start_key.scan_key_len] = '\0';
               
            } else {
                update_client_mapping(dir, &rpc_reply.readdir_return_t_u.result.bitmap);
            }
            
        }

        ents += rpc_reply.readdir_return_t_u.result.num_entries;
    } while(more_ents_flag != 0);
   
    LOG_MSG("readdir[s%d]=%d", server_id, ents);

    //pthread_exit((void*)ret);
    pthread_exit(NULL);
}

scan_list_t rpc_readdir(int dir_id, const char *path)
{
    //scan_list_t final_ls_start = NULL;
    //scan_list_t final_ls_end = NULL;
    
    pthread_mutex_init(&final_ls_mtx, NULL);
    ACQUIRE_MUTEX(&final_ls_mtx, "readdir_result_mtx([%d][%s])", dir_id, path);
    final_ls_start = NULL;
    final_ls_end = NULL;
    RELEASE_MUTEX(&final_ls_mtx, "readdir_result_mtx([%d][%s])", dir_id, path);

    int max_servers = giga_options_t.num_servers;
    pthread_t tid[max_servers];
   
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    
    struct readdir_args args[max_servers];

    void *status;
    int i=0;
    for (i=0; i<max_servers; i++) { //#### start_for_loop for all partitions
        args[i].dir_id = dir_id;
        args[i].server_id = i;

        if (pthread_create(&tid[i], &attr, readdir_thread, (void *)&args[i])) {
            fprintf(stderr, "pthread_create() error: scan on s[%d]\n", i);
            exit(1);
        }
    }
    
    pthread_attr_destroy(&attr);
    for (i=0; i<max_servers; i++) { //#### start_for_loop for all partitions

        int ret = pthread_join(tid[i], &status);
        if (ret) {
            fprintf(stderr, "pthread_join(s[%d]) error: %d\n", i, ret);
            exit(1);
        }
        LOG_MSG("<<< readdir[%d]: status=[%ld]", i, (long)status);


    }

    LOG_MSG("<<< RPC_readdir(%s): return", path);
   
    return final_ls_start; 
}

#if 0
scan_list_t rpc_readdir(int dir_id, const char *path)
{
    int ret = 0;
    
    struct giga_directory *dir = cache_fetch(&dir_id);
    if (dir == NULL) {
        LOG_MSG("ERR_cache: dir(%d) missing!", dir_id);
        ret = -EIO;
    }
    
    scan_list_t final_ls_start = NULL;
    scan_list_t final_ls_end = NULL;
    
    int server_id = 0;

    giga_print_mapping(&dir->mapping);

    int partitions[MAX_GIGA_PARTITIONS] = {0};
    giga_get_all_partitions(&dir->mapping, partitions);

    int i=0;
    for (i=0; i<MAX_GIGA_PARTITIONS; i++) 
    {   //#### start_for_loop for all partitions

    if (partitions[i] == 0)
        continue;
    int p_id = i;

    readdir_return_t rpc_reply;
    memset(&rpc_reply, 0, sizeof(rpc_reply));
    scan_list_t ls;
    int more_ents_flag = 0;

retry:
    server_id = giga_get_server_for_index(&dir->mapping, p_id);
    //server_id = get_server_for_file(dir, path);
    CLIENT *rpc_clnt = getConnection(server_id);

    LOG_MSG(">>> RPC_readdir(%s): to s[%d]", path, server_id);

    scan_args_t args;
    args.dir_id = dir_id;
    args.partition_id = p_id;
    args.start_key.scan_key_val = NULL;
    args.start_key.scan_key_len = 0;
    
    do {
        memset(&rpc_reply, 0, sizeof(rpc_reply));

        LOG_MSG("readdir_again with (%d,%d,key=[%s],len=%d)", 
                args.dir_id, args.partition_id, 
                args.start_key.scan_key_val, args.start_key.scan_key_len);

        if (giga_rpc_readdir_serial_1(args, &rpc_reply, rpc_clnt) != RPC_SUCCESS) {
            LOG_ERR("ERR_rpc_readdir(%s)", clnt_spcreateerror(path));
            exit(1);//TODO: retry again?
        }

        // check return condition 
        //
        ret = rpc_reply.errnum;
        if (ret == -EAGAIN) {
            update_client_mapping(dir, &rpc_reply.readdir_return_t_u.bitmap);
            LOG_MSG("bitmap update from s%d -- RETRY ...", server_id); 
            goto retry;
        } else if (ret < 0) {
            ;
        } else {
            if ((args.start_key.scan_key_val == NULL) && (final_ls_start == NULL))
                final_ls_start = rpc_reply.readdir_return_t_u.result.list;
            else 
                final_ls_end->next =rpc_reply.readdir_return_t_u.result.list;

            LOG_MSG("num_ents = %d", rpc_reply.readdir_return_t_u.result.num_entries);
            for (ls=rpc_reply.readdir_return_t_u.result.list; ls!=NULL; ls=ls->next) {
                //LOG_MSG("dentry=[%s]", ls->entry_name);
                if (ls->next == NULL) 
                        final_ls_end = ls;
            }
            
            more_ents_flag = rpc_reply.readdir_return_t_u.result.more_entries_flag;

            if (more_ents_flag) {
                args.start_key.scan_key_len = rpc_reply.readdir_return_t_u.result.end_key.scan_key_len;
                args.start_key.scan_key_val = strdup(rpc_reply.readdir_return_t_u.result.end_key.scan_key_val);
                args.start_key.scan_key_val[args.start_key.scan_key_len] = '\0';
            } else {
                update_client_mapping(dir, &rpc_reply.readdir_return_t_u.result.bitmap);
                giga_get_all_partitions(&dir->mapping, partitions);
            }
            
        }
    } while(more_ents_flag != 0);

    } //### end_for_loop for all partitions

    LOG_MSG("<<< RPC_readdir(%s): status=[%d]%s", path, ret, strerror(ret));
   
    return final_ls_start; 
}
#endif

int rpc_releasedir(int dir_id, const char *path)
{
    int ret = 0;
    
    LOG_MSG(">>> RPC_releasedir(%s, d%d)", path, dir_id);
    LOG_MSG("<<< RPC_releasedir(%s): status=[%d]%s", path, ret, strerror(ret));

    return ret;
}

int rpc_opendir(int dir_id, const char *path)
{
    int ret = 0;
    
    LOG_MSG(">>> RPC_opendir(%s, d%d)", path, dir_id);
    LOG_MSG("<<< RPC_opendir(%s): status=[%d]%s", path, ret, strerror(ret));

    return ret;
}


/*
int local_symlink(const char *path, const char *link)
{
    int ret = 0;

    if ((ret = symlink(path, link)) < 0) {
        logMessage(LOG_FATAL, __func__,
                   "symlink(%s,%s) failed: %s", path, link, strerror(errno));
        ret = errno;
    }
    
    return ret;
}

int local_readlink(const char *path, char *link, size_t size)
{
    int ret = 0;

    if ((ret = readlink(path, link, size-1)) < 0) {
        logMessage(LOG_FATAL, __func__,
                   "readlink(%s,%s) failed: %s", path, link, strerror(errno));
        ret = errno;
    }
    else  {
	    link[ret] = '\0';
        ret = 0;
    }
    
    return ret;
}

// NOTE: ret_fd is to return the fd from the open call (needed for fi->fh)
int local_open(const char *path, int flags, int *ret_fd)
{
    int ret = 0;
    int fd;
    
    if ((fd = open(path, flags)) < 0) {
        logMessage(LOG_FATAL, __func__,
                   "open(%s) failed: %s", path, strerror(errno));
        ret = errno;
    }

    *ret_fd = fd;

    return ret;
}

int local_mknod(const char *path, mode_t mode, dev_t dev)
{
    int ret = 0;

    // On Linux this could just be 'mknod(path, mode, rdev)' but this
    //  is more portable
    if (S_ISREG(mode)) {
        if ((ret = open(path, O_CREAT | O_EXCL | O_WRONLY, mode) < 0)) {
            logMessage(LOG_FATAL, __func__,
                       "open(%s, CRT|EXCL|WR) failed: %s", path, strerror(errno));
            ret = (errno);
        }
        else {
            // close the opened file.
            if ((ret = close(ret)) < 0) {
                logMessage(LOG_FATAL, __func__,
                           "close(%d) failed: %s", ret, strerror(errno));
                ret = (errno);
            }
	    }
    } 
    else if (S_ISFIFO(mode)) {
	    if ((ret = mkfifo(path, mode)) < 0) {
            logMessage(LOG_FATAL, __func__,
                       "mkfifo(%s) failed: %s", path, strerror(errno));
            ret = (errno);
        }
	} else {
	    if ((ret = mknod(path, mode, dev)) < 0) {
            logMessage(LOG_FATAL, __func__,
                       "mknod(%s) failed: %s", path, strerror(errno));
            ret = (errno);
        }
	}

    return ret;
}
*/
