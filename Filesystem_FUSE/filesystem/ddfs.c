#include "config.h"
#include "params.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <openssl/sha.h>

#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif

#include "log.h"

#define BLOCK_SIZE 4096
#define MAX_FILES 10
#define MAX_FILE_SIZE 65536
#define MAX_BLOCKS 16

/* Get absolute path inside storage directory 
 * 
 * All paths are relative to mountdir.
 * This function takes a relative path in mountdir
 * and returns the absolute path in storage directory.
 * Storage directory is save in internal data (DD_DATA) in main
 */
static void dd_storagefullpath(char fpath[PATH_MAX], const char *path)
{
	int i = strlen(DD_DATA->storagedir) + 1 + strlen(path);
	strcpy(fpath, DD_DATA->storagedir);
	if (*path != '/') {
		strncat(fpath, "/", 1);
	}
	strncat(fpath, path, PATH_MAX); // ridiculously long paths will
	// break here
	fpath[i] = '\0';

	log_msg(
			"    dd_storagefullpath:  storageDir = \"%s\", path = \"%s\", \n        fpath = \"%s\"\n",
			DD_DATA->storagedir, path, fpath);
}

/* Update reference counter in block file
 *
 * Open the block file, reads ref counter and writes 
 * new value based on operation.
 * 
 * path is the path to block file
 * op is the operation, either '+' or '-'
 * returns 1 for errors else 0
 */
int update_refcnt(char *path, char op)
{
	int fd, nread, nwritten;
	int ref_cnt;

	fd = open(path, O_RDWR);
	if (fd == -1) {
		log_error(
				"update_refcnt: while opening block file to update ref counter.\n");
		return -1;
	}
	/* skip block data and ',' to reach ref counter */
	lseek(fd, BLOCK_SIZE + 1, SEEK_SET);

	nread = read(fd, &ref_cnt, sizeof(int));
	if (nread == -1) {
		log_error(
				"update_refcnt: while reading reference counter.\n");
		close(fd);
		return -1;
	}

	lseek(fd, -nread, SEEK_END);
	/* in case ref counter reaches 0, remove block file */
	if (op == '-') {
		ref_cnt--;
		if (ref_cnt == 0) {
			close(fd);
			if (unlink(path) != 0) {
				log_error(
						"update_refcnt: while unlinking block file.\n");
				return -1;
			}
			return 0;
		}
	} else {
		ref_cnt++;
	}
	nwritten = write(fd, &ref_cnt,
			sizeof(int));
	if (nwritten == -1) {
		log_error(
				"update_refcnt: while updating reference counter.\n");
		close(fd);
		return -1;
	}
	close(fd);

	return 0;
}

/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int dd_getattr(const char *path, struct stat *statbuf)
{
	int retstat;
	char fpath[PATH_MAX];

	log_msg("\ndd_getattr(path=\"%s\", statbuf=0x%08x)\n",
			path, statbuf);
	dd_storagefullpath(fpath, path);

	retstat = log_syscall("lstat", lstat(fpath, statbuf), 0);

	if (strcmp(path, "/") > 0) { /* only for files inside mountdir */
		// fix size according to blocks
		int fd, nread;
		char block_hash[SHA_DIGEST_LENGTH + 1];
		int block_cnt = 0;

		/* count blocks for this file */
		fd = open(fpath, O_RDONLY);
		if (fd != -1) {
			while ((nread = read(fd, block_hash,
					SHA_DIGEST_LENGTH + 1)) != 0)
			{
				if (nread < SHA_DIGEST_LENGTH + 1)
						{
					log_error(
							"dd_getattr, while reading block hashes.\n");
					retstat = -1;
					break;
				}
				block_cnt++;
			}
		}

		/* update stat values to represent the actual size */
		statbuf->st_blocks = block_cnt;
		statbuf->st_size = block_cnt * BLOCK_SIZE;
	}

	log_stat(statbuf);
	return retstat;
}

/** Create a file node
 *
 * There is no create() operation, mknod() will be called for
 * creation of all non-directory, non-symlink nodes.
 */
