/* The library-map check calls the same conversion as the compiler. */
#include "base/path.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char path[MO_LIMIT_PATH_SIZE + 1];
    if (argc != 2 || !mo_module_path((const uint8_t *)argv[1], strlen(argv[1]), path))
        return 1;
    return printf("%s\n", path) < 0;
}
