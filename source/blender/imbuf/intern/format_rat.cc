/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 *
 * Houdini RAT (Random Access Texture) reading.
 *
 * RAT is SideFX Houdini's tiled, MIP-mapped texture format. It is not publicly documented, so
 * the layout below was derived by inspecting files written by Houdini 20.5 and verified by
 * comparing decoded pixels against the same images converted by Houdini itself. No SideFX
 * source or header was used.
 *
 * Only the "TBF" flavor is handled (magic `fbtH`), which is what modern Houdini writes. The
 * older "RATclassic" flavor (magic `tarH`) has an unrelated layout and is rejected.
 *
 * \section rat_layout File layout
 *
 * \subsection rat_header File header (64 bytes)
 * \code{.unparsed}
 * 0   char[4]  magic "fbtH"
 * 4   uint32   version (5)
 * 8   uint64   file offset of the first chunk of the directory
 * 16  uint32   unused (2)
 * 20  uint32   compression of tiles and tables: 1 = none, 2 = deflate, 3 = blosc
 * 24  ...      reserved, zero
 * 64  ...      first tile payload
 * \endcode
 *
 * \subsection rat_chunk Directory chunk (24 byte header + payload)
 * Chunks form a singly linked list, each holding one record.
 * \code{.unparsed}
 * 0   char[4]  magic, "?ahC" where '?' is the payload compression ('n', 'z' or 'b')
 * 4   uint32   unused
 * 8   uint64   file offset of the next chunk, 0 when last
 * 16  uint32   flags
 * 20  uint32   payload size in bytes, as stored
 * 24  ...      payload, decompressing to a record
 * \endcode
 *
 * \subsection rat_record Record (only the leading fields are used here)
 * \code{.unparsed}
 * 0   uint32   record index
 * 4   uint32   kind: 1 = data (strings, options), 2 = image
 * 8   uint32   pixel storage type (image records)
 * 12  uint32   channel count (image records)
 * 24  uint32   index of the next record in the chain, ~0 when last
 * 32  uint64   file offset of the block offset table
 * 40  uint64   file offset of the block uncompressed size table, 0 when absent
 * 48  uint32   x resolution (image records) / block count (data records)
 * 52  uint32   y resolution (image records)
 * 56  uint32   tile width (image records)
 * 60  uint32   tile height (image records)
 * 176 uint64   file offset of the block stored size table
 * 184 uint32   stored size of the block offset table
 * 188 uint32   stored size of the block uncompressed size table
 * 192 uint32   stored size of the block stored size table
 * \endcode
 *
 * An image record is one MIP level; levels are chained largest first, each half the size of the
 * previous one. Only the largest level is loaded, the rest is skipped.
 *
 * \subsection rat_tiles Tiles
 * A level is cut into `ceil(xres / tile_width) * ceil(yres / tile_height)` tiles, ordered left
 * to right then bottom to top: like #ImBuf, RAT stores rows bottom-up, so tiles need no
 * flipping. Tiles at the right and top edge are clipped to the pixels that remain rather than
 * padded. Within a tile, pixels are channel-interleaved and row-major.
 *
 * The offset table holds one int64 per tile. A negative entry means the tile duplicates an
 * earlier one (Houdini de-duplicates identical tiles); its low 32 bits hold `-1 - index` of the
 * tile to read instead. The stored size table holds one uint32 per tile. A tile or table whose
 * stored size equals its uncompressed size is stored verbatim, otherwise it is compressed with
 * the file's compression method.
 */

#include <algorithm>
#include <climits>
#include <cstring>

#include <zlib.h>

#include "BLI_array.hh"
#include "BLI_math_half.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "CLG_log.h"

#include "IMB_filetype.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

static CLG_LogRef LOG = {"image.rat"};

const char *imb_file_extensions_rat[] = {".rat", nullptr};

