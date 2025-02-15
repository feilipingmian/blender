/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2001-2002 NaN Holding BV. All rights reserved. */

/** \file
 * \ingroup imbuf
 */

/* It's become a bit messy... Basically, only the IMB_ prefixed files
 * should remain. */

#include <stddef.h>

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "IMB_allocimbuf.h"
#include "IMB_colormanagement_intern.h"
#include "IMB_filetype.h"
#include "IMB_metadata.h"

#include "imbuf.h"

#include "MEM_guardedalloc.h"

#include "BLI_implicit_sharing.hh"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

static SpinLock refcounter_spin;

void imb_refcounter_lock_init(void)
{
  BLI_spin_init(&refcounter_spin);
}

void imb_refcounter_lock_exit(void)
{
  BLI_spin_end(&refcounter_spin);
}

#ifndef WIN32
static SpinLock mmap_spin;

void imb_mmap_lock_init(void)
{
  BLI_spin_init(&mmap_spin);
}

void imb_mmap_lock_exit(void)
{
  BLI_spin_end(&mmap_spin);
}

void imb_mmap_lock(void)
{
  BLI_spin_lock(&mmap_spin);
}

void imb_mmap_unlock(void)
{
  BLI_spin_unlock(&mmap_spin);
}
#endif

/* Free the specified buffer storage, freeing memory when needed and restoring the state of the
 * buffer to its defaults. */
template<class BufferType> static void imb_free_buffer(BufferType &buffer)
{
  if (buffer.implicit_sharing) {
    blender::implicit_sharing::free_shared_data(&buffer.data, &buffer.implicit_sharing);
  }
  else if (buffer.data) {
    switch (buffer.ownership) {
      case IB_DO_NOT_TAKE_OWNERSHIP:
        break;

      case IB_TAKE_OWNERSHIP:
        BLI_assert(buffer.implicit_sharing == nullptr);
        MEM_freeN(buffer.data);
        break;
    }
  }

  /* Reset buffer to defaults. */
  buffer.data = nullptr;
  buffer.ownership = IB_DO_NOT_TAKE_OWNERSHIP;
  buffer.implicit_sharing = nullptr;
}

/* Allocate pixel storage of the given buffer. The buffer owns the allocated memory.
 * Returns true of allocation succeeded, false otherwise. */
template<class BufferType>
bool imb_alloc_buffer(
    BufferType &buffer, const uint x, const uint y, const uint channels, const size_t type_size)
{
  buffer.data = static_cast<decltype(BufferType::data)>(
      imb_alloc_pixels(x, y, channels, type_size, __func__));
  if (!buffer.data) {
    return false;
  }

  buffer.ownership = IB_TAKE_OWNERSHIP;
  buffer.implicit_sharing = nullptr;

  return true;
}

/* Make the buffer available for modification.
 * Is achieved by ensuring that the buffer is the only owner of its data. */
template<class BufferType> void imb_make_writeable_buffer(BufferType &buffer)
{
  if (!buffer.data) {
    return;
  }

  switch (buffer.ownership) {
    case IB_DO_NOT_TAKE_OWNERSHIP:
      buffer.data = static_cast<decltype(BufferType::data)>(MEM_dupallocN(buffer.data));
      buffer.ownership = IB_TAKE_OWNERSHIP;

      if (buffer.implicit_sharing) {
        buffer.implicit_sharing->remove_user_and_delete_if_last();
        buffer.implicit_sharing = nullptr;
      }
      break;

    case IB_TAKE_OWNERSHIP:
      break;
  }
}

template<class BufferType>
auto imb_steal_buffer_data(BufferType &buffer) -> decltype(BufferType::data)
{
  if (!buffer.data) {
    return nullptr;
  }

  switch (buffer.ownership) {
    case IB_DO_NOT_TAKE_OWNERSHIP:
      BLI_assert(!"Unexpected behavior: stealing non-owned data pointer");
      return nullptr;

    case IB_TAKE_OWNERSHIP: {
      decltype(BufferType::data) data = buffer.data;

      buffer.data = nullptr;
      buffer.ownership = IB_DO_NOT_TAKE_OWNERSHIP;

      return data;
    }
  }

  BLI_assert_unreachable();

  return nullptr;
}

