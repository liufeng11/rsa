#include "rsa.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOMEDIR_LEN 255
#define RSADIR_LEN 4
#define RSASUBDIR_LEN 3
#define FILENAME_PREFFIX_LEN 255
#define COUNTER_MAX_LEN 4
#define FILENAME_SUFFIX_LEN 4
#define RSAPATH_LEN (HOMEDIR_LEN + 1 + RSADIR_LEN + 1 + RSASUBDIR_LEN + 1 + \
    FILENAME_PREFFIX_LEN + COUNTER_MAX_LEN + FILENAME_SUFFIX_LEN)

#define ENV_VAR_PATH "RSA_PATH"
#define ENV_HOME_DIR "HOME"
#define RSA_DIR ".rsa"
#define RSA_PRV_DIR "prv"
#define RSA_PUB_DIR "pub"
#define LOCAL_DIR "./"

typedef int (* stat_func_t) (const char *fname, struct stat *buf);
typedef int (* io_func_t)(void *ptr, size_t size, size_t nmemb, FILE *stream);

static char rsa_path[RSAPATH_LEN];
#if RSA_MASTER || RSA_DECRYPTER
static char rsa_path_prv[RSAPATH_LEN];
#endif
#if RSA_MASTER || RSA_ENCRYPTER
static char rsa_path_pub[RSAPATH_LEN];
#endif

static int rsa_mkdir_single(char *path, char *new_dir)
{
    struct stat buf;
    char new_path[HOMEDIR_LEN + 1 + RSADIR_LEN];

    if (path && *path && stat(path, &buf))
	return -1;

    snprintf(new_path, sizeof(new_path), "%s/%s", path, new_dir);
    if (stat(new_path, &buf) && (mkdir(new_path, 0) || chmod(new_path, 
	S_IRWXU | S_IRWXG | S_IRWXO)))
    {
	return -1;
    }

    return 0;
}

static int rsa_mkdir_full(char *path)
{
    char buf[RSAPATH_LEN], *ptr = buf, *next = NULL;

    snprintf(buf, RSAPATH_LEN, "%s", path);
    if (*ptr == '/')
    {
	*ptr = 0;
	ptr++;
    }

    next = ptr;
    while (*ptr)
    {
	if (*ptr != '/')
	{
	    ptr++;
	    continue;
	}

	*ptr = 0;
	if (rsa_mkdir_single(buf, next))
	    return -1;
	*(buf + strlen(buf)) = '/';
	ptr++;
	next = ptr;
    }
    return 0;
}

int rsa_io_init(void)
{
    char *env_path;
    int ret = 0;

    if ((env_path = getenv(ENV_VAR_PATH)))
    {
	snprintf(rsa_path, HOMEDIR_LEN, "%s/", env_path);
	goto Exit;
    }
	
    snprintf(rsa_path, HOMEDIR_LEN, "%s/", getenv(ENV_HOME_DIR));
    if (HOMEDIR_LEN <= strlen(rsa_path) + strlen(RSA_DIR))
	return -1;

    strcat(rsa_path, RSA_DIR "/");

Exit:
#if RSA_MASTER || RSA_DECRYPTER
    snprintf(rsa_path_prv, RSAPATH_LEN, "%s%s/", rsa_path, RSA_PRV_DIR);
    ret += rsa_mkdir_full(rsa_path_prv);
#endif
#if RSA_MASTER || RSA_ENCRYPTER
    snprintf(rsa_path_pub, RSAPATH_LEN, "%s%s/", rsa_path, RSA_PUB_DIR);
    ret += rsa_mkdir_full(rsa_path_pub);
#endif

    return ret;
}