namespace {

constexpr size_t RAT_HEADER_SIZE = 64;
constexpr size_t RAT_CHUNK_HEADER_SIZE = 24;
/** Number of leading record bytes this reader relies on. */
constexpr size_t RAT_RECORD_SIZE = 200;
/** Upper bound on the record size, used to size the inflate destination. */
constexpr size_t RAT_RECORD_SIZE_MAX = 4096;
/** Guard against a corrupt or malicious chunk list looping forever. */
constexpr int RAT_CHUNK_NUM_MAX = 8192;
/** Amount of a data block read when looking for the options of the file. */
constexpr size_t RAT_OPTIONS_SIZE_MAX = 64 * 1024;

/** Payload compression, from the leading byte of a chunk magic. */
enum {
  RAT_COMPRESS_NONE = 'n',
  RAT_COMPRESS_DEFLATE = 'z',
  RAT_COMPRESS_BLOSC = 'b',
};

/** Record kind, at byte 4 of a record. */
enum {
  RAT_RECORD_DATA = 1,
  RAT_RECORD_IMAGE = 2,
};

/** Pixel storage type, at byte 8 of an image record. */
enum {
  RAT_TYPE_UINT8 = 5,
  RAT_TYPE_UINT16 = 6,
  RAT_TYPE_HALF = 9,
  RAT_TYPE_FLOAT = 10,
};

/** Bounds checked view over the file contents. */
struct FileSpan {
  const uchar *data = nullptr;
  size_t size = 0;

  bool contains(const uint64_t offset, const uint64_t length) const
  {
    return offset <= size && length <= uint64_t(size) - offset;
  }