/* Assign the new data of the buffer which is implicitly shared via the given handle.
 * The old content of the buffer is freed using imb_free_buffer. */
template<class BufferType>
void imb_assign_shared_buffer(BufferType &buffer,
                              decltype(BufferType::data) buffer_data,
                              const ImplicitSharingInfoHandle *implicit_sharing)
{
  imb_free_buffer(buffer);

  if (implicit_sharing) {
    BLI_assert(buffer_data != nullptr);

    blender::implicit_sharing::copy_shared_pointer(
        buffer_data, implicit_sharing, &buffer.data, &buffer.implicit_sharing);
  }
  else {
    BLI_assert(buffer_data == nullptr);
    buffer.data = nullptr;
    buffer.implicit_sharing = nullptr;
  }

  buffer.ownership = IB_DO_NOT_TAKE_OWNERSHIP;
}

void imb_freemipmapImBuf(ImBuf *ibuf)
{
  int a;

  /* Do not trust ibuf->miptot, in some cases IMB_remakemipmap can leave unfreed unused levels,
   * leading to memory leaks... */
  for (a = 0; a < IMB_MIPMAP_LEVELS; a++) {
    if (ibuf->mipmap[a] != nullptr) {
      IMB_freeImBuf(ibuf->mipmap[a]);
      ibuf->mipmap[a] = nullptr;
    }
  }

  ibuf->miptot = 0;
}

void imb_freerectfloatImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return;
  }

  imb_free_buffer(ibuf->float_buffer);

  imb_freemipmapImBuf(ibuf);

  ibuf->flags &= ~IB_rectfloat;
}

void imb_freerectImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return;
  }

  imb_free_buffer(ibuf->byte_buffer);

  imb_freemipmapImBuf(ibuf);

  ibuf->flags &= ~IB_rect;
}

static void freeencodedbufferImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return;
  }

  imb_free_buffer(ibuf->encoded_buffer);

  ibuf->encoded_buffer_size = 0;
  ibuf->encoded_size = 0;

  ibuf->flags &= ~IB_mem;
}

void IMB_freezbufImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return;
  }

  imb_free_buffer(ibuf->z_buffer);

  ibuf->flags &= ~IB_zbuf;
}

void IMB_freezbuffloatImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return;
  }

  imb_free_buffer(ibuf->float_z_buffer);

  ibuf->flags &= ~IB_zbuffloat;
}

void imb_freerectImbuf_all(ImBuf *ibuf)
{
  imb_freerectImBuf(ibuf);
  imb_freerectfloatImBuf(ibuf);
  IMB_freezbufImBuf(ibuf);
  IMB_freezbuffloatImBuf(ibuf);
  freeencodedbufferImBuf(ibuf);
}

void IMB_freeImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return;
  }

  bool needs_free = false;

  BLI_spin_lock(&refcounter_spin);
  if (ibuf->refcounter > 0) {
    ibuf->refcounter--;
  }
  else {
    needs_free = true;
  }
  BLI_spin_unlock(&refcounter_spin);

  if (needs_free) {
    /* Include this check here as the path may be manipulated after creation. */
    BLI_assert_msg(!(ibuf->filepath[0] == '/' && ibuf->filepath[1] == '/'),
                   "'.blend' relative \"//\" must not be used in ImBuf!");

    imb_freerectImbuf_all(ibuf);
    IMB_metadata_free(ibuf->metadata);
    colormanage_cache_free(ibuf);

    if (ibuf->dds_data.data != nullptr) {
      /* dds_data.data is allocated by DirectDrawSurface::readData(), so don't use MEM_freeN! */
      free(ibuf->dds_data.data);
    }
    MEM_freeN(ibuf);
  }
}

void IMB_refImBuf(ImBuf *ibuf)
{
  BLI_spin_lock(&refcounter_spin);
  ibuf->refcounter++;
  BLI_spin_unlock(&refcounter_spin);
}

