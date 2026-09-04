#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

// RN-15b: the write half of the same vendored library, for the block-preview
// export. PNG only, matching the read side — the exporter's output has to be
// something a wiki screenshot can be put beside, and nothing here needs a second
// image format. No new dependency: stb_image_write.h has been sitting in
// vendor/stb-src next to stb_image.h all along.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_WARNING
#include <stb_image_write.h>