  const uchar *at(const uint64_t offset, const uint64_t length) const
  {
    return this->contains(offset, length) ? this->data + offset : nullptr;
  }
};

/* RAT is little-endian and Blender only targets little-endian platforms, but read through
 * memcpy anyway so that the alignment of the file contents does not matter. */

uint32_t read_u32(const uchar *data)
{
  uint32_t value;
  memcpy(&value, data, sizeof(value));
  return value;
}

uint64_t read_u64(const uchar *data)
{
  uint64_t value;
  memcpy(&value, data, sizeof(value));
  return value;
}

/** One entry of the file directory, see \ref rat_record. */
struct RatRecord {
  uint32_t index;
  uint32_t kind;
  uint32_t data_type;
  uint32_t channels;
  uint32_t xres, yres;
  uint32_t tile_width, tile_height;
  /** File offset of the block offset table, holding one int64 per block. */
  uint64_t offset_table;
  /** File offset of the block stored size table, holding one uint32 per block. */
  uint64_t size_table;
  uint32_t offset_table_stored_size;
  uint32_t size_table_stored_size;
};

struct RatFile {
  FileSpan file;
  /** Compression of tiles and tables, see #RAT_COMPRESS_NONE and friends. */
  uint8_t compression = RAT_COMPRESS_NONE;
  Vector<RatRecord> records;
};

const char *compression_name(const uint8_t compression)
{
  switch (compression) {
    case RAT_COMPRESS_NONE:
      return "none";
    case RAT_COMPRESS_DEFLATE:
      return "deflate";
    case RAT_COMPRESS_BLOSC:
      return "blosc";
  }
  return "unknown";
}

/**
 * Inflate a deflate stream into `dst`.
 *
 * Returns the number of bytes written, or -1 on failure. Unless `allow_partial` is set, the
 * stream has to end exactly when `dst_size` bytes have been written.
 */
int64_t rat_inflate(const uchar *src,
                    const size_t src_size,
                    uchar *dst,
                    const size_t dst_size,
                    const bool allow_partial)
{
  z_stream stream = {};
  if (inflateInit(&stream) != Z_OK) {
    return -1;
  }
  stream.next_in = const_cast<Bytef *>(static_cast<const Bytef *>(src));
  stream.avail_in = uInt(src_size);
  stream.next_out = static_cast<Bytef *>(dst);
  stream.avail_out = uInt(dst_size);

  const int status = inflate(&stream, Z_FINISH);
  const int64_t size = int64_t(dst_size) - int64_t(stream.avail_out);
  inflateEnd(&stream);

  if (status == Z_STREAM_END) {
    return size;
  }
  /* Z_OK and Z_BUF_ERROR mean the destination filled up before the stream ended, which is
   * expected when only the leading part of a block is wanted. */
  if (allow_partial && ELEM(status, Z_OK, Z_BUF_ERROR) && size > 0) {
    return size;
  }
  return -1;
}

/**
 * Read one tile or table into `dst`, decompressing it when needed.
 *
 * Blocks whose stored size matches their uncompressed size are stored verbatim, which is what
 * Houdini does for blocks too small or too noisy to gain from compression.
 */
bool rat_read_block(const RatFile &rat,
                    const uint64_t offset,
                    const uint64_t stored_size,
                    uchar *dst,
                    const size_t dst_size)
{
  const uchar *src = rat.file.at(offset, stored_size);
  if (src == nullptr) {
    CLOG_ERROR(&LOG, "Block at %zu is out of bounds", size_t(offset));
    return false;
  }

  if (stored_size == dst_size) {
    memcpy(dst, src, dst_size);
    return true;
  }

  if (rat.compression != RAT_COMPRESS_DEFLATE) {
    CLOG_ERROR(&LOG,
               "Block at %zu uses unsupported %s compression",
               size_t(offset),
               compression_name(rat.compression));
    return false;
  }

  if (rat_inflate(src, stored_size, dst, dst_size, false) != int64_t(dst_size)) {
    CLOG_ERROR(&LOG, "Failed to inflate %zu bytes at %zu", dst_size, size_t(offset));
    return false;
  }
  return true;
}

/** Decode the record carried by the chunk at `offset`, and follow the link to the next chunk. */
bool rat_read_chunk(const FileSpan &file,
                    const uint64_t offset,
                    RatRecord &r_record,
                    uint64_t &r_next_offset)
{
  const uchar *chunk = file.at(offset, RAT_CHUNK_HEADER_SIZE);
  if (chunk == nullptr) {
    CLOG_ERROR(&LOG, "Chunk at %zu is out of bounds", size_t(offset));
    return false;
  }
  if (memcmp(chunk + 1, "ahC", 3) != 0) {
    CLOG_ERROR(&LOG, "Chunk at %zu has an invalid signature", size_t(offset));
    return false;
  }

  const uint8_t compression = chunk[0];
  r_next_offset = read_u64(chunk + 8);
  const uint32_t payload_size = read_u32(chunk + 20);

  const uchar *payload = file.at(offset + RAT_CHUNK_HEADER_SIZE, payload_size);
  if (payload == nullptr) {
    CLOG_ERROR(&LOG, "Chunk payload at %zu is out of bounds", size_t(offset));
    return false;
  }

  uchar record[RAT_RECORD_SIZE_MAX];
  int64_t record_size = -1;
  switch (compression) {
    case RAT_COMPRESS_NONE:
      record_size = int64_t(std::min(size_t(payload_size), sizeof(record)));
      memcpy(record, payload, size_t(record_size));
      break;
    case RAT_COMPRESS_DEFLATE:
      record_size = rat_inflate(payload, payload_size, record, sizeof(record), true);
      break;
    default:
      CLOG_ERROR(&LOG,
                 "Chunk at %zu uses unsupported %s compression",
                 size_t(offset),
                 compression_name(compression));
      return false;
  }

  if (record_size < int64_t(RAT_RECORD_SIZE)) {
    CLOG_ERROR(&LOG, "Chunk at %zu holds a truncated record", size_t(offset));
    return false;
  }

  r_record.index = read_u32(record + 0);
  r_record.kind = read_u32(record + 4);
  r_record.data_type = read_u32(record + 8);
  r_record.channels = read_u32(record + 12);
  r_record.offset_table = read_u64(record + 32);
  r_record.xres = read_u32(record + 48);
  r_record.yres = read_u32(record + 52);
  r_record.tile_width = read_u32(record + 56);
  r_record.tile_height = read_u32(record + 60);
  r_record.size_table = read_u64(record + 176);
  r_record.offset_table_stored_size = read_u32(record + 184);
  r_record.size_table_stored_size = read_u32(record + 192);
  return true;
}

/** Read the file header and every record of the directory. */
bool rat_read_directory(const uchar *mem, const size_t size, RatFile &r_rat)
{
  r_rat.file = {mem, size};

  const uint32_t version = read_u32(mem + 4);
  if (version > 5) {
    CLOG_ERROR(&LOG, "Unsupported file version %u", version);
    return false;
  }

  const uint32_t compression = read_u32(mem + 20);
  switch (compression) {
    case 1:
      r_rat.compression = RAT_COMPRESS_NONE;
      break;
    case 2:
      r_rat.compression = RAT_COMPRESS_DEFLATE;
      break;
    case 3:
      r_rat.compression = RAT_COMPRESS_BLOSC;
      break;
    default:
      CLOG_ERROR(&LOG, "Unsupported compression method %u", compression);
      return false;
  }

  uint64_t offset = read_u64(mem + 8);
  for (int i = 0; offset != 0; i++) {
    if (i == RAT_CHUNK_NUM_MAX) {
      CLOG_ERROR(&LOG, "Directory holds more than %d chunks", RAT_CHUNK_NUM_MAX);
      return false;
    }
    RatRecord record;
    if (!rat_read_chunk(r_rat.file, offset, record, offset)) {
      return false;
    }
    r_rat.records.append(record);
  }

  return true;
}

/** The record of the largest MIP level, or null when the file holds no image. */
const RatRecord *rat_find_largest_level(const RatFile &rat)
{
  const RatRecord *best = nullptr;
  for (const RatRecord &record : rat.records) {
    if (record.kind != RAT_RECORD_IMAGE) {
      continue;
    }
    if (best == nullptr ||
        uint64_t(record.xres) * record.yres > uint64_t(best->xres) * best->yres)
    {
      best = &record;
    }
  }
  return best;
}

/** Size in bytes of one pixel component, or 0 for a type this reader cannot handle. */
int rat_type_size(const uint32_t data_type)
{
  switch (data_type) {
    case RAT_TYPE_UINT8:
      return 1;
    case RAT_TYPE_UINT16:
    case RAT_TYPE_HALF:
      return 2;
    case RAT_TYPE_FLOAT:
      return 4;
  }
  return 0;
}

/**
 * Copy one tile into the 4 channel #ImBuf pixel buffer, converting to the buffer's type.
 *
 * `dst` points at the first pixel of the tile and `dst_stride` is the buffer row length in
 * pixels. RAT and #ImBuf both store rows bottom-up, so rows are copied in order.
 */
template<typename DstType, typename SrcType, typename ConvertFn>
void rat_tile_scatter(DstType *dst,
                      const int64_t dst_stride,
                      const SrcType *src,
                      const int width,
                      const int height,
                      const int channels,
                      const int channels_used,
                      const ConvertFn convert)
{
  for (int y = 0; y < height; y++) {
    DstType *dst_row = dst + int64_t(y) * dst_stride * 4;
    const SrcType *src_row = src + int64_t(y) * width * channels;
    for (int x = 0; x < width; x++) {
      for (int c = 0; c < channels_used; c++) {
        dst_row[x * 4 + c] = convert(src_row[x * channels + c]);
      }
    }
  }
}

/** Expand an n-channel pixel buffer in place to the 4 channels #ImBuf requires. */
template<typename T>
void rat_fill_channels(T *pixels, const int64_t pixel_num, const int channels)
{
  const T alpha = std::is_same_v<T, float> ? T(1) : T(0xFF);
  switch (channels) {
    case 1:
      for (int64_t i = 0; i < pixel_num; i++) {
        T *pixel = pixels + i * 4;
        pixel[1] = pixel[2] = pixel[0];
        pixel[3] = alpha;
      }
      break;
    case 2:
      /* A two channel image is luminance and alpha. */
      for (int64_t i = 0; i < pixel_num; i++) {
        T *pixel = pixels + i * 4;
        pixel[3] = pixel[1];
        pixel[1] = pixel[2] = pixel[0];
      }
      break;
    case 3:
      for (int64_t i = 0; i < pixel_num; i++) {
        pixels[i * 4 + 3] = alpha;
      }
      break;
  }
}

/**
 * Resolve a de-duplicated tile to the tile that actually holds the data.
 *
 * Returns -1 when the reference is broken.
 */
int rat_resolve_tile(const Span<int64_t> offsets, int tile)
{
  for (int hops = 0; offsets[tile] < 0; hops++) {
    if (hops == offsets.size()) {
      CLOG_ERROR(&LOG, "Cyclic tile reference");
      return -1;
    }
    const int32_t reference = int32_t(uint32_t(uint64_t(offsets[tile])));
    const int64_t next = -1 - int64_t(reference);
    if (next < 0 || next >= offsets.size() || next == tile) {
      CLOG_ERROR(&LOG, "Invalid reference in tile %d", tile);
      return -1;
    }
    tile = int(next);
  }
  return tile;
}

/** Decode every tile of `record` into the pixel buffer of `ibuf`. */
bool rat_read_tiles(const RatFile &rat, const RatRecord &record, ImBuf *ibuf)
{
  const int width = int(record.xres);
  const int height = int(record.yres);
  const int tile_width = int(record.tile_width);
  const int tile_height = int(record.tile_height);
  const int channels = int(record.channels);
  const int channels_used = std::min(channels, 4);
  const int type_size = rat_type_size(record.data_type);

  const int tiles_x = (width + tile_width - 1) / tile_width;
  const int tiles_y = (height + tile_height - 1) / tile_height;
  const int tile_num = tiles_x * tiles_y;

  Vector<int64_t> offsets(tile_num);
  Vector<uint32_t> sizes(tile_num);
  if (!rat_read_block(rat,
                      record.offset_table,
                      record.offset_table_stored_size,
                      reinterpret_cast<uchar *>(offsets.data()),
                      size_t(tile_num) * sizeof(int64_t)) ||
      !rat_read_block(rat,
                      record.size_table,
                      record.size_table_stored_size,
                      reinterpret_cast<uchar *>(sizes.data()),
                      size_t(tile_num) * sizeof(uint32_t)))
  {
    return false;
  }

  Array<uchar> tile_data(size_t(tile_width) * tile_height * channels * type_size);

  uchar *byte_buffer = ibuf->byte_buffer.data;
  float *float_buffer = ibuf->float_buffer.data;

  for (int tile = 0; tile < tile_num; tile++) {
    const int x = (tile % tiles_x) * tile_width;
    const int y = (tile / tiles_x) * tile_height;
    /* Tiles along the right and top edge only hold the pixels that are in the image. */
    const int tile_width_used = std::min(tile_width, width - x);
    const int tile_height_used = std::min(tile_height, height - y);
    const size_t tile_size = size_t(tile_width_used) * tile_height_used * channels * type_size;

    const int source_tile = rat_resolve_tile(offsets, tile);
    if (source_tile == -1) {
      return false;
    }
    if (!rat_read_block(
            rat, uint64_t(offsets[source_tile]), sizes[source_tile], tile_data.data(), tile_size))
    {
      return false;
    }

    const int64_t pixel_offset = int64_t(y) * width + x;
    switch (record.data_type) {
      case RAT_TYPE_UINT8:
        rat_tile_scatter(byte_buffer + pixel_offset * 4,
                         width,
                         tile_data.data(),
                         tile_width_used,
                         tile_height_used,
                         channels,
                         channels_used,
                         [](const uchar value) { return value; });
        break;
      case RAT_TYPE_UINT16:
        rat_tile_scatter(float_buffer + pixel_offset * 4,
                         width,
                         reinterpret_cast<const uint16_t *>(tile_data.data()),
                         tile_width_used,
                         tile_height_used,
                         channels,
                         channels_used,
                         [](const uint16_t value) { return float(value) * (1.0f / 65535.0f); });
        break;
      case RAT_TYPE_HALF:
        rat_tile_scatter(float_buffer + pixel_offset * 4,
                         width,
                         reinterpret_cast<const uint16_t *>(tile_data.data()),
                         tile_width_used,
                         tile_height_used,
                         channels,
                         channels_used,
                         [](const uint16_t value) { return math::half_to_float(value); });
        break;
      case RAT_TYPE_FLOAT:
        rat_tile_scatter(float_buffer + pixel_offset * 4,
                         width,
                         reinterpret_cast<const float *>(tile_data.data()),
                         tile_width_used,
                         tile_height_used,
                         channels,
                         channels_used,
                         [](const float value) { return value; });
        break;
    }
  }

  const int64_t pixel_num = int64_t(width) * height;
  if (byte_buffer != nullptr) {
    rat_fill_channels(byte_buffer, pixel_num, channels_used);
  }
  else {
    rat_fill_channels(float_buffer, pixel_num, channels_used);
  }

  return true;
}

/** Read the JSON options block of the file, if it has one. */
bool rat_read_options(const RatFile &rat, Array<char> &r_options, int64_t &r_options_size)
{
  for (const RatRecord &record : rat.records) {
    if (record.kind != RAT_RECORD_DATA) {
      continue;
    }
    /* Data records hold a table of blocks; the options are the block holding JSON. Only tables
     * stored verbatim are handled, which is how Houdini writes these small tables. */
    const int block_num = int(record.offset_table_stored_size / sizeof(uint64_t));
    if (block_num < 1 || block_num > RAT_CHUNK_NUM_MAX) {
      continue;
    }
    Vector<uint64_t> offsets(block_num);
    Vector<uint32_t> sizes(block_num);
    if (!rat_read_block(rat,
                        record.offset_table,
                        record.offset_table_stored_size,
                        reinterpret_cast<uchar *>(offsets.data()),
                        size_t(block_num) * sizeof(uint64_t)) ||
        !rat_read_block(rat,
                        record.size_table,
                        record.size_table_stored_size,
                        reinterpret_cast<uchar *>(sizes.data()),
                        size_t(block_num) * sizeof(uint32_t)))
    {
      continue;
    }

    for (int block = 0; block < block_num; block++) {
      const uchar *data = rat.file.at(offsets[block], sizes[block]);
      if (data == nullptr || sizes[block] == 0) {
        continue;
      }
      /* The uncompressed size of a data block is not always recorded, so inflate as much as
       * fits and give up on anything that does not turn out to be JSON. */
      if (data[0] == '{') {
        r_options_size = std::min(int64_t(sizes[block]), int64_t(RAT_OPTIONS_SIZE_MAX));
        memcpy(r_options.data(), data, size_t(r_options_size));
        return true;
      }
      if (rat.compression != RAT_COMPRESS_DEFLATE) {
        continue;
      }
      const int64_t size = rat_inflate(data,
                                       sizes[block],
                                       reinterpret_cast<uchar *>(r_options.data()),
                                       RAT_OPTIONS_SIZE_MAX,
                                       true);
      if (size > 0 && r_options[0] == '{') {
        r_options_size = size;
        return true;
      }
    }
  }
  return false;
}

/**
 * Read the color space Houdini recorded for the record with index `record_index`.
 *
 * The options block holds a `"channel-<record>::cspace"` entry per image record, naming the
 * color space its pixels are in.
 */
void rat_read_colorspace(const RatFile &rat,
                         const uint32_t record_index,
                         ImFileColorSpace &r_colorspace)
{
  Array<char> options(RAT_OPTIONS_SIZE_MAX);
  int64_t options_size = 0;
  if (!rat_read_options(rat, options, options_size)) {
    return;
  }

  char key[64];
  SNPRINTF(key, "\"channel-%u::cspace\"", record_index);

  const StringRef text(options.data(), options_size);
  const int64_t key_pos = text.find(key);
  if (key_pos == StringRef::not_found) {
    return;
  }
  const int64_t value_pos = text.find("\"value\"", key_pos);
  if (value_pos == StringRef::not_found) {
    return;
  }
  const int64_t open_pos = text.find('"', value_pos + 8);
  if (open_pos == StringRef::not_found) {
    return;
  }
  const int64_t close_pos = text.find('"', open_pos + 1);
  if (close_pos == StringRef::not_found) {
    return;
  }
  const StringRef value = text.substr(open_pos + 1, close_pos - open_pos - 1);

  /* Map the names Houdini writes onto the matching Blender color spaces. Anything else is
   * passed through in case it happens to name a color space of the current OCIO config, unknown
   * names are ignored by the caller. */
  if (value == "srgb") {
    STRNCPY_UTF8(r_colorspace.metadata_colorspace, "sRGB");
  }
  else if (value == "linear") {
    STRNCPY_UTF8(r_colorspace.metadata_colorspace, "Linear Rec.709");
  }
  else if (!value.is_empty()) {
    BLI_strncpy_utf8(r_colorspace.metadata_colorspace,
                     std::string(value).c_str(),
                     sizeof(r_colorspace.metadata_colorspace));
  }
}

/** Check that the largest MIP level is something this reader can turn into an #ImBuf. */
bool rat_validate_level(const RatRecord &record)
{
  if (rat_type_size(record.data_type) == 0) {
    CLOG_ERROR(&LOG, "Unsupported pixel type %u", record.data_type);
    return false;
  }
  if (record.xres == 0 || record.yres == 0 || record.channels == 0 || record.tile_width == 0 ||
      record.tile_height == 0)
  {
    CLOG_ERROR(&LOG, "Image has an invalid size");
    return false;
  }
  if (record.xres > uint32_t(INT_MAX) || record.yres > uint32_t(INT_MAX) ||
      record.tile_width > uint32_t(INT_MAX) || record.tile_height > uint32_t(INT_MAX) ||
      record.channels > uint32_t(INT_MAX))
  {
    CLOG_ERROR(&LOG, "Image is too large");
    return false;
  }
  /* Keep the tile count, the tile size and the pixel buffer well inside what an int and an
   * int64 can hold, so the decoding below cannot overflow. */
  const uint64_t tiles_x = (uint64_t(record.xres) + record.tile_width - 1) / record.tile_width;
  const uint64_t tiles_y = (uint64_t(record.yres) + record.tile_height - 1) / record.tile_height;
  const uint64_t tile_size = uint64_t(record.tile_width) * record.tile_height * record.channels *
                             rat_type_size(record.data_type);
  const uint64_t pixel_num = uint64_t(record.xres) * record.yres;
  if (tiles_x * tiles_y > uint64_t(INT_MAX) || tile_size > uint64_t(INT_MAX) ||
      pixel_num > uint64_t(INT64_MAX) / 16)
  {
    CLOG_ERROR(&LOG, "Image is too large");
    return false;
  }
  return true;
}

}  // namespace

