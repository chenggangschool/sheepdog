/*
 * Copyright (C) 2011 Taobao Inc.
 *
 * Liu Yuan <namei.unix@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <dirent.h>
#include <pthread.h>
#include <linux/limits.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/xattr.h>

#include "farm.h"
#include "sheep_priv.h"
#include "sheepdog_proto.h"
#include "sheep.h"

char farm_obj_dir[PATH_MAX];
char farm_dir[PATH_MAX];

static int create_directory(char *p)
{
	int i, ret = 0;
	struct strbuf buf = STRBUF_INIT;

	strbuf_addstr(&buf, p);
	strbuf_addstr(&buf, ".farm");
	if (mkdir(buf.buf, 0755) < 0) {
		if (errno != EEXIST) {
			eprintf("%m\n");
			ret = -1;
			goto err;
		}
	}

	if (!strlen(farm_dir))
		strbuf_copyout(&buf, farm_dir, sizeof(farm_dir));

	strbuf_addstr(&buf, "/objects");
	if (mkdir(buf.buf, 0755) < 0) {
		if (errno != EEXIST) {
			eprintf("%m\n");
			ret = -1;
			goto err;
		}
	}
	for (i = 0; i < 256; i++) {
		strbuf_addf(&buf, "/%02x", i);
		if (mkdir(buf.buf, 0755) < 0) {
			if (errno != EEXIST) {
				eprintf("%m\n");
				ret = -1;
				goto err;
			}
		}
		strbuf_remove(&buf, buf.len - 3, 3);
	}

	if (!strlen(farm_obj_dir))
		strbuf_copyout(&buf, farm_obj_dir, sizeof(farm_obj_dir));
err:
	strbuf_release(&buf);
	return ret;
}

static int write_last_sector(int fd, uint32_t length)
{
	const int size = SECTOR_SIZE;
	char *buf;
	int ret;
	off_t off = length - size;

	buf = valloc(size);
	if (!buf) {
		eprintf("failed to allocate memory\n");
		return SD_RES_NO_MEM;
	}
	memset(buf, 0, size);

	ret = xpwrite(fd, buf, size, off);
	if (ret != size)
		ret = SD_RES_EIO;
	else
		ret = SD_RES_SUCCESS;
	free(buf);

	return ret;
}

/*
 * Preallocate the whole object to get a better filesystem layout.
 */
int prealloc(int fd, uint32_t size)
{
	int ret = fallocate(fd, 0, 0, size);
	if (ret < 0) {
		if (errno != ENOSYS && errno != EOPNOTSUPP) {
			dprintf("%m\n");
			ret = SD_RES_SYSTEM_ERROR;
		} else
			ret = write_last_sector(fd, size);
	} else
		ret = SD_RES_SUCCESS;
	return ret;
}

static int get_trunk_sha1(uint32_t epoch, unsigned char *outsha1)
{
	int i, nr_logs = -1, ret = -1;
	struct snap_log *log_buf, *log_free = NULL;
	void *snap_buf = NULL;
	struct sha1_file_hdr hdr;

	log_free = log_buf = snap_log_read(&nr_logs);
	dprintf("%d\n", nr_logs);
	if (nr_logs < 0)
		goto out;

	for (i = 0; i < nr_logs; i++, log_buf++) {
		if (log_buf->epoch != epoch)
			continue;
		snap_buf = snap_file_read(log_buf->sha1, &hdr);
		if (!snap_buf)
			goto out;
		memcpy(outsha1, snap_buf, SHA1_LEN);
		ret = 0;
		break;
	}
out:
	free(log_free);
	free(snap_buf);
	return ret;
}

static bool is_xattr_enabled(char *path)
{
	int ret, dummy;

	ret = getxattr(path, "user.dummy", &dummy, sizeof(dummy));

	return !(ret == -1 && errno == ENOTSUP);
}

static int farm_init(char *p)
{
	dprintf("use farm store driver\n");
	if (create_directory(p) < 0)
		goto err;

	if (!is_xattr_enabled(p)) {
		eprintf("xattrs are not enabled on %s\n", p);
		goto err;
	}

	if (snap_init() < 0)
		goto err;

	if (default_init(p) < 0)
		goto err;

	return SD_RES_SUCCESS;
err:
	return SD_RES_EIO;
}