ImBuf *IMB_makeSingleUser(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return nullptr;
  }

  BLI_spin_lock(&refcounter_spin);
  const bool is_single = (ibuf->refcounter == 0);
  BLI_spin_unlock(&refcounter_spin);
  if (is_single) {
    return ibuf;
  }

  ImBuf *rval = IMB_dupImBuf(ibuf);

  IMB_metadata_copy(rval, ibuf);

  IMB_freeImBuf(ibuf);

  return rval;
}

bool addzbufImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return false;
  }

  IMB_freezbufImBuf(ibuf);

  if (!imb_alloc_buffer(ibuf->z_buffer, ibuf->x, ibuf->y, 1, sizeof(uint))) {
    return false;
  }

  ibuf->flags |= IB_zbuf;

  return false;
}

bool addzbuffloatImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return false;
  }

  IMB_freezbuffloatImBuf(ibuf);

  if (!imb_alloc_buffer(ibuf->float_z_buffer, ibuf->x, ibuf->y, 1, sizeof(float))) {
    return false;
  }

  ibuf->flags |= IB_zbuffloat;

  return true;
}

bool imb_addencodedbufferImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return false;
  }

  freeencodedbufferImBuf(ibuf);

  if (ibuf->encoded_buffer_size == 0) {
    ibuf->encoded_buffer_size = 10000;
  }

  ibuf->encoded_size = 0;

  if (!imb_alloc_buffer(ibuf->encoded_buffer, ibuf->encoded_buffer_size, 1, 1, sizeof(uint8_t))) {
    return false;
  }

  ibuf->flags |= IB_mem;

  return true;
}

bool imb_enlargeencodedbufferImBuf(ImBuf *ibuf)
{
  if (ibuf == nullptr) {
    return false;
  }

  if (ibuf->encoded_buffer_size < ibuf->encoded_size) {
    printf("%s: error in parameters\n", __func__);
    return false;
  }

  uint newsize = 2 * ibuf->encoded_buffer_size;
  if (newsize < 10000) {
    newsize = 10000;
  }

  ImBufByteBuffer new_buffer;
  if (!imb_alloc_buffer(new_buffer, newsize, 1, 1, sizeof(uint8_t))) {
    return false;
  }

  if (ibuf->encoded_buffer.data) {
    memcpy(new_buffer.data, ibuf->encoded_buffer.data, ibuf->encoded_size);
  }
  else {
    ibuf->encoded_size = 0;
  }

  imb_free_buffer(ibuf->encoded_buffer);

  ibuf->encoded_buffer = new_buffer;
  ibuf->encoded_buffer_size = newsize;
  ibuf->flags |= IB_mem;

  return true;
}

void *imb_alloc_pixels(uint x, uint y, uint channels, size_t typesize, const char *alloc_name)
{
  /* Protect against buffer overflow vulnerabilities from files specifying
   * a width and height that overflow and alloc too little memory. */
  if (!(uint64_t(x) * uint64_t(y) < (SIZE_MAX / (channels * typesize)))) {
    return nullptr;
  }

  size_t size = size_t(x) * size_t(y) * size_t(channels) * typesize;
  return MEM_callocN(size, alloc_name);
}

bool imb_addrectfloatImBuf(ImBuf *ibuf, const uint channels)
{
  if (ibuf == nullptr) {
    return false;
  }

  /* NOTE: Follows the historical code.
   * Is unclear if it is desired or not to free mipmaps. If mipmaps are to be preserved a simple
   * `imb_free_buffer(ibuf->float_buffer)` can be used instead. */
  if (ibuf->float_buffer.data) {
    imb_freerectfloatImBuf(ibuf); /* frees mipmap too, hrm */
  }

  if (!imb_alloc_buffer(ibuf->float_buffer, ibuf->x, ibuf->y, channels, sizeof(float))) {
    return false;
  }

  ibuf->channels = channels;
  ibuf->flags |= IB_rectfloat;

  return true;
}

bool imb_addrectImBuf(ImBuf *ibuf)
{
  /* Question; why also add ZBUF (when `planes > 32`)? */

  if (ibuf == nullptr) {
    return false;
  }

  /* Don't call imb_freerectImBuf, it frees mipmaps,
   * this call is used only too give float buffers display. */
  imb_free_buffer(ibuf->byte_buffer);

  if (!imb_alloc_buffer(ibuf->byte_buffer, ibuf->x, ibuf->y, 4, sizeof(uint8_t))) {
    return false;
  }

  ibuf->flags |= IB_rect;

  if (ibuf->planes > 32) {
    return addzbufImBuf(ibuf);
  }

  return true;
}