int dd_mknod(const char *path, mode_t mode, dev_t dev)
{
	int retstat;
	char fpath[PATH_MAX];

	log_msg("\ndd_mknod(path=\"%s\", mode=0%3o, dev=%lld)\n",
			path, mode, dev);
	dd_storagefullpath(fpath, path);

	if (S_ISREG(mode)) {
		retstat = log_syscall("open",
				open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode),
				0);
		if (retstat >= 0)
			retstat = log_syscall("close", close(retstat), 0);
	} else {
		log_error("dd_mknod: Only regural files are supported.");
		retstat = -1;
	}
	return retstat;
}

/* Covert_to_hex
 * 
 * Convert a binary digest (SHA1) to hex
 * hash is the digest in binary
 * hash_in_hex is the digest in hex form
 */
void convert_to_hex(unsigned char* hash, char* hash_in_hex)
{
	int i;
	char* p;

	p = &hash_in_hex[0];
	for (i = 0; i < SHA_DIGEST_LENGTH; i++, p += 2) {
		snprintf(p, 3, "%02x", hash[i]);
	}
	hash_in_hex[2 * SHA_DIGEST_LENGTH] = '\0';
}

/* dd_unlink 
 * 
 * Removes files. Also reduces ref counters in the block files
 * which consisted it. If reference counter reaches 0 for a block 
 * file, it is removed as we ll.
 */
