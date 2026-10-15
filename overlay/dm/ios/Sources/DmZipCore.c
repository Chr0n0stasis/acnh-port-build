// 只编入 ZIP 读取、CRC 和流式解压；不带压缩写入器。
// Include ZIP reading, CRC, and streaming decompression without the archive writer.
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_APIS
#include "../../third_party/miniz/miniz.c"
#include "../../third_party/miniz/miniz_tinfl.c"
#include "../../third_party/miniz/miniz_zip.c"