static int farm_snapshot(struct siocb *iocb)
{
	unsigned char snap_sha1[SHA1_LEN];
	unsigned char trunk_sha1[SHA1_LEN];
	struct sd_node nodes[SD_MAX_NODES];
	int nr_nodes;
	void *buffer;
	int log_nr, ret = SD_RES_EIO, epoch;

	buffer = snap_log_read(&log_nr);
	if (!buffer)
		goto out;

	epoch = log_nr + 1;
	dprintf("user epoch %d\n", epoch);

	nr_nodes = epoch_log_read(sys->epoch, nodes, sizeof(nodes));
	if (nr_nodes < 0)
		goto out;

	if (trunk_file_write(trunk_sha1) < 0)
		goto out;

	if (snap_file_write(sys->epoch, nodes, nr_nodes,
			    trunk_sha1, snap_sha1) < 0)
		goto out;

	if (snap_log_write(epoch, snap_sha1) < 0)
		goto out;

	ret = SD_RES_SUCCESS;
out:
	free(buffer);
	return ret;
}

static int cleanup_working_dir(void)
{
	DIR *dir;
	struct dirent *d;

	dprintf("try clean up working dir\n");
	dir = opendir(obj_path);
	if (!dir)
		return -1;

	while ((d = readdir(dir))) {
		char p[PATH_MAX];
		if (!strncmp(d->d_name, ".", 1))
			continue;
		snprintf(p, sizeof(p), "%s%s", obj_path, d->d_name);
		if (unlink(p) < 0) {
			eprintf("%s:%m\n", p);
			continue;
		}
		dprintf("remove file %s\n", d->d_name);
	}
	closedir(dir);
	return 0;
}

static int restore_objects_from_snap(uint32_t epoch)
{
	struct sha1_file_hdr hdr;
	struct trunk_entry *trunk_buf, *trunk_free = NULL;
	unsigned char trunk_sha1[SHA1_LEN];
	uint64_t nr_trunks, i;
	int ret = SD_RES_EIO;

	if (get_trunk_sha1(epoch, trunk_sha1) < 0)
		goto out;

	trunk_free = trunk_buf = trunk_file_read(trunk_sha1, &hdr);
	if (!trunk_buf)
		goto out;

	nr_trunks = hdr.priv;
	ret = SD_RES_SUCCESS;
	for (i = 0; i < nr_trunks; i++, trunk_buf++) {
		struct sha1_file_hdr h;
		struct siocb io = { 0 };
		uint64_t oid;
		void *buffer = NULL;

		oid = trunk_buf->oid;
		buffer = sha1_file_read(trunk_buf->sha1, &h);
		if (!buffer) {
			eprintf("oid %"PRIx64" not restored\n", oid);
			goto out;
		}
		io.length = h.size;
		io.buf = buffer;
		ret = default_atomic_put(oid, &io);
		if (ret != SD_RES_SUCCESS) {
			eprintf("oid %"PRIx64" not restored\n", oid);
			goto out;
		} else
			dprintf("oid %"PRIx64" restored\n", oid);

		free(buffer);
	}
out:
	free(trunk_free);
	return ret;
}

static int farm_restore(struct siocb *iocb)
{
	int ret = SD_RES_EIO, epoch = iocb->epoch;

	dprintf("try recover user epoch %d\n", epoch);

	if (cleanup_working_dir() < 0) {
		eprintf("failed to clean up the working dir %m\n");
		goto out;
	}

	ret = restore_objects_from_snap(epoch);
	if (ret != SD_RES_SUCCESS)
		goto out;
out:
	return ret;
}

static int farm_get_snap_file(struct siocb *iocb)
{
	int ret = SD_RES_EIO;
	void *buffer = NULL;
	size_t size;
	int nr;

	dprintf("try get snap file\n");
	buffer = snap_log_read(&nr);
	if (!buffer)
		goto out;
	size = nr * sizeof(struct snap_log);
	memcpy(iocb->buf, buffer, size);
	iocb->length = size;
	ret = SD_RES_SUCCESS;
out:
	free(buffer);
	return ret;
}

struct store_driver farm = {
	.name = "farm",
	.init = farm_init,
	.exist = default_exist,
	.write = default_write,
	.read = default_read,
	.link = default_link,
	.atomic_put = default_atomic_put,
	.end_recover = default_end_recover,
	.snapshot = farm_snapshot,
	.cleanup = default_cleanup,
	.restore = farm_restore,
	.get_snap_file = farm_get_snap_file,
	.format = default_format,
	.purge_obj = default_purge_obj,
	.remove_object = default_remove_object,
	.flush = default_flush,
};

add_store_driver(farm);