int dd_unlink(const char *path)
{
	unsigned char hash_buffer[SHA_DIGEST_LENGTH];
	char hex_hash[2 * SHA_DIGEST_LENGTH + 1]; // \0
	char fpath[PATH_MAX];
	char block_path[PATH_MAX];
	int nread;
	int retstat;

	log_msg("dd_unlink(path=\"%s\")\n",
			path);
	dd_storagefullpath(fpath, path);

	/* open file to read block hashes */
	retstat = log_syscall("open", open(fpath, O_RDONLY), 0);

	if (retstat >= 0) { /* successfull open */
		/* read a hash and consume '\n' */
		while ((nread = read(retstat, hash_buffer, SHA_DIGEST_LENGTH))
				!= 0) {
			if (nread != SHA_DIGEST_LENGTH) {
				log_error("dd_unlink: while reading hash.");
				close(retstat);
				return -1;
			}
			lseek(retstat, 1, SEEK_CUR);
			convert_to_hex(hash_buffer, hex_hash);
			/* build block path */
			dd_storagefullpath(block_path, hex_hash);

			if (update_refcnt(block_path, '-') != 0) {
				close(retstat);
				return -1;
			}
		}
		/* remove file */
		return log_syscall("unlink", unlink(fpath), 0);
	}
	return retstat;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 */
int dd_open(const char *path, struct fuse_file_info *fi)
{
	int fd;
	int retstat = 0;
	char fpath[PATH_MAX];

	log_msg("\ndd_open(path\"%s\", fi=0x%08x)\n",
			path, fi);
	dd_storagefullpath(fpath, path);

	/* if open succeeds, retstat is the fd, else it's -errno.
	 * In that case the saved fd is -1 
	 */
	fd = log_syscall("open", open(fpath, O_RDWR), 0);
	if (fd < 0)
		retstat = log_error("dd_open: open");

	fi->fh = fd;
	log_fi(fi);

	return retstat;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 */
int dd_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	int nrblocks = size / BLOCK_SIZE; /* number of blocks */
	unsigned char block_hash[SHA_DIGEST_LENGTH + 1]; /* digest + \n */
	int i, j, k, fd2;
	int nread;
	char temp_hexhash[SHA_DIGEST_LENGTH * 2 + 1];
	int total_read = 0;
	char block_path[PATH_MAX];

	/* seek to first block's hash */
	lseek(fi->fh, (offset / BLOCK_SIZE) * (SHA_DIGEST_LENGTH + 1),
			SEEK_SET);

	log_msg(
			"\ndd_read(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
			path, buf, size, offset, fi);
	log_fi(fi);

	for (i = 0; i < nrblocks; i++) {
		/* read a hash which corresponds to a block filename in storagedir */
		nread = read(fi->fh, &block_hash[0], SHA_DIGEST_LENGTH + 1); //consume \n

		if (nread == 0) { /* EOF */
			return 0;
		}
		if (nread < SHA_DIGEST_LENGTH + 1) {
			log_error(
					"dd_read: while reading hash from hash file.");
			return -1;
		}
		block_hash[SHA_DIGEST_LENGTH] = '\0';

		/* convert hash to hex (this is the name of the block file in storagedir) */
		for (j = 0, k = 0; j < SHA_DIGEST_LENGTH; j++) {
			snprintf(&temp_hexhash[k], 3, "%02x", block_hash[j]);
			k += 2;
		}
		temp_hexhash[j * 2 + 1] = '\0';
		dd_storagefullpath(block_path, temp_hexhash);

		/* open block file and read block data */
		fd2 = log_syscall("open", open(block_path, O_RDONLY), 0);
		if (!fd2) {
			log_error("dd_read: opening block file.");
			return -1;
		}

		nread = log_syscall("read",
				read(fd2, &buf[4096 * i], BLOCK_SIZE), 4096);
		if (nread < BLOCK_SIZE) {
			log_error("dd_read: reading block data.\n");
			return -1;
		}
		total_read += nread;
		close(fd2);
	}
	log_msg("total_read: %d\n", total_read);
	return total_read;
}

/* update_hash
 * 
 * changes a line in the hash file with new hash
 * returns 0 on success, -1 otherwise
 */
int update_hash(unsigned char *new_hash, struct fuse_file_info *fi)
{
	int nwritten = 0;

	/* update hash in hash file */
	nwritten = write(fi->fh, new_hash,
			SHA_DIGEST_LENGTH);
	if (nwritten == -1) {
		log_error(
				"Error while updating hash.\n");
		return -1;
	}
	nwritten = write(fi->fh, "\n",
			1);
	if (nwritten == -1) {
		log_error(
				"Error while updating hash.\n");
		return -1;
	}
	return 0;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
int dd_write(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	unsigned char new_data_hash[SHA_DIGEST_LENGTH];
	char hex_hash[2 * SHA_DIGEST_LENGTH + 1]; // \0
	unsigned char hash_in_file[SHA_DIGEST_LENGTH];
	char block_path[PATH_MAX];
	int fd;
	int nwritten, nread, ref_cnt;
	int retstat = 0;
	errno = 0;

	log_msg(
			"\ndd_write(path=\"%s\", buf=0x%08x, size=%d, offset=%lld, fi=0x%08x)\n",
			path, buf, size, offset, fi);
	log_fi(fi);

	/* seek to corresponding hash in hash file */
	lseek(fi->fh, (offset / BLOCK_SIZE) * (SHA_DIGEST_LENGTH + 1),
			SEEK_SET);

	while (size > 0) {
		/* hash buf data */
		SHA1((const unsigned char *)buf, BLOCK_SIZE, new_data_hash);

		/* read from hash file (hash to be replaced) */
		nread = read(fi->fh, hash_in_file, SHA_DIGEST_LENGTH);
		if (nread == -1) {
			log_error("dd_write: open/create block file.");
			return -1;

		} else { // read was successful
			if (nread != 0) { //NOT EOF
				lseek(fi->fh, -nread, SEEK_CUR); //seek to the start of the hash in hash file

				if (!strncmp((char *)new_data_hash,
						(char *)hash_in_file,
						SHA_DIGEST_LENGTH)) { //same block, same offset
					log_msg(
							"    SAME BLOCK IN THE SAME LINE OF HASH FILE\n");
					retstat += BLOCK_SIZE;
					size -= BLOCK_SIZE;
					buf += BLOCK_SIZE;
					continue;
				} else {
					/* different hashes, decrement ref count in "old" block file*/
					convert_to_hex(hash_in_file, hex_hash);
					dd_storagefullpath(block_path,
							hex_hash);

					if (update_refcnt(block_path, '-')
							!= 0) {
						return -1;
					}
				}
			}

			/* open/create new block and write/update ref counter */
			convert_to_hex(new_data_hash, hex_hash);
			/* new block file path */
			dd_storagefullpath(block_path, hex_hash);

			// Attempt to open "new" block file based on buf data's digest (in hex)
			log_msg("\nopen(block_path=\"%s\")\n", block_path);
			fd = open(block_path, O_RDWR | O_CREAT | O_EXCL,
					0660);
			if (fd == -1) {
				if (errno == EEXIST) {
					// Block file already exists, increment refcnt and skip it
					log_msg(
							"BLOCK FILE already exists: %s\n",
							block_path);

					if (update_refcnt(block_path, '+')
							!= 0) {
						return -1;
					}

					size -= BLOCK_SIZE;
					retstat += BLOCK_SIZE;
					buf += BLOCK_SIZE;
					/* update hash in hash file */
					if (log_fcall("update_hash",
							update_hash(
									new_data_hash,
									fi), 0)
							!= 0) {
						return -1;
					}
					continue;
				}
				log_error(
						"Error while opening/creating block file.\n");
				return -1;
			}

			// Write block data "," 1 in block file
			nwritten = write(fd, buf, BLOCK_SIZE);
			if (nwritten == -1) {
				log_error(
						"Error while writting to block file.\n");
				close(fd);
				return -1;
			}
			nwritten = write(fd, ",", 1);
			if (nwritten == -1) {
				log_error(
						"Error while writting to block file.\n");
				close(fd);
				return -1;
			}
			ref_cnt = 1;
			nwritten = write(fd, &ref_cnt, sizeof(int));
			if (nwritten == -1) {
				log_error(
						"Error while writting to block file.\n");
				close(fd);
				return -1;
			}
			size = size - BLOCK_SIZE;
			retstat += BLOCK_SIZE;
			buf += BLOCK_SIZE;
			close(fd);

			/* update hash in hash file */
			if (log_fcall("update_hash",
					update_hash(new_data_hash, fi), 0)
					!= 0) {
				return -1;
			}

		}
	}
	return retstat;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 */
int dd_release(const char *path, struct fuse_file_info *fi)
{
	log_msg("\ndd_release(path=\"%s\", fi=0x%08x)\n",
			path, fi);
	log_fi(fi);

	// We need to close the file.  Had we allocated any resources
	// (buffers etc) we'd need to free them here as well.
	return log_syscall("close", close(fi->fh), 0);
}

/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 */
int dd_opendir(const char *path, struct fuse_file_info *fi)
{
	DIR *dp;
	int retstat = 0;
	char fpath[PATH_MAX];

	log_msg("\ndd_opendir(path=\"%s\", fi=0x%08x)\n",
			path, fi);
	dd_storagefullpath(fpath, path);

	// since opendir returns a pointer, takes some custom handling of
	// return status.
	dp = opendir(fpath);
	log_msg("    opendir returned 0x%p\n", dp);
	if (dp == NULL)
		retstat = log_error("dd_opendir opendir");

	fi->fh = (intptr_t)dp;

	log_fi(fi);

	return retstat;
}

/* islbock
 * 
 * Uses filename length and characters (hexdigits for hash filenames)
 * to decide whether given file is a normal file or a block 
 * 
 * Returns 1 if filename is used for a block, 0 otherwise.
 */
int isblock(char *entry)
{
	int i;
	// log_msg("\n    isblock: %s, len: %d", entry, strlen(entry));
	if (strlen(entry) == SHA_DIGEST_LENGTH * 2) {
		for (i = 0; i < SHA_DIGEST_LENGTH * 2; i++) {
			if (isxdigit(entry[i] == 0)) {
				// log_msg("\n        not a block file\n");
				return 0;
			} else {
				continue;
			}
		}
		// log_msg("\n        block file, ignoring\n");
		return 1;
	}
	// log_msg("\n        not a block file\n");
	return 0;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 */
int dd_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset,
		struct fuse_file_info *fi)
{
	int retstat = 0;
	DIR *dp;
	struct dirent *de;

	log_msg(
			"\ndd_readdir(path=\"%s\", buf=0x%08x, filler=0x%08x, offset=%lld, fi=0x%08x)\n",
			path, buf, filler, offset, fi);
	/* directory is already open and accessible through fi->fh */
	dp = (DIR *)(uintptr_t)fi->fh;

	/* if first call to readdir returns NULL there is an error */
	de = readdir(dp);
	log_msg("    readdir returned 0x%p\n", de);
	if (de == 0) {
		retstat = log_error("dd_readdir readdir");
		return retstat;
	}

	/* call filler function to copy the entire directory into the buffer */
	do {
		if (isblock(de->d_name)) /* skip block files */
			continue;
		log_msg("    calling filler with name %s\n", de->d_name);
		if (filler(buf, de->d_name, NULL, 0) != 0) {
			log_msg("    ERROR dd_readdir filler:  buffer full");
			return -ENOMEM;
		}
	} while ((de = readdir(dp)) != NULL); /* read the whole dir */

	log_fi(fi);
	return retstat;
}

/*
 * Remove all regular files in a directory
 * 
 * Returns 0 on success
 */
int clean_dir(const char *path)
{
	DIR *d = opendir(path);
	size_t len = strlen(path);
	int retstat = -1;

	if (d) {
		struct dirent *p;
		retstat = 0;

		while (!retstat && (p = readdir(d))) {
			int rmvres = -1;
			char *buf;

			/* Skip ""." and ".." */
			if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, ".."))
				continue;

			len = len + strlen(p->d_name) + 2; /* / and \0 */
			buf = (char *)malloc(sizeof(char) * len);

			if (buf) {
				snprintf(buf, len, "%s/%s", path,
						p->d_name);
				/* unlink current file */
				rmvres = unlink(buf);
			}
			free(buf);
			retstat = rmvres;
		}
		if (closedir(d) == -1) {
			log_error("clean_dir closedir");
			return -1;
		}
	}
	return retstat;
}

/*
 * Initialize filesystem
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 */
void *dd_init(struct fuse_conn_info *conn)
{
	log_msg("\ndd_init()\n");

	log_conn(conn);
	log_fuse_context (fuse_get_context());

	/* setup (clean) storage directory */
log_msg	("\nclean_dir(%s)\n", DD_DATA->storagedir);
	int retstat = log_fcall("clean_dir\n", clean_dir(DD_DATA->storagedir),
			0);
	if (retstat != 0) {
		abort();
	}
	return DD_DATA;
}

/* 
 * Clean up filesystem
 * Called on filesystem exit.
 */
void dd_destroy(void *userdata)
{
	log_msg("\ndd_destroy(userdata=0x%08x)\n", userdata);
	log_fcall("clean_dir", clean_dir(DD_DATA->storagedir), 0);
}

struct fuse_operations dd_oper = {
		.getattr = dd_getattr,
		.mknod = dd_mknod,
		.unlink = dd_unlink,
		.open = dd_open,
		.read = dd_read,
		.write = dd_write,
		.release = dd_release,
		.opendir = dd_opendir,
		.readdir = dd_readdir,
		.init = dd_init,
		.destroy = dd_destroy,
};

void dd_usage()
{
	fprintf(stderr,
			"usage:  ddfs [FUSE and mount options] mountdir storagedir\n");
	abort();
}

int main(int argc, char *argv[])
{
	int fuse_stat;
	struct dd_state *dd_data;

	/* 
	 * Check if root is trying to mount the filesystem and return if that's the case. 
	 * The somewhat smaller hole of an ordinary user doing it with the allow_other flag
	 * is still there.
	 */
	if ((getuid() == 0) || (geteuid() == 0)) {
		fprintf(stderr,
				"Running DDFS as root opens unnacceptable security holes\n");
		return 1;
	}

	/* Print fuse version */
	fprintf(stderr, "Fuse library version %d.%d\n", FUSE_MAJOR_VERSION,
			FUSE_MINOR_VERSION);

	/* argument checking */
	if ((argc < 3) || (argv[argc - 2][0] == '-')
			|| (argv[argc - 1][0] == '-'))
		dd_usage();

	dd_data = malloc(sizeof(struct dd_state));
	if (dd_data == NULL) {
		perror("main calloc");
		abort();
	}

	/* save storage directory in internal data */
	dd_data->storagedir = realpath(argv[argc - 1], NULL);
	argv[argc - 1] = NULL;
	argc--;

	dd_data->logfile = log_open();
	/* turn control over to fuse */
	fprintf(stderr, "about to call fuse_main\n");
	fuse_stat = fuse_main(argc, argv, &dd_oper, dd_data);
	fprintf(stderr, "fuse_main returned %d\n", fuse_stat);

	return fuse_stat;
}