bool imb_is_a_rat(const uchar *mem, const size_t size)
{
  return size >= RAT_HEADER_SIZE && memcmp(mem, "fbtH", 4) == 0;
}

ImBuf *imb_load_rat(const uchar *mem, const size_t size, int flags, ImFileColorSpace &r_colorspace)
{
  if (!imb_is_a_rat(mem, size)) {
    return nullptr;
  }

  RatFile rat;
  if (!rat_read_directory(mem, size, rat)) {
    return nullptr;
  }

  const RatRecord *record = rat_find_largest_level(rat);
  if (record == nullptr) {
    CLOG_ERROR(&LOG, "File holds no image");
    return nullptr;
  }
  if (!rat_validate_level(*record)) {
    return nullptr;
  }

  const int channels_used = std::min(int(record->channels), 4);
  /* Blender reserves all 32 planes for images that carry their own alpha. */
  const int planes = (channels_used == 4) ? 32 : 8 * channels_used;
  const bool is_float = record->data_type != RAT_TYPE_UINT8;

  /* Report the color space before the #IB_test early out below: an image is probed with
   * #IB_test first and keeps whatever that pass reports. Half and float pixels can hold values
   * outside 0..1, integer pixels never do. */
  r_colorspace.is_hdr_float = ELEM(record->data_type, RAT_TYPE_HALF, RAT_TYPE_FLOAT);
  rat_read_colorspace(rat, record->index, r_colorspace);

  if (flags & IB_test) {
    ImBuf *ibuf = IMB_allocImBuf(record->xres, record->yres, planes, 0);
    if (ibuf) {
      ibuf->ftype = IMB_FTYPE_RAT;
    }
    return ibuf;
  }

  const uint buffer_flags = (is_float ? IB_float_data : IB_byte_data) | IB_uninitialized_pixels;
  ImBuf *ibuf = IMB_allocImBuf(record->xres, record->yres, planes, buffer_flags);
  if (ibuf == nullptr) {
    return nullptr;
  }

  if (!rat_read_tiles(rat, *record, ibuf)) {
    IMB_freeImBuf(ibuf);
    return nullptr;
  }

  ibuf->ftype = IMB_FTYPE_RAT;
  if (record->data_type == RAT_TYPE_HALF) {
    ibuf->foptions.flag |= OPENEXR_HALF;
  }

  if (is_float && (flags & IB_byte_data)) {
    IMB_byte_from_float(ibuf);
  }

  return ibuf;
}

}  // namespace blender