static char *rsa_file_name(char *path, char *preffix, char *suffix, 
    stat_func_t stat_f, int is_new)
{
    static char name_buf[RSAPATH_LEN]; 
    char name_counter[COUNTER_MAX_LEN], *counter_ptr = name_buf + 
	strlen(path) + strlen(preffix);
    int i = 1, file_exists;
    struct stat buf;

    if (strlen(preffix) > FILENAME_PREFFIX_LEN)
	return NULL;
    bzero(name_buf, sizeof(name_buf));
    sprintf(name_buf, path);
    strcat(name_buf, preffix);
    strcat(name_buf, suffix);

    while ((file_exists = !stat_f(name_buf, &buf)) && is_new)
    {
	if (snprintf(name_counter, COUNTER_MAX_LEN, "_%i", i++) >= 
	    COUNTER_MAX_LEN)
	{
	    return NULL;
	}
	*counter_ptr = 0;
	strcat(name_buf, name_counter);
	strcat(name_buf, suffix);
    }

    return (file_exists && !is_new) || (!file_exists && is_new) ? name_buf : 
	NULL;
}

FILE *rsa_file_open(char *path, char *preffix, char *suffix, int is_slink, 
    int is_new)
{
    char *fname = NULL;

    if (!(fname = rsa_file_name(path, preffix, suffix, is_slink ? lstat : 
	stat, is_new)))
    {
	return NULL;
    }

    return fopen(fname, is_new ? "w+" : "r+");
}

int rsa_file_close(FILE *fp)
{
    return fclose(fp);
}

#if RSA_MASTER || RSA_DECRYPTER
FILE *rsa_file_create(char *suffix)
{
    char *preffix;

#if RSA_MASTER
    preffix = "master";
#else
    preffix = SIG;
#endif
    return rsa_file_open(LOCAL_DIR, preffix, suffix, 0, 1);
}

FILE *rsa_file_create_private(void)
{
    return rsa_file_create(".prv");
}

FILE *rsa_file_create_public(void)
{
    return rsa_file_create(".pub");
}
#endif

static int rsa_file_io_u1024(FILE *fptr, void *buf, int is_half, int is_write)
{
    int nmemb = BYTES_SZ(u1024_t) / BYTES_SZ(u64);
    int size = sizeof(u64);
    io_func_t io_func = is_write ? (io_func_t)fwrite : (io_func_t)fread;

    if (is_half)
	nmemb /= 2;

    return io_func(buf, size, nmemb, fptr);
}

static int rsa_file_io_u1024_half(FILE *fptr, u1024_t *num, int is_write, 
    int is_hi)
{
    u64 *ptr = is_hi ? (u64 *)num + (BYTES_SZ(u1024_t)) / BYTES_SZ(u64) : 
	(u64 *)num;

    return rsa_file_io_u1024(fptr, (void *)ptr, 1, is_write);
}

int rsa_file_write_u1024_hi(FILE *fptr, u1024_t *num)
{
    return rsa_file_io_u1024_half(fptr, num, 1, 1);
}

int rsa_file_read_u1024_hi(FILE *fptr, u1024_t *num)
{
    return rsa_file_io_u1024_half(fptr, num, 0, 1);
}

int rsa_file_write_u1024_low(FILE *fptr, u1024_t *num)
{
    return rsa_file_io_u1024_half(fptr, num, 1, 0);
}

int rsa_file_read_u1024_low(FILE *fptr, u1024_t *num)
{
    return rsa_file_io_u1024_half(fptr, num, 0, 0);
}

int rsa_file_write_u1024(FILE *fptr, u1024_t *num)
{
    return rsa_file_io_u1024(fptr, (void *)fptr, 0, 1);
}

int rsa_file_read_u1024(FILE *fptr, u1024_t *num)
{
    return rsa_file_io_u1024(fptr, (void *)fptr, 0, 0);
}

int str2u1024_t(u1024_t *num, char *str)
{
    int i;

    number_reset(num);
    for (i = 0; i < (sizeof(u1024_t) / sizeof(char)) && str + i && *(str + i); 
	i++)
    {
	*((char *)num + i) = *(str + i);
    }

    return i;
}

int u1024_t2str(u1024_t *num, char *str)
{
    int i;

    bzero(str, sizeof(u1024_t) / sizeof(char));
    for (i = 0; i < (sizeof(u1024_t) / sizeof(char)) && *((char *)num + i); i++)
	*(str + i) = *((char *)num + i);

    return i;
}