uint8_t *IMB_steal_byte_buffer(ImBuf *ibuf)
{
  uint8_t *data = imb_steal_buffer_data(ibuf->byte_buffer);
  ibuf->flags &= ~IB_rect;
  return data;
}

float *IMB_steal_float_buffer(ImBuf *ibuf)
{
  float *data = imb_steal_buffer_data(ibuf->float_buffer);
  ibuf->flags &= ~IB_rectfloat;
  return data;
}

uint8_t *IMB_steal_encoded_buffer(ImBuf *ibuf)
{
  uint8_t *data = imb_steal_buffer_data(ibuf->encoded_buffer);

  ibuf->encoded_size = 0;
  ibuf->encoded_buffer_size = 0;

  ibuf->flags &= ~IB_mem;

  return data;
}

void IMB_make_writable_byte_buffer(ImBuf *ibuf)
{
  imb_make_writeable_buffer(ibuf->byte_buffer);
}

void IMB_make_writable_float_buffer(ImBuf *ibuf)
{
  imb_make_writeable_buffer(ibuf->float_buffer);
}

void IMB_assign_shared_byte_buffer(ImBuf *ibuf,
                                   uint8_t *buffer_data,
                                   const ImplicitSharingInfoHandle *implicit_sharing)
{
  imb_free_buffer(ibuf->byte_buffer);
  ibuf->flags &= ~IB_rect;

  if (buffer_data) {
    imb_assign_shared_buffer(ibuf->byte_buffer, buffer_data, implicit_sharing);
    ibuf->flags |= IB_rect;
  }
}

void IMB_assign_shared_float_buffer(ImBuf *ibuf,
                                    float *buffer_data,
                                    const ImplicitSharingInfoHandle *implicit_sharing)
{
  imb_free_buffer(ibuf->float_buffer);
  ibuf->flags &= ~IB_rectfloat;

  if (buffer_data) {
    imb_assign_shared_buffer(ibuf->float_buffer, buffer_data, implicit_sharing);
    ibuf->flags |= IB_rectfloat;
  }
}

void IMB_assign_shared_float_z_buffer(ImBuf *ibuf,
                                      float *buffer_data,
                                      const ImplicitSharingInfoHandle *implicit_sharing)
{
  imb_free_buffer(ibuf->float_z_buffer);
  ibuf->flags &= ~IB_zbuffloat;

  if (buffer_data) {
    imb_assign_shared_buffer(ibuf->float_z_buffer, buffer_data, implicit_sharing);
    ibuf->flags |= IB_zbuffloat;
  }
}

void IMB_assign_byte_buffer(ImBuf *ibuf, uint8_t *buffer_data, const ImBufOwnership ownership)
{
  imb_free_buffer(ibuf->byte_buffer);
  ibuf->flags &= ~IB_rect;

  if (buffer_data) {
    ibuf->byte_buffer.data = buffer_data;
    ibuf->byte_buffer.ownership = ownership;

    ibuf->flags |= IB_rect;
  }
}

void IMB_assign_float_buffer(ImBuf *ibuf, float *buffer_data, const ImBufOwnership ownership)
{
  imb_free_buffer(ibuf->float_buffer);
  ibuf->flags &= ~IB_rectfloat;

  if (buffer_data) {
    ibuf->float_buffer.data = buffer_data;
    ibuf->float_buffer.ownership = ownership;

    ibuf->flags |= IB_rectfloat;
  }
}

void IMB_assign_z_buffer(struct ImBuf *ibuf, int *buffer_data, ImBufOwnership ownership)
{
  imb_free_buffer(ibuf->z_buffer);
  ibuf->flags &= ~IB_zbuf;

  if (buffer_data) {
    ibuf->z_buffer.ownership = ownership;
    ibuf->z_buffer.data = buffer_data;

    ibuf->flags |= IB_zbuf;
  }
}

