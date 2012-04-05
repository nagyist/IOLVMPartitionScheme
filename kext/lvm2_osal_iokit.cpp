/*-
 * Copyright (C) 2012 Erik Larsson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "lvm2_osal.h"
#include "lvm2_types.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOStorage.h>

#include <machine/limits.h>
#include <sys/errno.h>

__private_extern__ int lvm2_malloc(size_t size, void **out_ptr)
{
	void *ptr;

	ptr = IOMalloc(size);
	if(ptr) {
		*out_ptr = ptr;
		return 0;
	}
	else {
		return ENOMEM;
	}
}

__private_extern__ void lvm2_free(void **ptr, size_t size)
{
	IOFree(*ptr, size);
	*ptr = NULL;
}

/* Device layer implementation. */

struct lvm2_io_buffer {
	IOBufferMemoryDescriptor *buffer;
};

__private_extern__ int lvm2_io_buffer_create(size_t size,
		struct lvm2_io_buffer **out_buf)
{
	int err;
	struct lvm2_io_buffer *buf = NULL;

	err = lvm2_malloc(sizeof(struct lvm2_io_buffer), (void**) &buf);
	if(!err) {
		IOBufferMemoryDescriptor *buffer;

		buffer = IOBufferMemoryDescriptor::withCapacity(
			/* capacity      */ size,
			/* withDirection */ kIODirectionIn);
		if(buffer == NULL) {
			LogError("Error while allocating "
				"IOBufferMemoryDescriptor with buffer size: "
				"%" FMTzu "bytes",
				ARGzu(size));
			err = ENOMEM;
		}
		else {
			buf->buffer = buffer;
			*out_buf = buf;
		}

		if(err) {
			lvm2_free((void**) &buf, sizeof(struct lvm2_io_buffer));
		}
	}

	return err;
}

__private_extern__ const void* lvm2_io_buffer_get_bytes(
		struct lvm2_io_buffer *buf)
{
	return buf->buffer->getBytesNoCopy();
}

__private_extern__ void lvm2_io_buffer_destroy(struct lvm2_io_buffer **buf)
{
	(*buf)->buffer->release();
	lvm2_free((void**) buf, sizeof(struct lvm2_io_buffer));
}

struct lvm2_device {
	IOStorage *storage;
	IOMedia *media;
	u32 block_size;
};

__private_extern__ int lvm2_unix_device_create(IOStorage *const storage,
		IOMedia *const media, struct lvm2_device **const out_dev)
{
	int err;
	UInt64 mediaBlockSize;
	bool mediaIsOpen = false;

	mediaBlockSize = media->getPreferredBlockSize();
	if(mediaBlockSize > U32_MAX) {
		LogError("Unrealistic media block size: %" FMTllu,
			ARGllu(mediaBlockSize));
		err = EINVAL;
	}
	else {
		/* Open the media with read-only access. */

		mediaIsOpen = storage->open(storage, 0, kIOStorageAccessReader);
		if(mediaIsOpen == false) {
			LogError("Error while opening media.");
			err = EACCES;
		}
		else {
			struct lvm2_device *dev;

			err = lvm2_malloc(sizeof(struct lvm2_device),
				(void**) &dev);
			if(!err) {
				memset(dev, 0, sizeof(struct lvm2_device));
				dev->storage = storage;
				dev->media = media;
				dev->block_size = (u32) mediaBlockSize;

				*out_dev = dev;
			}

			if(err) {
				storage->close(storage);
			}
		}
	}

	return err;
}

__private_extern__ void lvm2_unix_device_destroy(struct lvm2_device **dev)
{
	(*dev)->storage->close((*dev)->storage);

	lvm2_free((void**) dev, sizeof(struct lvm2_device));
}

__private_extern__ int lvm2_device_read(struct lvm2_device *const dev,
		const u64 in_pos, const size_t in_count,
		struct lvm2_io_buffer *const in_buf)
{
	int err;
	IOReturn status;

	u32 lead_in;
	u32 lead_out;

	u64 pos;
	size_t count;
	IOBufferMemoryDescriptor *buf;

	if(in_count > SSIZE_MAX)
		return ERANGE;

	lead_in = (u32) (in_pos % dev->block_size);
	lead_out = dev->block_size -
		(u32) ((lead_in + in_count) % dev->block_size);

	if(lead_in != 0 || lead_out != 0 ||
		in_count != in_buf->buffer->getLength())
	{
		u64 aligned_pos;
		size_t aligned_count;
		IOBufferMemoryDescriptor *aligned_buf = NULL;

		aligned_pos = in_pos - lead_in;
		aligned_count = lead_in + count + lead_out;

		LogError("Warning: Unaligned read. Aligning (%" FMTllu ", "
			"%" FMTzu ") -> (%" FMTllu ", %" FMTzu ")...",
			ARGllu(in_pos), ARGzu(in_count), ARGllu(aligned_pos),
			ARGzu(aligned_count));

		aligned_buf = IOBufferMemoryDescriptor::withCapacity(
			/* capacity      */ aligned_count,
			/* withDirection */ kIODirectionIn);
		if(aligned_buf == NULL) {
			LogError("Temporary memory allocation (%" FMTzu " "
				"bytes) failed.",
				ARGzu(aligned_count));
			return ENOMEM;
		}

		pos = aligned_pos;
		count = aligned_count;
		buf = aligned_buf;
	}
	else {
		pos = in_pos;
		count = in_count;
		buf = in_buf->buffer;
	}

	status = dev->media->read(dev->storage, pos, buf);
	if(status != kIOReturnSuccess) {
		err = EIO;
	}
	else {
		if(buf != in_buf->buffer) {
			IOByteCount res;
			res = in_buf->buffer->writeBytes(0,
				buf->getBytesNoCopy(), in_count);
			if(res != in_count) {
				LogError("Failed to write data back into the "
					"input buffer. Wrote "
					"%" FMTzu "/%" FMTzu " bytes.",
					ARGzu(res), ARGzu(in_count));
			}
		}

		err = 0;
	}

	if(buf != in_buf->buffer) {
		buf->release();
	}

	return err;
}