void IMB_assign_float_z_buffer(struct ImBuf *ibuf, float *buffer_data, ImBufOwnership ownership)
{
  imb_free_buffer(ibuf->float_z_buffer);
  ibuf->flags &= ~IB_zbuffloat;

  if (buffer_data) {
    ibuf->float_z_buffer.ownership = ownership;
    ibuf->float_z_buffer.data = buffer_data;

    ibuf->flags |= IB_zbuffloat;
  }
}

ImBuf *IMB_allocFromBufferOwn(
    uint8_t *byte_buffer, float *float_buffer, uint w, uint h, uint channels)
{
  if (!(byte_buffer || float_buffer)) {
    return nullptr;
  }

  ImBuf *ibuf = IMB_allocImBuf(w, h, 32, 0);

  ibuf->channels = channels;

  if (float_buffer) {
    /* TODO(sergey): The 4 channels is the historical code. Should probably be `channels`, but
     * needs a dedicated investigation. */
    BLI_assert(MEM_allocN_len(float_buffer) == sizeof(float[4]) * w * h);
    IMB_assign_float_buffer(ibuf, float_buffer, IB_TAKE_OWNERSHIP);
  }

  if (byte_buffer) {
    BLI_assert(MEM_allocN_len(byte_buffer) == sizeof(uint8_t[4]) * w * h);
    IMB_assign_byte_buffer(ibuf, byte_buffer, IB_TAKE_OWNERSHIP);
  }

  return ibuf;
}

struct ImBuf *IMB_allocFromBuffer(
    const uint8_t *byte_buffer, const float *float_buffer, uint w, uint h, uint channels)
{
  ImBuf *ibuf = nullptr;

  if (!(byte_buffer || float_buffer)) {
    return nullptr;
  }

  ibuf = IMB_allocImBuf(w, h, 32, 0);

  ibuf->channels = channels;

  /* NOTE: Avoid #MEM_dupallocN since the buffers might not be allocated using guarded-allocation.
   */
  if (float_buffer) {
    /* TODO(sergey): The 4 channels is the historical code. Should probably be `channels`, but
     * needs a dedicated investigation. */
    imb_alloc_buffer(ibuf->float_buffer, w, h, 4, sizeof(float));

    memcpy(ibuf->float_buffer.data, float_buffer, sizeof(float[4]) * w * h);
  }

  if (byte_buffer) {
    imb_alloc_buffer(ibuf->byte_buffer, w, h, 4, sizeof(uint8_t));

    memcpy(ibuf->byte_buffer.data, byte_buffer, sizeof(uint8_t[4]) * w * h);
  }

  return ibuf;
}

ImBuf *IMB_allocImBuf(uint x, uint y, uchar planes, uint flags)
{
  ImBuf *ibuf = MEM_cnew<ImBuf>("ImBuf_struct");

  if (ibuf) {
    if (!IMB_initImBuf(ibuf, x, y, planes, flags)) {
      IMB_freeImBuf(ibuf);
      return nullptr;
    }
  }

  return ibuf;
}

bool IMB_initImBuf(struct ImBuf *ibuf, uint x, uint y, uchar planes, uint flags)
{
  memset(ibuf, 0, sizeof(ImBuf));

  ibuf->x = x;
  ibuf->y = y;
  ibuf->planes = planes;
  ibuf->ftype = IMB_FTYPE_PNG;
  /* The '15' means, set compression to low ratio but not time consuming. */
  ibuf->foptions.quality = 15;
  /* float option, is set to other values when buffers get assigned. */
  ibuf->channels = 4;
  /* IMB_DPI_DEFAULT -> pixels-per-meter. */
  ibuf->ppm[0] = ibuf->ppm[1] = IMB_DPI_DEFAULT / 0.0254f;

  if (flags & IB_rect) {
    if (imb_addrectImBuf(ibuf) == false) {
      return false;
    }
  }

  if (flags & IB_rectfloat) {
    if (imb_addrectfloatImBuf(ibuf, ibuf->channels) == false) {
      return false;
    }
  }

  if (flags & IB_zbuf) {
    if (addzbufImBuf(ibuf) == false) {
      return false;
    }
  }

  if (flags & IB_zbuffloat) {
    if (addzbuffloatImBuf(ibuf) == false) {
      return false;
    }
  }

  /* assign default spaces */
  colormanage_imbuf_set_default_spaces(ibuf);

  return true;
}

ImBuf *IMB_dupImBuf(const ImBuf *ibuf1)
{
  ImBuf *ibuf2, tbuf;
  int flags = 0;
  int a, x, y;

  if (ibuf1 == nullptr) {
    return nullptr;
  }

  /* TODO(sergey): Use implicit sharing. */

  if (ibuf1->byte_buffer.data) {
    flags |= IB_rect;
  }
  if (ibuf1->float_buffer.data) {
    flags |= IB_rectfloat;
  }
  if (ibuf1->z_buffer.data) {
    flags |= IB_zbuf;
  }
  if (ibuf1->float_z_buffer.data) {
    flags |= IB_zbuffloat;
  }

  x = ibuf1->x;
  y = ibuf1->y;

  ibuf2 = IMB_allocImBuf(x, y, ibuf1->planes, flags);
  if (ibuf2 == nullptr) {
    return nullptr;
  }

  if (flags & IB_rect) {
    memcpy(ibuf2->byte_buffer.data, ibuf1->byte_buffer.data, size_t(x) * y * 4 * sizeof(uint8_t));
  }

  if (flags & IB_rectfloat) {
    memcpy(ibuf2->float_buffer.data,
           ibuf1->float_buffer.data,
           size_t(ibuf1->channels) * x * y * sizeof(float));
  }

  if (flags & IB_zbuf) {
    memcpy(ibuf2->z_buffer.data, ibuf1->z_buffer.data, size_t(x) * y * sizeof(int));
  }

  if (flags & IB_zbuffloat) {
    memcpy(ibuf2->float_buffer.data, ibuf1->float_buffer.data, size_t(x) * y * sizeof(float));
  }

  if (ibuf1->encoded_buffer.data) {
    ibuf2->encoded_buffer_size = ibuf1->encoded_buffer_size;
    if (imb_addencodedbufferImBuf(ibuf2) == false) {
      IMB_freeImBuf(ibuf2);
      return nullptr;
    }

    memcpy(ibuf2->encoded_buffer.data, ibuf1->encoded_buffer.data, ibuf1->encoded_size);
  }

  /* silly trick to copy the entire contents of ibuf1 struct over to ibuf */
  tbuf = *ibuf1;

  /* fix pointers */
  tbuf.byte_buffer = ibuf2->byte_buffer;
  tbuf.float_buffer = ibuf2->float_buffer;
  tbuf.encoded_buffer = ibuf2->encoded_buffer;
  tbuf.z_buffer = ibuf2->z_buffer;
  tbuf.float_z_buffer = ibuf2->float_z_buffer;
  for (a = 0; a < IMB_MIPMAP_LEVELS; a++) {
    tbuf.mipmap[a] = nullptr;
  }
  tbuf.dds_data.data = nullptr;

  /* set malloc flag */
  tbuf.refcounter = 0;

  /* for now don't duplicate metadata */
  tbuf.metadata = nullptr;

  tbuf.display_buffer_flags = nullptr;
  tbuf.colormanage_cache = nullptr;

  *ibuf2 = tbuf;

  return ibuf2;
}

size_t IMB_get_rect_len(const ImBuf *ibuf)
{
  return size_t(ibuf->x) * size_t(ibuf->y);
}

size_t IMB_get_size_in_memory(ImBuf *ibuf)
{
  int a;
  size_t size = 0, channel_size = 0;

  size += sizeof(ImBuf);

  if (ibuf->byte_buffer.data) {
    channel_size += sizeof(char);
  }

  if (ibuf->float_buffer.data) {
    channel_size += sizeof(float);
  }

  size += channel_size * ibuf->x * ibuf->y * ibuf->channels;

  if (ibuf->miptot) {
    for (a = 0; a < ibuf->miptot; a++) {
      if (ibuf->mipmap[a]) {
        size += IMB_get_size_in_memory(ibuf->mipmap[a]);
      }
    }
  }

  return size;
}
